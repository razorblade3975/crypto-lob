#pragma once

#include <cstdint>
#include <vector>

#include "core/enums.hpp"
#include "core/price.hpp"
#include "core/timestamp.hpp"
#include "exchanges/base/message_types.hpp"
#include "orderbook/order_book.hpp"
#include "orderbook/price_level.hpp"

namespace crypto_lob::orderbook {

// Type aliases for clarity between different PriceLevel types
using ExchangePriceLevel = exchanges::base::PriceLevel;  // From message types
using CorePriceLevel = core::PriceLevel;                 // For order book

/**
 * HFT-optimized Order Book Synchronizer
 *
 * Design Philosophy:
 * - NO buffering - simpler, faster, more deterministic
 * - Return status codes for zero-overhead communication
 * - State machine: AWAITING_SNAPSHOT → SYNCHRONIZED
 * - Gap detection triggers re-snapshot request
 *
 * The synchronizer does NOT buffer messages. If a gap is detected,
 * it immediately requests a new snapshot. This is optimal for HFT:
 * - Snapshot fetch is 50-500ms anyway
 * - Buffering adds complexity and memory overhead
 * - Simpler state machine = more predictable latency
 */
class OrderBookSynchronizer {
  public:
    enum class ProcessStatus : uint8_t {
        OK = 0,              // Message processed successfully
        NEEDS_SNAPSHOT = 1,  // Gap detected or not synchronized
        STALE = 2,           // Old message, discarded
    };

    struct ProcessResult {
        ProcessStatus status;
        bool top_of_book_changed;  // For downstream processing
    };

    OrderBookSynchronizer(core::OrderBook& order_book) noexcept
        : order_book_(order_book),
          state_(State::AWAITING_SNAPSHOT),
          last_processed_sequence_(0),
          expected_next_first_id_(0) {
        // Initialize statistics
        stats_.gaps_detected = 0;
        stats_.snapshots_processed = 0;
        stats_.deltas_processed = 0;
        stats_.stale_messages = 0;
    }

    [[nodiscard]] ProcessResult process_message(exchanges::base::MarketDataMessage* msg) noexcept {
        using exchanges::base::MessageType;

        if (msg->header.type == MessageType::SNAPSHOT) {
            return process_snapshot(msg);
        } else if (msg->header.type == MessageType::DELTA) {
            return process_delta(msg);
        }

        // Other message types pass through
        return {ProcessStatus::OK, false};
    }

    [[nodiscard]] bool needs_snapshot() const noexcept {
        return state_ == State::AWAITING_SNAPSHOT;
    }

    [[nodiscard]] bool is_synchronized() const noexcept {
        return state_ == State::SYNCHRONIZED;
    }

    [[nodiscard]] uint64_t get_last_sequence() const noexcept {
        return last_processed_sequence_;
    }

    struct Statistics {
        uint64_t gaps_detected;
        uint64_t snapshots_processed;
        uint64_t deltas_processed;
        uint64_t stale_messages;
    };

    [[nodiscard]] const Statistics& get_stats() const noexcept {
        return stats_;
    }

    void reset() noexcept {
        state_ = State::AWAITING_SNAPSHOT;
        last_processed_sequence_ = 0;
        expected_next_first_id_ = 0;
    }

  private:
    enum class State : uint8_t { AWAITING_SNAPSHOT, SYNCHRONIZED };

    core::OrderBook& order_book_;
    State state_;

    // Sequence tracking
    uint64_t last_processed_sequence_;
    uint64_t expected_next_first_id_;  // For Binance U/u tracking

    // Statistics
    Statistics stats_;

    [[nodiscard]] ProcessResult process_snapshot(exchanges::base::MarketDataMessage* msg) noexcept {
        // Convert exchange PriceLevels to core PriceLevels for OrderBook
        std::vector<CorePriceLevel> bid_levels;
        std::vector<CorePriceLevel> ask_levels;

        bid_levels.reserve(msg->header.bid_count);
        for (size_t i = 0; i < msg->header.bid_count; ++i) {
            const auto& exchange_level = msg->bids[i];
            bid_levels.emplace_back(exchange_level.price, exchange_level.quantity);
        }

        ask_levels.reserve(msg->header.ask_count);
        for (size_t i = 0; i < msg->header.ask_count; ++i) {
            const auto& exchange_level = msg->asks[i];
            ask_levels.emplace_back(exchange_level.price, exchange_level.quantity);
        }

        // Apply snapshot to order book
        bool top_changed = order_book_.snapshot(bid_levels, ask_levels);

        // Update sequence tracking based on exchange
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                // For Binance, snapshot has lastUpdateId
                last_processed_sequence_ = msg->header.exchange_sequence;
                expected_next_first_id_ = msg->header.exchange_sequence + 1;
                break;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                // KuCoin uses sequence_end from snapshot
                last_processed_sequence_ = msg->sequence.sequence_end;
                expected_next_first_id_ = msg->sequence.sequence_end + 1;
                break;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                // OKX uses sequence_number
                last_processed_sequence_ = msg->sequence.sequence_number;
                break;

            default:
                // Generic exchange uses header sequence
                last_processed_sequence_ = msg->header.exchange_sequence;
                break;
        }

        state_ = State::SYNCHRONIZED;
        stats_.snapshots_processed++;

        return {ProcessStatus::OK, top_changed};
    }

    [[nodiscard]] ProcessResult process_delta(exchanges::base::MarketDataMessage* msg) noexcept {
        if (state_ == State::AWAITING_SNAPSHOT) {
            // Not synchronized yet, need snapshot first
            return {ProcessStatus::NEEDS_SNAPSHOT, false};
        }

        // Check if this is a stale message (older than our snapshot)
        if (is_stale_message(msg)) {
            stats_.stale_messages++;
            return {ProcessStatus::STALE, false};
        }

        // Check sequence continuity
        if (!is_sequence_continuous(msg)) {
            // Gap detected - need new snapshot
            stats_.gaps_detected++;
            state_ = State::AWAITING_SNAPSHOT;
            reset();
            return {ProcessStatus::NEEDS_SNAPSHOT, false};
        }

        // Apply the delta
        bool top_changed = apply_delta(msg);

        // Update sequence tracking
        update_sequence_tracking(msg);

        stats_.deltas_processed++;
        return {ProcessStatus::OK, top_changed};
    }

    [[nodiscard]] bool is_stale_message(const exchanges::base::MarketDataMessage* msg) const noexcept {
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                // Message is stale if its last_update_id is less than our last processed
                return msg->sequence.last_update_id < last_processed_sequence_;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                return msg->sequence.sequence_end < last_processed_sequence_;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                return msg->sequence.sequence_number <= last_processed_sequence_;

            default:
                return msg->header.exchange_sequence <= last_processed_sequence_;
        }
    }

    [[nodiscard]] bool is_sequence_continuous(const exchanges::base::MarketDataMessage* msg) const noexcept {
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                // Binance: first_update_id should be last_processed + 1
                // Special case: can apply if first_update_id <= last_processed + 1 <= last_update_id
                if (msg->sequence.first_update_id <= expected_next_first_id_ &&
                    expected_next_first_id_ <= msg->sequence.last_update_id) {
                    return true;
                }
                return msg->sequence.first_update_id == expected_next_first_id_;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                // KuCoin: sequence_start should be last + 1
                return msg->sequence.sequence_start == last_processed_sequence_ + 1;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                // OKX: previous_update_id should match last_processed
                return msg->sequence.previous_update_id == last_processed_sequence_;

            default:
                // Generic: simple increment
                return msg->header.exchange_sequence == last_processed_sequence_ + 1;
        }
    }

    void update_sequence_tracking(const exchanges::base::MarketDataMessage* msg) noexcept {
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                last_processed_sequence_ = msg->sequence.last_update_id;
                expected_next_first_id_ = msg->sequence.last_update_id + 1;
                break;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                last_processed_sequence_ = msg->sequence.sequence_end;
                break;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                last_processed_sequence_ = msg->sequence.sequence_number;
                break;

            default:
                last_processed_sequence_ = msg->header.exchange_sequence;
                break;
        }
    }

    [[nodiscard]] bool apply_delta(exchanges::base::MarketDataMessage* msg) noexcept {
        // Apply updates to order book
        bool top_changed = false;

        // Process bid updates - use Side::BUY for bids
        for (size_t i = 0; i < msg->header.bid_count; ++i) {
            const auto& level = msg->bids[i];
            bool level_changed = order_book_.update(core::Side::BUY, level.price, level.quantity);
            top_changed = top_changed || level_changed;
        }

        // Process ask updates - use Side::SELL for asks
        for (size_t i = 0; i < msg->header.ask_count; ++i) {
            const auto& level = msg->asks[i];
            bool level_changed = order_book_.update(core::Side::SELL, level.price, level.quantity);
            top_changed = top_changed || level_changed;
        }

        return top_changed;
    }
};

}  // namespace crypto_lob::orderbook