#pragma once

#include <atomic>
#include <chrono>
#include <cmath>
#include <string_view>
#include <thread>

#include "../core/enums.hpp"
#include "../core/memory_pool.hpp"
#include "../core/spsc_ring.hpp"
#include "../core/timestamp.hpp"
#include "message_types.hpp"
#include "../networking/raw_message_pool.hpp"
#include "base_parser.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace crypto_lob::exchanges::binance {

// Forward declaration for CRTP
class BinanceSpotParser;

}  // namespace crypto_lob::exchanges::binance

// Specialize ParserTraits for BinanceSpotParser
template <>
struct crypto_lob::parsing::ParserTraits<crypto_lob::exchanges::binance::BinanceSpotParser> {
    static constexpr crypto_lob::core::ExchangeId exchange_id = crypto_lob::core::ExchangeId::BINANCE_SPOT;
    static constexpr bool supports_checksum = false;
};

namespace crypto_lob::exchanges::binance {

using namespace crypto_lob::parsing;
using namespace crypto_lob::exchanges::base;
using namespace crypto_lob::core;

/**
 * High-performance Binance Spot WebSocket JSON parser
 *
 * Supports WebSocket streams:
 * - Depth updates (symbol@depth)
 * - Aggregate trades (symbol@aggTrade)
 * - Individual trades (symbol@trade)
 * - Book ticker (symbol@bookTicker)
 *
 * Optimized for HFT with:
 * - Zero-allocation parsing using memory pool
 * - Thread-local simdjson parser reuse
 * - Precise decimal string to uint64_t conversion
 * - Minimal string copies via string_view
 */
class BinanceSpotParser : public ParserBase<BinanceSpotParser> {
  public:
    explicit BinanceSpotParser(Pool& message_pool) noexcept : ParserBase(message_pool) {}

    // Required CRTP implementations

    /**
     * Parse snapshot messages from WebSocket depth stream
     * Message format: depth@symbol with lastUpdateId, bids[], asks[]
     */
    [[nodiscard]] MessagePtr parse_snapshot_impl(std::string_view json_data,
                                                 const std::string& symbol,
                                                 Timestamp receive_time,
                                                 ParseErrorInfo* error_out = nullptr) noexcept {
        auto msg_guard = allocate_message();
        if (!msg_guard) {
            set_error(error_out, ParseError::BUFFER_TOO_SMALL);
            return nullptr;
        }

        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return nullptr;
            }

            auto* msg = msg_guard.get();
            initialize_header(*msg, MessageType::SNAPSHOT, symbol, receive_time);

            // Parse lastUpdateId for snapshot
            uint64_t last_update_id;
            if (!find_and_parse_uint64(json_data, "lastUpdateId", last_update_id, error_out)) {
                return nullptr;
            }
            msg->header.exchange_sequence = last_update_id;

            // Parse bids and asks arrays
            if (!parse_price_levels_from_json(json_data, "bids", msg->bids.data(), msg->header.bid_count, error_out) ||
                !parse_price_levels_from_json(json_data, "asks", msg->asks.data(), msg->header.ask_count, error_out)) {
                return nullptr;
            }

            return msg_guard.release();

        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return nullptr;
        }
    }

    /**
     * Parse update messages from various WebSocket streams
     * Dispatches based on event type field
     */
    [[nodiscard]] MessagePtr parse_update_impl(std::string_view json_data,
                                               const std::string& symbol,
                                               Timestamp receive_time,
                                               ParseErrorInfo* error_out = nullptr) noexcept {
        try {
            // Try to parse as JSON first
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return nullptr;
            }

            // Detect message type by checking for event field
            std::string event_type;
            if (find_string_field(json_data, "e", event_type)) {
                // Has event field - dispatch based on type
                if (event_type == "depthUpdate") {
                    return parse_depth_update(json_data, symbol, receive_time, error_out);
                } else if (event_type == "aggTrade") {
                    return parse_agg_trade(json_data, symbol, receive_time, error_out);
                } else if (event_type == "trade") {
                    return parse_trade(json_data, symbol, receive_time, error_out);
                } else if (event_type == "bookTicker") {
                    return parse_book_ticker(json_data, symbol, receive_time, error_out);
                } else {
                    set_error(error_out, ParseError::UNSUPPORTED_MESSAGE_TYPE);
                    return nullptr;
                }
            } else {
                // No event field - check if it's bookTicker (spot) which omits "e"
                uint64_t dummy;
                if (find_and_parse_uint64(json_data, "u", dummy, nullptr) &&
                    find_string_field(json_data, "b", event_type) &&  // reuse variable for price check
                    find_string_field(json_data, "a", event_type)) {
                    return parse_book_ticker(json_data, symbol, receive_time, error_out);
                }

                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return nullptr;
            }

        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return nullptr;
        }
    }

    /**
     * Parse quantity string to scaled uint64_t
     * Uses 10^9 scaling factor for consistency with Price class
     */
    [[nodiscard]] bool parse_quantity_string(std::string_view qty_str,
                                             uint64_t& result_out,
                                             ParseErrorInfo* error_out = nullptr) const noexcept {
        if (qty_str.empty()) {
            set_error(error_out, ParseError::INVALID_QUANTITY_FORMAT);
            return false;
        }

        // Find decimal point position
        size_t decimal_pos = qty_str.find('.');

        // Extract integer part
        std::string_view integer_part;
        std::string_view fractional_part;

        if (decimal_pos == std::string_view::npos) {
            // No decimal point - entire string is integer
            integer_part = qty_str;
            fractional_part = "";
        } else {
            integer_part = qty_str.substr(0, decimal_pos);
            fractional_part = qty_str.substr(decimal_pos + 1);
        }

        // Validate characters
        for (char c : integer_part) {
            if (c < '0' || c > '9') {
                set_error(error_out, ParseError::INVALID_QUANTITY_FORMAT);
                return false;
            }
        }
        for (char c : fractional_part) {
            if (c < '0' || c > '9') {
                set_error(error_out, ParseError::INVALID_QUANTITY_FORMAT);
                return false;
            }
        }

        // Parse integer part
        uint64_t integer_value = 0;
        for (char c : integer_part) {
            if (integer_value > (UINT64_MAX - (c - '0')) / 10) {
                set_error(error_out, ParseError::NUMERIC_OVERFLOW);
                return false;
            }
            integer_value = integer_value * 10 + (c - '0');
        }

        // Parse fractional part with 10^9 scaling
        uint64_t fractional_value = 0;
        uint64_t scale = 1;
        constexpr uint64_t SCALE_FACTOR = 1'000'000'000ULL;  // 10^9

        for (size_t i = 0; i < fractional_part.size() && i < 9; ++i) {
            scale *= 10;
            fractional_value = fractional_value * 10 + (fractional_part[i] - '0');
        }

        // Scale fractional part to 10^9
        while (scale < SCALE_FACTOR) {
            fractional_value *= 10;
            scale *= 10;
        }

        // Check for overflow in final calculation
        if (integer_value > (UINT64_MAX - fractional_value) / SCALE_FACTOR) {
            set_error(error_out, ParseError::NUMERIC_OVERFLOW);
            return false;
        }

        result_out = integer_value * SCALE_FACTOR + fractional_value;
        return true;
    }

  private:
    void initialize_header(MarketDataMessage& msg,
                           MessageType type,
                           const std::string& /* symbol */,
                           Timestamp receive_time) noexcept {
        msg.header = MessageHeader{};  // Zero-initialize
        msg.header.type = type;
        msg.header.exchange_id = ExchangeId::BINANCE_SPOT;
        msg.header.local_timestamp = receive_time;

        // For exchange_timestamp, we still need wall clock time since it represents
        // when the exchange generated the message. We'll extract this from the JSON later.
        // For now, use high-resolution clock as fallback
        msg.header.exchange_timestamp =
            std::chrono::duration_cast<Timestamp>(std::chrono::steady_clock::now().time_since_epoch());
    }

    // Helper to find and parse uint64 field without simdjson ondemand issues
    bool find_and_parse_uint64(std::string_view json_data,
                               const char* field_name,
                               uint64_t& result,
                               ParseErrorInfo* error_out) const noexcept {
        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return false;
            }

            auto field = doc_result.find_field(field_name);
            if (field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }

            auto value_result = field.get_uint64();
            if (value_result.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }

            result = value_result.value();
            return true;
        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return false;
        }
    }

    // Helper to find string field
    bool find_string_field(std::string_view json_data, const char* field_name, std::string& result) const noexcept {
        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error())
                return false;

            auto field = doc_result.find_field(field_name);
            if (field.error())
                return false;

            auto str_result = field.get_string();
            if (str_result.error())
                return false;

            result = std::string(str_result.value());
            return true;
        } catch (...) {
            return false;
        }
    }

    // Parse price levels from JSON using separate parsing pass
    bool parse_price_levels_from_json(std::string_view json_data,
                                      const char* field_name,
                                      PriceLevel* levels,
                                      uint16_t& count_out,
                                      ParseErrorInfo* error_out) const noexcept {
        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return false;
            }

            auto levels_field = doc_result.find_field(field_name);
            if (levels_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }

            auto levels_array = levels_field.get_array();
            if (levels_array.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }

            size_t level_count = 0;
            for (auto level_item : levels_array.value()) {
                if (level_count >= MAX_LEVELS_PER_MESSAGE)
                    break;

                auto level_array = level_item.get_array();
                if (level_array.error()) {
                    set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                    return false;
                }

                auto iter = level_array.value().begin();

                // Parse price (first element)
                if (iter == level_array.value().end()) {
                    set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                    return false;
                }

                auto price_str = (*iter).get_string();
                if (price_str.error()) {
                    set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                    return false;
                }

                if (!parse_price_string(price_str.value(), levels[level_count].price, error_out)) {
                    return false;
                }
                ++iter;

                // Parse quantity (second element)
                if (iter == level_array.value().end()) {
                    set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                    return false;
                }

                auto qty_str = (*iter).get_string();
                if (qty_str.error()) {
                    set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                    return false;
                }

                if (!parse_quantity_string(qty_str.value(), levels[level_count].quantity, error_out)) {
                    return false;
                }

                level_count++;
            }

            count_out = static_cast<uint16_t>(level_count);
            return true;
        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return false;
        }
    }

    [[nodiscard]] MessagePtr parse_depth_update(std::string_view json_data,
                                                const std::string& symbol,
                                                Timestamp receive_time,
                                                ParseErrorInfo* error_out) noexcept {
        auto msg_guard = allocate_message();
        if (!msg_guard) {
            set_error(error_out, ParseError::BUFFER_TOO_SMALL);
            return nullptr;
        }

        auto* msg = msg_guard.get();
        initialize_header(*msg, MessageType::DELTA, symbol, receive_time);

        // Parse sequence IDs (U, u)
        uint64_t first_update_id, last_update_id;
        if (!find_and_parse_uint64(json_data, "U", first_update_id, error_out) ||
            !find_and_parse_uint64(json_data, "u", last_update_id, error_out)) {
            return nullptr;
        }
        msg->header.exchange_sequence = last_update_id;

        // Parse event time (E) - optional
        uint64_t event_time_ms;
        if (find_and_parse_uint64(json_data, "E", event_time_ms, nullptr)) {
            msg->header.exchange_timestamp = timestamp_from_millis(static_cast<int64_t>(event_time_ms));
        }

        // Parse bids and asks
        if (!parse_price_levels_from_json(json_data, "b", msg->bids.data(), msg->header.bid_count, error_out) ||
            !parse_price_levels_from_json(json_data, "a", msg->asks.data(), msg->header.ask_count, error_out)) {
            return nullptr;
        }

        return msg_guard.release();
    }

    [[nodiscard]] MessagePtr parse_agg_trade(std::string_view json_data,
                                             const std::string& symbol,
                                             Timestamp receive_time,
                                             ParseErrorInfo* error_out) noexcept {
        auto msg_guard = allocate_message();
        if (!msg_guard) {
            set_error(error_out, ParseError::BUFFER_TOO_SMALL);
            return nullptr;
        }

        auto* msg = msg_guard.get();
        initialize_header(*msg, MessageType::TRADE, symbol, receive_time);

        // Parse aggregate trade ID (a)
        uint64_t trade_id;
        if (!find_and_parse_uint64(json_data, "a", trade_id, error_out)) {
            return nullptr;
        }
        msg->header.exchange_sequence = trade_id;

        // Parse trade time (T) - optional
        uint64_t trade_time_ms;
        if (find_and_parse_uint64(json_data, "T", trade_time_ms, nullptr)) {
            msg->header.exchange_timestamp = timestamp_from_millis(static_cast<int64_t>(trade_time_ms));
        }

        // Parse price and quantity
        if (!parse_trade_price_qty(json_data, msg->bids[0], error_out)) {
            return nullptr;
        }

        msg->header.bid_count = 1;
        msg->header.ask_count = 0;

        return msg_guard.release();
    }

    [[nodiscard]] MessagePtr parse_trade(std::string_view json_data,
                                         const std::string& symbol,
                                         Timestamp receive_time,
                                         ParseErrorInfo* error_out) noexcept {
        auto msg_guard = allocate_message();
        if (!msg_guard) {
            set_error(error_out, ParseError::BUFFER_TOO_SMALL);
            return nullptr;
        }

        auto* msg = msg_guard.get();
        initialize_header(*msg, MessageType::TRADE, symbol, receive_time);

        // Parse trade ID (t)
        uint64_t trade_id;
        if (!find_and_parse_uint64(json_data, "t", trade_id, error_out)) {
            return nullptr;
        }
        msg->header.exchange_sequence = trade_id;

        // Parse trade time (T) - optional
        uint64_t trade_time_ms;
        if (find_and_parse_uint64(json_data, "T", trade_time_ms, nullptr)) {
            msg->header.exchange_timestamp = timestamp_from_millis(static_cast<int64_t>(trade_time_ms));
        }

        // Parse price and quantity
        if (!parse_trade_price_qty(json_data, msg->bids[0], error_out)) {
            return nullptr;
        }

        msg->header.bid_count = 1;
        msg->header.ask_count = 0;

        return msg_guard.release();
    }

    [[nodiscard]] MessagePtr parse_book_ticker(std::string_view json_data,
                                               const std::string& symbol,
                                               Timestamp receive_time,
                                               ParseErrorInfo* error_out) noexcept {
        auto msg_guard = allocate_message();
        if (!msg_guard) {
            set_error(error_out, ParseError::BUFFER_TOO_SMALL);
            return nullptr;
        }

        auto* msg = msg_guard.get();
        initialize_header(*msg, MessageType::TICKER, symbol, receive_time);

        // Parse update ID (u)
        uint64_t update_id;
        if (!find_and_parse_uint64(json_data, "u", update_id, error_out)) {
            return nullptr;
        }
        msg->header.exchange_sequence = update_id;

        // Parse best bid and ask
        if (!parse_book_ticker_levels(json_data, msg->bids[0], msg->asks[0], error_out)) {
            return nullptr;
        }

        msg->header.bid_count = 1;
        msg->header.ask_count = 1;

        return msg_guard.release();
    }

    // Helper to parse price and quantity for trades
    bool parse_trade_price_qty(std::string_view json_data,
                               PriceLevel& level,
                               ParseErrorInfo* error_out) const noexcept {
        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc_result = parser.iterate(padded);
            if (doc_result.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return false;
            }

            // Parse price
            auto price_field = doc_result.find_field("p");
            if (price_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto price_str = price_field.get_string();
            if (price_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_price_string(price_str.value(), level.price, error_out)) {
                return false;
            }

            // Need to restart parsing for quantity
            auto padded2 = simdjson::padded_string(json_data);
            auto doc_result2 = parser.iterate(padded2);
            if (doc_result2.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return false;
            }

            // Parse quantity
            auto qty_field = doc_result2.find_field("q");
            if (qty_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto qty_str = qty_field.get_string();
            if (qty_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_quantity_string(qty_str.value(), level.quantity, error_out)) {
                return false;
            }

            return true;
        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return false;
        }
    }

    // Helper to parse book ticker bid/ask levels
    bool parse_book_ticker_levels(std::string_view json_data,
                                  PriceLevel& bid_level,
                                  PriceLevel& ask_level,
                                  ParseErrorInfo* error_out) const noexcept {
        try {
            auto& parser = get_parser();

            // Parse bid price (b)
            auto padded1 = simdjson::padded_string(json_data);
            auto doc1 = parser.iterate(padded1);
            if (doc1.error()) {
                set_error(error_out, ParseError::INVALID_JSON);
                return false;
            }
            auto bid_price_field = doc1.find_field("b");
            if (bid_price_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto bid_price_str = bid_price_field.get_string();
            if (bid_price_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_price_string(bid_price_str.value(), bid_level.price, error_out)) {
                return false;
            }

            // Parse bid quantity (B)
            auto padded2 = simdjson::padded_string(json_data);
            auto doc2 = parser.iterate(padded2);
            auto bid_qty_field = doc2.find_field("B");
            if (bid_qty_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto bid_qty_str = bid_qty_field.get_string();
            if (bid_qty_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_quantity_string(bid_qty_str.value(), bid_level.quantity, error_out)) {
                return false;
            }

            // Parse ask price (a)
            auto padded3 = simdjson::padded_string(json_data);
            auto doc3 = parser.iterate(padded3);
            auto ask_price_field = doc3.find_field("a");
            if (ask_price_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto ask_price_str = ask_price_field.get_string();
            if (ask_price_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_price_string(ask_price_str.value(), ask_level.price, error_out)) {
                return false;
            }

            // Parse ask quantity (A)
            auto padded4 = simdjson::padded_string(json_data);
            auto doc4 = parser.iterate(padded4);
            auto ask_qty_field = doc4.find_field("A");
            if (ask_qty_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return false;
            }
            auto ask_qty_str = ask_qty_field.get_string();
            if (ask_qty_str.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            if (!parse_quantity_string(ask_qty_str.value(), ask_level.quantity, error_out)) {
                return false;
            }

            return true;
        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return false;
        }
    }
};

/**
 * Dedicated parser thread for Binance Spot WebSocket messages
 *
 * RESPONSIBILITY:
 * This thread consumes raw JSON messages from the network thread and
 * parses them into MarketDataMessage structures for the LOB worker thread.
 *
 * THREADING MODEL:
 * - Runs on dedicated Parser thread (one per exchange)
 * - Consumes from Network thread via lock-free SPSCRing
 * - Produces to LOB Worker thread via lock-free SPSCRing
 * - Zero contention design with thread-local resources
 *
 * MESSAGE FLOW:
 * Network Thread → RawMessageQueue → Parser Thread → ParsedMessageQueue → LOB Worker
 *
 * PERFORMANCE:
 * - Thread-local parser instance (no contention)
 * - Message pool for zero-allocation parsing
 * - ~20ns queue operations
 * - RDTSC timestamps for latency tracking
 * - CPU core pinning for cache locality
 *
 * ERROR HANDLING:
 * - Parse errors: Message dropped, stats incremented
 * - Queue full: Message dropped with backpressure signal
 * - Graceful shutdown via stop flag
 */
class BinanceSpotParserThread {
  public:
    using RawMessageQueue = core::SPSCRing<networking::RawMessage*>;
    using ParsedMessageQueue = core::SPSCRing<exchanges::base::MarketDataMessage*>;
    using MessagePool = core::MemoryPool<exchanges::base::MarketDataMessage>;

    struct Config {
        size_t message_pool_size;  // Pre-allocated parsed messages
        int cpu_core;              // CPU core to pin thread to (-1 = no pinning)
        bool enable_stats;         // Track parsing statistics

        Config() : message_pool_size(8192), cpu_core(-1), enable_stats(true) {}
    };

    struct Stats {
        std::atomic<uint64_t> messages_parsed{0};
        std::atomic<uint64_t> parse_errors{0};
        std::atomic<uint64_t> queue_full_drops{0};
        std::atomic<uint64_t> total_parse_time_ns{0};
        std::atomic<uint64_t> min_parse_time_ns{UINT64_MAX};
        std::atomic<uint64_t> max_parse_time_ns{0};
    };

    BinanceSpotParserThread(RawMessageQueue& raw_queue, ParsedMessageQueue& parsed_queue, const Config& config = {})
        : raw_queue_(raw_queue),
          parsed_queue_(parsed_queue),
          config_(config),
          message_pool_(config.message_pool_size),
          parser_(message_pool_),
          running_(false) {}

    ~BinanceSpotParserThread() {
        stop();
    }

    // Start the parser thread
    void start() {
        if (running_.load(std::memory_order_acquire)) {
            return;  // Already running
        }

        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&BinanceSpotParserThread::run, this);

        // Pin to CPU core if configured
        if (config_.cpu_core >= 0) {
            pin_thread_to_core(thread_.native_handle(), config_.cpu_core);
        }
    }

    // Stop the parser thread
    void stop() {
        if (!running_.load(std::memory_order_acquire)) {
            return;  // Already stopped
        }

        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Get current statistics (returns copies of atomic values)
    struct StatsSnapshot {
        uint64_t messages_parsed;
        uint64_t parse_errors;
        uint64_t queue_full_drops;
        uint64_t total_parse_time_ns;
        uint64_t min_parse_time_ns;
        uint64_t max_parse_time_ns;
    };

    StatsSnapshot get_stats() const {
        return StatsSnapshot{.messages_parsed = stats_.messages_parsed.load(std::memory_order_relaxed),
                             .parse_errors = stats_.parse_errors.load(std::memory_order_relaxed),
                             .queue_full_drops = stats_.queue_full_drops.load(std::memory_order_relaxed),
                             .total_parse_time_ns = stats_.total_parse_time_ns.load(std::memory_order_relaxed),
                             .min_parse_time_ns = stats_.min_parse_time_ns.load(std::memory_order_relaxed),
                             .max_parse_time_ns = stats_.max_parse_time_ns.load(std::memory_order_relaxed)};
    }

    // Reset statistics
    void reset_stats() {
        stats_.messages_parsed.store(0, std::memory_order_relaxed);
        stats_.parse_errors.store(0, std::memory_order_relaxed);
        stats_.queue_full_drops.store(0, std::memory_order_relaxed);
        stats_.total_parse_time_ns.store(0, std::memory_order_relaxed);
        stats_.min_parse_time_ns.store(UINT64_MAX, std::memory_order_relaxed);
        stats_.max_parse_time_ns.store(0, std::memory_order_relaxed);
    }

  private:
    // Main parsing loop
    void run() {
        // Thread-local raw message pool for returning messages
        auto& raw_pool = networking::get_thread_pool();

        while (running_.load(std::memory_order_acquire)) {
            networking::RawMessage* raw_msg = nullptr;

            // Try to get a raw message from the queue
            if (!raw_queue_.try_pop(raw_msg)) {
                // No message available, yield CPU
                std::this_thread::yield();
                continue;
            }

            // Parse the message
            auto start_time = core::rdtsc();
            auto* parsed_msg = parse_message(raw_msg);
            auto parse_time = core::rdtsc() - start_time;

            // Update statistics
            if (config_.enable_stats) {
                update_stats(parsed_msg != nullptr, parse_time);
            }

            // Return raw message to pool
            raw_pool.release(raw_msg);

            // Queue parsed message if successful
            if (parsed_msg) {
                if (!parsed_queue_.try_push(parsed_msg)) {
                    // Queue full - return message to pool
                    message_pool_.destroy(parsed_msg);
                    stats_.queue_full_drops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    // Parse a raw message into MarketDataMessage
    exchanges::base::MarketDataMessage* parse_message(networking::RawMessage* raw_msg) {
        if (!raw_msg || raw_msg->size == 0) {
            return nullptr;
        }

        // Convert raw data to string_view
        std::string_view json_data(raw_msg->data, raw_msg->size);

        // Detect message type and parse accordingly
        // For now, we'll parse as update (the parser will detect the actual type)
        // In production, we'd examine the JSON to determine snapshot vs update

        // Convert RDTSC timestamp to nanoseconds
        auto receive_time = std::chrono::nanoseconds(raw_msg->timestamp_ns);

        // TODO: Extract symbol from the message or maintain symbol mapping
        std::string symbol = "BTCUSDT";  // Placeholder - should be extracted from message

        parsing::ParseErrorInfo error_info;
        exchanges::base::MarketDataMessage* parsed = nullptr;

        // Try parsing as update first (most common)
        parsed = parser_.parse_update(json_data, symbol, receive_time, &error_info);

        if (!parsed && error_info.error == parsing::ParseError::MISSING_REQUIRED_FIELD) {
            // Might be a snapshot
            parsed = parser_.parse_snapshot(json_data, symbol, receive_time, &error_info);
        }

        if (!parsed) {
            stats_.parse_errors.fetch_add(1, std::memory_order_relaxed);
        }

        return parsed;
    }

    // Update parsing statistics
    void update_stats(bool success, uint64_t parse_time_cycles) {
        if (success) {
            stats_.messages_parsed.fetch_add(1, std::memory_order_relaxed);
        }

        // Convert cycles to nanoseconds (approximate)
        // Assuming 3GHz CPU: 1 cycle ≈ 0.33ns
        uint64_t parse_time_ns = parse_time_cycles / 3;

        stats_.total_parse_time_ns.fetch_add(parse_time_ns, std::memory_order_relaxed);

        // Update min/max with CAS loop
        uint64_t current_min = stats_.min_parse_time_ns.load(std::memory_order_relaxed);
        while (parse_time_ns < current_min) {
            if (stats_.min_parse_time_ns.compare_exchange_weak(current_min, parse_time_ns, std::memory_order_relaxed)) {
                break;
            }
        }

        uint64_t current_max = stats_.max_parse_time_ns.load(std::memory_order_relaxed);
        while (parse_time_ns > current_max) {
            if (stats_.max_parse_time_ns.compare_exchange_weak(current_max, parse_time_ns, std::memory_order_relaxed)) {
                break;
            }
        }
    }

    // Pin thread to specific CPU core
    static void pin_thread_to_core(std::thread::native_handle_type handle, int core) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core, &cpuset);
        pthread_setaffinity_np(handle, sizeof(cpu_set_t), &cpuset);
#else
        (void)handle;
        (void)core;
        // CPU pinning not supported on this platform
#endif
    }

  private:
    // Queues for communication
    RawMessageQueue& raw_queue_;        // Input: raw JSON from network thread
    ParsedMessageQueue& parsed_queue_;  // Output: parsed messages to LOB worker

    // Configuration
    Config config_;

    // Parser resources
    MessagePool message_pool_;  // Pool for parsed messages
    BinanceSpotParser parser_;  // The actual JSON parser

    // Thread management
    std::atomic<bool> running_;
    std::thread thread_;

    // Statistics
    Stats stats_;
};

}  // namespace crypto_lob::exchanges::binance