#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/cache.hpp"
#include "core/enums.hpp"
#include "core/price.hpp"

namespace crypto_lob::exchanges::base {

// Import core types into exchange namespace
using crypto_lob::core::ExchangeId;
using crypto_lob::core::Price;
using crypto_lob::core::Side;

// Common timestamp type - nanoseconds since epoch
using Timestamp = std::chrono::nanoseconds;

// Convert milliseconds to nanoseconds safely
constexpr Timestamp timestamp_from_millis(int64_t millis) noexcept {
    // Use chrono for overflow-safe conversion
    return std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{millis});
}

// Price level data - matches orderbook/price_level.hpp format
struct PriceLevel {
    Price price;
    uint64_t quantity;  // Raw quantity, not Price type to avoid extra conversions

    constexpr PriceLevel() noexcept : price(Price::zero()), quantity(0) {}
    constexpr PriceLevel(Price p, uint64_t q) noexcept : price(p), quantity(q) {}

    [[nodiscard]] constexpr bool operator==(const PriceLevel& other) const noexcept {
        return price == other.price && quantity == other.quantity;
    }
};

// Message type enumeration for fast dispatch
enum class MessageType : uint8_t {
    SNAPSHOT = 0,
    DELTA = 1,
    TRADE = 2,
    TICKER = 3,
    HEARTBEAT = 4,
    CONNECTION_STATUS = 5,
    SUBSCRIPTION_ACK = 6
};

// Maximum levels we support in a single message
// 512 covers all exchange WebSocket messages (most cap at 400-500)
static constexpr size_t MAX_LEVELS_PER_MESSAGE = 512;

// Base message header - common to all messages
// Hot data packed together for cache efficiency
struct alignas(64) MessageHeader {
    // Hot data - 32 bytes
    MessageType type;              // 1 byte
    ExchangeId exchange_id;        // 1 byte
    uint16_t instrument_index;     // 2 bytes - index into instrument table
    uint32_t sequence_number;      // 4 bytes - internal sequence
    Timestamp exchange_timestamp;  // 8 bytes
    Timestamp local_timestamp;     // 8 bytes
    uint64_t exchange_sequence;    // 8 bytes - exchange's sequence/update_id

    // Cold data - checksum and flags
    uint32_t checksum;       // 4 bytes - CRC32 for some exchanges
    uint16_t bid_count;      // 2 bytes - number of bid levels
    uint16_t ask_count;      // 2 bytes - number of ask levels
    uint8_t flags;           // 1 byte - various flags
    uint8_t reserved[23]{};  // Padding to 64 bytes - zero-initialized

    constexpr MessageHeader() noexcept
        : type(MessageType::SNAPSHOT),
          exchange_id(ExchangeId::BINANCE_SPOT),
          instrument_index(0),
          sequence_number(0),
          exchange_timestamp(Timestamp{0}),
          local_timestamp(Timestamp{0}),
          exchange_sequence(0),
          checksum(0),
          bid_count(0),
          ask_count(0),
          flags(0),
          reserved{} {}
};

static_assert(sizeof(MessageHeader) == 64, "MessageHeader must be cache-line sized");
static_assert(alignof(MessageHeader) == 64, "MessageHeader must be cache-aligned");

// Flags for MessageHeader.flags field
enum MessageFlags : uint8_t {
    FLAG_NONE = 0,
    FLAG_TICK_BY_TICK = 1 << 0,    // OKX tick-by-tick update
    FLAG_HAS_CHECKSUM = 1 << 1,    // Checksum field is valid
    FLAG_SEQUENCE_RESET = 1 << 2,  // Sequence number reset
    FLAG_SNAPSHOT_SYNC = 1 << 3,   // Part of snapshot synchronization
    FLAG_COMPRESSED = 1 << 4,      // Data is compressed (future use)
};

// Exchange-specific sequence tracking
// Different exchanges use different sequence number schemes
struct alignas(64) SequenceInfo {
    // Binance/Gate.io style: first_update_id (U) and last_update_id (u)
    uint64_t first_update_id;
    uint64_t last_update_id;

    // KuCoin style: sequence_start and sequence_end
    uint64_t sequence_start;
    uint64_t sequence_end;

    // OKX style: previous_update_id
    uint64_t previous_update_id;

    // Single sequence number (Bitget, Bybit)
    uint64_t sequence_number;

    // Reserved for future use - zero-initialized
    uint64_t reserved[2]{};

    constexpr SequenceInfo() noexcept
        : first_update_id(0),
          last_update_id(0),
          sequence_start(0),
          sequence_end(0),
          previous_update_id(0),
          sequence_number(0),
          reserved{} {}

    // Helper to set values based on exchange type
    // For OKX: val1 = previous_update_id (U), val2 = update_id (u)
    void set_for_exchange(ExchangeId exchange, uint64_t val1, uint64_t val2 = 0) noexcept {
        // Clear all fields first to prevent stale data
        *this = SequenceInfo{};

        switch (exchange) {
            case ExchangeId::BINANCE_SPOT:
            case ExchangeId::BINANCE_FUT:
                first_update_id = val1;
                last_update_id = val2 ? val2 : val1;
                break;
            case ExchangeId::KUCOIN_SPOT:
            case ExchangeId::KUCOIN_FUT:
                sequence_start = val1;
                sequence_end = val2 ? val2 : val1;
                break;
            case ExchangeId::OKX_SPOT:
            case ExchangeId::OKX_FUT:
                previous_update_id = val1;
                sequence_number = val2;
                break;
            case ExchangeId::BYBIT_SPOT:
            case ExchangeId::BYBIT_FUT:
                sequence_number = val1;
                break;
            default:
                // Unknown exchange - fail fast
                std::terminate();
        }
    }
};

static_assert(sizeof(SequenceInfo) == 64, "SequenceInfo must be cache-line sized");
static_assert(alignof(SequenceInfo) == 64, "SequenceInfo must be cache-aligned");

// Market data message - combines snapshot and delta
// Uses fixed arrays to avoid heap allocation
struct alignas(64) MarketDataMessage {
    MessageHeader header;
    SequenceInfo sequence;

    // Fixed arrays for price levels
    // These arrays are sized to handle worst case but only
    // header.bid_count/ask_count entries are valid
    std::array<PriceLevel, MAX_LEVELS_PER_MESSAGE> bids;
    std::array<PriceLevel, MAX_LEVELS_PER_MESSAGE> asks;

    // Prevent accidental copies of this large structure
    MarketDataMessage(const MarketDataMessage&) = delete;
    MarketDataMessage& operator=(const MarketDataMessage&) = delete;

    // Default constructor is allowed
    MarketDataMessage() = default;

    // Move operations are allowed for pool allocation
    MarketDataMessage(MarketDataMessage&&) = default;
    MarketDataMessage& operator=(MarketDataMessage&&) = default;

    // Check if this is a snapshot or delta based on header
    [[nodiscard]] bool is_snapshot() const noexcept {
        return header.type == MessageType::SNAPSHOT;
    }

    [[nodiscard]] bool is_delta() const noexcept {
        return header.type == MessageType::DELTA;
    }

    // Get valid bid levels with bounds checking
    [[nodiscard]] std::span<const PriceLevel> get_bids() const noexcept {
        const size_t count = std::min(static_cast<size_t>(header.bid_count), bids.size());
        return std::span<const PriceLevel>(bids.data(), count);
    }

    // Get valid ask levels with bounds checking
    [[nodiscard]] std::span<const PriceLevel> get_asks() const noexcept {
        const size_t count = std::min(static_cast<size_t>(header.ask_count), asks.size());
        return std::span<const PriceLevel>(asks.data(), count);
    }

    // Check continuity with previous message
    [[nodiscard]] bool is_continuous_with(const MarketDataMessage& prev) const noexcept {
        if (header.exchange_id != prev.header.exchange_id)
            return false;

        switch (header.exchange_id) {
            case ExchangeId::BINANCE_SPOT:
            case ExchangeId::BINANCE_FUT:
                return sequence.first_update_id == prev.sequence.last_update_id + 1;

            case ExchangeId::KUCOIN_SPOT:
            case ExchangeId::KUCOIN_FUT:
                // KuCoin requires strict increment (no overlaps)
                return sequence.sequence_start == prev.sequence.sequence_end + 1;

            case ExchangeId::OKX_SPOT:
            case ExchangeId::OKX_FUT:
                return sequence.previous_update_id == prev.sequence.sequence_number;

            case ExchangeId::BYBIT_SPOT:
            case ExchangeId::BYBIT_FUT:
                return sequence.sequence_number == prev.sequence.sequence_number + 1;

            default:
                return false;
        }
    }
};

// Trade message - single executed trade
// Layout optimized to keep hot fields in first cache line
struct alignas(64) TradeMessage {
    MessageHeader header;  // header.type = MessageType::TRADE

    // Hot data - frequently accessed fields (24 bytes)
    Price price;             // 8 bytes
    uint64_t quantity;       // 8 bytes
    Side side;               // 1 byte
    bool is_maker;           // 1 byte
    uint8_t reserved1[6]{};  // 6 bytes padding to align trade_id - zero-initialized

    // Cold data - rarely accessed
    uint64_t trade_id;        // 8 bytes
    uint8_t reserved2[32]{};  // Padding to 128 bytes (2 cache lines) - zero-initialized

    constexpr TradeMessage() noexcept
        : header(),
          price(Price::zero()),
          quantity(0),
          side(Side::BUY),
          is_maker(false),
          reserved1{},
          trade_id(0),
          reserved2{} {
        header.type = MessageType::TRADE;
    }
};

static_assert(sizeof(TradeMessage) == 128, "TradeMessage should be 2 cache lines");

// Ticker/Summary message
struct alignas(64) TickerMessage {
    MessageHeader header;  // header.type = MessageType::TICKER

    // Price data
    Price best_bid;
    Price best_ask;
    uint64_t best_bid_qty;
    uint64_t best_ask_qty;
    Price last_price;
    Price volume_24h;
    Price high_24h;
    Price low_24h;

    constexpr TickerMessage() noexcept
        : header(),
          best_bid(Price::zero()),
          best_ask(Price::zero()),
          best_bid_qty(0),
          best_ask_qty(0),
          last_price(Price::zero()),
          volume_24h(Price::zero()),
          high_24h(Price::zero()),
          low_24h(Price::zero()) {
        header.type = MessageType::TICKER;
    }
};

static_assert(sizeof(TickerMessage) == 128, "TickerMessage should be 2 cache lines");

// Connection status message
enum class ConnectionStatus : uint8_t {
    CONNECTED = 0,
    DISCONNECTED = 1,
    RECONNECTING = 2,
    AUTHENTICATED = 3,
    SUBSCRIBED = 4,
    ERROR = 5
};

struct alignas(64) ConnectionMessage {
    MessageHeader header;  // header.type = MessageType::CONNECTION_STATUS
    ConnectionStatus status;
    uint8_t error_code;
    uint16_t retry_count;
    uint32_t reserved1{};
    char details[56];  // Short error/status message (padded to 128 bytes total)

    constexpr ConnectionMessage() noexcept
        : header(), status(ConnectionStatus::DISCONNECTED), error_code(0), retry_count(0), reserved1{}, details{} {
        header.type = MessageType::CONNECTION_STATUS;
        // details{} already zero-initializes in constexpr context
    }
};

static_assert(sizeof(ConnectionMessage) == 128, "ConnectionMessage should be 2 cache lines");

// Unified message type using a union for zero-copy access
// This allows us to avoid runtime polymorphism while keeping type safety
union alignas(64) Message {
    MessageHeader header;  // Common header for type dispatch
    MarketDataMessage market_data;
    TradeMessage trade;
    TickerMessage ticker;
    ConnectionMessage connection;

    // Default constructor creates header only (minimal overhead)
    constexpr Message() noexcept : header() {}

    // Type-safe accessors
    [[nodiscard]] MessageType type() const noexcept {
        return header.type;
    }

    [[nodiscard]] bool is_market_data() const noexcept {
        return header.type == MessageType::SNAPSHOT || header.type == MessageType::DELTA;
    }
};

// Message pool hint - for pre-allocating message buffers
struct MessagePoolConfig {
    size_t market_data_capacity = 10000;  // Number of market data messages
    size_t trade_capacity = 5000;         // Number of trade messages
    size_t ticker_capacity = 1000;        // Number of ticker messages
    size_t other_capacity = 1000;         // Other message types
};

}  // namespace crypto_lob::exchanges::base