#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <toml++/toml.hpp>

namespace crypto_lob::core {

/**
 * @brief System-wide configuration parameters
 */
struct SystemConfig {
    std::string log_level{"info"};
    std::string log_file{"/workspace/output/crypto_lob.log"};

    void validate() const;
};

/**
 * @brief Thread affinity and priority configuration
 */
struct ThreadingConfig {
    int control_thread_core{-1};  // -1 means no pinning
    int orderbook_thread_core{-1};

    void validate() const;
};

/**
 * @brief Per-exchange configuration
 */
struct ExchangeConfig {
    // Thread affinity
    int connector_core{-1};
    int parser_core{-1};

    // WebSocket connection settings
    std::string websocket_url;
    bool use_ssl{true};

    // Connection timeouts (milliseconds)
    uint32_t connect_timeout_ms{5000};
    uint32_t handshake_timeout_ms{5000};
    uint32_t read_timeout_ms{30000};

    // Ping/Pong settings
    uint32_t ping_interval_ms{20000};
    uint32_t pong_timeout_ms{60000};

    // Reconnection strategy
    uint32_t reconnect_delay_ms{1000};
    uint32_t max_reconnect_attempts{0};  // 0 = infinite
    uint32_t max_reconnect_delay_ms{60000};

    // Stream subscriptions
    std::vector<std::string> subscriptions;

    void validate() const;
};

/**
 * @brief Memory pool and queue configuration
 */
struct MemoryConfig {
    size_t raw_message_pool_size{8192};
    size_t normalized_pool_size{8192};
    size_t queue_size{4096};

    void validate() const;
};

/**
 * @brief Debug and development settings
 */
struct DebugConfig {
    bool verbose_logging{false};
    bool save_raw_messages{false};
    std::string raw_message_dir{"/workspace/output/raw_messages/"};
    bool enable_profiling{false};
    std::string profile_output{"/workspace/output/profile.json"};
    bool simulate_disconnects{false};
    uint32_t disconnect_interval_sec{300};
    uint32_t max_messages_per_second{0};  // 0 = unlimited
};

/**
 * @brief Main configuration class
 *
 * This class loads and manages all system configuration from TOML files.
 * Configuration is immutable after loading to ensure thread safety.
 *
 * Example usage:
 * @code
 * Configuration config;
 * config.load_from_file("/path/to/config.toml");
 *
 * auto system_cfg = config.get_system();
 * auto exchange_cfg = config.get_exchange("binance_spot");
 * @endcode
 */
class Configuration {
  public:
    Configuration() = default;
    ~Configuration() = default;

    // Delete copy/move to ensure single instance
    Configuration(const Configuration&) = delete;
    Configuration& operator=(const Configuration&) = delete;
    Configuration(Configuration&&) = delete;
    Configuration& operator=(Configuration&&) = delete;

    /**
     * @brief Load configuration from TOML file
     * @param filepath Path to TOML configuration file
     * @throws std::runtime_error on parse errors or validation failures
     */
    void load_from_file(const std::filesystem::path& filepath);

    /**
     * @brief Load configuration from TOML string
     * @param toml_content TOML configuration as string
     * @throws std::runtime_error on parse errors or validation failures
     */
    void load_from_string(std::string_view toml_content);

    /**
     * @brief Get system configuration
     * @return System configuration
     */
    [[nodiscard]] const SystemConfig& get_system() const noexcept {
        return system_;
    }

    /**
     * @brief Get threading configuration
     * @return Threading configuration
     */
    [[nodiscard]] const ThreadingConfig& get_threading() const noexcept {
        return threading_;
    }

    /**
     * @brief Get memory configuration
     * @return Memory configuration
     */
    [[nodiscard]] const MemoryConfig& get_memory() const noexcept {
        return memory_;
    }

    /**
     * @brief Get debug configuration
     * @return Debug configuration
     */
    [[nodiscard]] const DebugConfig& get_debug() const noexcept {
        return debug_;
    }

    /**
     * @brief Get exchange configuration
     * @param name Exchange name (e.g., "binance_spot")
     * @return Exchange configuration if found
     */
    [[nodiscard]] std::optional<ExchangeConfig> get_exchange(const std::string& name) const;

    /**
     * @brief Get all exchange names
     * @return Vector of configured exchange names
     */
    [[nodiscard]] std::vector<std::string> get_exchange_names() const;

    /**
     * @brief Check if configuration has been loaded
     * @return True if configuration is loaded
     */
    [[nodiscard]] bool is_loaded() const noexcept {
        return loaded_;
    }

    /**
     * @brief Get the configuration file path (if loaded from file)
     * @return Path to configuration file
     */
    [[nodiscard]] const std::filesystem::path& get_filepath() const noexcept {
        return filepath_;
    }

    /**
     * @brief Validate all configuration parameters
     * @throws std::runtime_error on validation failures
     */
    void validate() const;

    /**
     * @brief Clear all configuration
     */
    void clear();

  private:
    /**
     * @brief Parse TOML table and populate configuration
     * @param table TOML table to parse
     */
    void parse_toml(const toml::table& table);

    /**
     * @brief Parse system configuration from TOML
     * @param table System configuration table
     */
    void parse_system(const toml::table& table);

    /**
     * @brief Parse threading configuration from TOML
     * @param table Threading configuration table
     */
    void parse_threading(const toml::table& table);

    /**
     * @brief Parse memory configuration from TOML
     * @param table Memory configuration table
     */
    void parse_memory(const toml::table& table);

    /**
     * @brief Parse debug configuration from TOML
     * @param table Debug configuration table
     */
    void parse_debug(const toml::table& table);

    /**
     * @brief Parse exchange configurations from TOML
     * @param table Exchanges configuration table
     */
    void parse_exchanges(const toml::table& table);

    /**
     * @brief Parse single exchange configuration
     * @param name Exchange name
     * @param table Exchange configuration table
     */
    void parse_exchange(const std::string& name, const toml::table& table);

    // Configuration data
    SystemConfig system_;
    ThreadingConfig threading_;
    MemoryConfig memory_;
    DebugConfig debug_;
    std::unordered_map<std::string, ExchangeConfig> exchanges_;

    // State
    bool loaded_{false};
    std::filesystem::path filepath_;
};

/**
 * @brief Global configuration instance getter
 *
 * Provides thread-safe access to the global configuration.
 * Configuration must be loaded before first use.
 *
 * @return Reference to global configuration instance
 */
Configuration& get_config();

}  // namespace crypto_lob::core