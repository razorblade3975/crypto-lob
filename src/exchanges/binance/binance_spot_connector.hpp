#pragma once

#include <sstream>
#include <vector>

#include "../../core/memory_pool.hpp"
#include "../../core/timestamp.hpp"
#include "../../networking/websocket_client.hpp"
#include "binance_spot_parser.hpp"

namespace crypto_lob::exchanges::binance {

/**
 * HFT-optimized Binance Spot WebSocket connector
 *
 * Implements Binance Spot-specific protocol requirements:
 * - Combined streams via /stream?streams=
 * - 20-second ping interval with 1-minute pong deadline
 * - Depth snapshot + delta synchronization
 * - Trade and book ticker streams
 */
class BinanceSpotConnector : public networking::WebSocketClient<BinanceSpotConnector> {
  public:
    using Base = networking::WebSocketClient<BinanceSpotConnector>;
    using MessageCallback = std::function<void(MarketDataMessage*)>;

    // Binance-specific configuration
    struct Config {
        std::vector<std::string> symbols;  // e.g., ["BTCUSDT", "ETHUSDT"]
        std::vector<std::string> streams;  // e.g., ["depth", "trade", "bookTicker"]
        int depth_levels = 20;             // 5, 10, or 20 for depth streams
        bool use_combined_stream = true;

        // Convert to WebSocket config
        networking::WebSocketConfig to_websocket_config() const {
            networking::WebSocketConfig ws_config;
            ws_config.host = "stream.binance.com";
            ws_config.port = "9443";
            ws_config.use_ssl = true;

            // Build target path
            if (use_combined_stream && !symbols.empty() && !streams.empty()) {
                std::stringstream path;
                path << "/stream?streams=";

                bool first = true;
                for (const auto& symbol : symbols) {
                    // Convert to lowercase for Binance
                    std::string lower_symbol;
                    lower_symbol.reserve(symbol.size());
                    for (char c : symbol) {
                        lower_symbol += std::tolower(c);
                    }

                    for (const auto& stream : streams) {
                        if (!first)
                            path << "/";
                        first = false;

                        if (stream == "depth") {
                            path << lower_symbol << "@depth" << depth_levels << "@100ms";
                        } else if (stream == "trade") {
                            path << lower_symbol << "@trade";
                        } else if (stream == "bookTicker") {
                            path << lower_symbol << "@bookTicker";
                        } else {
                            path << lower_symbol << "@" << stream;
                        }
                    }
                }

                ws_config.target = path.str();
            } else {
                ws_config.target = "/ws";
            }

            // Binance-specific timeouts
            ws_config.ping_interval_ms = 20000;  // 20 seconds
            ws_config.pong_timeout_ms = 60000;   // 1 minute
            ws_config.read_timeout_ms = 90000;   // 1.5 minutes

            return ws_config;
        }
    };

    BinanceSpotConnector(asio::io_context& io_context,
                         ssl::context& ssl_context,
                         const Config& config,
                         Base::MessageQueue& outbound_queue,
                         MessageCallback callback)
        : Base(io_context, ssl_context, config.to_websocket_config(), outbound_queue),
          config_(config),
          message_callback_(callback),
          parser_(message_pool_) {}

    // Subscribe to additional streams after connection
    asio::awaitable<bool> subscribe(const std::vector<std::string>& streams) {
        if (get_state() != networking::ConnectionState::CONNECTED) {
            co_return false;
        }

        // Build subscription message
        std::stringstream msg;
        msg << R"({"method":"SUBSCRIBE","params":[)";

        bool first = true;
        for (const auto& stream : streams) {
            if (!first)
                msg << ",";
            first = false;
            msg << '"' << stream << '"';
        }

        msg << R"(],"id":)" << next_request_id_++ << "}";

        co_return co_await send_message(msg.str());
    }

    // Unsubscribe from streams
    asio::awaitable<bool> unsubscribe(const std::vector<std::string>& streams) {
        if (get_state() != networking::ConnectionState::CONNECTED) {
            co_return false;
        }

        // Build unsubscription message
        std::stringstream msg;
        msg << R"({"method":"UNSUBSCRIBE","params":[)";

        bool first = true;
        for (const auto& stream : streams) {
            if (!first)
                msg << ",";
            first = false;
            msg << '"' << stream << '"';
        }

        msg << R"(],"id":)" << next_request_id_++ << "}";

        co_return co_await send_message(msg.str());
    }

    // CRTP implementation - called when connected
    asio::awaitable<void> on_connected_impl() {
        // Subscribe to configured streams if not using combined stream
        if (!config_.use_combined_stream && !config_.symbols.empty()) {
            std::vector<std::string> streams;
            for (const auto& symbol : config_.symbols) {
                std::string lower_symbol;
                lower_symbol.reserve(symbol.size());
                for (char c : symbol) {
                    lower_symbol += std::tolower(c);
                }

                for (const auto& stream : config_.streams) {
                    if (stream == "depth") {
                        streams.push_back(lower_symbol + "@depth" + std::to_string(config_.depth_levels) + "@100ms");
                    } else if (stream == "trade") {
                        streams.push_back(lower_symbol + "@trade");
                    } else if (stream == "bookTicker") {
                        streams.push_back(lower_symbol + "@bookTicker");
                    }
                }
            }

            if (!streams.empty()) {
                co_await subscribe(streams);
            }
        }

        co_return;
    }

    // CRTP implementation - called for each message
    void on_message_impl(networking::RawMessage* msg) {
        // Capture RDTSC timestamp as early as possible for minimum latency
        // This gives us a high-precision relative timestamp for latency measurement
        uint64_t arrival_tsc = crypto_lob::core::rdtsc();

        // Prefetch parser's thread-local simdjson instance
        __builtin_prefetch(&parser_, 0, 3);

        // Parse message
        std::string_view json_data(msg->data, msg->size);

        // Check if it's a combined stream message
        if (config_.use_combined_stream) {
            // Combined stream wraps data in {"stream":"...","data":{...}}
            // For now, pass through to parser which handles this
        }

        // Parse with HFT-optimized parser
        auto result = parser_.parse_update(json_data);

        if (result.has_value()) {
            MarketDataMessage* market_msg = result.value();

            // For now, use the timestamp from the raw message which should already be
            // captured at the earliest point in the network layer.
            // In the future, the network layer should use RDTSC for capture.
            market_msg->local_timestamp = msg->timestamp_ns;

            // Invoke callback
            if (message_callback_) {
                message_callback_(market_msg);
            }
        } else {
            // Parsing error - might be a control message
            // Check for subscription responses, errors, etc.
            handle_control_message(json_data);
        }
    }

    // CRTP implementation - called when ping received
    asio::awaitable<void> on_ping_impl(std::string_view data) {
        // Binance doesn't require custom ping handling
        // Beast handles ping/pong automatically
        co_return;
    }

    // CRTP implementation - called when disconnected
    void on_disconnected_impl() {
        // Could notify upstream systems
        // Reset any state if needed
    }

  private:
    // Handle control messages (subscription responses, errors)
    void handle_control_message(std::string_view json_data) {
        // Quick check for control message patterns
        if (json_data.find("\"result\":null") != std::string_view::npos) {
            // Successful subscription/unsubscription
            return;
        }

        if (json_data.find("\"error\"") != std::string_view::npos) {
            // Error message - log if needed
            // In HFT, we avoid logging on hot path
            return;
        }
    }

  private:
    Config config_;
    MessageCallback message_callback_;
    std::atomic<uint32_t> next_request_id_{1};

    // Parser and memory pool
    core::MemoryPool<MarketDataMessage> message_pool_{8192};  // 8K messages
    BinanceSpotParser parser_;
};

}  // namespace crypto_lob::exchanges::binance