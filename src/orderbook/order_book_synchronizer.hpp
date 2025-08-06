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
    /**
     * Synchronization state enumeration
     */
    enum class State : uint8_t {
        AWAITING_SNAPSHOT,  // Needs snapshot before processing deltas
        SYNCHRONIZED        // Ready to process incremental updates
    };

    enum class ProcessStatus : uint8_t {
        OK = 0,              // Message processed successfully
        NEEDS_SNAPSHOT = 1,  // Gap detected or not synchronized
        STALE = 2,           // Old message, discarded
    };

    struct ProcessResult {
        ProcessStatus status;
        bool top_of_book_changed;  // For downstream processing
    };

    /**
     * Constructs an OrderBookSynchronizer for managing order book state synchronization.
     *
     * @param order_book Reference to the OrderBook instance to synchronize.
     *                   Must remain valid for the lifetime of this synchronizer.
     *
     * Initialization:
     * - Sets initial state to AWAITING_SNAPSHOT (requires snapshot before processing deltas)
     * - Resets sequence tracking counters to zero
     * - Initializes statistics counters for monitoring synchronization health
     * - No memory allocation - all operations are stack-based for HFT performance
     */
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

    /**
     * Returns the current synchronization state.
     *
     * State meanings:
     * - AWAITING_SNAPSHOT: Synchronizer needs a snapshot before processing deltas.
     *   This occurs on initialization or after gap detection.
     * - SYNCHRONIZED: Ready to process incremental delta messages.
     *   Order book is in sync with exchange state.
     *
     * @return Current State enumeration value
     */
    [[nodiscard]] State get_state() const noexcept {
        return state_;
    }

    [[nodiscard]] bool needs_snapshot() const noexcept {
        return state_ == State::AWAITING_SNAPSHOT;
    }

    [[nodiscard]] bool is_synchronized() const noexcept {
        return state_ == State::SYNCHRONIZED;
    }

    /**
     * Provides const access to the underlying order book.
     *
     * Const access patterns:
     * - Safe for read operations during message processing
     * - Use this method for price queries, spread calculations, and analysis
     * - No risk of accidental modifications during synchronization
     * - Thread-safe for concurrent reads (order book implementation dependent)
     *
     * @return Const reference to the managed OrderBook instance
     */
    [[nodiscard]] const core::OrderBook& get_order_book() const noexcept {
        return order_book_;
    }

    /**
     * Provides non-const access to the underlying order book.
     *
     * ⚠️  NON-CONST ACCESS WARNINGS:
     * - Use with extreme caution during synchronization
     * - Direct modifications can break sequence continuity
     * - Can invalidate synchronization state if used improperly
     * - Prefer using process_message() for all order book updates
     * - Only use for manual resets or emergency interventions
     *
     * @return Non-const reference to the managed OrderBook instance
     */
    [[nodiscard]] core::OrderBook& order_book() noexcept {
        return order_book_;
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

    /**
     * Resets the synchronizer to its initial state.
     *
     * When to use reset:
     * - Connection recovery after network outages
     * - Manual intervention when synchronization is corrupted
     * - Switching to a different market data stream
     * - Testing and development scenarios
     * - Emergency recovery when gap detection fails
     *
     * Effects:
     * - Transitions state back to AWAITING_SNAPSHOT
     * - Clears all sequence tracking counters
     * - Does NOT clear statistics (use for debugging)
     * - Does NOT modify the underlying order book
     * - Next message processing will require a snapshot
     */
    void reset() noexcept {
        state_ = State::AWAITING_SNAPSHOT;
        last_processed_sequence_ = 0;
        expected_next_first_id_ = 0;
    }

  private:
    /**
     * State Machine Transitions:
     *
     * AWAITING_SNAPSHOT → SYNCHRONIZED:
     *   - Triggered by successful snapshot processing
     *   - Establishes baseline sequence numbers for gap detection
     *   - Order book is fully rebuilt from snapshot data
     *
     * SYNCHRONIZED → AWAITING_SNAPSHOT:
     *   - Triggered by sequence gap detection in delta messages
     *   - Triggered by manual reset() call
     *   - Order book remains in last known state until new snapshot
     *
     * State Invariants:
     *   - AWAITING_SNAPSHOT: All delta messages return NEEDS_SNAPSHOT
     *   - SYNCHRONIZED: Delta messages undergo gap detection and processing
     *   - No intermediate states - transitions are atomic and immediate
     */

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

        /**
         * Exchange-Specific Sequence Handling for Snapshots:
         *
         * Each exchange has different sequence numbering schemes requiring
         * specialized handling for gap detection and synchronization.
         */
        // Update sequence tracking based on exchange
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                /**
                 * Binance Sequence Logic:
                 * - Snapshot provides 'lastUpdateId' in exchange_sequence field
                 * - Delta messages have 'first_update_id' and 'last_update_id'
                 * - Next delta should have first_update_id == lastUpdateId + 1
                 * - Special case: overlapping ranges are acceptable for continuity
                 */
                last_processed_sequence_ = msg->header.exchange_sequence;
                expected_next_first_id_ = msg->header.exchange_sequence + 1;
                break;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                /**
                 * KuCoin Sequence Logic:
                 * - Snapshot provides 'sequence_end' as the last processed sequence
                 * - Delta messages have 'sequence_start' and 'sequence_end'
                 * - Next delta should have sequence_start == sequence_end + 1
                 * - Strict sequential numbering with no overlaps
                 */
                last_processed_sequence_ = msg->sequence.sequence_end;
                expected_next_first_id_ = msg->sequence.sequence_end + 1;
                break;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                /**
                 * OKX Sequence Logic:
                 * - Uses 'sequence_number' for current state
                 * - Delta messages include 'previous_update_id' for continuity check
                 * - No expected_next_first_id needed - uses previous_update_id matching
                 */
                last_processed_sequence_ = msg->sequence.sequence_number;
                break;

            default:
                /**
                 * Generic Exchange Logic:
                 * - Simple increment-based sequence numbering
                 * - Uses header.exchange_sequence as the primary sequence identifier
                 */
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

        /**
         * GAP DETECTION AND RECOVERY:
         *
         * Sequence gap detection is critical for maintaining order book integrity.
         * When gaps are detected, immediate recovery is initiated to prevent
         * corrupted market data from propagating downstream.
         *
         * Recovery Strategy:
         * - Immediate transition to AWAITING_SNAPSHOT state
         * - Reject all delta messages until new snapshot received
         * - No message buffering - request fresh snapshot instead
         * - Reset sequence tracking to prevent false positives
         *
         * Gap Detection Triggers:
         * - Missing sequence numbers (most common case)
         * - Duplicate sequence numbers (replay protection)
         * - Out-of-order sequence numbers (network reordering)
         * - Sequence numbers jumping beyond expected range
         *
         * Performance Benefits:
         * - Zero memory overhead (no buffering)
         * - Predictable latency (no complex gap filling logic)
         * - Fast recovery (snapshot fetch is typically 50-500ms)
         * - Simple state machine (only 2 states)
         */
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

    /**
     * Exchange-Specific Stale Message Detection:
     *
     * Determines if a delta message is older than our last processed state.
     * Stale messages are typically caused by:
     * - Network redelivery of old messages
     * - Slow consumer catching up with buffered messages
     * - Race conditions in multi-threaded feed handlers
     * - Recovery scenarios where old deltas arrive after snapshot
     */
    [[nodiscard]] bool is_stale_message(const exchanges::base::MarketDataMessage* msg) const noexcept {
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                /**
                 * Binance Staleness Check:
                 * - Compare against last_update_id from the delta message
                 * - Message is stale if its entire range is before our last processed
                 * - Use < (not <=) because equal last_update_id might contain new data
                 */
                return msg->sequence.last_update_id < last_processed_sequence_;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                /**
                 * KuCoin Staleness Check:
                 * - Compare sequence_end to determine if message is fully processed
                 * - Strict < comparison since KuCoin sequences don't overlap
                 */
                return msg->sequence.sequence_end < last_processed_sequence_;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                /**
                 * OKX Staleness Check:
                 * - Use <= comparison since OKX sequence_number must be strictly increasing
                 * - Equal sequence_number means duplicate message (always stale)
                 */
                return msg->sequence.sequence_number <= last_processed_sequence_;

            default:
                /**
                 * Generic Staleness Check:
                 * - Use <= comparison for simple increment-based sequences
                 * - Equal exchange_sequence indicates duplicate message
                 */
                return msg->header.exchange_sequence <= last_processed_sequence_;
        }
    }

    /**
     * Exchange-Specific Sequence Continuity Validation:
     *
     * Critical function for gap detection. Each exchange implements different
     * sequence numbering schemes requiring specialized continuity logic.
     *
     * Gap Detection Scenarios:
     * - Missing messages: sequence jump > 1
     * - Network reordering: out-of-order arrival
     * - Duplicate messages: same sequence number
     * - Overlapping ranges: partial message overlap
     */
    [[nodiscard]] bool is_sequence_continuous(const exchanges::base::MarketDataMessage* msg) const noexcept {
        switch (msg->header.exchange_id) {
            case core::ExchangeId::BINANCE_SPOT:
            case core::ExchangeId::BINANCE_FUT:
                /**
                 * Binance Continuity Logic:
                 * - Standard case: first_update_id == expected_next_first_id (last + 1)
                 * - Special overlap case: message spans across our expected sequence
                 * - Overlap condition: first_update_id <= expected <= last_update_id
                 * - This allows for network redelivery with partial overlap
                 *
                 * Example:
                 * - Last processed: sequence 100
                 * - Expected next: 101
                 * - Message arrives: first=99, last=102 (VALID - contains 101)
                 * - Message arrives: first=102, last=105 (INVALID - gap at 101)
                 */
                if (msg->sequence.first_update_id <= expected_next_first_id_ &&
                    expected_next_first_id_ <= msg->sequence.last_update_id) {
                    return true;
                }
                return msg->sequence.first_update_id == expected_next_first_id_;

            case core::ExchangeId::KUCOIN_SPOT:
            case core::ExchangeId::KUCOIN_FUT:
                /**
                 * KuCoin Continuity Logic:
                 * - Strict sequential numbering: sequence_start == last_processed + 1
                 * - No overlapping ranges - each message covers distinct sequence range
                 * - Gap detection is straightforward: any non-consecutive start indicates gap
                 *
                 * Example:
                 * - Last processed: sequence_end = 100
                 * - Next valid message: sequence_start = 101
                 * - Gap if message arrives with: sequence_start = 103 (missing 101-102)
                 */
                return msg->sequence.sequence_start == last_processed_sequence_ + 1;

            case core::ExchangeId::OKX_SPOT:
            case core::ExchangeId::OKX_FUT:
                /**
                 * OKX Continuity Logic:
                 * - Uses previous_update_id field for explicit linking
                 * - Message is continuous if previous_update_id == our last_processed
                 * - This allows OKX to handle non-sequential sequence_number values
                 * - More robust than simple increment checking
                 *
                 * Example:
                 * - Last processed: sequence_number = 1000
                 * - Next message: previous_update_id = 1000, sequence_number = 1005 (VALID)
                 * - Gap message: previous_update_id = 1002, sequence_number = 1005 (INVALID)
                 */
                return msg->sequence.previous_update_id == last_processed_sequence_;

            default:
                /**
                 * Generic Continuity Logic:
                 * - Simple increment-based sequence checking
                 * - Message is continuous if exchange_sequence == last_processed + 1
                 * - Used for exchanges with straightforward sequence numbering
                 */
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