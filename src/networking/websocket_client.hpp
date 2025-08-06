#pragma once

#include <atomic>
#include <chrono>
#include <cmath>  // For proper floating point calculations
#include <coroutine>
#include <memory>
#include <string>
#include <string_view>
#include <thread>  // For std::this_thread::sleep_for
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "../core/spsc_ring.hpp"
#include "raw_message_pool.hpp"

namespace crypto_lob::networking {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// Forward declaration for CRTP
template <typename Derived>
class WebSocketClient;

// WebSocket connection state
enum class ConnectionState : uint8_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3,
    RECONNECTING = 4
};

// Connection statistics for monitoring - properly cache-aligned to 128 bytes
struct alignas(128) ConnectionStats {
    // First cache line (64 bytes)
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<uint64_t> last_message_timestamp{0};
    std::atomic<uint64_t> last_ping_timestamp{0};
    std::atomic<uint64_t> last_pong_timestamp{0};

    // Second cache line (64 bytes)
    std::atomic<uint64_t> fragmented_messages{0};
    std::atomic<uint64_t> oversized_messages{0};
    std::atomic<uint64_t> connection_errors{0};
    std::atomic<uint64_t> parse_errors{0};
    // Padding to ensure full 128 byte alignment
    char padding[32];
};

// Configuration for WebSocket client
struct WebSocketConfig {
    std::string host;
    std::string port;
    std::string target;  // WebSocket path
    bool use_ssl = true;

    // Timeouts in milliseconds
    uint32_t connect_timeout_ms = 5000;
    uint32_t handshake_timeout_ms = 5000;
    uint32_t read_timeout_ms = 30000;
    uint32_t ping_interval_ms = 20000;
    uint32_t pong_timeout_ms = 60000;

    // Reconnection settings
    uint32_t initial_reconnect_delay_ms = 1000;
    uint32_t max_reconnect_delay_ms = 60000;
    double reconnect_backoff_factor = 2.0;  // Use double for safe calculation
    uint32_t max_reconnect_attempts = 0;    // 0 = infinite

    // Message handling
    bool enable_compression = false;          // WebSocket compression
    uint32_t max_message_size = 1024 * 1024;  // 1MB max for fragmented messages
};

/**
 * @brief HFT-optimized WebSocket client using Boost.Beast with C++20 coroutines
 *
 * This class template provides the core WebSocket functionality with zero-copy
 * message handling via RawMessagePool. Exchange-specific implementations should
 * derive from this class using CRTP for static polymorphism.
 *
 * @tparam Derived The derived exchange-specific implementation
 */
template <typename Derived>
class alignas(64) WebSocketClient : public std::enable_shared_from_this<WebSocketClient<Derived>> {
  public:
    using MessageQueue = core::SPSCRing<RawMessage*>;

    WebSocketClient(asio::io_context& io_context,
                    ssl::context& ssl_context,
                    const WebSocketConfig& config,
                    MessageQueue& outbound_queue)
        : io_context_(io_context),
          ssl_context_(ssl_context),
          config_(config),
          outbound_queue_(outbound_queue),
          resolver_(io_context),
          state_(ConnectionState::DISCONNECTED),
          active_tasks_(0),
          reconnect_timer_(io_context),
          ping_timer_(io_context),
          pong_timer_(io_context),
          current_reconnect_delay_ms_(config.initial_reconnect_delay_ms) {
        // Reserve space for message assembly buffer
        if (config_.max_message_size > RawMessage::MAX_FRAME_SIZE) {
            assembly_buffer_.reserve(config_.max_message_size);
        }
    }

    virtual ~WebSocketClient() {
        // Synchronously stop all operations
        stop_and_wait();
    }

    // Start the WebSocket client (initiates connection)
    void start() {
        auto self = this->shared_from_this();
        active_tasks_.fetch_add(1, std::memory_order_acq_rel);

        asio::co_spawn(
            io_context_,
            [self, this]() -> asio::awaitable<void> {
                co_await connect_with_retry();
                active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            },
            asio::detached);
    }

    // Stop the WebSocket client (graceful shutdown)
    void stop() {
        auto self = this->shared_from_this();
        active_tasks_.fetch_add(1, std::memory_order_acq_rel);

        asio::co_spawn(
            io_context_,
            [self, this]() -> asio::awaitable<void> {
                co_await disconnect();
                active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
            },
            asio::detached);
    }

    // Synchronous stop and wait for all tasks
    void stop_and_wait() {
        // Signal shutdown
        state_.store(ConnectionState::DISCONNECTING, std::memory_order_release);

        // Cancel all operations
        stop_active_operations();

        // Wait for all active tasks to complete
        // Use exponential backoff for checking
        int wait_ms = 1;
        while (active_tasks_.load(std::memory_order_acquire) > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
            wait_ms = std::min(wait_ms * 2, 100);  // Max 100ms between checks
        }

        // Now safe to destroy streams
        ws_.reset();
        ws_plain_.reset();
    }

    // Send a message with error handling
    asio::awaitable<bool> send_message(std::string_view message) {
        if (state_.load(std::memory_order_acquire) != ConnectionState::CONNECTED) {
            co_return false;
        }

        boost::system::error_code ec;

        // Check for null before use
        if (config_.use_ssl && ws_) {
            co_await ws_->async_write(asio::buffer(message.data(), message.size()),
                                      asio::redirect_error(asio::use_awaitable, ec));
        } else if (!config_.use_ssl && ws_plain_) {
            co_await ws_plain_->async_write(asio::buffer(message.data(), message.size()),
                                            asio::redirect_error(asio::use_awaitable, ec));
        } else {
            co_return false;
        }

        if (!ec) {
            stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes_sent.fetch_add(static_cast<uint64_t>(message.size()), std::memory_order_relaxed);
            co_return true;
        }

        co_return false;
    }

    // Get connection state
    [[nodiscard]] ConnectionState get_state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    // Get connection statistics
    [[nodiscard]] const ConnectionStats& get_stats() const noexcept {
        return stats_;
    }

  protected:
    // CRTP interface - derived class must implement these
    Derived& derived() {
        return static_cast<Derived&>(*this);
    }
    const Derived& derived() const {
        return static_cast<const Derived&>(*this);
    }

    // Called when connection is established
    asio::awaitable<void> on_connected() {
        co_return co_await derived().on_connected_impl();
    }

    // Called to handle incoming messages
    void on_message(RawMessage* msg) {
        derived().on_message_impl(msg);
    }

    // Called to handle ping messages (exchange-specific)
    asio::awaitable<void> on_ping(std::string_view data) {
        co_return co_await derived().on_ping_impl(data);
    }

    // Called when connection is lost
    void on_disconnected() {
        derived().on_disconnected_impl();
    }

  private:
    // Control frame callback handler for pong messages
    void handle_control_frame(websocket::frame_type kind, beast::string_view payload) {
        if (kind == websocket::frame_type::pong) {
            stats_.last_pong_timestamp.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                             std::memory_order_relaxed);

            // Cancel pong timer since we received the pong
            pong_timer_.cancel();
        } else if (kind == websocket::frame_type::ping) {
            // Beast handles ping responses automatically
        }
    }

    // Stop all active operations before reconnection
    void stop_active_operations() {
        // Cancel all timers
        ping_timer_.cancel();
        pong_timer_.cancel();
        reconnect_timer_.cancel();

        // Close WebSocket streams properly before canceling
        boost::system::error_code ec;
        if (ws_) {
            // Try to close gracefully first
            ws_->async_close(websocket::close_code::going_away, [](boost::system::error_code) {});
            // Then cancel underlying socket
            beast::get_lowest_layer(*ws_).cancel(ec);
        }
        if (ws_plain_) {
            ws_plain_->async_close(websocket::close_code::going_away, [](boost::system::error_code) {});
            ws_plain_->next_layer().cancel(ec);
        }
    }

    // Connect with exponential backoff retry
    asio::awaitable<void> connect_with_retry() {
        uint32_t attempts = 0;
        auto self = this->shared_from_this();

        while (config_.max_reconnect_attempts == 0 || attempts < config_.max_reconnect_attempts) {
            // Check if we should stop
            if (state_.load(std::memory_order_acquire) == ConnectionState::DISCONNECTING) {
                co_return;
            }

            // Set state to reconnecting
            state_.store(ConnectionState::RECONNECTING, std::memory_order_release);

            // Stop any existing operations
            stop_active_operations();

            // Wait for active tasks to complete
            // Fixed logic: wait until active_tasks reaches 1 (just this coroutine)
            for (int wait_count = 0; wait_count < 100; ++wait_count) {
                if (active_tasks_.load(std::memory_order_acquire) <= 1) {
                    break;
                }
                co_await asio::steady_timer(io_context_, std::chrono::milliseconds(10)).async_wait(asio::use_awaitable);
            }

            // Now safe to clear websocket connections
            ws_.reset();
            ws_plain_.reset();

            if (co_await connect_once()) {
                // Reset reconnect delay on successful connection
                current_reconnect_delay_ms_ = config_.initial_reconnect_delay_ms;

                // Start read loop
                active_tasks_.fetch_add(1, std::memory_order_acq_rel);
                asio::co_spawn(
                    io_context_,
                    [self, this]() -> asio::awaitable<void> {
                        co_await read_loop();
                        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                    },
                    asio::detached);

                // Start ping loop
                active_tasks_.fetch_add(1, std::memory_order_acq_rel);
                asio::co_spawn(
                    io_context_,
                    [self, this]() -> asio::awaitable<void> {
                        co_await ping_loop();
                        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                    },
                    asio::detached);

                co_return;
            }

            // Connection failed, wait before retry
            attempts++;
            stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);

            reconnect_timer_.expires_after(std::chrono::milliseconds(current_reconnect_delay_ms_));

            boost::system::error_code ec;
            co_await reconnect_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {  // Timer cancelled
                co_return;
            }

            // Exponential backoff with safe calculation
            double next_delay = static_cast<double>(current_reconnect_delay_ms_) * config_.reconnect_backoff_factor;
            current_reconnect_delay_ms_ =
                static_cast<uint32_t>(std::min(next_delay, static_cast<double>(config_.max_reconnect_delay_ms)));
        }
    }

    // Single connection attempt with proper exception handling
    asio::awaitable<bool> connect_once() {
        state_.store(ConnectionState::CONNECTING, std::memory_order_release);

        boost::system::error_code ec;

        // Resolve the host
        auto const results =
            co_await resolver_.async_resolve(config_.host, config_.port, asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
            co_return false;
        }

        if (config_.use_ssl) {
            // Create SSL WebSocket stream
            auto stream =
                std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(io_context_, ssl_context_);

            // Connect TCP socket with timeout
            auto& socket = beast::get_lowest_layer(*stream);
            socket.expires_after(std::chrono::milliseconds(config_.connect_timeout_ms));

            co_await socket.async_connect(results, asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // Perform SSL handshake
            socket.expires_after(std::chrono::milliseconds(config_.handshake_timeout_ms));
            co_await stream->next_layer().async_handshake(ssl::stream_base::client,
                                                          asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // Set WebSocket options
            stream->set_option(websocket::stream_base::timeout{std::chrono::milliseconds(config_.read_timeout_ms),
                                                               std::chrono::milliseconds(config_.ping_interval_ms),
                                                               true});

            stream->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) { req.set(beast::http::field::user_agent, "crypto-lob-hft/1.0"); }));

            // Set control callback for pong handling
            stream->control_callback([this](websocket::frame_type kind, beast::string_view payload) {
                handle_control_frame(kind, payload);
            });

            // Enable compression if configured
            if (config_.enable_compression) {
                websocket::permessage_deflate pmd;
                pmd.client_enable = true;
                stream->set_option(pmd);
            }

            // Perform WebSocket handshake
            socket.expires_after(std::chrono::milliseconds(config_.handshake_timeout_ms));
            co_await stream->async_handshake(
                config_.host, config_.target, asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            ws_ = std::move(stream);
        } else {
            // Non-SSL WebSocket (for testing)
            auto stream = std::make_unique<websocket::stream<tcp::socket>>(io_context_);

            // Connect TCP socket
            stream->next_layer().expires_after(std::chrono::milliseconds(config_.connect_timeout_ms));

            co_await stream->next_layer().async_connect(results, asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // Set WebSocket options
            stream->set_option(websocket::stream_base::timeout{std::chrono::milliseconds(config_.read_timeout_ms),
                                                               std::chrono::milliseconds(config_.ping_interval_ms),
                                                               true});

            // Set control callback for pong handling
            stream->control_callback([this](websocket::frame_type kind, beast::string_view payload) {
                handle_control_frame(kind, payload);
            });

            // Perform WebSocket handshake
            stream->next_layer().expires_after(std::chrono::milliseconds(config_.handshake_timeout_ms));

            co_await stream->async_handshake(
                config_.host, config_.target, asio::redirect_error(asio::use_awaitable, ec));

            if (ec) {
                stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            ws_plain_ = std::move(stream);
        }

        state_.store(ConnectionState::CONNECTED, std::memory_order_release);

        // Notify derived class
        co_await on_connected();

        co_return true;
    }

    // Main read loop with proper fragmentation handling
    asio::awaitable<void> read_loop() {
        // Get thread-local pool
        auto& pool = get_thread_pool();
        auto self = this->shared_from_this();

        // Check state at start of each iteration
        while (state_.load(std::memory_order_acquire) == ConnectionState::CONNECTED) {
            // Acquire buffer from thread-local pool
            RawMessage* msg = pool.acquire();
            if (!msg) [[unlikely]] {
                // Pool exhausted
                co_await asio::steady_timer(io_context_, std::chrono::microseconds(100))
                    .async_wait(asio::use_awaitable);
                continue;
            }

            boost::system::error_code ec;
            size_t bytes_read = 0;
            bool is_complete = false;

            // Read message (may be fragmented) - check for null streams
            if (config_.use_ssl && ws_) {
                // Use async_read_some to respect buffer size limit
                bytes_read = co_await ws_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                           asio::redirect_error(asio::use_awaitable, ec));

                if (!ec && ws_) {  // Check ws_ still valid after await
                    is_complete = ws_->is_message_done();
                }
            } else if (!config_.use_ssl && ws_plain_) {
                bytes_read = co_await ws_plain_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                                 asio::redirect_error(asio::use_awaitable, ec));

                if (!ec && ws_plain_) {  // Check ws_plain_ still valid after await
                    is_complete = ws_plain_->is_message_done();
                }
            } else {
                // Stream was destroyed while waiting
                pool.release(msg);
                co_return;
            }

            if (ec) {
                pool.release(msg);

                // Check if we're disconnecting (expected)
                if (state_.load(std::memory_order_acquire) != ConnectionState::CONNECTED) {
                    co_return;
                }

                // Unexpected error - trigger reconnection
                state_.store(ConnectionState::DISCONNECTED, std::memory_order_release);
                on_disconnected();

                // Start reconnection
                active_tasks_.fetch_add(1, std::memory_order_acq_rel);
                asio::co_spawn(
                    io_context_,
                    [self, this]() -> asio::awaitable<void> {
                        co_await connect_with_retry();
                        active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                    },
                    asio::detached);
                co_return;
            }

            // Validate bytes_read to prevent buffer overflow
            if (bytes_read > RawMessage::MAX_FRAME_SIZE) [[unlikely]] {
                pool.release(msg);
                state_.store(ConnectionState::DISCONNECTED, std::memory_order_release);
                co_return;
            }

            // Handle fragmented messages
            if (!is_complete) {
                // Message is fragmented - accumulate in assembly buffer
                stats_.fragmented_messages.fetch_add(1, std::memory_order_relaxed);

                // Check if accumulated size would exceed limit
                if (assembly_buffer_.size() + bytes_read > config_.max_message_size) {
                    // Message too large - drop it
                    stats_.oversized_messages.fetch_add(1, std::memory_order_relaxed);
                    assembly_buffer_.clear();

                    // Continue reading to consume rest of oversized message
                    // Keep ownership of buffer until drain is complete
                    while (!is_complete && state_.load(std::memory_order_acquire) == ConnectionState::CONNECTED) {
                        if (config_.use_ssl && ws_) {
                            size_t drain_bytes =
                                co_await ws_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                              asio::redirect_error(asio::use_awaitable, ec));
                            if (!ec && ws_) {
                                is_complete = ws_->is_message_done();
                            }
                            if (drain_bytes > RawMessage::MAX_FRAME_SIZE) [[unlikely]] {
                                break;
                            }
                        } else if (!config_.use_ssl && ws_plain_) {
                            size_t drain_bytes =
                                co_await ws_plain_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                                    asio::redirect_error(asio::use_awaitable, ec));
                            if (!ec && ws_plain_) {
                                is_complete = ws_plain_->is_message_done();
                            }
                            if (drain_bytes > RawMessage::MAX_FRAME_SIZE) [[unlikely]] {
                                break;
                            }
                        } else {
                            break;  // Stream destroyed
                        }
                        if (ec)
                            break;
                    }
                    // Now safe to release buffer after drain is complete
                    pool.release(msg);
                    continue;
                }

                // Accumulate fragment
                assembly_buffer_.append(msg->data, bytes_read);
                pool.release(msg);

                // Continue reading fragments
                continue;
            }

            // Message is complete
            if (!assembly_buffer_.empty()) {
                // Complete assembled message
                assembly_buffer_.append(msg->data, bytes_read);

                // Check if assembled message fits in a single buffer
                if (assembly_buffer_.size() <= RawMessage::MAX_FRAME_SIZE) {
                    // Copy assembled message back to buffer
                    std::memcpy(msg->data, assembly_buffer_.data(), assembly_buffer_.size());
                    msg->size = static_cast<uint32_t>(assembly_buffer_.size());
                } else {
                    // Message too large for single buffer - drop it
                    stats_.oversized_messages.fetch_add(1, std::memory_order_relaxed);
                    pool.release(msg);
                    assembly_buffer_.clear();
                    continue;
                }

                assembly_buffer_.clear();
            } else {
                // Single-fragment message
                msg->size = static_cast<uint32_t>(bytes_read);
            }

            // Set metadata
            msg->timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
            stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes_received.fetch_add(static_cast<uint64_t>(msg->size), std::memory_order_relaxed);
            stats_.last_message_timestamp.store(msg->timestamp_ns, std::memory_order_relaxed);

            // Queue for parser thread
            if (!outbound_queue_.try_push(msg)) [[unlikely]] {
                // Queue full - release buffer
                pool.release(msg);
            }
        }
    }

    // Ping loop for keepalive with null checks
    asio::awaitable<void> ping_loop() {
        auto self = this->shared_from_this();

        while (state_.load(std::memory_order_acquire) == ConnectionState::CONNECTED) {
            ping_timer_.expires_after(std::chrono::milliseconds(config_.ping_interval_ms));

            boost::system::error_code ec;
            co_await ping_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (ec || state_.load(std::memory_order_acquire) != ConnectionState::CONNECTED) {
                co_return;
            }

            // Send ping - check for null streams
            websocket::ping_data ping_payload = {};

            if (config_.use_ssl && ws_) {
                co_await ws_->async_ping(ping_payload, asio::redirect_error(asio::use_awaitable, ec));
            } else if (!config_.use_ssl && ws_plain_) {
                co_await ws_plain_->async_ping(ping_payload, asio::redirect_error(asio::use_awaitable, ec));
            } else {
                // Stream was destroyed
                co_return;
            }

            if (ec) {
                continue;
            }

            stats_.last_ping_timestamp.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                             std::memory_order_relaxed);

            // Cancel any existing pong timer
            pong_timer_.cancel();

            // Start pong timeout timer
            pong_timer_.expires_after(std::chrono::milliseconds(config_.pong_timeout_ms));

            co_await pong_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (!ec) {  // Timer expired without cancellation
                // Check if we received pong in time
                auto last_pong = stats_.last_pong_timestamp.load(std::memory_order_relaxed);
                auto last_ping = stats_.last_ping_timestamp.load(std::memory_order_relaxed);

                if (last_pong < last_ping) {
                    // Pong timeout - reconnect
                    state_.store(ConnectionState::DISCONNECTED, std::memory_order_release);

                    // Start reconnection
                    active_tasks_.fetch_add(1, std::memory_order_acq_rel);
                    asio::co_spawn(
                        io_context_,
                        [self, this]() -> asio::awaitable<void> {
                            co_await disconnect();
                            co_await connect_with_retry();
                            active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
                        },
                        asio::detached);
                    co_return;
                }
            }
        }
    }

    // Graceful disconnect
    asio::awaitable<void> disconnect() {
        if (state_.load(std::memory_order_acquire) == ConnectionState::DISCONNECTED) {
            co_return;
        }

        state_.store(ConnectionState::DISCONNECTING, std::memory_order_release);

        // Cancel timers
        ping_timer_.cancel();
        pong_timer_.cancel();
        reconnect_timer_.cancel();

        // Close WebSocket gracefully
        boost::system::error_code ec;
        if (config_.use_ssl && ws_) {
            co_await ws_->async_close(websocket::close_code::normal, asio::redirect_error(asio::use_awaitable, ec));
        } else if (!config_.use_ssl && ws_plain_) {
            co_await ws_plain_->async_close(websocket::close_code::normal,
                                            asio::redirect_error(asio::use_awaitable, ec));
        }

        state_.store(ConnectionState::DISCONNECTED, std::memory_order_release);
        on_disconnected();
    }

  private:
    // Core components
    asio::io_context& io_context_;
    ssl::context& ssl_context_;
    WebSocketConfig config_;
    MessageQueue& outbound_queue_;

    // WebSocket streams (only one will be active)
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    std::unique_ptr<websocket::stream<tcp::socket>> ws_plain_;

    // Networking components
    tcp::resolver resolver_;

    // State management
    std::atomic<ConnectionState> state_;
    std::atomic<uint32_t> active_tasks_;  // Track active coroutines

    // Timers
    asio::steady_timer reconnect_timer_;
    asio::steady_timer ping_timer_;
    asio::steady_timer pong_timer_;

    // Reconnection state
    uint32_t current_reconnect_delay_ms_;

    // Message assembly buffer for fragmented messages
    std::string assembly_buffer_;

    // Statistics - properly cache-aligned to 128 bytes
    alignas(128) ConnectionStats stats_;
};

}  // namespace crypto_lob::networking