#pragma once

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstring>
#include <memory>

#include <boost/asio.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include "../core/cache.hpp"
#include "../core/spsc_ring.hpp"
#include "../core/timestamp.hpp"
#include "raw_message_pool.hpp"

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace crypto_lob::networking {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

// WebSocket connection state - packed into single atomic for efficiency
enum class ConnectionState : uint32_t {
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2,
    DISCONNECTING = 3,
    RECONNECTING = 4,
    READING = 5,
    ERROR = 6
};

// HFT-optimized statistics - separate hot/cold counters
struct alignas(128) HotStats {
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> bytes_received{0};
    std::atomic<uint64_t> last_message_tsc{0};  // TSC instead of timestamp
    std::atomic<uint64_t> last_ping_tsc{0};
    std::atomic<uint64_t> last_pong_tsc{0};
    // Padding to avoid false sharing
    char padding[64 - (5 * sizeof(std::atomic<uint64_t>))];
};

struct alignas(64) ColdStats {
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> reconnect_count{0};
    std::atomic<uint64_t> connection_errors{0};
    std::atomic<uint64_t> fragmented_messages{0};
    std::atomic<uint64_t> oversized_messages{0};
};

// Configuration for WebSocket client
struct WebSocketConfig {
    std::string host;
    std::string port;
    std::string target;
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
    uint32_t max_reconnect_attempts = 0;  // 0 = infinite

    // HFT settings
    int cpu_affinity = -1;  // CPU core to pin to (-1 = no pinning)
    bool use_huge_pages = true;
    bool prefetch_next = true;

    // Message handling
    static constexpr size_t MAX_MESSAGE_SIZE = 1024 * 1024;  // 1MB max
};

/**
 * HFT-optimized WebSocket client using Boost.Beast with C++20 coroutines
 *
 * Key optimizations:
 * - RDTSC timestamps (~3ns vs ~50ns for clock_gettime)
 * - Fixed-size buffers (no heap allocations)
 * - Hot/cold data separation
 * - CPU affinity support
 * - Prefetching for predictable access patterns
 * - Single state machine coroutine (reduced overhead)
 * - No virtual functions or shared_ptr overhead
 */
template <typename Derived>
class alignas(64) WebSocketClient {
  public:
    using MessageQueue = core::SPSCRing<RawMessage*>;

    WebSocketClient(asio::io_context& io_context,
                    ssl::context& ssl_context,
                    const WebSocketConfig& config,
                    MessageQueue& outbound_queue)
        : hot_{io_context, outbound_queue, ConnectionState::DISCONNECTED},
          cold_{ssl_context, config},
          resolver_(io_context),
          reconnect_timer_(io_context),
          ping_timer_(io_context),
          pong_timer_(io_context) {
        // Pin to CPU if requested
        if (config.cpu_affinity >= 0) {
            pin_to_cpu(config.cpu_affinity);
        }
    }

    // Non-virtual destructor - CRTP doesn't need virtual
    ~WebSocketClient() {
        stop_and_wait();
    }

    // Start the WebSocket client
    void start() {
        hot_.state.store(static_cast<uint32_t>(ConnectionState::CONNECTING), std::memory_order_release);

        // Single state machine coroutine
        asio::co_spawn(
            hot_.io_context, [this]() -> asio::awaitable<void> { co_await run_state_machine(); }, asio::detached);
    }

    // Stop the WebSocket client
    void stop() {
        uint32_t expected = static_cast<uint32_t>(ConnectionState::CONNECTED);
        hot_.state.compare_exchange_strong(
            expected, static_cast<uint32_t>(ConnectionState::DISCONNECTING), std::memory_order_acq_rel);
    }

    // Synchronous stop and wait
    void stop_and_wait() {
        stop();

        // Busy wait with exponential backoff
        uint32_t wait_us = 1;
        while (hot_.state.load(std::memory_order_acquire) != static_cast<uint32_t>(ConnectionState::DISCONNECTED)) {
            if (wait_us < 1000) {
                // Spin for short waits
                for (uint32_t i = 0; i < wait_us * 10; ++i) {
                    __builtin_ia32_pause();
                }
                wait_us *= 2;
            } else {
                // Yield for longer waits
                std::this_thread::yield();
            }
        }

        // Clean up streams
        ws_.reset();
        ws_plain_.reset();
    }

    // Send a message
    asio::awaitable<bool> send_message(std::string_view message) {
        if (hot_.state.load(std::memory_order_acquire) != static_cast<uint32_t>(ConnectionState::CONNECTED)) {
            co_return false;
        }

        boost::system::error_code ec;

        if (cold_.config.use_ssl && ws_) {
            co_await ws_->async_write(asio::buffer(message.data(), message.size()),
                                      asio::redirect_error(asio::use_awaitable, ec));
        } else if (!cold_.config.use_ssl && ws_plain_) {
            co_await ws_plain_->async_write(asio::buffer(message.data(), message.size()),
                                            asio::redirect_error(asio::use_awaitable, ec));
        } else {
            co_return false;
        }

        if (!ec) {
            cold_stats_.messages_sent.fetch_add(1, std::memory_order_relaxed);
            cold_stats_.bytes_sent.fetch_add(message.size(), std::memory_order_relaxed);
            co_return true;
        }

        co_return false;
    }

    // Get connection state
    [[nodiscard]] ConnectionState get_state() const noexcept {
        return static_cast<ConnectionState>(hot_.state.load(std::memory_order_acquire));
    }

    // Get statistics
    [[nodiscard]] const HotStats& get_hot_stats() const noexcept {
        return hot_stats_;
    }

    [[nodiscard]] const ColdStats& get_cold_stats() const noexcept {
        return cold_stats_;
    }

  protected:
    // CRTP interface - derived class must implement these
    Derived& derived() {
        return static_cast<Derived&>(*this);
    }
    const Derived& derived() const {
        return static_cast<const Derived&>(*this);
    }

    // Callbacks for derived class
    asio::awaitable<void> on_connected() {
        co_return co_await derived().on_connected_impl();
    }

    void on_message(RawMessage* msg) {
        derived().on_message_impl(msg);
    }

    void on_disconnected() {
        derived().on_disconnected_impl();
    }

  private:
    // Pin thread to specific CPU core
    void pin_to_cpu(int cpu_id) {
#ifdef __linux__
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);

        pthread_t thread = pthread_self();
        if (pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) != 0) {
            // Log warning but continue
        }
#endif
    }

    // Main state machine - single coroutine manages all states
    asio::awaitable<void> run_state_machine() {
        uint32_t reconnect_delay_ms = cold_.config.initial_reconnect_delay_ms;
        uint32_t reconnect_attempts = 0;

        while (true) {
            auto state = static_cast<ConnectionState>(hot_.state.load(std::memory_order_acquire));

            switch (state) {
                case ConnectionState::CONNECTING: {
                    if (co_await connect_once()) {
                        hot_.state.store(static_cast<uint32_t>(ConnectionState::CONNECTED), std::memory_order_release);
                        reconnect_delay_ms = cold_.config.initial_reconnect_delay_ms;
                        reconnect_attempts = 0;
                        co_await on_connected();
                    } else {
                        // Connection failed
                        if (cold_.config.max_reconnect_attempts > 0 &&
                            ++reconnect_attempts >= cold_.config.max_reconnect_attempts) {
                            hot_.state.store(static_cast<uint32_t>(ConnectionState::ERROR), std::memory_order_release);
                            co_return;
                        }

                        // Wait before reconnecting
                        reconnect_timer_.expires_after(std::chrono::milliseconds(reconnect_delay_ms));
                        co_await reconnect_timer_.async_wait(asio::use_awaitable);

                        // Exponential backoff (safe calculation)
                        reconnect_delay_ms = std::min(reconnect_delay_ms * 2, cold_.config.max_reconnect_delay_ms);
                        cold_stats_.reconnect_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    break;
                }

                case ConnectionState::CONNECTED: {
                    // Read messages and handle ping/pong
                    co_await parallel_read_and_ping();

                    // If we exit, we disconnected
                    if (hot_.state.load(std::memory_order_acquire) !=
                        static_cast<uint32_t>(ConnectionState::DISCONNECTING)) {
                        // Unexpected disconnect - reconnect
                        hot_.state.store(static_cast<uint32_t>(ConnectionState::CONNECTING), std::memory_order_release);
                        on_disconnected();
                    }
                    break;
                }

                case ConnectionState::DISCONNECTING: {
                    co_await disconnect();
                    hot_.state.store(static_cast<uint32_t>(ConnectionState::DISCONNECTED), std::memory_order_release);
                    on_disconnected();
                    co_return;
                }

                case ConnectionState::DISCONNECTED:
                case ConnectionState::ERROR:
                    co_return;

                default:
                    // Unknown state
                    co_return;
            }
        }
    }

    // Connect once
    asio::awaitable<bool> connect_once() {
        boost::system::error_code ec;

        // Resolve host
        auto results = co_await resolver_.async_resolve(
            cold_.config.host, cold_.config.port, asio::redirect_error(asio::use_awaitable, ec));

        if (ec) {
            cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
            co_return false;
        }

        if (cold_.config.use_ssl) {
            auto stream =
                std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(hot_.io_context, cold_.ssl_context);

            auto& socket = beast::get_lowest_layer(*stream);
            socket.expires_after(std::chrono::milliseconds(cold_.config.connect_timeout_ms));

            co_await socket.async_connect(results, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // SSL handshake
            socket.expires_after(std::chrono::milliseconds(cold_.config.handshake_timeout_ms));
            co_await stream->next_layer().async_handshake(ssl::stream_base::client,
                                                          asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // WebSocket handshake
            co_await stream->async_handshake(
                cold_.config.host, cold_.config.target, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            // Set control callback for pong
            stream->control_callback([this](websocket::frame_type kind, beast::string_view) {
                if (kind == websocket::frame_type::pong) {
                    hot_stats_.last_pong_tsc.store(core::rdtsc(), std::memory_order_relaxed);
                    pong_timer_.cancel();
                }
            });

            ws_ = std::move(stream);
        } else {
            // Non-SSL version (similar logic)
            auto stream = std::make_unique<websocket::stream<tcp::socket>>(hot_.io_context);

            stream->next_layer().expires_after(std::chrono::milliseconds(cold_.config.connect_timeout_ms));

            co_await stream->next_layer().async_connect(results, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            co_await stream->async_handshake(
                cold_.config.host, cold_.config.target, asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
                cold_stats_.connection_errors.fetch_add(1, std::memory_order_relaxed);
                co_return false;
            }

            stream->control_callback([this](websocket::frame_type kind, beast::string_view) {
                if (kind == websocket::frame_type::pong) {
                    hot_stats_.last_pong_tsc.store(core::rdtsc(), std::memory_order_relaxed);
                    pong_timer_.cancel();
                }
            });

            ws_plain_ = std::move(stream);
        }

        co_return true;
    }

    // Parallel read and ping handling
    asio::awaitable<void> parallel_read_and_ping() {
        auto read_task = read_messages();
        auto ping_task = ping_loop();

        // Run both in parallel, exit when either completes
        auto [read_result, ping_result] = co_await asio::experimental::make_parallel_group(
                                              asio::co_spawn(hot_.io_context, std::move(read_task), asio::deferred),
                                              asio::co_spawn(hot_.io_context, std::move(ping_task), asio::deferred))
                                              .async_wait(asio::experimental::wait_for_one(), asio::use_awaitable);
    }

    // Read messages with zero-copy and prefetching
    asio::awaitable<void> read_messages() {
        auto& pool = get_thread_pool();

        while (hot_.state.load(std::memory_order_acquire) == static_cast<uint32_t>(ConnectionState::CONNECTED)) {
            RawMessage* msg = pool.acquire();
            if (!msg) [[unlikely]] {
                // Pool exhausted - wait briefly
                co_await asio::steady_timer(hot_.io_context, std::chrono::microseconds(10))
                    .async_wait(asio::use_awaitable);
                continue;
            }

            // Prefetch message buffer for write
            if (cold_.config.prefetch_next) {
                __builtin_prefetch(msg->data, 1, 3);  // Write, high temporal locality
            }

            boost::system::error_code ec;
            size_t bytes_read = 0;
            bool is_complete = false;

            // Read message
            if (cold_.config.use_ssl && ws_) {
                bytes_read = co_await ws_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                           asio::redirect_error(asio::use_awaitable, ec));

                if (!ec) {
                    is_complete = ws_->is_message_done();
                }
            } else if (ws_plain_) {
                bytes_read = co_await ws_plain_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                                 asio::redirect_error(asio::use_awaitable, ec));

                if (!ec) {
                    is_complete = ws_plain_->is_message_done();
                }
            }

            if (ec) {
                pool.release(msg);
                co_return;  // Exit on error
            }

            // Handle fragmented messages
            if (!is_complete) {
                // Accumulate in fixed buffer
                if (assembly_size_ + bytes_read > WebSocketConfig::MAX_MESSAGE_SIZE) {
                    // Message too large - drain and drop
                    cold_stats_.oversized_messages.fetch_add(1, std::memory_order_relaxed);
                    assembly_size_ = 0;

                    // Drain remaining fragments
                    while (!is_complete) {
                        if (cold_.config.use_ssl && ws_) {
                            co_await ws_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                          asio::redirect_error(asio::use_awaitable, ec));
                            if (!ec)
                                is_complete = ws_->is_message_done();
                        } else if (ws_plain_) {
                            co_await ws_plain_->async_read_some(asio::buffer(msg->data, RawMessage::MAX_FRAME_SIZE),
                                                                asio::redirect_error(asio::use_awaitable, ec));
                            if (!ec)
                                is_complete = ws_plain_->is_message_done();
                        }
                        if (ec)
                            break;
                    }
                    pool.release(msg);
                    continue;
                }

                // Accumulate fragment
                std::memcpy(assembly_buffer_ + assembly_size_, msg->data, bytes_read);
                assembly_size_ += bytes_read;
                pool.release(msg);
                cold_stats_.fragmented_messages.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            // Complete message
            if (assembly_size_ > 0) {
                // Append final fragment
                if (assembly_size_ + bytes_read <= RawMessage::MAX_FRAME_SIZE) {
                    std::memcpy(assembly_buffer_ + assembly_size_, msg->data, bytes_read);
                    assembly_size_ += bytes_read;

                    // Copy back to message buffer
                    std::memcpy(msg->data, assembly_buffer_, assembly_size_);
                    msg->size = static_cast<uint32_t>(assembly_size_);
                    assembly_size_ = 0;
                } else {
                    // Too large
                    cold_stats_.oversized_messages.fetch_add(1, std::memory_order_relaxed);
                    assembly_size_ = 0;
                    pool.release(msg);
                    continue;
                }
            } else {
                msg->size = static_cast<uint32_t>(bytes_read);
            }

            // Set TSC timestamp (3ns)
            msg->timestamp_ns = core::rdtsc();

            // Update statistics
            hot_stats_.messages_received.fetch_add(1, std::memory_order_relaxed);
            hot_stats_.bytes_received.fetch_add(msg->size, std::memory_order_relaxed);
            hot_stats_.last_message_tsc.store(msg->timestamp_ns, std::memory_order_relaxed);

            // Queue for parser thread
            if (!hot_.outbound_queue.try_push(msg)) [[unlikely]] {
                pool.release(msg);
            }
        }
    }

    // Ping loop
    asio::awaitable<void> ping_loop() {
        while (hot_.state.load(std::memory_order_acquire) == static_cast<uint32_t>(ConnectionState::CONNECTED)) {
            ping_timer_.expires_after(std::chrono::milliseconds(cold_.config.ping_interval_ms));

            boost::system::error_code ec;
            co_await ping_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (ec)
                co_return;

            // Send ping
            websocket::ping_data ping_payload = {};

            if (cold_.config.use_ssl && ws_) {
                co_await ws_->async_ping(ping_payload, asio::redirect_error(asio::use_awaitable, ec));
            } else if (ws_plain_) {
                co_await ws_plain_->async_ping(ping_payload, asio::redirect_error(asio::use_awaitable, ec));
            }

            if (ec)
                continue;

            hot_stats_.last_ping_tsc.store(core::rdtsc(), std::memory_order_relaxed);

            // Wait for pong
            pong_timer_.expires_after(std::chrono::milliseconds(cold_.config.pong_timeout_ms));

            co_await pong_timer_.async_wait(asio::redirect_error(asio::use_awaitable, ec));

            if (!ec) {
                // Timeout - check if pong received
                auto last_pong = hot_stats_.last_pong_tsc.load(std::memory_order_relaxed);
                auto last_ping = hot_stats_.last_ping_tsc.load(std::memory_order_relaxed);

                if (last_pong < last_ping) {
                    // No pong received - disconnect
                    co_return;
                }
            }
        }
    }

    // Disconnect
    asio::awaitable<void> disconnect() {
        boost::system::error_code ec;

        if (cold_.config.use_ssl && ws_) {
            co_await ws_->async_close(websocket::close_code::normal, asio::redirect_error(asio::use_awaitable, ec));
        } else if (ws_plain_) {
            co_await ws_plain_->async_close(websocket::close_code::normal,
                                            asio::redirect_error(asio::use_awaitable, ec));
        }
    }

  private:
    // Hot data - frequently accessed (single cache line)
    alignas(64) struct {
        asio::io_context& io_context;
        MessageQueue& outbound_queue;
        std::atomic<uint32_t> state;
        uint32_t padding;
    } hot_;

    // Cold data - rarely accessed
    alignas(64) struct {
        ssl::context& ssl_context;
        WebSocketConfig config;
    } cold_;

    // WebSocket streams
    std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
    std::unique_ptr<websocket::stream<tcp::socket>> ws_plain_;

    // Networking components
    tcp::resolver resolver_;

    // Timers
    asio::steady_timer reconnect_timer_;
    asio::steady_timer ping_timer_;
    asio::steady_timer pong_timer_;

    // Fixed-size assembly buffer (no heap allocation)
    alignas(64) char assembly_buffer_[WebSocketConfig::MAX_MESSAGE_SIZE];
    size_t assembly_size_ = 0;

    // Statistics - separated hot/cold
    alignas(128) HotStats hot_stats_;
    alignas(64) ColdStats cold_stats_;
};

}  // namespace crypto_lob::networking