#pragma once

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "../core/cache.hpp"
#include "../core/instrument.hpp"
#include "../core/memory_pool.hpp"
#include "../core/normalized_message.hpp"
#include "../core/spsc_ring.hpp"
#include "../core/timestamp.hpp"
#include "order_book.hpp"

namespace crypto_lob::orderbook {

using namespace crypto_lob::core;

/**
 * @brief Manages multiple OrderBook instances and routes messages to them
 *
 * Architecture alignment (Section 4):
 * "OrderBookManager: Coordinates all the OrderBook instances and the incoming data stream.
 * It runs in the OrderBook thread. The OrderBookManager holds a mapping from symbol
 * (instrument_id) -> OrderBook object."
 *
 * Thread model (Section 2.1):
 * - Runs on dedicated OrderBook thread core (e.g., core 1)
 * - Reads from SPSC queue (normalized messages from Parser)
 * - Single-threaded design (no locks needed)
 * - Applies deltas/snapshots to in-memory books with sequence checks
 *
 * Performance target: <50μs wire-to-book latency
 * OrderBookManager contribution: ~5-10μs per message
 */
class alignas(CACHELINE_SIZE) OrderBookManager {
  public:
    using NormalizedMessageQueue = SPSCRing<NormalizedMessage*>;
    using NormalizedMessagePool = MemoryPool<NormalizedMessage>;
    using OrderBookPool = MemoryPool<PriceLevelNode>;
    using OrderBookPtr = std::unique_ptr<OrderBook>;

    struct Config {
        size_t initial_book_capacity = 4096;   // Initial capacity per order book
        size_t top_n_depth = 20;               // Depth for top-N tracking
        size_t max_instruments = 256;          // Maximum number of instruments
        bool enable_sequence_checking = true;  // Enable sequence number validation
    };

    /**
     * @brief Tracks sequence state for an instrument
     */
    struct SequenceState {
        uint64_t last_update_id = 0;    // Last processed update ID
        uint64_t gaps_detected = 0;     // Number of sequence gaps
        bool awaiting_snapshot = true;  // Waiting for initial snapshot
    };

    /**
     * @brief Statistics for monitoring
     */
    struct Statistics {
        std::atomic<uint64_t> messages_processed{0};
        std::atomic<uint64_t> snapshots_applied{0};
        std::atomic<uint64_t> deltas_applied{0};
        std::atomic<uint64_t> trades_processed{0};
        std::atomic<uint64_t> sequence_gaps{0};
        std::atomic<uint64_t> unknown_instruments{0};
        std::atomic<uint64_t> bbo_changes{0};
        std::atomic<uint64_t> parse_errors{0};
    };

    OrderBookManager(const Config& config,
                     NormalizedMessageQueue& message_queue,
                     NormalizedMessagePool& message_pool,
                     OrderBookPool& book_pool)
        : config_(config), message_queue_(message_queue), message_pool_(message_pool), book_pool_(book_pool) {
        // Pre-allocate space for expected instruments
        order_books_.reserve(config.max_instruments);
        sequence_states_.reserve(config.max_instruments);
    }

    ~OrderBookManager() = default;

    // Delete copy/move - manager is stateful and thread-bound
    OrderBookManager(const OrderBookManager&) = delete;
    OrderBookManager& operator=(const OrderBookManager&) = delete;
    OrderBookManager(OrderBookManager&&) = delete;
    OrderBookManager& operator=(OrderBookManager&&) = delete;

    /**
     * @brief Add an instrument to manage
     *
     * Pre-allocates OrderBook for the instrument.
     * Should be called during initialization before run().
     */
    void add_instrument(const InstrumentId& instrument) {
        if (order_books_.find(instrument) == order_books_.end()) {
            auto book = std::make_unique<OrderBook>(instrument,
                                                    book_pool_,
                                                    OrderBook::Config{.initial_capacity = config_.initial_book_capacity,
                                                                      .top_n_depth = config_.top_n_depth});
            order_books_[instrument] = std::move(book);
            sequence_states_[instrument] = SequenceState{};
        }
    }

    /**
     * @brief Process one message from the queue
     *
     * Hot path operation (Section 3):
     * 1. Dequeue normalized message
     * 2. Route to correct OrderBook by instrument
     * 3. Check sequence for deltas (drop if gap detected)
     * 4. Apply update to book
     * 5. Return message to pool
     *
     * @return true to continue processing, false to stop
     */
    bool process_one() {
        // Try to get message from input queue
        NormalizedMessage* msg = nullptr;
        if (!message_queue_.try_pop(msg)) {
            return true;  // No message available, continue
        }

        if (!msg) [[unlikely]] {
            stats_.parse_errors++;
            return true;
        }

        // Route to correct order book
        auto book_it = order_books_.find(msg->instrument);
        if (book_it == order_books_.end()) [[unlikely]] {
            // Unknown instrument - could auto-create or drop
            stats_.unknown_instruments++;
            message_pool_.deallocate(msg);
            return true;
        }

        OrderBook& book = *book_it->second;
        SequenceState& seq_state = sequence_states_[msg->instrument];
        bool bbo_changed = false;

        // Process based on message type
        switch (msg->type) {
            case MessageType::SNAPSHOT: {
                const auto& snapshot = std::get<SnapshotMessage>(msg->data);

                // Convert to vectors for OrderBook API
                std::vector<PriceLevel> bids, asks;
                bids.reserve(snapshot.bid_count);
                asks.reserve(snapshot.ask_count);

                for (size_t i = 0; i < snapshot.bid_count; ++i) {
                    bids.push_back(PriceLevel{snapshot.bids[i].price,
                                              static_cast<uint64_t>(snapshot.bids[i].quantity.raw_value())});
                }
                for (size_t i = 0; i < snapshot.ask_count; ++i) {
                    asks.push_back(PriceLevel{snapshot.asks[i].price,
                                              static_cast<uint64_t>(snapshot.asks[i].quantity.raw_value())});
                }

                bbo_changed = book.snapshot(bids, asks);

                // Update sequence state
                seq_state.last_update_id = snapshot.last_update_id;
                seq_state.awaiting_snapshot = false;

                stats_.snapshots_applied++;
                break;
            }

            case MessageType::DELTA: {
                const auto& delta = std::get<DeltaMessage>(msg->data);

                // Check sequence if enabled
                if (config_.enable_sequence_checking) {
                    if (seq_state.awaiting_snapshot) {
                        // Need snapshot first
                        message_pool_.deallocate(msg);
                        return true;
                    }

                    // Detect gaps (first_update_id should be last_update_id + 1)
                    if (delta.first_update_id > seq_state.last_update_id + 1) {
                        seq_state.gaps_detected++;
                        seq_state.awaiting_snapshot = true;  // Request new snapshot
                        stats_.sequence_gaps++;
                        message_pool_.deallocate(msg);
                        return true;
                    }

                    // Skip old updates
                    if (delta.last_update_id <= seq_state.last_update_id) {
                        message_pool_.deallocate(msg);
                        return true;
                    }
                }

                // Apply bid updates
                for (size_t i = 0; i < delta.bid_update_count; ++i) {
                    const auto& update = delta.bid_updates[i];
                    bool changed = book.update(core::Side::BUY, update.price, update.quantity.raw_value());
                    bbo_changed = bbo_changed || changed;
                }

                // Apply ask updates
                for (size_t i = 0; i < delta.ask_update_count; ++i) {
                    const auto& update = delta.ask_updates[i];
                    bool changed = book.update(core::Side::SELL, update.price, update.quantity.raw_value());
                    bbo_changed = bbo_changed || changed;
                }

                // Update sequence state
                seq_state.last_update_id = delta.last_update_id;

                stats_.deltas_applied++;
                break;
            }

            case MessageType::TRADE: {
                // Trades don't modify order book, just track for statistics
                stats_.trades_processed++;
                break;
            }

            default:
                stats_.parse_errors++;
                break;
        }

        // Track BBO changes
        if (bbo_changed) {
            stats_.bbo_changes++;
        }

        stats_.messages_processed++;

        // Return message to pool
        message_pool_.deallocate(msg);

        return true;  // Continue processing
    }

    /**
     * @brief Run the order book manager loop
     *
     * Call this from the dedicated OrderBook thread.
     * Runs until stop() is called.
     */
    void run() {
        running_ = true;
        while (running_.load(std::memory_order_relaxed)) {
            if (!process_one()) {
                break;
            }
        }
    }

    /**
     * @brief Signal the manager to stop
     */
    void stop() {
        running_.store(false, std::memory_order_relaxed);
    }

    /**
     * @brief Get order book for an instrument
     *
     * For monitoring/debugging purposes only.
     * Not on hot path.
     */
    [[nodiscard]] const OrderBook* get_order_book(const InstrumentId& instrument) const {
        auto it = order_books_.find(instrument);
        return (it != order_books_.end()) ? it->second.get() : nullptr;
    }

    /**
     * @brief Get list of managed instruments
     */
    [[nodiscard]] std::vector<InstrumentId> get_instruments() const {
        std::vector<InstrumentId> instruments;
        instruments.reserve(order_books_.size());
        for (const auto& [instrument, _] : order_books_) {
            instruments.push_back(instrument);
        }
        return instruments;
    }

    /**
     * @brief Get statistics for monitoring
     */
    [[nodiscard]] const Statistics& stats() const noexcept {
        return stats_;
    }

    /**
     * @brief Get sequence state for an instrument
     */
    [[nodiscard]] const SequenceState* get_sequence_state(const InstrumentId& instrument) const {
        auto it = sequence_states_.find(instrument);
        return (it != sequence_states_.end()) ? &it->second : nullptr;
    }

  private:
    Config config_;
    NormalizedMessageQueue& message_queue_;  // Input from Parser
    NormalizedMessagePool& message_pool_;    // For returning messages
    OrderBookPool& book_pool_;               // Shared pool for price levels

    // Instrument -> OrderBook mapping
    std::unordered_map<InstrumentId, OrderBookPtr, InstrumentIdHash> order_books_;

    // Instrument -> SequenceState mapping
    std::unordered_map<InstrumentId, SequenceState, InstrumentIdHash> sequence_states_;

    std::atomic<bool> running_{false};
    Statistics stats_;
};

}  // namespace crypto_lob::orderbook