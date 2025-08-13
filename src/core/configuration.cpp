#include "configuration.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

namespace crypto_lob::core {

// Validation functions

void SystemConfig::validate() const {
    static const std::array<std::string_view, 5> valid_levels = {"trace", "debug", "info", "warn", "error"};

    auto it = std::find(valid_levels.begin(), valid_levels.end(), log_level);
    if (it == valid_levels.end()) {
        throw std::runtime_error("Invalid log level: " + log_level);
    }

    if (log_file.empty()) {
        throw std::runtime_error("Log file path cannot be empty");
    }
}

void ThreadingConfig::validate() const {
    // Core affinity validation
    // -1 means no pinning, otherwise must be valid core number
    auto validate_core = [](int core, const std::string& name) {
        if (core != -1) {
            // Get number of available cores
            unsigned int num_cores = std::thread::hardware_concurrency();
            if (num_cores == 0) {
                spdlog::warn("Unable to determine number of CPU cores");
                return;
            }

            if (core < 0 || static_cast<unsigned int>(core) >= num_cores) {
                throw std::runtime_error(name + " core " + std::to_string(core) + " is invalid (available cores: 0-" +
                                         std::to_string(num_cores - 1) + ")");
            }
        }
    };

    validate_core(control_thread_core, "Control thread");
    validate_core(orderbook_thread_core, "OrderBook thread");
}

void ExchangeConfig::validate() const {
    // Validate WebSocket URL
    if (websocket_url.empty()) {
        throw std::runtime_error("WebSocket URL cannot be empty");
    }

    // Basic URL validation
    if (!websocket_url.starts_with("ws://") && !websocket_url.starts_with("wss://")) {
        throw std::runtime_error("WebSocket URL must start with ws:// or wss://");
    }

    // Validate SSL consistency
    if (use_ssl && !websocket_url.starts_with("wss://")) {
        throw std::runtime_error("SSL enabled but URL does not use wss://");
    }
    if (!use_ssl && !websocket_url.starts_with("ws://")) {
        throw std::runtime_error("SSL disabled but URL does not use ws://");
    }

    // Validate timeouts
    if (connect_timeout_ms == 0) {
        throw std::runtime_error("Connect timeout must be > 0");
    }
    if (handshake_timeout_ms == 0) {
        throw std::runtime_error("Handshake timeout must be > 0");
    }
    if (read_timeout_ms == 0) {
        throw std::runtime_error("Read timeout must be > 0");
    }
    if (ping_interval_ms == 0) {
        throw std::runtime_error("Ping interval must be > 0");
    }
    if (pong_timeout_ms == 0) {
        throw std::runtime_error("Pong timeout must be > 0");
    }

    // Validate reconnection settings
    if (reconnect_delay_ms == 0) {
        throw std::runtime_error("Reconnect delay must be > 0");
    }
    if (max_reconnect_delay_ms < reconnect_delay_ms) {
        throw std::runtime_error("Max reconnect delay must be >= initial delay");
    }

    // Validate subscriptions
    if (subscriptions.empty()) {
        throw std::runtime_error("At least one subscription is required");
    }

    // Validate core assignments
    auto validate_core = [](int core, const std::string& name) {
        if (core != -1) {
            unsigned int num_cores = std::thread::hardware_concurrency();
            if (num_cores > 0 && (core < 0 || static_cast<unsigned int>(core) >= num_cores)) {
                throw std::runtime_error(name + " core " + std::to_string(core) + " is invalid (available cores: 0-" +
                                         std::to_string(num_cores - 1) + ")");
            }
        }
    };

    validate_core(connector_core, "Connector");
    validate_core(parser_core, "Parser");
}

void MemoryConfig::validate() const {
    // Validate pool sizes
    if (raw_message_pool_size == 0) {
        throw std::runtime_error("Raw message pool size must be > 0");
    }
    if (normalized_pool_size == 0) {
        throw std::runtime_error("Normalized pool size must be > 0");
    }
    if (queue_size == 0) {
        throw std::runtime_error("Queue size must be > 0");
    }

    // Queue size should be power of 2 for SPSC ring buffer efficiency
    if ((queue_size & (queue_size - 1)) != 0) {
        spdlog::warn("Queue size {} is not a power of 2, may impact performance", queue_size);
    }

    // Pool sizes should be larger than queue size
    if (raw_message_pool_size < queue_size) {
        spdlog::warn("Raw message pool size {} is smaller than queue size {}", raw_message_pool_size, queue_size);
    }
    if (normalized_pool_size < queue_size) {
        spdlog::warn("Normalized pool size {} is smaller than queue size {}", normalized_pool_size, queue_size);
    }
}

// Configuration class implementation

void Configuration::load_from_file(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) {
        throw std::runtime_error("Configuration file not found: " + filepath.string());
    }

    try {
        auto config = toml::parse_file(filepath.string());
        parse_toml(config);
        filepath_ = filepath;
        loaded_ = true;

        // Validate after loading
        validate();

        spdlog::info("Configuration loaded from: {}", filepath.string());
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << "Failed to parse TOML file: " << e;
        throw std::runtime_error(oss.str());
    }
}

void Configuration::load_from_string(std::string_view toml_content) {
    try {
        auto config = toml::parse(toml_content);
        parse_toml(config);
        loaded_ = true;

        // Validate after loading
        validate();

        spdlog::info("Configuration loaded from string");
    } catch (const toml::parse_error& e) {
        std::ostringstream oss;
        oss << "Failed to parse TOML string: " << e;
        throw std::runtime_error(oss.str());
    }
}

std::optional<ExchangeConfig> Configuration::get_exchange(const std::string& name) const {
    auto it = exchanges_.find(name);
    if (it != exchanges_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::vector<std::string> Configuration::get_exchange_names() const {
    std::vector<std::string> names;
    names.reserve(exchanges_.size());
    for (const auto& [name, _] : exchanges_) {
        names.push_back(name);
    }
    return names;
}

void Configuration::validate() const {
    system_.validate();
    threading_.validate();
    memory_.validate();

    for (const auto& [name, config] : exchanges_) {
        try {
            config.validate();
        } catch (const std::exception& e) {
            throw std::runtime_error("Exchange '" + name + "' validation failed: " + e.what());
        }
    }
}

void Configuration::clear() {
    system_ = SystemConfig{};
    threading_ = ThreadingConfig{};
    memory_ = MemoryConfig{};
    debug_ = DebugConfig{};
    exchanges_.clear();
    loaded_ = false;
    filepath_.clear();
}

void Configuration::parse_toml(const toml::table& table) {
    // Parse each section
    if (auto system = table["system"].as_table()) {
        parse_system(*system);
    }

    if (auto threading = table["threading"].as_table()) {
        parse_threading(*threading);
    }

    if (auto memory = table["memory"].as_table()) {
        parse_memory(*memory);
    }

    if (auto debug = table["debug"].as_table()) {
        parse_debug(*debug);
    }

    if (auto exchanges = table["exchanges"].as_table()) {
        parse_exchanges(*exchanges);
    }
}

void Configuration::parse_system(const toml::table& table) {
    if (auto val = table["log_level"].value<std::string>()) {
        system_.log_level = *val;
    }
    if (auto val = table["log_file"].value<std::string>()) {
        system_.log_file = *val;
    }
}

void Configuration::parse_threading(const toml::table& table) {
    if (auto val = table["control_thread_core"].value<int64_t>()) {
        threading_.control_thread_core = static_cast<int>(*val);
    }
    if (auto val = table["orderbook_thread_core"].value<int64_t>()) {
        threading_.orderbook_thread_core = static_cast<int>(*val);
    }
}

void Configuration::parse_memory(const toml::table& table) {
    if (auto val = table["raw_message_pool_size"].value<int64_t>()) {
        memory_.raw_message_pool_size = static_cast<size_t>(*val);
    }
    if (auto val = table["normalized_pool_size"].value<int64_t>()) {
        memory_.normalized_pool_size = static_cast<size_t>(*val);
    }
    if (auto val = table["queue_size"].value<int64_t>()) {
        memory_.queue_size = static_cast<size_t>(*val);
    }
}

void Configuration::parse_debug(const toml::table& table) {
    if (auto val = table["verbose_logging"].value<bool>()) {
        debug_.verbose_logging = *val;
    }
    if (auto val = table["save_raw_messages"].value<bool>()) {
        debug_.save_raw_messages = *val;
    }
    if (auto val = table["raw_message_dir"].value<std::string>()) {
        debug_.raw_message_dir = *val;
    }
    if (auto val = table["enable_profiling"].value<bool>()) {
        debug_.enable_profiling = *val;
    }
    if (auto val = table["profile_output"].value<std::string>()) {
        debug_.profile_output = *val;
    }
    if (auto val = table["simulate_disconnects"].value<bool>()) {
        debug_.simulate_disconnects = *val;
    }
    if (auto val = table["disconnect_interval_sec"].value<int64_t>()) {
        debug_.disconnect_interval_sec = static_cast<uint32_t>(*val);
    }
    if (auto val = table["max_messages_per_second"].value<int64_t>()) {
        debug_.max_messages_per_second = static_cast<uint32_t>(*val);
    }
}

void Configuration::parse_exchanges(const toml::table& table) {
    for (auto&& [key, value] : table) {
        if (auto exchange_table = value.as_table()) {
            std::string name(key.str());
            parse_exchange(name, *exchange_table);
        }
    }
}

void Configuration::parse_exchange(const std::string& name, const toml::table& table) {
    ExchangeConfig config;

    // Thread affinity
    if (auto val = table["connector_core"].value<int64_t>()) {
        config.connector_core = static_cast<int>(*val);
    }
    if (auto val = table["parser_core"].value<int64_t>()) {
        config.parser_core = static_cast<int>(*val);
    }

    // WebSocket settings
    if (auto val = table["websocket_url"].value<std::string>()) {
        config.websocket_url = *val;
    }
    if (auto val = table["use_ssl"].value<bool>()) {
        config.use_ssl = *val;
    }

    // Timeouts
    if (auto val = table["connect_timeout_ms"].value<int64_t>()) {
        config.connect_timeout_ms = static_cast<uint32_t>(*val);
    }
    if (auto val = table["handshake_timeout_ms"].value<int64_t>()) {
        config.handshake_timeout_ms = static_cast<uint32_t>(*val);
    }
    if (auto val = table["read_timeout_ms"].value<int64_t>()) {
        config.read_timeout_ms = static_cast<uint32_t>(*val);
    }

    // Ping/Pong
    if (auto val = table["ping_interval_ms"].value<int64_t>()) {
        config.ping_interval_ms = static_cast<uint32_t>(*val);
    }
    if (auto val = table["pong_timeout_ms"].value<int64_t>()) {
        config.pong_timeout_ms = static_cast<uint32_t>(*val);
    }

    // Reconnection
    if (auto val = table["reconnect_delay_ms"].value<int64_t>()) {
        config.reconnect_delay_ms = static_cast<uint32_t>(*val);
    }
    if (auto val = table["max_reconnect_attempts"].value<int64_t>()) {
        config.max_reconnect_attempts = static_cast<uint32_t>(*val);
    }
    if (auto val = table["max_reconnect_delay_ms"].value<int64_t>()) {
        config.max_reconnect_delay_ms = static_cast<uint32_t>(*val);
    }

    // Subscriptions
    if (auto arr = table["subscriptions"].as_array()) {
        for (auto&& elem : *arr) {
            if (auto sub = elem.value<std::string>()) {
                config.subscriptions.push_back(*sub);
            }
        }
    }

    exchanges_[name] = std::move(config);
}

// Global configuration instance
Configuration& get_config() {
    static Configuration instance;
    return instance;
}

}  // namespace crypto_lob::core