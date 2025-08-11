#pragma once

#include <atomic>
#include <memory>
#include <string_view>

#include "../core/memory_pool.hpp"
#include "../core/normalized_message.hpp"
#include "../core/raw_message.hpp"
#include "../core/spsc_ring.hpp"

namespace crypto_lob::exchanges {

using namespace crypto_lob::core;

/**
 * @brief Abstract base class for exchange-specific message parsers
 *
 * Architecture alignment (Section 4):
 * "Parser (abstract class): Defines a generic interface for translating
 * raw JSON exchange messages from the ExchangeConnector into the
 * NormalizedMessage structure."
 *
 * Thread model (Section 2.1):
 * - Runs on dedicated core (e.g., core 3 for Binance)
 * - Reads from SPSC queue (raw messages from Connector)
 * - Writes to SPSC queue (normalized messages to OrderBook)
 * - Uses memory pool for NormalizedMessage allocation
 *
 * Performance target: <50μs wire-to-book latency
 * Parser contribution: ~5-10μs per message
 */
class Parser {
  public:
    using RawMessageQueue = SPSCRing<RawMessage*>;                // By pointer from Connector
    using NormalizedMessageQueue = SPSCRing<NormalizedMessage*>;  // By pointer to OrderBook
    using RawMessagePool = MemoryPool<RawMessage>;
    using NormalizedMessagePool = MemoryPool<NormalizedMessage>;

    struct Config {
        ExchangeId exchange_id = ExchangeId::UNKNOWN;
        size_t message_pool_size = 8192;  // Pre-allocated normalized messages
        size_t raw_pool_size = 1024;      // Pre-allocated raw messages
    };

    Parser(const Config& config,
           RawMessageQueue& raw_queue,
           NormalizedMessageQueue& normalized_queue,
           RawMessagePool& raw_message_pool,
           NormalizedMessagePool& message_pool)
        : config_(config),
          raw_queue_(raw_queue),
          normalized_queue_(normalized_queue),
          raw_message_pool_(raw_message_pool),
          message_pool_(message_pool),
          messages_parsed_(0),
          parse_errors_(0),
          queue_full_drops_(0) {}

    virtual ~Parser() = default;

    // Delete copy/move - parsers are stateful and thread-bound
    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;
    Parser(Parser&&) = delete;
    Parser& operator=(Parser&&) = delete;

    /**
     * @brief Process one message from the queue
     *
     * Hot path operation (Section 3):
     * 1. Dequeue raw message (by value)
     * 2. Allocate NormalizedMessage from pool
     * 3. Parse JSON using simdjson
     * 4. Enqueue to OrderBook (by pointer)
     *
     * @return true to continue processing, false to stop
     */
    bool process_one() {
        // Try to get raw message pointer from input queue
        RawMessage* raw_msg = nullptr;
        if (!raw_queue_.try_pop(raw_msg)) {
            return true;  // No message available, continue
        }

        if (!raw_msg || !raw_msg->is_valid()) {
            parse_errors_++;
            if (raw_msg) {
                raw_message_pool_.deallocate(raw_msg);  // Return to pool
            }
            return true;
        }

        // Allocate normalized message from pool
        NormalizedMessage* normalized = message_pool_.allocate();
        if (!normalized) [[unlikely]] {
            // Pool exhausted - should not happen with proper sizing
            parse_errors_++;
            return true;
        }

        // Parse the raw JSON message
        bool success = parse_message(raw_msg->get_json(), *normalized);

        if (success) {
            // Set common fields
            normalized->exchange_id = raw_msg->exchange_id;
            normalized->receive_time = raw_msg->receive_timestamp;

            // Try to enqueue to output
            if (!normalized_queue_.try_push(normalized)) {
                // Output queue full - drop newest as per architecture
                queue_full_drops_++;
                message_pool_.deallocate(normalized);
            } else {
                messages_parsed_++;
            }
        } else {
            // Parse error - return message to pool
            parse_errors_++;
            message_pool_.deallocate(normalized);
        }

        // Return raw message to pool
        raw_message_pool_.deallocate(raw_msg);

        return true;  // Continue processing
    }

    /**
     * @brief Run the parser loop
     *
     * Call this from the dedicated parser thread.
     * Runs until stop() is called.
     */
    void run() {
        running_ = true;
        while (running_) {
            if (!process_one()) {
                break;
            }
        }
    }

    /**
     * @brief Signal the parser to stop
     */
    void stop() {
        running_ = false;
    }

    // Statistics
    [[nodiscard]] uint64_t messages_parsed() const noexcept {
        return messages_parsed_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t parse_errors() const noexcept {
        return parse_errors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t queue_full_drops() const noexcept {
        return queue_full_drops_.load(std::memory_order_relaxed);
    }

  protected:
    /**
     * @brief Exchange-specific JSON parsing implementation
     *
     * Derived classes implement this to parse their specific format.
     * Must use simdjson for performance as specified in architecture.
     *
     * @param json Raw JSON string
     * @param msg Output normalized message (already allocated)
     * @return true if parse successful, false otherwise
     */
    virtual bool parse_message(std::string_view json, NormalizedMessage& msg) = 0;

    Config config_;
    RawMessageQueue& raw_queue_;                // Input from Connector
    NormalizedMessageQueue& normalized_queue_;  // Output to OrderBook
    RawMessagePool& raw_message_pool_;          // Connector's pool for returning
    NormalizedMessagePool& message_pool_;       // Pre-allocated messages

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> messages_parsed_;
    std::atomic<uint64_t> parse_errors_;
    std::atomic<uint64_t> queue_full_drops_;
};

}  // namespace crypto_lob::exchanges