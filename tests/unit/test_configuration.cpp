#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include "core/configuration.hpp"

using namespace crypto_lob::core;

class ConfigurationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create temporary directory for test files
        test_dir_ = std::filesystem::temp_directory_path() / "crypto_lob_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        std::filesystem::remove_all(test_dir_);
    }

    std::filesystem::path create_test_file(const std::string& content, const std::string& filename = "test.toml") {
        auto filepath = test_dir_ / filename;
        std::ofstream file(filepath);
        file << content;
        file.close();
        return filepath;
    }

    std::filesystem::path test_dir_;
};

TEST_F(ConfigurationTest, LoadMinimalConfig) {
    const std::string config_content = R"(
[system]
log_level = "info"
log_file = "/tmp/test.log"
)";

    auto filepath = create_test_file(config_content);

    Configuration config;
    ASSERT_NO_THROW(config.load_from_file(filepath));
    EXPECT_TRUE(config.is_loaded());

    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "info");
    EXPECT_EQ(system.log_file, "/tmp/test.log");
}

TEST_F(ConfigurationTest, LoadFromString) {
    const std::string config_content = R"(
[system]
log_level = "debug"
log_file = "/tmp/test2.log"

[threading]
control_thread_core = 0
orderbook_thread_core = 1
)";

    Configuration config;
    ASSERT_NO_THROW(config.load_from_string(config_content));
    EXPECT_TRUE(config.is_loaded());

    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "debug");

    auto& threading = config.get_threading();
    EXPECT_EQ(threading.control_thread_core, 0);
    EXPECT_EQ(threading.orderbook_thread_core, 1);
}

TEST_F(ConfigurationTest, LoadCompleteConfig) {
    const std::string config_content = R"(
[system]
log_level = "info"
log_file = "/workspace/output/test.log"

[threading]
control_thread_core = 0
orderbook_thread_core = 1

[memory]
raw_message_pool_size = 4096
normalized_pool_size = 4096
queue_size = 2048

[debug]
verbose_logging = true
save_raw_messages = false

[exchanges.binance_spot]
connector_core = 2
parser_core = 3
websocket_url = "wss://stream.binance.com:9443/ws"
use_ssl = true
connect_timeout_ms = 3000
handshake_timeout_ms = 3000
ping_interval_ms = 15000
pong_timeout_ms = 45000
reconnect_delay_ms = 500
max_reconnect_attempts = 5
subscriptions = ["btcusdt@depth", "ethusdt@trade"]
)";

    Configuration config;
    ASSERT_NO_THROW(config.load_from_string(config_content));

    // Check system config
    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "info");
    EXPECT_EQ(system.log_file, "/workspace/output/test.log");

    // Check memory config
    auto& memory = config.get_memory();
    EXPECT_EQ(memory.raw_message_pool_size, 4096);
    EXPECT_EQ(memory.normalized_pool_size, 4096);
    EXPECT_EQ(memory.queue_size, 2048);

    // Check debug config
    auto& debug = config.get_debug();
    EXPECT_TRUE(debug.verbose_logging);
    EXPECT_FALSE(debug.save_raw_messages);

    // Check exchange config
    auto exchange = config.get_exchange("binance_spot");
    ASSERT_TRUE(exchange.has_value());
    EXPECT_EQ(exchange->connector_core, 2);
    EXPECT_EQ(exchange->parser_core, 3);
    EXPECT_EQ(exchange->websocket_url, "wss://stream.binance.com:9443/ws");
    EXPECT_TRUE(exchange->use_ssl);
    EXPECT_EQ(exchange->connect_timeout_ms, 3000);
    EXPECT_EQ(exchange->subscriptions.size(), 2);
    EXPECT_EQ(exchange->subscriptions[0], "btcusdt@depth");
    EXPECT_EQ(exchange->subscriptions[1], "ethusdt@trade");
}

TEST_F(ConfigurationTest, MultipleExchanges) {
    const std::string config_content = R"(
[exchanges.binance_spot]
websocket_url = "wss://stream.binance.com:9443/ws"
subscriptions = ["btcusdt@depth"]

[exchanges.coinbase]
websocket_url = "wss://ws-feed.exchange.coinbase.com"
subscriptions = ["BTC-USD"]

[exchanges.kraken]
websocket_url = "wss://ws.kraken.com"
subscriptions = ["XBT/USD"]
)";

    Configuration config;
    ASSERT_NO_THROW(config.load_from_string(config_content));

    auto names = config.get_exchange_names();
    EXPECT_EQ(names.size(), 3);

    // Order might vary, so check existence
    EXPECT_TRUE(std::find(names.begin(), names.end(), "binance_spot") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "coinbase") != names.end());
    EXPECT_TRUE(std::find(names.begin(), names.end(), "kraken") != names.end());

    // Check individual exchanges
    auto binance = config.get_exchange("binance_spot");
    ASSERT_TRUE(binance.has_value());
    EXPECT_EQ(binance->websocket_url, "wss://stream.binance.com:9443/ws");

    auto coinbase = config.get_exchange("coinbase");
    ASSERT_TRUE(coinbase.has_value());
    EXPECT_EQ(coinbase->websocket_url, "wss://ws-feed.exchange.coinbase.com");
}

TEST_F(ConfigurationTest, ValidationInvalidLogLevel) {
    const std::string config_content = R"(
[system]
log_level = "invalid"
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationEmptyWebSocketUrl) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = ""
subscriptions = ["test"]
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationInvalidWebSocketUrl) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "http://invalid.com"
subscriptions = ["test"]
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationSSLMismatch) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "ws://example.com"
use_ssl = true
subscriptions = ["test"]
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationEmptySubscriptions) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "wss://example.com"
subscriptions = []
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationInvalidTimeout) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "wss://example.com"
connect_timeout_ms = 0
subscriptions = ["test"]
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, ValidationInvalidMemorySize) {
    const std::string config_content = R"(
[memory]
raw_message_pool_size = 0
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, DefaultValues) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "wss://example.com"
subscriptions = ["test"]
)";

    Configuration config;
    ASSERT_NO_THROW(config.load_from_string(config_content));

    // Check defaults for system
    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "info");

    // Check defaults for threading
    auto& threading = config.get_threading();
    EXPECT_EQ(threading.control_thread_core, -1);
    EXPECT_EQ(threading.orderbook_thread_core, -1);

    // Check defaults for memory
    auto& memory = config.get_memory();
    EXPECT_EQ(memory.raw_message_pool_size, 8192);
    EXPECT_EQ(memory.queue_size, 4096);

    // Check defaults for exchange
    auto exchange = config.get_exchange("test");
    ASSERT_TRUE(exchange.has_value());
    EXPECT_EQ(exchange->connector_core, -1);
    EXPECT_EQ(exchange->parser_core, -1);
    EXPECT_TRUE(exchange->use_ssl);
    EXPECT_EQ(exchange->connect_timeout_ms, 5000);
    EXPECT_EQ(exchange->ping_interval_ms, 20000);
    EXPECT_EQ(exchange->reconnect_delay_ms, 1000);
    EXPECT_EQ(exchange->max_reconnect_attempts, 0);
}

TEST_F(ConfigurationTest, ClearConfiguration) {
    const std::string config_content = R"(
[system]
log_level = "debug"
)";

    Configuration config;
    config.load_from_string(config_content);
    EXPECT_TRUE(config.is_loaded());

    config.clear();
    EXPECT_FALSE(config.is_loaded());

    // After clear, should have default values
    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "info");
}

TEST_F(ConfigurationTest, NonExistentExchange) {
    const std::string config_content = R"(
[exchanges.test]
websocket_url = "wss://example.com"
subscriptions = ["test"]
)";

    Configuration config;
    config.load_from_string(config_content);

    auto exchange = config.get_exchange("non_existent");
    EXPECT_FALSE(exchange.has_value());
}

TEST_F(ConfigurationTest, FileNotFound) {
    Configuration config;
    EXPECT_THROW(config.load_from_file("/non/existent/file.toml"), std::runtime_error);
}

TEST_F(ConfigurationTest, InvalidTOMLSyntax) {
    const std::string config_content = R"(
[system
log_level = "info"
)";

    Configuration config;
    EXPECT_THROW(config.load_from_string(config_content), std::runtime_error);
}

TEST_F(ConfigurationTest, GlobalConfigInstance) {
    // Get global instance
    Configuration& config1 = get_config();
    Configuration& config2 = get_config();

    // Should be the same instance
    EXPECT_EQ(&config1, &config2);
}

TEST_F(ConfigurationTest, PartialConfiguration) {
    // Test with only some sections present
    const std::string config_content = R"(
[threading]
control_thread_core = 2

[exchanges.partial]
websocket_url = "wss://partial.com"
subscriptions = ["test"]
)";

    Configuration config;
    ASSERT_NO_THROW(config.load_from_string(config_content));

    // System section should have defaults
    auto& system = config.get_system();
    EXPECT_EQ(system.log_level, "info");

    // Threading section should have our value and default
    auto& threading = config.get_threading();
    EXPECT_EQ(threading.control_thread_core, 2);
    EXPECT_EQ(threading.orderbook_thread_core, -1);

    // Memory section should have all defaults
    auto& memory = config.get_memory();
    EXPECT_EQ(memory.raw_message_pool_size, 8192);
}