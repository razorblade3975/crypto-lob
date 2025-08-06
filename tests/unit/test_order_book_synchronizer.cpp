#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "../../src/core/price.hpp"
#include "../../src/core/timestamp.hpp"
#include "../../src/exchange/message_types.hpp"
#include "../../src/orderbook/order_book.hpp"
#include "../../src/orderbook/order_book_synchronizer.hpp"
#include "../../src/orderbook/price_level.hpp"

using namespace crypto_lob;
using namespace crypto_lob::orderbook;
using namespace crypto_lob::exchange;
using namespace crypto_lob::core;

class OrderBookSynchronizerTest : public ::testing::Test {
  protected:
    using PoolType = core::MemoryPool<core::PriceLevelNode>;

    void SetUp() override {
        // Create memory pool for order book
        pool_ = std::make_unique<PoolType>(10000);

        // Create order book with instrument ID and pool
        core::InstrumentId instrument_id{core::ExchangeId::BINANCE_SPOT, "BTCUSDT"};
        order_book_ = std::make_unique<core::OrderBook>(instrument_id, *pool_);

        // Create synchronizer
        synchronizer_ = std::make_unique<OrderBookSynchronizer>(*order_book_);
    }

    MarketDataMessage create_snapshot_message(ExchangeId exchange_id,
                                              uint64_t sequence,
                                              const std::vector<std::pair<double, double>>& bids,
                                              const std::vector<std::pair<double, double>>& asks) {
        MarketDataMessage msg{};
        msg.header.type = MessageType::SNAPSHOT;
        msg.header.exchange_id = exchange_id;
        msg.header.exchange_sequence = sequence;
        msg.header.exchange_timestamp = std::chrono::nanoseconds(0);
        msg.header.local_timestamp = std::chrono::nanoseconds(0);

        // Set bids
        msg.header.bid_count = std::min(bids.size(), size_t(512));
        for (size_t i = 0; i < msg.header.bid_count; ++i) {
            msg.bids[i].price = Price::from_string(std::to_string(bids[i].first));
            msg.bids[i].quantity = static_cast<uint64_t>(bids[i].second * 1e9);
        }

        // Set asks
        msg.header.ask_count = std::min(asks.size(), size_t(512));
        for (size_t i = 0; i < msg.header.ask_count; ++i) {
            msg.asks[i].price = Price::from_string(std::to_string(asks[i].first));
            msg.asks[i].quantity = static_cast<uint64_t>(asks[i].second * 1e9);
        }

        // Set exchange-specific sequence info
        if (exchange_id == ExchangeId::BINANCE_SPOT) {
            msg.sequence.last_update_id = sequence;
        } else if (exchange_id == ExchangeId::KUCOIN_SPOT) {
            msg.sequence.sequence_end = sequence;
        } else if (exchange_id == ExchangeId::OKX_SPOT) {
            msg.sequence.sequence_number = sequence;
        }

        return msg;
    }

    MarketDataMessage create_delta_message(ExchangeId exchange_id,
                                           uint64_t first_seq,
                                           uint64_t last_seq,
                                           const std::vector<std::pair<double, double>>& bid_updates,
                                           const std::vector<std::pair<double, double>>& ask_updates) {
        MarketDataMessage msg{};
        msg.header.type = MessageType::DELTA;
        msg.header.exchange_id = exchange_id;
        msg.header.exchange_timestamp = std::chrono::nanoseconds(0);
        msg.header.local_timestamp = std::chrono::nanoseconds(0);

        // Set bid updates
        msg.header.bid_count = std::min(bid_updates.size(), size_t(512));
        for (size_t i = 0; i < msg.header.bid_count; ++i) {
            msg.bids[i].price = Price::from_string(std::to_string(bid_updates[i].first));
            msg.bids[i].quantity = static_cast<uint64_t>(bid_updates[i].second * 1e9);
        }

        // Set ask updates
        msg.header.ask_count = std::min(ask_updates.size(), size_t(512));
        for (size_t i = 0; i < msg.header.ask_count; ++i) {
            msg.asks[i].price = Price::from_string(std::to_string(ask_updates[i].first));
            msg.asks[i].quantity = static_cast<uint64_t>(ask_updates[i].second * 1e9);
        }

        // Set exchange-specific sequence info
        if (exchange_id == ExchangeId::BINANCE_SPOT) {
            msg.sequence.first_update_id = first_seq;
            msg.sequence.last_update_id = last_seq;
            msg.header.exchange_sequence = last_seq;
        } else if (exchange_id == ExchangeId::KUCOIN_SPOT) {
            msg.sequence.sequence_start = first_seq;
            msg.sequence.sequence_end = last_seq;
            msg.header.exchange_sequence = last_seq;
        } else if (exchange_id == ExchangeId::OKX_SPOT) {
            msg.sequence.previous_update_id = first_seq;
            msg.sequence.sequence_number = last_seq;
            msg.header.exchange_sequence = last_seq;
        } else {
            msg.header.exchange_sequence = last_seq;
        }

        return msg;
    }

    std::unique_ptr<PoolType> pool_;
    std::unique_ptr<core::OrderBook> order_book_;
    std::unique_ptr<OrderBookSynchronizer> synchronizer_;
};

TEST_F(OrderBookSynchronizerTest, InitialState) {
    EXPECT_TRUE(synchronizer_->needs_snapshot());
    EXPECT_FALSE(synchronizer_->is_synchronized());
    EXPECT_EQ(synchronizer_->get_last_sequence(), 0);
}

TEST_F(OrderBookSynchronizerTest, SnapshotSynchronization) {
    // Create and apply snapshot
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT,
                                            1000,
                                            {{100.0, 1.0}, {99.0, 2.0}},  // bids
                                            {{101.0, 1.5}, {102.0, 2.5}}  // asks
    );

    auto result = synchronizer_->process_message(&snapshot);

    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::OK);
    EXPECT_TRUE(result.top_of_book_changed);
    EXPECT_FALSE(synchronizer_->needs_snapshot());
    EXPECT_TRUE(synchronizer_->is_synchronized());
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1000);

    // Verify order book state
    auto top = order_book_->top_of_book();
    EXPECT_EQ(top.bid_price, Price::from_string(std::to_string(100.0)));
    EXPECT_EQ(top.ask_price, Price::from_string(std::to_string(101.0)));
}

TEST_F(OrderBookSynchronizerTest, DeltaAfterSnapshot_Binance) {
    // Apply snapshot first
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Apply continuous delta
    auto delta = create_delta_message(ExchangeId::BINANCE_SPOT,
                                      1001,            // first_update_id
                                      1002,            // last_update_id
                                      {{100.5, 2.0}},  // new best bid
                                      {});

    auto result = synchronizer_->process_message(&delta);

    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::OK);
    EXPECT_TRUE(result.top_of_book_changed);
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1002);

    // Verify order book update
    auto top = order_book_->top_of_book();
    EXPECT_EQ(top.bid_price, Price::from_string(std::to_string(100.5)));
}

TEST_F(OrderBookSynchronizerTest, GapDetection_Binance) {
    // Apply snapshot
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Apply delta with gap (skipping sequences 1001-1004)
    auto delta = create_delta_message(ExchangeId::BINANCE_SPOT,
                                      1005,  // Gap! Expected 1001
                                      1006,
                                      {{99.5, 3.0}},
                                      {});

    auto result = synchronizer_->process_message(&delta);

    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::NEEDS_SNAPSHOT);
    EXPECT_FALSE(result.top_of_book_changed);
    EXPECT_TRUE(synchronizer_->needs_snapshot());
    EXPECT_FALSE(synchronizer_->is_synchronized());

    // Verify statistics
    auto stats = synchronizer_->get_stats();
    EXPECT_EQ(stats.gaps_detected, 1);
}

TEST_F(OrderBookSynchronizerTest, DeltaBeforeSnapshot) {
    // Send delta before snapshot (should return NEEDS_SNAPSHOT)
    auto delta = create_delta_message(ExchangeId::BINANCE_SPOT, 101, 102, {{100.5, 2.0}}, {});

    auto result = synchronizer_->process_message(&delta);
    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::NEEDS_SNAPSHOT);
    EXPECT_FALSE(result.top_of_book_changed);

    // Verify statistics
    auto stats = synchronizer_->get_stats();
    EXPECT_EQ(stats.deltas_processed, 0);  // Delta not processed
}

TEST_F(OrderBookSynchronizerTest, StaleMessageHandling) {
    // Apply snapshot with sequence 1000
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Apply some updates
    auto delta1 = create_delta_message(ExchangeId::BINANCE_SPOT, 1001, 1005, {{100.5, 2.0}}, {});
    synchronizer_->process_message(&delta1);

    // Try to apply old message (should be marked as STALE)
    auto old_delta = create_delta_message(ExchangeId::BINANCE_SPOT, 998, 999, {{99.0, 1.0}}, {});

    auto result = synchronizer_->process_message(&old_delta);

    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::STALE);
    EXPECT_FALSE(result.top_of_book_changed);

    // Verify statistics
    auto stats = synchronizer_->get_stats();
    EXPECT_EQ(stats.stale_messages, 1);
}

TEST_F(OrderBookSynchronizerTest, BinanceOverlappingSequences) {
    // Binance special case: can apply delta if sequence range overlaps
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Delta with overlapping range (998-1002 overlaps with expected 1001)
    auto delta = create_delta_message(ExchangeId::BINANCE_SPOT,
                                      998,   // first_update_id before expected
                                      1002,  // last_update_id after expected
                                      {{100.5, 2.0}},
                                      {});

    auto result = synchronizer_->process_message(&delta);

    EXPECT_EQ(result.status, OrderBookSynchronizer::ProcessStatus::OK);
    EXPECT_TRUE(result.top_of_book_changed);
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1002);
}

TEST_F(OrderBookSynchronizerTest, KuCoinSequenceValidation) {
    // KuCoin uses strict sequence increment
    auto snapshot = create_snapshot_message(ExchangeId::KUCOIN_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Valid delta (sequence_start = 1001)
    auto delta1 = create_delta_message(ExchangeId::KUCOIN_SPOT,
                                       1001,  // sequence_start
                                       1005,  // sequence_end
                                       {{100.5, 2.0}},
                                       {});

    auto result1 = synchronizer_->process_message(&delta1);
    EXPECT_EQ(result1.status, OrderBookSynchronizer::ProcessStatus::OK);
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1005);

    // Invalid delta (gap in sequence)
    auto delta2 = create_delta_message(ExchangeId::KUCOIN_SPOT,
                                       1007,  // Gap! Expected 1006
                                       1010,
                                       {{100.6, 3.0}},
                                       {});

    auto result2 = synchronizer_->process_message(&delta2);
    EXPECT_EQ(result2.status, OrderBookSynchronizer::ProcessStatus::NEEDS_SNAPSHOT);
    EXPECT_TRUE(synchronizer_->needs_snapshot());
}

TEST_F(OrderBookSynchronizerTest, OKXSequenceValidation) {
    // OKX uses previous_update_id matching
    auto snapshot = create_snapshot_message(ExchangeId::OKX_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // Valid delta (previous_update_id = 1000)
    auto delta1 = create_delta_message(ExchangeId::OKX_SPOT,
                                       1000,  // previous_update_id
                                       1001,  // sequence_number
                                       {{100.5, 2.0}},
                                       {});

    auto result1 = synchronizer_->process_message(&delta1);
    EXPECT_EQ(result1.status, OrderBookSynchronizer::ProcessStatus::OK);
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1001);

    // Invalid delta (wrong previous_update_id)
    auto delta2 = create_delta_message(ExchangeId::OKX_SPOT,
                                       999,  // Wrong! Expected 1001
                                       1002,
                                       {{100.6, 3.0}},
                                       {});

    auto result2 = synchronizer_->process_message(&delta2);
    EXPECT_EQ(result2.status, OrderBookSynchronizer::ProcessStatus::NEEDS_SNAPSHOT);
}

TEST_F(OrderBookSynchronizerTest, Reset) {
    // Apply snapshot and delta
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    auto delta = create_delta_message(ExchangeId::BINANCE_SPOT, 1001, 1002, {{100.5, 2.0}}, {});
    synchronizer_->process_message(&delta);

    EXPECT_TRUE(synchronizer_->is_synchronized());
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1002);

    // Reset
    synchronizer_->reset();

    EXPECT_TRUE(synchronizer_->needs_snapshot());
    EXPECT_FALSE(synchronizer_->is_synchronized());
    EXPECT_EQ(synchronizer_->get_last_sequence(), 0);
}

TEST_F(OrderBookSynchronizerTest, MultipleInstrumentsIndependence) {
    // This test verifies that multiple synchronizers can work independently
    core::InstrumentId instrument2{core::ExchangeId::BINANCE_SPOT, "ETHUSDT"};
    core::OrderBook book2(instrument2, *pool_);
    OrderBookSynchronizer sync2(book2);

    // Synchronize first book
    auto snapshot1 = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot1);

    // Synchronize second book with different sequence
    auto snapshot2 = create_snapshot_message(ExchangeId::BINANCE_SPOT, 2000, {{200.0, 2.0}}, {{201.0, 2.5}});
    sync2.process_message(&snapshot2);

    // Verify independence
    EXPECT_EQ(synchronizer_->get_last_sequence(), 1000);
    EXPECT_EQ(sync2.get_last_sequence(), 2000);

    auto top1 = order_book_->top_of_book();
    auto top2 = book2.top_of_book();

    EXPECT_EQ(top1.bid_price, Price::from_string(std::to_string(100.0)));
    EXPECT_EQ(top2.bid_price, Price::from_string(std::to_string(200.0)));
}

TEST_F(OrderBookSynchronizerTest, StatisticsAccumulation) {
    // Perform various operations

    // 1. Apply snapshot
    auto snapshot = create_snapshot_message(ExchangeId::BINANCE_SPOT, 1000, {{100.0, 1.0}}, {{101.0, 1.5}});
    synchronizer_->process_message(&snapshot);

    // 2. Apply valid delta
    auto delta1 = create_delta_message(ExchangeId::BINANCE_SPOT, 1001, 1002, {{100.5, 2.0}}, {});
    synchronizer_->process_message(&delta1);

    // 3. Apply stale message
    auto stale = create_delta_message(ExchangeId::BINANCE_SPOT, 999, 1000, {{99.0, 1.0}}, {});
    synchronizer_->process_message(&stale);

    // 4. Create a gap
    auto delta_with_gap = create_delta_message(ExchangeId::BINANCE_SPOT,
                                               1100,  // Gap!
                                               1101,
                                               {{99.0, 1.0}},
                                               {});
    synchronizer_->process_message(&delta_with_gap);

    // Check statistics
    auto stats = synchronizer_->get_stats();
    EXPECT_EQ(stats.snapshots_processed, 1);
    EXPECT_EQ(stats.deltas_processed, 1);
    EXPECT_EQ(stats.stale_messages, 1);
    EXPECT_EQ(stats.gaps_detected, 1);
}