/**
 * @file configuration_example.cpp
 * @brief Example demonstrating Configuration class usage
 *
 * This example shows how to:
 * - Load configuration from TOML file
 * - Access configuration values
 * - Validate configuration
 * - Handle configuration errors
 */

#include <filesystem>
#include <iomanip>
#include <iostream>

#include <spdlog/spdlog.h>

#include "core/configuration.hpp"

using namespace crypto_lob::core;

void print_separator() {
    std::cout << std::string(60, '-') << std::endl;
}

void print_system_config(const SystemConfig& config) {
    std::cout << "System Configuration:" << std::endl;
    std::cout << "  Log Level: " << config.log_level << std::endl;
    std::cout << "  Log File:  " << config.log_file << std::endl;
}

void print_threading_config(const ThreadingConfig& config) {
    std::cout << "Threading Configuration:" << std::endl;
    std::cout << "  Control Thread Core:   "
              << (config.control_thread_core == -1 ? "No pinning" : std::to_string(config.control_thread_core))
              << std::endl;
    std::cout << "  OrderBook Thread Core: "
              << (config.orderbook_thread_core == -1 ? "No pinning" : std::to_string(config.orderbook_thread_core))
              << std::endl;
}

void print_memory_config(const MemoryConfig& config) {
    std::cout << "Memory Configuration:" << std::endl;
    std::cout << "  Raw Message Pool Size:  " << config.raw_message_pool_size << std::endl;
    std::cout << "  Normalized Pool Size:   " << config.normalized_pool_size << std::endl;
    std::cout << "  Queue Size:             " << config.queue_size << std::endl;
}

void print_exchange_config(const std::string& name, const ExchangeConfig& config) {
    std::cout << "Exchange Configuration [" << name << "]:" << std::endl;
    std::cout << "  WebSocket URL:     " << config.websocket_url << std::endl;
    std::cout << "  SSL Enabled:       " << (config.use_ssl ? "Yes" : "No") << std::endl;
    std::cout << "  Connector Core:    "
              << (config.connector_core == -1 ? "No pinning" : std::to_string(config.connector_core)) << std::endl;
    std::cout << "  Parser Core:       "
              << (config.parser_core == -1 ? "No pinning" : std::to_string(config.parser_core)) << std::endl;
    std::cout << "  Connect Timeout:   " << config.connect_timeout_ms << " ms" << std::endl;
    std::cout << "  Ping Interval:     " << config.ping_interval_ms << " ms" << std::endl;
    std::cout << "  Max Reconnects:    "
              << (config.max_reconnect_attempts == 0 ? "Infinite" : std::to_string(config.max_reconnect_attempts))
              << std::endl;
    std::cout << "  Subscriptions (" << config.subscriptions.size() << "):" << std::endl;
    for (size_t i = 0; i < std::min(size_t(3), config.subscriptions.size()); ++i) {
        std::cout << "    - " << config.subscriptions[i] << std::endl;
    }
    if (config.subscriptions.size() > 3) {
        std::cout << "    ... and " << (config.subscriptions.size() - 3) << " more" << std::endl;
    }
}

int main(int argc, char* argv[]) {
    // Set up logging
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    std::cout << "=== Configuration Example ===" << std::endl << std::endl;

    // Determine config file path
    std::filesystem::path config_file;
    if (argc > 1) {
        config_file = argv[1];
    } else {
        // Try to find config file in common locations
        std::vector<std::filesystem::path> search_paths = {"config/config.toml",
                                                           "../config/config.toml",
                                                           "../../config/config.toml",
                                                           "/workspace/config/config.toml",
                                                           "config/test_binance.toml",
                                                           "../config/test_binance.toml"};

        for (const auto& path : search_paths) {
            if (std::filesystem::exists(path)) {
                config_file = path;
                break;
            }
        }

        if (config_file.empty()) {
            std::cerr << "Error: No configuration file found." << std::endl;
            std::cerr << "Usage: " << argv[0] << " [config_file.toml]" << std::endl;
            return 1;
        }
    }

    std::cout << "Loading configuration from: " << config_file << std::endl << std::endl;

    try {
        // Load configuration
        Configuration config;
        config.load_from_file(config_file);

        if (!config.is_loaded()) {
            std::cerr << "Error: Configuration not loaded properly" << std::endl;
            return 1;
        }

        // Display configuration
        print_separator();
        print_system_config(config.get_system());
        print_separator();
        print_threading_config(config.get_threading());
        print_separator();
        print_memory_config(config.get_memory());
        print_separator();

        // Display exchange configurations
        auto exchange_names = config.get_exchange_names();
        std::cout << "Configured Exchanges: " << exchange_names.size() << std::endl;
        for (const auto& name : exchange_names) {
            std::cout << "  - " << name << std::endl;
        }
        std::cout << std::endl;

        // Display details for each exchange
        for (const auto& name : exchange_names) {
            auto exchange_config = config.get_exchange(name);
            if (exchange_config) {
                print_separator();
                print_exchange_config(name, *exchange_config);
            }
        }

        print_separator();

        // Test global instance
        std::cout << std::endl << "Testing global configuration instance..." << std::endl;
        Configuration& global_config = get_config();

        // Load into global instance
        global_config.load_from_file(config_file);
        std::cout << "Global configuration loaded successfully" << std::endl;

        // Verify it's the same
        if (global_config.get_system().log_level == config.get_system().log_level) {
            std::cout << "Global instance working correctly" << std::endl;
        }

        std::cout << std::endl << "Configuration example completed successfully!" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error loading configuration: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}