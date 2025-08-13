#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "../../src/core/memory_pool.hpp"
#include "../../src/core/normalized_message.hpp"
#include "../../src/core/spsc_ring.hpp"
#include "../../src/orderbook/order_book_manager.hpp"

using namespace crypto_lob::orderbook;
using namespace crypto_lob::core;

class OrderBookManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Create pools and queues
        message_pool = std::make_unique<MemoryPool<NormalizedMessage>>(1024);
        book_pool = std::make_unique<MemoryPool<PriceLevelNode>>(8192);
        message_queue = std::make_unique<SPSCRing<NormalizedMessage*>>(256);

        // Create manager with default config
        config = OrderBookManager::Config{
            .initial_book_capacity = 1024, .top_n_depth = 10, .max_instruments = 64, .enable_sequence_checking = true};

        manager = std::make_unique<OrderBookManager>(config, *message_queue, *message_pool, *book_pool);

        // Create test instruments
        btc_usd = InstrumentId(ExchangeId::BINANCE_SPOT, "BTCUSDT");
        eth_usd = InstrumentId(ExchangeId::BINANCE_SPOT, "ETHUSDT");
    }

    // Helper to create and enqueue a message
    void enqueue_snapshot(const InstrumentId& instrument,
                          uint64_t last_update_id,
                          const std::vector<MessagePriceLevel>& bids,
                          const std::vector<MessagePriceLevel>& asks) {
        auto* msg = message_pool->allocate();
        ASSERT_NE(msg, nullptr);

        msg->type = MessageType::SNAPSHOT;
        msg->exchange_id = ExchangeId::BINANCE_SPOT;
        msg->instrument = instrument;
        msg->receive_time = rdtsc();

        auto& snapshot = msg->data.emplace<SnapshotMessage>();
        snapshot.last_update_id = last_update_id;
        snapshot.bid_count = std::min(bids.size(), size_t(20));
        snapshot.ask_count = std::min(asks.size(), size_t(20));

        for (size_t i = 0; i < snapshot.bid_count; ++i) {
            snapshot.bids[i] = bids[i];
        }
        for (size_t i = 0; i < snapshot.ask_count; ++i) {
            snapshot.asks[i] = asks[i];
        }

        ASSERT_TRUE(message_queue->try_push(msg));
    }

    void enqueue_delta(const InstrumentId& instrument,
                       uint64_t first_update_id,
                       uint64_t last_update_id,
                       const std::vector<MessagePriceLevel>& bid_updates,
                       const std::vector<MessagePriceLevel>& ask_updates) {
        auto* msg = message_pool->allocate();
        ASSERT_NE(msg, nullptr);

        msg->type = MessageType::DELTA;
        msg->exchange_id = ExchangeId::BINANCE_SPOT;
        msg->instrument = instrument;
        msg->receive_time = rdtsc();

        auto& delta = msg->data.emplace<DeltaMessage>();
        delta.first_update_id = first_update_id;
        delta.last_update_id = last_update_id;
        delta.bid_update_count = std::min(bid_updates.size(), size_t(100));
        delta.ask_update_count = std::min(ask_updates.size(), size_t(100));

        for (size_t i = 0; i < delta.bid_update_count; ++i) {
            delta.bid_updates[i] = bid_updates[i];
        }
        for (size_t i = 0; i < delta.ask_update_count; ++i) {
            delta.ask_updates[i] = ask_updates[i];
        }

        ASSERT_TRUE(message_queue->try_push(msg));
    }

    void enqueue_trade(const InstrumentId& instrument, Price price, uint64_t quantity, bool is_buyer_maker) {
        auto* msg = message_pool->allocate();
        ASSERT_NE(msg, nullptr);

        msg->type = MessageType::TRADE;
        msg->exchange_id = ExchangeId::BINANCE_SPOT;
        msg->instrument = instrument;
        msg->receive_time = rdtsc();

        auto& trade = msg->data.emplace<TradeMessage>();
        trade.trade_id = 12345;
        trade.price = price;
        trade.quantity = Quantity(quantity);
        trade.timestamp = rdtsc();
        trade.is_buyer_maker = is_buyer_maker;

        ASSERT_TRUE(message_queue->try_push(msg));
    }

    OrderBookManager::Config config;
    std::unique_ptr<MemoryPool<NormalizedMessage>> message_pool;
    std::unique_ptr<MemoryPool<PriceLevelNode>> book_pool;
    std::unique_ptr<SPSCRing<NormalizedMessage*>> message_queue;
    std::unique_ptr<OrderBookManager> manager;

    InstrumentId btc_usd;
    InstrumentId eth_usd;
};

TEST_F(OrderBookManagerTest, AddInstrument) {
    // Add instruments
    manager->add_instrument(btc_usd);
    manager->add_instrument(eth_usd);

    // Verify instruments are registered
    auto instruments = manager->get_instruments();
    EXPECT_EQ(instruments.size(), 2);

    // Verify order books exist
    EXPECT_NE(manager->get_order_book(btc_usd), nullptr);
    EXPECT_NE(manager->get_order_book(eth_usd), nullptr);

    // Unknown instrument should return nullptr
    auto unknown = InstrumentId(ExchangeId::BINANCE_SPOT, "UNKNOWN");
    EXPECT_EQ(manager->get_order_book(unknown), nullptr);
}

TEST_F(OrderBookManagerTest, ProcessSnapshot) {
    manager->add_instrument(btc_usd);

    // Create snapshot data
    std::vector<MessagePriceLevel> bids = {
        {Price{50000'00000000}, Quantity(100)},  // $50,000
        {Price{49999'00000000}, Quantity(200)},  // $49,999
        {Price{49998'00000000}, Quantity(150)}   // $49,998
    };

    std::vector<MessagePriceLevel> asks = {
        {Price{50001'00000000}, Quantity(80)},   // $50,001
        {Price{50002'00000000}, Quantity(120)},  // $50,002
        {Price{50003'00000000}, Quantity(90)}    // $50,003
    };

    // Enqueue snapshot
    enqueue_snapshot(btc_usd, 1000, bids, asks);

    // Process message
    EXPECT_TRUE(manager->process_one());

    // Verify statistics
    EXPECT_EQ(manager->stats().messages_processed, 1);
    EXPECT_EQ(manager->stats().snapshots_applied, 1);

    // Verify order book state
    auto* book = manager->get_order_book(btc_usd);
    ASSERT_NE(book, nullptr);

    auto tob = book->top_of_book();
    EXPECT_EQ(tob.bid_price, Price{50000'00000000});
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price, Price{50001'00000000});
    EXPECT_EQ(tob.ask_qty, 80);

    // Verify sequence state
    auto* seq_state = manager->get_sequence_state(btc_usd);
    ASSERT_NE(seq_state, nullptr);
    EXPECT_EQ(seq_state->last_update_id, 1000);
    EXPECT_FALSE(seq_state->awaiting_snapshot);
}

TEST_F(OrderBookManagerTest, ProcessDelta) {
    manager->add_instrument(btc_usd);

    // First need a snapshot
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);
    EXPECT_TRUE(manager->process_one());

    // Create delta updates
    std::vector<MessagePriceLevel> bid_updates = {
        {Price{50000'00000000}, Quantity(150)}  // Update bid
    };
    std::vector<MessagePriceLevel> ask_updates = {
        {Price{50001'00000000}, Quantity(0)},   // Remove ask
        {Price{50002'00000000}, Quantity(100)}  // Add new ask
    };

    // Enqueue delta with correct sequence
    enqueue_delta(btc_usd, 1001, 1003, bid_updates, ask_updates);

    // Process delta
    EXPECT_TRUE(manager->process_one());

    // Verify statistics
    EXPECT_EQ(manager->stats().messages_processed, 2);
    EXPECT_EQ(manager->stats().deltas_applied, 1);

    // Verify order book state
    auto* book = manager->get_order_book(btc_usd);
    auto tob = book->top_of_book();
    EXPECT_EQ(tob.bid_price, Price{50000'00000000});
    EXPECT_EQ(tob.bid_qty, 150);                      // Updated quantity
    EXPECT_EQ(tob.ask_price, Price{50002'00000000});  // New best ask
    EXPECT_EQ(tob.ask_qty, 100);

    // Verify sequence state
    auto* seq_state = manager->get_sequence_state(btc_usd);
    EXPECT_EQ(seq_state->last_update_id, 1003);
}

TEST_F(OrderBookManagerTest, SequenceGapDetection) {
    manager->add_instrument(btc_usd);

    // Apply snapshot
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);
    EXPECT_TRUE(manager->process_one());

    // Enqueue delta with gap (should be 1001 but is 1005)
    std::vector<MessagePriceLevel> bid_updates = {{Price{50000'00000000}, Quantity(200)}};
    std::vector<MessagePriceLevel> ask_updates = {};
    enqueue_delta(btc_usd, 1005, 1005, bid_updates, ask_updates);

    // Process - should detect gap and drop
    EXPECT_TRUE(manager->process_one());

    // Verify gap detected
    EXPECT_EQ(manager->stats().sequence_gaps, 1);
    auto* seq_state = manager->get_sequence_state(btc_usd);
    EXPECT_EQ(seq_state->gaps_detected, 1);
    EXPECT_TRUE(seq_state->awaiting_snapshot);  // Should need new snapshot

    // Order book should not be updated
    auto* book = manager->get_order_book(btc_usd);
    auto tob = book->top_of_book();
    EXPECT_EQ(tob.bid_qty, 100);  // Unchanged
}

TEST_F(OrderBookManagerTest, SkipOldUpdates) {
    manager->add_instrument(btc_usd);

    // Apply snapshot
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);
    EXPECT_TRUE(manager->process_one());

    // Apply newer delta
    std::vector<MessagePriceLevel> bid_updates1 = {{Price{50000'00000000}, Quantity(150)}};
    std::vector<MessagePriceLevel> ask_updates1 = {};
    enqueue_delta(btc_usd, 1001, 1001, bid_updates1, ask_updates1);
    EXPECT_TRUE(manager->process_one());

    // Try to apply old delta (should be skipped)
    std::vector<MessagePriceLevel> bid_updates2 = {{Price{50000'00000000}, Quantity(200)}};
    std::vector<MessagePriceLevel> ask_updates2 = {};
    enqueue_delta(btc_usd, 999, 1000, bid_updates2, ask_updates2);
    EXPECT_TRUE(manager->process_one());

    // Verify old update was skipped
    auto* book = manager->get_order_book(btc_usd);
    auto tob = book->top_of_book();
    EXPECT_EQ(tob.bid_qty, 150);  // Should still be 150, not 200
}

TEST_F(OrderBookManagerTest, ProcessTrade) {
    manager->add_instrument(btc_usd);

    // Enqueue trade
    enqueue_trade(btc_usd, Price{50000'00000000}, 10, true);

    // Process trade
    EXPECT_TRUE(manager->process_one());

    // Verify statistics
    EXPECT_EQ(manager->stats().messages_processed, 1);
    EXPECT_EQ(manager->stats().trades_processed, 1);
}

TEST_F(OrderBookManagerTest, UnknownInstrument) {
    // Don't add btc_usd instrument

    // Enqueue message for unknown instrument
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);

    // Process - should count as unknown instrument
    EXPECT_TRUE(manager->process_one());

    // Verify statistics
    EXPECT_EQ(manager->stats().unknown_instruments, 1);
    EXPECT_EQ(manager->stats().messages_processed, 0);  // Not counted as processed
}

TEST_F(OrderBookManagerTest, MultipleInstruments) {
    manager->add_instrument(btc_usd);
    manager->add_instrument(eth_usd);

    // Apply snapshots to both instruments
    std::vector<MessagePriceLevel> btc_bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> btc_asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, btc_bids, btc_asks);

    std::vector<MessagePriceLevel> eth_bids = {{Price{3000'00000000}, Quantity(500)}};
    std::vector<MessagePriceLevel> eth_asks = {{Price{3001'00000000}, Quantity(400)}};
    enqueue_snapshot(eth_usd, 2000, eth_bids, eth_asks);

    // Process both
    EXPECT_TRUE(manager->process_one());
    EXPECT_TRUE(manager->process_one());

    // Verify both books updated
    auto* btc_book = manager->get_order_book(btc_usd);
    auto* eth_book = manager->get_order_book(eth_usd);

    ASSERT_NE(btc_book, nullptr);
    ASSERT_NE(eth_book, nullptr);

    auto btc_tob = btc_book->top_of_book();
    EXPECT_EQ(btc_tob.bid_price, Price{50000'00000000});

    auto eth_tob = eth_book->top_of_book();
    EXPECT_EQ(eth_tob.bid_price, Price{3000'00000000});

    // Verify statistics
    EXPECT_EQ(manager->stats().messages_processed, 2);
    EXPECT_EQ(manager->stats().snapshots_applied, 2);
}

TEST_F(OrderBookManagerTest, BBOChangeDetection) {
    manager->add_instrument(btc_usd);

    // Apply initial snapshot
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)},
                                           {Price{49999'00000000}, Quantity(200)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)},
                                           {Price{50002'00000000}, Quantity(120)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);
    EXPECT_TRUE(manager->process_one());

    // Update that changes BBO
    std::vector<MessagePriceLevel> bid_updates1 = {{Price{50001'00000000}, Quantity(150)}};  // New best bid
    std::vector<MessagePriceLevel> ask_updates1 = {};
    enqueue_delta(btc_usd, 1001, 1001, bid_updates1, ask_updates1);
    EXPECT_TRUE(manager->process_one());

    // Update that doesn't change BBO (deeper level)
    std::vector<MessagePriceLevel> bid_updates2 = {{Price{49999'00000000}, Quantity(250)}};
    std::vector<MessagePriceLevel> ask_updates2 = {};
    enqueue_delta(btc_usd, 1002, 1002, bid_updates2, ask_updates2);
    EXPECT_TRUE(manager->process_one());

    // Verify BBO change count (snapshot + first delta)
    EXPECT_EQ(manager->stats().bbo_changes, 2);
}

TEST_F(OrderBookManagerTest, RunLoop) {
    manager->add_instrument(btc_usd);

    // Start manager in separate thread
    std::thread manager_thread([this]() { manager->run(); });

    // Give it time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Enqueue messages
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);

    // Give time to process
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Stop manager
    manager->stop();
    manager_thread.join();

    // Verify message was processed
    EXPECT_EQ(manager->stats().messages_processed, 1);
    EXPECT_EQ(manager->stats().snapshots_applied, 1);
}

TEST_F(OrderBookManagerTest, DisableSequenceChecking) {
    // Create manager with sequence checking disabled
    config.enable_sequence_checking = false;
    manager = std::make_unique<OrderBookManager>(config, *message_queue, *message_pool, *book_pool);

    manager->add_instrument(btc_usd);

    // Apply snapshot
    std::vector<MessagePriceLevel> bids = {{Price{50000'00000000}, Quantity(100)}};
    std::vector<MessagePriceLevel> asks = {{Price{50001'00000000}, Quantity(80)}};
    enqueue_snapshot(btc_usd, 1000, bids, asks);
    EXPECT_TRUE(manager->process_one());

    // Apply delta with gap (should still process when checking disabled)
    std::vector<MessagePriceLevel> bid_updates = {{Price{50000'00000000}, Quantity(200)}};
    std::vector<MessagePriceLevel> ask_updates = {};
    enqueue_delta(btc_usd, 1005, 1005, bid_updates, ask_updates);
    EXPECT_TRUE(manager->process_one());

    // Verify update was applied (no gap detection)
    EXPECT_EQ(manager->stats().sequence_gaps, 0);
    EXPECT_EQ(manager->stats().deltas_applied, 1);

    auto* book = manager->get_order_book(btc_usd);
    auto tob = book->top_of_book();
    EXPECT_EQ(tob.bid_qty, 200);  // Updated
}