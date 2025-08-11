#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../core/exchange_enum.hpp"
#include "../core/memory_pool.hpp"
#include "../core/raw_message.hpp"
#include "../core/spsc_ring.hpp"
#include "../core/timestamp.hpp"

namespace crypto_lob::exchanges {

using namespace crypto_lob::core;

/**
 * @brief Abstract base class for exchange-specific WebSocket connectors
 *
 * Architecture alignment (Section 4):
 * "ExchangeConnector manages the WebSocket connection to a crypto exchange
 * and handles subscription to relevant market data streams."
 *
 * Thread model (Section 2.1):
 * - Runs on dedicated core (e.g., core 2 for Binance)
 * - Maintains persistent WebSocket connection
 * - Pushes raw messages into SPSC queue to Parser
 * - Owns RawMessage memory pool for allocation
 *
 * Performance target: <50μs wire-to-book latency
 * Connector contribution: ~5-10μs per message
 */
class ExchangeConnector {
  public:
    using RawMessageQueue = SPSCRing<RawMessage*>;  // By pointer to Parser
    using RawMessagePool = MemoryPool<RawMessage>;

    struct Config {
        ExchangeId exchange_id = ExchangeId::UNKNOWN;
        std::string websocket_url;
        std::vector<std::string> subscriptions;
        size_t raw_message_pool_size = 1024;  // Pre-allocated raw messages
        int reconnect_delay_ms = 1000;
        int max_reconnect_attempts = 10;
        int ping_interval_sec = 20;
    };

    ExchangeConnector(const Config& config, RawMessageQueue& output_queue, RawMessagePool& raw_message_pool)
        : config_(config),
          output_queue_(output_queue),
          raw_message_pool_(raw_message_pool),
          running_(false),
          connected_(false),
          messages_received_(0),
          connection_errors_(0),
          queue_full_drops_(0) {}

    virtual ~ExchangeConnector() = default;

    /**
     * @brief Start the connector (connect and subscribe)
     * @return true if started successfully
     */
    virtual bool start() = 0;

    /**
     * @brief Stop the connector (disconnect)
     */
    virtual void stop() = 0;

    /**
     * @brief Check if connector is running
     */
    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    /**
     * @brief Check if WebSocket is connected
     */
    [[nodiscard]] bool is_connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    // Statistics
    [[nodiscard]] uint64_t messages_received() const noexcept {
        return messages_received_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t connection_errors() const noexcept {
        return connection_errors_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t queue_full_drops() const noexcept {
        return queue_full_drops_.load(std::memory_order_relaxed);
    }

  protected:
    /**
     * @brief Handle received WebSocket message
     *
     * Called when a complete WebSocket frame is received.
     * Should allocate RawMessage from pool, fill it, and push to queue.
     *
     * @param data Raw message data
     * @param size Message size
     */
    void handle_message(const char* data, size_t size) {
        // Allocate from pool
        RawMessage* raw_msg = raw_message_pool_.allocate();
        if (!raw_msg) [[unlikely]] {
            // Pool exhausted - should not happen with proper sizing
            queue_full_drops_++;
            return;
        }

        // Fill the message
        if (size > RawMessage::MAX_FRAME_SIZE) {
            size = RawMessage::MAX_FRAME_SIZE;  // Truncate if too large
        }
        std::memcpy(raw_msg->data.data(), data, size);
        raw_msg->size = static_cast<uint32_t>(size);
        raw_msg->exchange_id = config_.exchange_id;
        raw_msg->sequence_number = ++sequence_number_;
        raw_msg->receive_timestamp = rdtsc();  // Get TSC timestamp

        // Try to push to output queue
        if (!output_queue_.try_push(raw_msg)) {
            // Queue full - drop newest as per architecture
            queue_full_drops_++;
            raw_message_pool_.deallocate(raw_msg);
        } else {
            messages_received_++;
        }
    }

    Config config_;
    RawMessageQueue& output_queue_;     // Output to Parser
    RawMessagePool& raw_message_pool_;  // Memory pool for allocations

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<uint64_t> messages_received_;
    std::atomic<uint64_t> connection_errors_;
    std::atomic<uint64_t> queue_full_drops_;
    std::atomic<uint64_t> sequence_number_{0};
};

}  // namespace crypto_lob::exchanges