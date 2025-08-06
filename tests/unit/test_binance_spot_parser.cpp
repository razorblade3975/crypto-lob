#include <chrono>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "core/memory_pool.hpp"
#include "exchanges/base/message_types.hpp"
#include "exchanges/binance/binance_spot_parser.hpp"

using namespace crypto_lob::exchanges::binance;
using namespace crypto_lob::parsing;
using namespace crypto_lob::exchanges::base;
using crypto_lob::core::MemoryPool;  // Use specific import to avoid conflict

class BinanceSpotParserTest : public ::testing::Test {
  protected:
    void SetUp() override {
        pool_ = std::make_unique<MemoryPool<MarketDataMessage>>(1000);
        parser_ = std::make_unique<BinanceSpotParser>(*pool_);
        receive_time_ = std::chrono::duration_cast<crypto_lob::exchanges::base::Timestamp>(
            std::chrono::system_clock::now().time_since_epoch());
    }

    void TearDown() override {
        parser_.reset();
        pool_.reset();
    }

    std::unique_ptr<MemoryPool<MarketDataMessage>> pool_;
    std::unique_ptr<BinanceSpotParser> parser_;
    crypto_lob::exchanges::base::Timestamp receive_time_;
};

// ===============================
// Parser Traits Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParserTraitsCorrect) {
    EXPECT_EQ(BinanceSpotParser::get_exchange_id(), ExchangeId::BINANCE_SPOT);
    EXPECT_FALSE(BinanceSpotParser::supports_checksum());
}

// ===============================
// Quantity String Parsing Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseQuantityString_Integer) {
    uint64_t result;
    ParseErrorInfo error;

    EXPECT_TRUE(parser_->parse_quantity_string("100", result, &error));
    EXPECT_EQ(result, 100'000'000'000ULL);  // 100 * 10^9
}

TEST_F(BinanceSpotParserTest, ParseQuantityString_Decimal) {
    uint64_t result;
    ParseErrorInfo error;

    EXPECT_TRUE(parser_->parse_quantity_string("123.456789", result, &error));
    EXPECT_EQ(result, 123'456'789'000ULL);  // 123.456789 * 10^9
}

TEST_F(BinanceSpotParserTest, ParseQuantityString_SmallDecimal) {
    uint64_t result;
    ParseErrorInfo error;

    EXPECT_TRUE(parser_->parse_quantity_string("0.001", result, &error));
    EXPECT_EQ(result, 1'000'000ULL);  // 0.001 * 10^9
}

TEST_F(BinanceSpotParserTest, ParseQuantityString_Zero) {
    uint64_t result;
    ParseErrorInfo error;

    EXPECT_TRUE(parser_->parse_quantity_string("0", result, &error));
    EXPECT_EQ(result, 0ULL);

    EXPECT_TRUE(parser_->parse_quantity_string("0.0", result, &error));
    EXPECT_EQ(result, 0ULL);
}

TEST_F(BinanceSpotParserTest, ParseQuantityString_InvalidFormat) {
    uint64_t result;
    ParseErrorInfo error;

    EXPECT_FALSE(parser_->parse_quantity_string("", result, &error));
    EXPECT_EQ(error.error, ParseError::INVALID_QUANTITY_FORMAT);

    EXPECT_FALSE(parser_->parse_quantity_string("abc", result, &error));
    EXPECT_EQ(error.error, ParseError::INVALID_QUANTITY_FORMAT);

    EXPECT_FALSE(parser_->parse_quantity_string("12.34.56", result, &error));
    EXPECT_EQ(error.error, ParseError::INVALID_QUANTITY_FORMAT);
}

// ===============================
// Depth Snapshot Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseSnapshot_Valid) {
    const char* json = R"({
        "lastUpdateId": 1027024,
        "bids": [
            ["4.00000000", "431.00000000"],
            ["3.99000000", "200.00000000"]
        ],
        "asks": [
            ["4.00000200", "12.00000000"],
            ["4.01000000", "50.00000000"]
        ]
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_snapshot(json, "BTCUSDT", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::SNAPSHOT);
    EXPECT_EQ(msg->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg->header.exchange_sequence, 1027024ULL);
    EXPECT_EQ(msg->header.bid_count, 2);
    EXPECT_EQ(msg->header.ask_count, 2);

    // Check bid levels
    EXPECT_EQ(msg->bids[0].price, Price::from_string("4.00000000"));
    EXPECT_EQ(msg->bids[0].quantity, 431'000'000'000ULL);
    EXPECT_EQ(msg->bids[1].price, Price::from_string("3.99000000"));
    EXPECT_EQ(msg->bids[1].quantity, 200'000'000'000ULL);

    // Check ask levels
    EXPECT_EQ(msg->asks[0].price, Price::from_string("4.00000200"));
    EXPECT_EQ(msg->asks[0].quantity, 12'000'000'000ULL);
    EXPECT_EQ(msg->asks[1].price, Price::from_string("4.01000000"));
    EXPECT_EQ(msg->asks[1].quantity, 50'000'000'000ULL);

    pool_->destroy(msg);
}

TEST_F(BinanceSpotParserTest, ParseSnapshot_MissingFields) {
    const char* json = R"({
        "bids": [["4.00", "100"]],
        "asks": [["4.01", "200"]]
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_snapshot(json, "BTCUSDT", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);
}

// ===============================
// Depth Update Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseDepthUpdate_Valid) {
    const char* json = R"({
        "e": "depthUpdate",
        "E": 1672515782136,
        "s": "BNBBTC",
        "U": 157,
        "u": 160,
        "b": [
            ["0.0024", "10"],
            ["0.0023", "0"]
        ],
        "a": [
            ["0.0026", "100"]
        ]
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBBTC", receive_time_, &error);

    if (msg == nullptr) {
        std::cout << "ERROR: Parse failed with error code: " << static_cast<int>(error.error) << std::endl;
    }
    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::DELTA);
    EXPECT_EQ(msg->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg->header.exchange_sequence, 160ULL);
    EXPECT_EQ(msg->header.bid_count, 2);
    EXPECT_EQ(msg->header.ask_count, 1);

    // Check timestamp conversion
    EXPECT_EQ(msg->header.exchange_timestamp, timestamp_from_millis(1672515782136));

    // Check levels
    EXPECT_EQ(msg->bids[0].price, Price::from_string("0.0024"));
    EXPECT_EQ(msg->bids[0].quantity, 10'000'000'000ULL);
    EXPECT_EQ(msg->bids[1].price, Price::from_string("0.0023"));
    EXPECT_EQ(msg->bids[1].quantity, 0ULL);  // Zero quantity (level removal)

    EXPECT_EQ(msg->asks[0].price, Price::from_string("0.0026"));
    EXPECT_EQ(msg->asks[0].quantity, 100'000'000'000ULL);

    pool_->destroy(msg);
}

TEST_F(BinanceSpotParserTest, ParseDepthUpdate_MissingSequence) {
    const char* json = R"({
        "e": "depthUpdate",
        "E": 1672515782136,
        "s": "BNBBTC",
        "b": [["0.0024", "10"]],
        "a": [["0.0026", "100"]]
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBBTC", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);
}

// ===============================
// Aggregate Trade Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseAggTrade_Valid) {
    const char* json = R"({
        "e": "aggTrade",
        "E": 1672515782136,
        "s": "BNBBTC",
        "a": 12345,
        "p": "0.001",
        "q": "100",
        "f": 100,
        "l": 105,
        "T": 1672515782136,
        "m": true,
        "M": true
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBBTC", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::TRADE);
    EXPECT_EQ(msg->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg->header.exchange_sequence, 12345ULL);
    EXPECT_EQ(msg->header.bid_count, 1);
    EXPECT_EQ(msg->header.ask_count, 0);

    // Trade stored in bids[0]
    EXPECT_EQ(msg->bids[0].price, Price::from_string("0.001"));
    EXPECT_EQ(msg->bids[0].quantity, 100'000'000'000ULL);

    pool_->destroy(msg);
}

// ===============================
// Individual Trade Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseTrade_Valid) {
    const char* json = R"({
        "e": "trade",
        "E": 1672515782136,
        "s": "BNBBTC",
        "t": 12345,
        "p": "0.001",
        "q": "100",
        "T": 1672515782136,
        "m": true,
        "M": true
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBBTC", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::TRADE);
    EXPECT_EQ(msg->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg->header.exchange_sequence, 12345ULL);
    EXPECT_EQ(msg->header.bid_count, 1);
    EXPECT_EQ(msg->header.ask_count, 0);

    EXPECT_EQ(msg->bids[0].price, Price::from_string("0.001"));
    EXPECT_EQ(msg->bids[0].quantity, 100'000'000'000ULL);

    pool_->destroy(msg);
}

// ===============================
// Book Ticker Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseBookTicker_SpotFormat) {
    const char* json = R"({
        "u": 400900217,
        "s": "BNBUSDT",
        "b": "25.35190000",
        "B": "31.21000000",
        "a": "25.36520000",
        "A": "40.66000000"
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBUSDT", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::TICKER);
    EXPECT_EQ(msg->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg->header.exchange_sequence, 400900217ULL);
    EXPECT_EQ(msg->header.bid_count, 1);
    EXPECT_EQ(msg->header.ask_count, 1);

    // Check best bid
    EXPECT_EQ(msg->bids[0].price, Price::from_string("25.35190000"));
    EXPECT_EQ(msg->bids[0].quantity, 31'210'000'000ULL);

    // Check best ask
    EXPECT_EQ(msg->asks[0].price, Price::from_string("25.36520000"));
    EXPECT_EQ(msg->asks[0].quantity, 40'660'000'000ULL);

    pool_->destroy(msg);
}

TEST_F(BinanceSpotParserTest, ParseBookTicker_FuturesFormat) {
    const char* json = R"({
        "e": "bookTicker",
        "u": 400900217,
        "E": 1568014460893,
        "T": 1568014460891,
        "s": "BNBUSDT",
        "b": "25.35190000",
        "B": "31.21000000",
        "a": "25.36520000",
        "A": "40.66000000"
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BNBUSDT", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    EXPECT_EQ(msg->header.type, MessageType::TICKER);
    EXPECT_EQ(msg->header.exchange_sequence, 400900217ULL);

    pool_->destroy(msg);
}

// ===============================
// Error Handling Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseUpdate_InvalidJSON) {
    // Empty string should trigger INVALID_JSON error
    const char* json = "";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BTCUSDT", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::INVALID_JSON);
}

TEST_F(BinanceSpotParserTest, ParseUpdate_MissingRequiredFields) {
    // Valid JSON but missing required fields should return MISSING_REQUIRED_FIELD
    const char* json = R"({"some_field": "some_value"})";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BTCUSDT", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);
}

TEST_F(BinanceSpotParserTest, ParseUpdate_UnsupportedEventType) {
    const char* json = R"({
        "e": "unknownEvent",
        "s": "BTCUSDT"
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BTCUSDT", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::UNSUPPORTED_MESSAGE_TYPE);
}

TEST_F(BinanceSpotParserTest, ParseUpdate_NoEventField_NotBookTicker) {
    const char* json = R"({
        "s": "BTCUSDT",
        "p": "50000"
    })";

    ParseErrorInfo error;
    auto* msg = parser_->parse_update(json, "BTCUSDT", receive_time_, &error);

    EXPECT_EQ(msg, nullptr);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);
}

// ===============================
// Performance/Edge Case Tests
// ===============================

TEST_F(BinanceSpotParserTest, ParseLargePriceLevels) {
    // Test with many price levels (close to MAX_LEVELS_PER_MESSAGE)
    std::string json = R"({"lastUpdateId": 123, "bids": [)";

    for (int i = 0; i < 500; ++i) {
        if (i > 0)
            json += ",";
        json += R"([")" + std::to_string(50000 - i) + R"(.0", "100.0"])";
    }

    json += R"(], "asks": [)";

    for (int i = 0; i < 500; ++i) {
        if (i > 0)
            json += ",";
        json += R"([")" + std::to_string(50001 + i) + R"(.0", "100.0"])";
    }

    json += "]}";

    ParseErrorInfo error;
    auto* msg = parser_->parse_snapshot(json, "BTCUSDT", receive_time_, &error);

    ASSERT_NE(msg, nullptr);
    // Should be capped at MAX_LEVELS_PER_MESSAGE
    EXPECT_LE(msg->header.bid_count, MAX_LEVELS_PER_MESSAGE);
    EXPECT_LE(msg->header.ask_count, MAX_LEVELS_PER_MESSAGE);

    pool_->destroy(msg);
}

TEST_F(BinanceSpotParserTest, ParseVeryLargeQuantity) {
    uint64_t result;
    ParseErrorInfo error;

    // Test with a large but valid quantity
    EXPECT_TRUE(parser_->parse_quantity_string("1000000000.123456789", result, &error));
    EXPECT_EQ(result, 1000000000'123456789ULL);
}

TEST_F(BinanceSpotParserTest, ParseQuantityOverflow) {
    uint64_t result;
    ParseErrorInfo error;

    // Test quantity that would overflow uint64_t
    EXPECT_FALSE(parser_->parse_quantity_string("99999999999999999999999", result, &error));
    EXPECT_EQ(error.error, ParseError::NUMERIC_OVERFLOW);
}

// ===============================
// Memory Management Tests
// ===============================

TEST_F(BinanceSpotParserTest, MemoryPoolExhaustion) {
    // Create a small pool that will be exhausted
    // Use THROW_EXCEPTION policy for tests to avoid terminating the process
    MemoryPool<MarketDataMessage> small_pool(2, PoolDepletionPolicy::THROW_EXCEPTION);
    BinanceSpotParser small_parser(small_pool);

    const char* json = R"({
        "lastUpdateId": 123,
        "bids": [["50000.0", "100.0"]],
        "asks": [["50001.0", "100.0"]]
    })";

    // Allocate all messages from the pool
    auto* msg1 = small_parser.parse_snapshot(json, "BTCUSDT", receive_time_);
    auto* msg2 = small_parser.parse_snapshot(json, "BTCUSDT", receive_time_);

    ASSERT_NE(msg1, nullptr);
    ASSERT_NE(msg2, nullptr);

    // Third allocation should fail
    ParseErrorInfo error;
    auto* msg3 = small_parser.parse_snapshot(json, "BTCUSDT", receive_time_, &error);
    EXPECT_EQ(msg3, nullptr);
    EXPECT_EQ(error.error, ParseError::BUFFER_TOO_SMALL);

    // Clean up
    small_pool.destroy(msg1);
    small_pool.destroy(msg2);
}

TEST_F(BinanceSpotParserTest, RAIIGuardBehavior) {
    const char* invalid_json = R"({"invalid": true})";

    ParseErrorInfo error;
    auto* msg = parser_->parse_snapshot(invalid_json, "BTCUSDT", receive_time_, &error);

    // Message should be null and memory should be automatically cleaned up
    EXPECT_EQ(msg, nullptr);
    // The JSON is valid but missing required field "lastUpdateId"
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);

    // Pool should still be usable for new allocations
    const char* valid_json = R"({
        "lastUpdateId": 123,
        "bids": [["50000.0", "100.0"]],
        "asks": [["50001.0", "100.0"]]
    })";

    auto* valid_msg = parser_->parse_snapshot(valid_json, "BTCUSDT", receive_time_);
    ASSERT_NE(valid_msg, nullptr);

    pool_->destroy(valid_msg);
}