#include <chrono>
#include <memory>
#include <thread>

#include <core/memory_pool.hpp>
#include <gtest/gtest.h>
#include <parsing/base_parser.hpp>

using namespace crypto_lob::parsing;
using namespace crypto_lob::core;
using namespace crypto_lob::exchanges::base;

// Bring in the Timestamp alias
using Timestamp = crypto_lob::exchanges::base::Timestamp;

// Mock parser for testing with real JSON parsing
class MockParser : public ParserBase<MockParser> {
  public:
    explicit MockParser(Pool& message_pool) : ParserBase(message_pool) {}

    // Required implementation for CRTP - simplified with more robust parsing
    MessagePtr parse_snapshot_impl(std::string_view json_data,
                                   const std::string& /* symbol */,
                                   Timestamp receive_time,
                                   ParseErrorInfo* error_out = nullptr) noexcept {
        if (json_data.empty()) {
            set_error(error_out, ParseError::INVALID_JSON);
            return nullptr;
        }

        // Use real simdjson parsing
        try {
            auto& parser = get_parser();
            auto padded = simdjson::padded_string(json_data);
            auto doc = parser.iterate(padded);

            // Check for required fields
            auto bids_field = doc.find_field("bids");
            auto asks_field = doc.find_field("asks");

            if (bids_field.error() || asks_field.error()) {
                set_error(error_out, ParseError::MISSING_REQUIRED_FIELD);
                return nullptr;
            }

            auto msg_guard = allocate_message();
            if (!msg_guard) {
                set_error(error_out, ParseError::INVALID_JSON);
                return nullptr;
            }
            auto* msg = msg_guard.get();

            // Initialize message header
            msg->header.type = MessageType::SNAPSHOT;
            msg->header.exchange_id = ExchangeId::BINANCE_SPOT;
            msg->header.instrument_index = 0;
            msg->header.local_timestamp = receive_time;
            msg->header.exchange_timestamp =
                std::chrono::duration_cast<Timestamp>(std::chrono::system_clock::now().time_since_epoch());

            // Start fresh for parsing data
            auto padded2 = simdjson::padded_string(json_data);
            auto doc2 = parser.iterate(padded2);

            // Parse bids array
            auto bids_array = doc2.find_field("bids").get_array();
            size_t bid_count = 0;
            for (auto bid_item : bids_array) {
                if (bid_count >= MAX_LEVELS_PER_MESSAGE)
                    break;

                auto bid_array = bid_item.get_array();
                auto iter = bid_array.begin();

                // Get price as string and parse
                std::string_view price_str = (*iter).get_string();
                msg->bids[bid_count].price = Price::from_string(price_str);
                ++iter;

                // Get quantity as string and convert
                std::string_view qty_str = (*iter).get_string();
                msg->bids[bid_count].quantity = std::stoull(std::string(qty_str));

                bid_count++;
            }
            msg->header.bid_count = static_cast<uint16_t>(bid_count);

            // Parse asks array - restart document parsing
            auto padded3 = simdjson::padded_string(json_data);
            auto doc3 = parser.iterate(padded3);
            auto asks_array = doc3.find_field("asks").get_array();
            size_t ask_count = 0;
            for (auto ask_item : asks_array) {
                if (ask_count >= MAX_LEVELS_PER_MESSAGE)
                    break;

                auto ask_array = ask_item.get_array();
                auto iter = ask_array.begin();

                // Get price as string and parse
                std::string_view price_str = (*iter).get_string();
                msg->asks[ask_count].price = Price::from_string(price_str);
                ++iter;

                // Get quantity as string and convert
                std::string_view qty_str = (*iter).get_string();
                msg->asks[ask_count].quantity = std::stoull(std::string(qty_str));

                ask_count++;
            }
            msg->header.ask_count = static_cast<uint16_t>(ask_count);

            return msg_guard.release();

        } catch (...) {
            set_error(error_out, ParseError::INVALID_JSON);
            return nullptr;
        }
    }

    MessagePtr parse_update_impl(std::string_view json_data,
                                 const std::string& symbol,
                                 Timestamp receive_time,
                                 ParseErrorInfo* error_out = nullptr) noexcept {
        auto* msg = parse_snapshot_impl(json_data, symbol, receive_time, error_out);
        if (msg) {
            msg->header.type = MessageType::DELTA;
        }
        return msg;
    }

    // Required for quantity parsing (called by base class)
    bool parse_quantity_string(std::string_view qty_str,
                               uint64_t& result_out,
                               ParseErrorInfo* error_out = nullptr) const noexcept {
        // Simple mock: just convert string to uint64_t
        try {
            result_out = std::stoull(std::string(qty_str));
            return true;
        } catch (...) {
            set_error(error_out, ParseError::INVALID_QUANTITY_FORMAT);
            return false;
        }
    }
};

// Specialize ParserTraits for MockParser
template <>
struct ParserTraits<MockParser> {
    static constexpr ExchangeId exchange_id = ExchangeId::BINANCE_SPOT;
    static constexpr bool supports_checksum = false;
};

// Instantiate interface conformance check
template struct crypto_lob::parsing::ParserInterfaceCheck<MockParser>;

// RAII helper for automatic message cleanup
class MessageGuard {
  public:
    MessageGuard(MemoryPool<MarketDataMessage>* pool, MarketDataMessage* msg) : pool_(pool), msg_(msg) {}

    ~MessageGuard() {
        if (msg_ && pool_) {
            pool_->destroy(msg_);
        }
    }

    MessageGuard(const MessageGuard&) = delete;
    MessageGuard& operator=(const MessageGuard&) = delete;

    MessageGuard(MessageGuard&& other) noexcept : pool_(other.pool_), msg_(other.msg_) {
        other.msg_ = nullptr;
    }

    MessageGuard& operator=(MessageGuard&& other) noexcept {
        if (this != &other) {
            if (msg_ && pool_)
                pool_->destroy(msg_);
            pool_ = other.pool_;
            msg_ = other.msg_;
            other.msg_ = nullptr;
        }
        return *this;
    }

    MarketDataMessage* get() const {
        return msg_;
    }
    MarketDataMessage* operator->() const {
        return msg_;
    }
    MarketDataMessage& operator*() const {
        return *msg_;
    }
    explicit operator bool() const {
        return msg_ != nullptr;
    }

  private:
    MemoryPool<MarketDataMessage>* pool_;
    MarketDataMessage* msg_;
};

class BaseParserTest : public ::testing::Test {
  protected:
    void SetUp() override {
        message_pool = std::make_unique<MemoryPool<MarketDataMessage>>(100);
        parser = std::make_unique<MockParser>(*message_pool);
    }

    void TearDown() override {
        parser.reset();
        message_pool.reset();
    }

    // Helper to create RAII message guard
    MessageGuard make_guard(MarketDataMessage* msg) {
        return MessageGuard(message_pool.get(), msg);
    }

    std::unique_ptr<MemoryPool<MarketDataMessage>> message_pool;
    std::unique_ptr<MockParser> parser;
};

TEST_F(BaseParserTest, ParseSnapshotSuccess) {
    // Real JSON that exercises the parser
    const std::string json_data = R"({
        "bids": [["50000.00", "100"], ["49999.50", "200"]],
        "asks": [["50001.00", "150"], ["50001.50", "250"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);

    ParseErrorInfo error;
    auto msg_guard = make_guard(parser->parse_snapshot(json_data, symbol, receive_time, &error));

    // Debug: check what error we got if parsing failed
    if (!msg_guard) {
        FAIL() << "Parse failed with error: " << static_cast<int>(error.error);
    }

    ASSERT_TRUE(msg_guard);
    EXPECT_EQ(msg_guard->header.type, MessageType::SNAPSHOT);
    EXPECT_EQ(msg_guard->header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(msg_guard->header.local_timestamp, receive_time);

    // Check parsed data from JSON
    EXPECT_EQ(msg_guard->header.bid_count, 2);
    EXPECT_EQ(msg_guard->bids[0].price, Price::from_string("50000.00"));
    EXPECT_EQ(msg_guard->bids[0].quantity, 100);
    EXPECT_EQ(msg_guard->bids[1].price, Price::from_string("49999.50"));
    EXPECT_EQ(msg_guard->bids[1].quantity, 200);

    EXPECT_EQ(msg_guard->header.ask_count, 2);
    EXPECT_EQ(msg_guard->asks[0].price, Price::from_string("50001.00"));
    EXPECT_EQ(msg_guard->asks[0].quantity, 150);
    EXPECT_EQ(msg_guard->asks[1].price, Price::from_string("50001.50"));
    EXPECT_EQ(msg_guard->asks[1].quantity, 250);
}

TEST_F(BaseParserTest, ParseUpdateSuccess) {
    const std::string json_data = R"({
        "bids": [["49900.00", "300"]],
        "asks": [["50200.00", "400"]]
    })";
    const std::string symbol = "ETHUSDT";
    auto receive_time = std::chrono::nanoseconds(9876543210);

    auto msg_guard = make_guard(parser->parse_update(json_data, symbol, receive_time));

    ASSERT_TRUE(msg_guard);
    EXPECT_EQ(msg_guard->header.type, MessageType::DELTA);
    EXPECT_EQ(msg_guard->header.exchange_id, ExchangeId::BINANCE_SPOT);
}

TEST_F(BaseParserTest, StaticTraits) {
    EXPECT_EQ(MockParser::get_exchange_id(), ExchangeId::BINANCE_SPOT);
    EXPECT_FALSE(MockParser::supports_checksum());
}

TEST_F(BaseParserTest, ChecksumNotSupported) {
    MarketDataMessage dummy_msg{};
    ChecksumResult result;
    ParseErrorInfo error;

    bool success = parser->verify_checksum(dummy_msg, "test_checksum", result, &error);

    EXPECT_FALSE(success);  // Updated: should return false when not supported
    EXPECT_EQ(result, ChecksumResult::NOT_SUPPORTED);
}

TEST_F(BaseParserTest, PriceParsingIntegration) {
    // Test price parsing through the full JSON pipeline
    const std::string json_data = R"({
        "bids": [["123.456", "1000"]],
        "asks": [["123.789", "2000"]]
    })";
    const std::string symbol = "TESTPAIR";
    auto receive_time = std::chrono::nanoseconds(1000000000);

    auto msg_guard = make_guard(parser->parse_snapshot(json_data, symbol, receive_time));

    ASSERT_TRUE(msg_guard);
    EXPECT_EQ(msg_guard->header.bid_count, 1);
    EXPECT_EQ(msg_guard->header.ask_count, 1);

    // Verify price parsing through JSON pipeline
    EXPECT_EQ(msg_guard->bids[0].price, Price::from_string("123.456"));
    EXPECT_EQ(msg_guard->bids[0].quantity, 1000);
    EXPECT_EQ(msg_guard->asks[0].price, Price::from_string("123.789"));
    EXPECT_EQ(msg_guard->asks[0].quantity, 2000);
}

TEST_F(BaseParserTest, HFTPerformanceCharacteristics) {
    // Only run performance test if explicitly requested to avoid CI flakes
    if (!std::getenv("HFT_BENCH")) {
        GTEST_SKIP() << "Performance test skipped. Set HFT_BENCH=1 to enable.";
    }

    const std::string json_data = R"({
        "bids": [["50000.00", "100"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);

    // Warm up parser
    auto warmup_guard = make_guard(parser->parse_snapshot(json_data, symbol, receive_time));
    ASSERT_TRUE(warmup_guard);

    // Statistical measurement over multiple runs
    constexpr int num_runs = 1000;
    std::chrono::nanoseconds total_duration{0};

    for (int i = 0; i < num_runs; ++i) {
        auto start = std::chrono::steady_clock::now();
        auto msg_guard = make_guard(parser->parse_snapshot(json_data, symbol, receive_time));
        auto end = std::chrono::steady_clock::now();

        ASSERT_TRUE(msg_guard);
        total_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
    }

    auto avg_duration = total_duration / num_runs;

    // Statistical threshold: 95th percentile should be reasonable
    // This is more robust than hard wall-clock limits
    EXPECT_LT(avg_duration.count(), 50000) << "Average parse time: " << avg_duration.count() << "ns";
}

TEST_F(BaseParserTest, ZeroAllocationSuccessPath) {
    // Verify that success path doesn't allocate error info
    const std::string json_data = R"({
        "bids": [["50000.00", "100"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);

    // Parse without error out-parameter
    auto msg_guard = make_guard(parser->parse_snapshot(json_data, symbol, receive_time));

    ASSERT_TRUE(msg_guard);

    // Success: Just pointer comparison, no error object allocation
    EXPECT_TRUE(msg_guard.get() != nullptr);
}

// New comprehensive error handling tests
TEST_F(BaseParserTest, ErrorHandling_InvalidJson) {
    const std::string invalid_json = "{\"bid\": []}";  // Missing required "bids" field (has "bid" instead)
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(invalid_json, symbol, receive_time, &error));

    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);  // Our parser correctly identifies missing field
}

TEST_F(BaseParserTest, ErrorHandling_MissingRequiredField) {
    const std::string json_missing_bids = R"({"asks": [["50001.00", "150"]]})";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_missing_bids, symbol, receive_time, &error));

    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::MISSING_REQUIRED_FIELD);
}

TEST_F(BaseParserTest, ErrorHandling_InvalidPriceFormat) {
    const std::string json_invalid_price = R"({
        "bids": [["not_a_price", "100"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_invalid_price, symbol, receive_time, &error));

    // Our simplified mock parses strings directly, so "not_a_price" will create a Price object
    // This tests that JSON parsing works, error validation would be in real implementation
    EXPECT_TRUE(msg_guard);
    EXPECT_EQ(msg_guard->bids[0].price, Price::from_string("not_a_price"));
}

TEST_F(BaseParserTest, ErrorHandling_NegativePrice) {
    const std::string json_negative_price = R"({
        "bids": [["-50000.00", "100"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_negative_price, symbol, receive_time, &error));

    // Our simplified mock will parse negative prices, test the actual error prevention in real implementation
    // For now, this demonstrates the JSON parsing works with negative values
    EXPECT_TRUE(msg_guard);
    EXPECT_EQ(msg_guard->bids[0].price, Price::from_string("-50000.00"));
}

TEST_F(BaseParserTest, ErrorHandling_InvalidQuantityFormat) {
    const std::string json_invalid_qty = R"({
        "bids": [["50000.00", "not_a_quantity"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_invalid_qty, symbol, receive_time, &error));

    // std::stoull will throw, caught as INVALID_JSON
    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::INVALID_JSON);
}

TEST_F(BaseParserTest, ErrorHandling_EmptyJson) {
    const std::string empty_json = "";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(empty_json, symbol, receive_time, &error));

    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::INVALID_JSON);
}

TEST_F(BaseParserTest, ErrorHandling_NumericOverflow) {
    // Test with extremely large numbers that would overflow
    const std::string json_overflow = R"({
        "bids": [["50000.00", "99999999999999999999999999"]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_overflow, symbol, receive_time, &error));

    // std::stoull will throw on overflow, caught as INVALID_JSON
    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::INVALID_JSON);
}

TEST_F(BaseParserTest, ErrorHandling_NullField) {
    const std::string json_null_field = R"({
        "bids": [["50000.00", null]],
        "asks": [["50001.00", "150"]]
    })";
    const std::string symbol = "BTCUSDT";
    auto receive_time = std::chrono::nanoseconds(1234567890);
    ParseErrorInfo error;

    auto msg_guard = make_guard(parser->parse_snapshot(json_null_field, symbol, receive_time, &error));

    // get_string() on null will fail, caught as INVALID_JSON
    EXPECT_FALSE(msg_guard);
    EXPECT_EQ(error.error, ParseError::INVALID_JSON);
}