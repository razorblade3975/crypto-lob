#include <chrono>
#include <cstring>
#include <memory>

#include <gtest/gtest.h>

#include "exchanges/base/message_types.hpp"

using namespace crypto_lob::exchanges::base;
using namespace crypto_lob::core;

class MessageTypesTest : public ::testing::Test {
  protected:
    // Helper to create a test timestamp
    Timestamp make_test_timestamp(int64_t millis) {
        return timestamp_from_millis(millis);
    }
};

// Basic construction and size tests
TEST_F(MessageTypesTest, BasicStructureSizes) {
    // Verify all structures have expected sizes for cache alignment
    EXPECT_EQ(sizeof(MessageHeader), 64);
    EXPECT_EQ(sizeof(SequenceInfo), 64);
    EXPECT_EQ(sizeof(TradeMessage), 128);
    EXPECT_EQ(sizeof(TickerMessage), 128);
    EXPECT_EQ(sizeof(ConnectionMessage), 128);

    // MarketDataMessage is large due to fixed arrays
    const size_t expected_market_data_size =
        sizeof(MessageHeader) + sizeof(SequenceInfo) + (2 * MAX_LEVELS_PER_MESSAGE * sizeof(PriceLevel));
    EXPECT_EQ(sizeof(MarketDataMessage), expected_market_data_size);
}

// Test MessageHeader construction and defaults
TEST_F(MessageTypesTest, MessageHeaderDefaults) {
    MessageHeader header;

    EXPECT_EQ(header.type, MessageType::SNAPSHOT);
    EXPECT_EQ(header.exchange_id, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(header.instrument_index, 0);
    EXPECT_EQ(header.sequence_number, 0);
    EXPECT_EQ(header.exchange_sequence, 0);
    EXPECT_EQ(header.checksum, 0);
    EXPECT_EQ(header.bid_count, 0);
    EXPECT_EQ(header.ask_count, 0);
    EXPECT_EQ(header.flags, 0);

    // Verify timestamps are zero
    EXPECT_EQ(header.exchange_timestamp.count(), 0);
    EXPECT_EQ(header.local_timestamp.count(), 0);

    // Verify padding is zero-initialized
    for (size_t i = 0; i < sizeof(header.reserved); ++i) {
        EXPECT_EQ(header.reserved[i], 0);
    }
}

// Test PriceLevel construction
TEST_F(MessageTypesTest, PriceLevelConstruction) {
    // Default construction
    PriceLevel level1;
    EXPECT_EQ(level1.price, Price::zero());
    EXPECT_EQ(level1.quantity, 0);

    // Value construction
    Price test_price = Price::from_string("123.45");
    PriceLevel level2(test_price, 1000);
    EXPECT_EQ(level2.price, test_price);
    EXPECT_EQ(level2.quantity, 1000);

    // Equality
    PriceLevel level3(test_price, 1000);
    EXPECT_EQ(level2, level3);

    PriceLevel level4(test_price, 2000);
    EXPECT_NE(level2, level4);
}

// Test timestamp conversion
TEST_F(MessageTypesTest, TimestampConversion) {
    // Test normal case
    int64_t millis = 1234567890123LL;
    Timestamp ts = timestamp_from_millis(millis);
    EXPECT_EQ(ts.count(), millis * 1'000'000);

    // Test zero
    Timestamp ts_zero = timestamp_from_millis(0);
    EXPECT_EQ(ts_zero.count(), 0);

    // Test negative (for relative timestamps)
    Timestamp ts_neg = timestamp_from_millis(-1000);
    EXPECT_EQ(ts_neg.count(), -1'000'000'000);
}

// Test SequenceInfo for different exchanges
TEST_F(MessageTypesTest, SequenceInfoExchangeSpecific) {
    SequenceInfo seq;

    // Binance style
    seq.set_for_exchange(ExchangeId::BINANCE_SPOT, 100, 105);
    EXPECT_EQ(seq.first_update_id, 100);
    EXPECT_EQ(seq.last_update_id, 105);
    // Other fields should be zero
    EXPECT_EQ(seq.sequence_start, 0);
    EXPECT_EQ(seq.sequence_end, 0);
    EXPECT_EQ(seq.previous_update_id, 0);
    EXPECT_EQ(seq.sequence_number, 0);

    // KuCoin style
    seq.set_for_exchange(ExchangeId::KUCOIN_SPOT, 200, 210);
    EXPECT_EQ(seq.sequence_start, 200);
    EXPECT_EQ(seq.sequence_end, 210);
    // Binance fields should be cleared
    EXPECT_EQ(seq.first_update_id, 0);
    EXPECT_EQ(seq.last_update_id, 0);

    // OKX style
    seq.set_for_exchange(ExchangeId::OKX_SPOT, 300, 301);
    EXPECT_EQ(seq.previous_update_id, 300);
    EXPECT_EQ(seq.sequence_number, 301);
    // Other fields cleared
    EXPECT_EQ(seq.sequence_start, 0);
    EXPECT_EQ(seq.sequence_end, 0);

    // Bybit style (single sequence)
    seq.set_for_exchange(ExchangeId::BYBIT_SPOT, 400);
    EXPECT_EQ(seq.sequence_number, 400);
    EXPECT_EQ(seq.previous_update_id, 0);
}

// Test MarketDataMessage basic functionality
TEST_F(MessageTypesTest, MarketDataMessageBasics) {
    // Stack allocation of union with header constructor
    Message msg;
    EXPECT_EQ(msg.type(), MessageType::SNAPSHOT);

    // Allocate MarketDataMessage on heap (can't use make_unique due to deleted copy ctor)
    std::unique_ptr<MarketDataMessage> market_msg(new MarketDataMessage());

    // Set up as snapshot
    market_msg->header.type = MessageType::SNAPSHOT;
    market_msg->header.exchange_id = ExchangeId::BINANCE_SPOT;
    market_msg->header.exchange_timestamp = make_test_timestamp(1000);
    market_msg->header.local_timestamp = make_test_timestamp(1001);

    EXPECT_TRUE(market_msg->is_snapshot());
    EXPECT_FALSE(market_msg->is_delta());

    // Add some price levels
    market_msg->bids[0] = PriceLevel(Price::from_string("50000.00"), 10);
    market_msg->bids[1] = PriceLevel(Price::from_string("49999.00"), 20);
    market_msg->asks[0] = PriceLevel(Price::from_string("50001.00"), 15);
    market_msg->asks[1] = PriceLevel(Price::from_string("50002.00"), 25);

    market_msg->header.bid_count = 2;
    market_msg->header.ask_count = 2;

    // Test span getters
    auto bids = market_msg->get_bids();
    EXPECT_EQ(bids.size(), 2);
    EXPECT_EQ(bids[0].price, Price::from_string("50000.00"));
    EXPECT_EQ(bids[0].quantity, 10);

    auto asks = market_msg->get_asks();
    EXPECT_EQ(asks.size(), 2);
    EXPECT_EQ(asks[0].price, Price::from_string("50001.00"));
    EXPECT_EQ(asks[0].quantity, 15);

    // unique_ptr automatically deletes
}

// Test bounds checking in get_bids/get_asks
TEST_F(MessageTypesTest, MarketDataBoundsChecking) {
    std::unique_ptr<MarketDataMessage> msg(new MarketDataMessage());

    // Set invalid counts
    msg->header.bid_count = 1000;  // Exceeds MAX_LEVELS_PER_MESSAGE
    msg->header.ask_count = 2000;  // Way over limit

    // Should clamp to array size
    auto bids = msg->get_bids();
    auto asks = msg->get_asks();

    EXPECT_EQ(bids.size(), MAX_LEVELS_PER_MESSAGE);
    EXPECT_EQ(asks.size(), MAX_LEVELS_PER_MESSAGE);
}

// Test continuity checking between messages
TEST_F(MessageTypesTest, MessageContinuityChecking) {
    std::unique_ptr<MarketDataMessage> msg1(new MarketDataMessage());
    std::unique_ptr<MarketDataMessage> msg2(new MarketDataMessage());

    // Set up Binance-style messages
    msg1->header.exchange_id = ExchangeId::BINANCE_SPOT;
    msg2->header.exchange_id = ExchangeId::BINANCE_SPOT;

    msg1->sequence.set_for_exchange(ExchangeId::BINANCE_SPOT, 100, 105);
    msg2->sequence.set_for_exchange(ExchangeId::BINANCE_SPOT, 106, 110);

    EXPECT_TRUE(msg2->is_continuous_with(*msg1));

    // Break continuity
    msg2->sequence.set_for_exchange(ExchangeId::BINANCE_SPOT, 107, 110);
    EXPECT_FALSE(msg2->is_continuous_with(*msg1));

    // Test KuCoin style (strict equality required)
    msg1->header.exchange_id = ExchangeId::KUCOIN_SPOT;
    msg2->header.exchange_id = ExchangeId::KUCOIN_SPOT;

    msg1->sequence.set_for_exchange(ExchangeId::KUCOIN_SPOT, 200, 205);
    msg2->sequence.set_for_exchange(ExchangeId::KUCOIN_SPOT, 206, 210);

    EXPECT_TRUE(msg2->is_continuous_with(*msg1));

    // KuCoin with overlap (should fail)
    msg2->sequence.set_for_exchange(ExchangeId::KUCOIN_SPOT, 205, 210);
    EXPECT_FALSE(msg2->is_continuous_with(*msg1));
}

// Test other message types
TEST_F(MessageTypesTest, TradeMessage) {
    TradeMessage trade;

    EXPECT_EQ(trade.header.type, MessageType::TRADE);
    EXPECT_EQ(trade.price, Price::zero());
    EXPECT_EQ(trade.quantity, 0);
    EXPECT_EQ(trade.side, Side::BUY);
    EXPECT_FALSE(trade.is_maker);
    EXPECT_EQ(trade.trade_id, 0);

    // Verify padding is zero
    for (size_t i = 0; i < sizeof(trade.reserved1); ++i) {
        EXPECT_EQ(trade.reserved1[i], 0);
    }
    for (size_t i = 0; i < sizeof(trade.reserved2); ++i) {
        EXPECT_EQ(trade.reserved2[i], 0);
    }
}

TEST_F(MessageTypesTest, ConnectionMessage) {
    ConnectionMessage conn;

    EXPECT_EQ(conn.header.type, MessageType::CONNECTION_STATUS);
    EXPECT_EQ(conn.status, ConnectionStatus::DISCONNECTED);
    EXPECT_EQ(conn.error_code, 0);
    EXPECT_EQ(conn.retry_count, 0);

    // Verify details array is zero-initialized
    for (size_t i = 0; i < sizeof(conn.details); ++i) {
        EXPECT_EQ(conn.details[i], 0);
    }
}

// Test message flags
TEST_F(MessageTypesTest, MessageFlags) {
    MessageHeader header;

    // Set multiple flags
    header.flags = FLAG_TICK_BY_TICK | FLAG_HAS_CHECKSUM;

    // Check individual flags
    EXPECT_TRUE(header.flags & FLAG_TICK_BY_TICK);
    EXPECT_TRUE(header.flags & FLAG_HAS_CHECKSUM);
    EXPECT_FALSE(header.flags & FLAG_SEQUENCE_RESET);

    // Clear a flag
    header.flags &= ~FLAG_TICK_BY_TICK;
    EXPECT_FALSE(header.flags & FLAG_TICK_BY_TICK);
    EXPECT_TRUE(header.flags & FLAG_HAS_CHECKSUM);
}