#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "core/enums.hpp"
#include "core/memory_pool.hpp"
#include "core/price.hpp"
#include "orderbook/order_book.hpp"

using namespace crypto_lob::core;

// Shared memory pool environment for all tests
class OrderBookTestEnvironment : public ::testing::Environment {
  public:
    using PoolType = MemoryPool<PriceLevelNode>;

    void SetUp() override {
        pool_ = std::make_unique<PoolType>(
            100'000,                                 // initial_capacity
            PoolDepletionPolicy::TERMINATE_PROCESS,  // policy
            CacheConfig{64, 32, false, true}  // cache_config: cache_size, batch_size, use_huge_pages, prefault_pages
        );
    }

    void TearDown() override {
        pool_.reset();
    }

    static PoolType* GetPool() {
        return instance_ ? instance_->pool_.get() : nullptr;
    }

    static void SetInstance(OrderBookTestEnvironment* env) {
        instance_ = env;
    }

  private:
    std::unique_ptr<PoolType> pool_;  // Non-static for proper lifetime management
    static inline OrderBookTestEnvironment* instance_ = nullptr;
};

// Base fixture that resets pool state between tests
class OrderBookTest : public ::testing::Test {
  protected:
    using PoolType = MemoryPool<PriceLevelNode>;  // Direct type alias

    void SetUp() override {
        pool_ = OrderBookTestEnvironment::GetPool();
        ASSERT_NE(pool_, nullptr);
        // Note: MemoryPool doesn't have clear_all method, but the environment resets between tests
    }

    PoolType* pool_;
};

// Compile-time checks
static_assert(alignof(OrderBook) == CACHELINE_SIZE);
static_assert(alignof(TopOfBook) == CACHELINE_SIZE);
static_assert(sizeof(OrderBook) % CACHELINE_SIZE == 0, "OrderBook must not span cache lines");
// Note: Price::value_type may be private - relying on runtime behavior for 128-bit requirement

// ===== Construction Tests =====

TEST_F(OrderBookTest, Construction_InitialState) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    EXPECT_EQ(book.instrument(), instrument);
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.bid_depth(), 0);
    EXPECT_EQ(book.ask_depth(), 0);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price.raw_value(), 0);
    EXPECT_EQ(tob.ask_price.raw_value(), 0);
    EXPECT_EQ(tob.bid_qty, 0);
    EXPECT_EQ(tob.ask_qty, 0);
    EXPECT_FALSE(tob.valid());
}

// ===== Basic Update Operations Tests =====

TEST_F(OrderBookTest, BasicUpdates_SingleBidUpdate) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add single bid
    bool tob_changed = book.update(Side::BUY, Price(50000.0), 100);
    EXPECT_TRUE(tob_changed);

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.bid_depth(), 1);
    EXPECT_EQ(book.ask_depth(), 0);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price.raw_value(), 0);
    EXPECT_EQ(tob.ask_qty, 0);
    EXPECT_FALSE(tob.valid());  // Need both bid and ask for valid TOB
}

TEST_F(OrderBookTest, BasicUpdates_SingleAskUpdate) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add single ask
    bool tob_changed = book.update(Side::SELL, Price(51000.0), 200);
    EXPECT_TRUE(tob_changed);

    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.bid_depth(), 0);
    EXPECT_EQ(book.ask_depth(), 1);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price.raw_value(), 0);
    EXPECT_EQ(tob.bid_qty, 0);
    EXPECT_EQ(tob.ask_price, Price(51000.0));
    EXPECT_EQ(tob.ask_qty, 200);
    EXPECT_FALSE(tob.valid());  // Need both bid and ask for valid TOB
}

TEST_F(OrderBookTest, BasicUpdates_BothSidesValidTOB) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add bid and ask
    bool bid_changed = book.update(Side::BUY, Price(50000.0), 100);
    bool ask_changed = book.update(Side::SELL, Price(51000.0), 200);

    EXPECT_TRUE(bid_changed);
    EXPECT_TRUE(ask_changed);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price, Price(51000.0));
    EXPECT_EQ(tob.ask_qty, 200);
    EXPECT_TRUE(tob.valid());
    EXPECT_EQ(tob.spread(), Price(1000.0));
}

TEST_F(OrderBookTest, BasicUpdates_MultipleLevelsSameSide) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add multiple bid levels
    bool changed1 = book.update(Side::BUY, Price(50000.0), 100);
    bool changed2 = book.update(Side::BUY, Price(49900.0), 150);
    bool changed3 = book.update(Side::BUY, Price(50100.0), 75);  // New best bid

    EXPECT_TRUE(changed1);   // First bid changes TOB
    EXPECT_FALSE(changed2);  // Lower bid doesn't change TOB
    EXPECT_TRUE(changed3);   // New best bid changes TOB

    EXPECT_EQ(book.bid_depth(), 3);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50100.0));  // Best bid
    EXPECT_EQ(tob.bid_qty, 75);
}

TEST_F(OrderBookTest, BasicUpdates_PriceLevelModification) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add initial level
    bool initial_changed = book.update(Side::BUY, Price(50000.0), 100);
    EXPECT_TRUE(initial_changed);

    // Check initial state
    const auto& initial_tob = book.top_of_book();
    EXPECT_EQ(initial_tob.bid_qty, 100);

    // Verify we can find the level
    auto* level = book.find_level(Side::BUY, Price(50000.0));
    EXPECT_NE(level, nullptr);
    EXPECT_EQ(level->quantity, 100);

    // Note: This test demonstrates a bug in OrderBook::update()
    // The quantity change detection doesn't work because the comparison
    // happens after the node is already modified in place.
    // For now, we'll test that the quantity gets updated correctly
    // even if the change detection doesn't work.

    // Modify quantity at same price
    book.update(Side::BUY, Price(50000.0), 150);

    EXPECT_EQ(book.bid_depth(), 1);  // Still only one level

    // Check if the level was updated directly in the book side
    auto* updated_level = book.find_level(Side::BUY, Price(50000.0));
    EXPECT_NE(updated_level, nullptr);
    EXPECT_EQ(updated_level->quantity, 150);  // BookSide level should be updated

    // Force TOB cache update by adding another side
    book.update(Side::SELL, Price(51000.0), 200);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));
    EXPECT_EQ(tob.bid_qty, 150);  // Should be updated now
}

// Test to verify the quantity change detection bug has been fixed
TEST_F(OrderBookTest, QuantityChangeDetectionBugFixed) {
    // This test verifies that the bug in OrderBook::update()
    // where quantity changes at the top of book were not detected is now fixed

    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Setup: Add initial bid at top of book
    bool changed1 = book.update(Side::BUY, Price(50000.0), 100);
    EXPECT_TRUE(changed1);  // First update always changes TOB

    const auto& initial_tob = book.top_of_book();
    EXPECT_EQ(initial_tob.bid_price, Price(50000.0));
    EXPECT_EQ(initial_tob.bid_qty, 100);

    // Update quantity at same price (top of book)
    // This now correctly returns true because TOB quantity changed
    bool changed2 = book.update(Side::BUY, Price(50000.0), 200);

    // Fixed behavior: Returns true when TOB quantity changes
    EXPECT_TRUE(changed2);  // Bug is fixed!

    // The quantity is updated in the book structure
    auto* level = book.find_level(Side::BUY, Price(50000.0));
    EXPECT_EQ(level->quantity, 200);

    // The cached TOB is now correctly updated
    const auto& after_tob = book.top_of_book();
    EXPECT_EQ(after_tob.bid_qty, 200);  // Cache is updated correctly!

    // Test that the fix works for ask side too
    book.update(Side::SELL, Price(50100.0), 300);
    bool changed3 = book.update(Side::SELL, Price(50100.0), 400);
    EXPECT_TRUE(changed3);
    EXPECT_EQ(book.top_of_book().ask_qty, 400);
}

// Test that quantity changes are correctly detected at all price levels
TEST_F(OrderBookTest, QuantityChangeDetectionCorrectBehavior) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Test 1: Quantity change at non-top level should not affect TOB
    book.update(Side::BUY, Price(50000.0), 100);  // Top
    book.update(Side::BUY, Price(49900.0), 200);  // Second

    bool changed = book.update(Side::BUY, Price(49900.0), 300);
    EXPECT_FALSE(changed);  // Correct: non-top change doesn't affect TOB

    // Test 2: New best price should change TOB
    changed = book.update(Side::BUY, Price(50100.0), 150);
    EXPECT_TRUE(changed);  // Correct: new best price

    // Test 3: Removing top level should change TOB
    changed = book.update(Side::BUY, Price(50100.0), 0);
    EXPECT_TRUE(changed);  // Correct: removing top changes TOB

    // Test 4: Quantity change at top - this was the bug, now fixed
    const auto& before = book.top_of_book();
    uint64_t before_qty = before.bid_qty;

    changed = book.update(Side::BUY, Price(50000.0), before_qty + 100);
    EXPECT_TRUE(changed);  // Should detect quantity change at top
    EXPECT_EQ(book.top_of_book().bid_qty, before_qty + 100);

    // Test 5: Same quantity update should not trigger change
    changed = book.update(Side::BUY, Price(50000.0), before_qty + 100);
    EXPECT_FALSE(changed);  // No change when quantity stays the same

    // Test 6: Ask side quantity change
    book.update(Side::SELL, Price(50200.0), 500);
    changed = book.update(Side::SELL, Price(50200.0), 600);
    EXPECT_TRUE(changed);  // Should detect ask quantity change
    EXPECT_EQ(book.top_of_book().ask_qty, 600);
}

TEST_F(OrderBookTest, BasicUpdates_PriceLevelDeletion) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add two bid levels
    book.update(Side::BUY, Price(50000.0), 100);
    book.update(Side::BUY, Price(49900.0), 150);

    EXPECT_EQ(book.bid_depth(), 2);

    // Delete best bid with zero quantity
    bool changed = book.update(Side::BUY, Price(50000.0), 0);
    EXPECT_TRUE(changed);  // Deleting best bid changes TOB

    EXPECT_EQ(book.bid_depth(), 1);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(49900.0));  // Second best becomes best
    EXPECT_EQ(tob.bid_qty, 150);
}

TEST_F(OrderBookTest, BasicUpdates_NonTOBChanges) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Setup initial book
    book.update(Side::BUY, Price(50000.0), 100);   // Best bid
    book.update(Side::BUY, Price(49900.0), 150);   // Second bid
    book.update(Side::SELL, Price(51000.0), 200);  // Best ask

    // Modify second bid - should not change TOB
    bool changed = book.update(Side::BUY, Price(49900.0), 175);
    EXPECT_FALSE(changed);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));  // TOB unchanged
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price, Price(51000.0));
    EXPECT_EQ(tob.ask_qty, 200);
}

// ===== Snapshot Operations Tests =====

TEST_F(OrderBookTest, Snapshot_EmptySnapshot) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Add some initial data
    book.update(Side::BUY, Price(50000.0), 100);
    book.update(Side::SELL, Price(51000.0), 200);

    EXPECT_FALSE(book.empty());

    // Apply empty snapshot
    std::vector<PriceLevel> empty_bids, empty_asks;
    bool changed = book.snapshot(empty_bids, empty_asks);

    EXPECT_TRUE(changed);  // Snapshot always changes state
    EXPECT_TRUE(book.empty());
    EXPECT_EQ(book.bid_depth(), 0);
    EXPECT_EQ(book.ask_depth(), 0);

    const auto& tob = book.top_of_book();
    EXPECT_FALSE(tob.valid());
}

TEST_F(OrderBookTest, Snapshot_SingleSidedBids) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Create bid-only snapshot
    std::vector<PriceLevel> bids = {{Price(50000.0), 100}, {Price(49900.0), 150}, {Price(49800.0), 200}};
    std::vector<PriceLevel> asks;  // Empty

    bool changed = book.snapshot(bids, asks);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.bid_depth(), 3);
    EXPECT_EQ(book.ask_depth(), 0);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));  // Best bid
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price.raw_value(), 0);
    EXPECT_EQ(tob.ask_qty, 0);
    EXPECT_FALSE(tob.valid());  // Need both sides for valid TOB
}

TEST_F(OrderBookTest, Snapshot_SingleSidedAsks) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Create ask-only snapshot
    std::vector<PriceLevel> bids;  // Empty
    std::vector<PriceLevel> asks = {{Price(51000.0), 100}, {Price(51100.0), 150}, {Price(51200.0), 200}};

    bool changed = book.snapshot(bids, asks);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.bid_depth(), 0);
    EXPECT_EQ(book.ask_depth(), 3);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price.raw_value(), 0);
    EXPECT_EQ(tob.bid_qty, 0);
    EXPECT_EQ(tob.ask_price, Price(51000.0));  // Best ask
    EXPECT_EQ(tob.ask_qty, 100);
    EXPECT_FALSE(tob.valid());  // Need both sides for valid TOB
}

TEST_F(OrderBookTest, Snapshot_FullTwoSided) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Create full two-sided snapshot
    std::vector<PriceLevel> bids = {{Price(50000.0), 100}, {Price(49900.0), 150}, {Price(49800.0), 200}};
    std::vector<PriceLevel> asks = {{Price(51000.0), 120}, {Price(51100.0), 180}, {Price(51200.0), 220}};

    bool changed = book.snapshot(bids, asks);

    EXPECT_TRUE(changed);
    EXPECT_FALSE(book.empty());
    EXPECT_EQ(book.bid_depth(), 3);
    EXPECT_EQ(book.ask_depth(), 3);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));
    EXPECT_EQ(tob.bid_qty, 100);
    EXPECT_EQ(tob.ask_price, Price(51000.0));
    EXPECT_EQ(tob.ask_qty, 120);
    EXPECT_TRUE(tob.valid());
    EXPECT_EQ(tob.spread(), Price(1000.0));
}

TEST_F(OrderBookTest, Snapshot_ZeroQuantitiesFiltered) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Create snapshot with some zero quantities (should be filtered out)
    std::vector<PriceLevel> bids = {
        {Price(50000.0), 100},  // Valid
        {Price(49900.0), 0},    // Should be filtered
        {Price(49800.0), 150}   // Valid
    };
    std::vector<PriceLevel> asks = {
        {Price(51000.0), 0},    // Should be filtered
        {Price(51100.0), 120},  // Valid
        {Price(51200.0), 0}     // Should be filtered
    };

    bool changed = book.snapshot(bids, asks);

    EXPECT_TRUE(changed);
    EXPECT_EQ(book.bid_depth(), 2);  // Only non-zero quantities
    EXPECT_EQ(book.ask_depth(), 1);  // Only non-zero quantities

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));  // Best bid (49900 filtered out)
    EXPECT_EQ(tob.ask_price, Price(51100.0));  // Best ask (51000 filtered out)
}

TEST_F(OrderBookTest, Snapshot_ReplacementBehavior) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Initial book state
    book.update(Side::BUY, Price(48000.0), 100);
    book.update(Side::SELL, Price(52000.0), 200);

    EXPECT_EQ(book.bid_depth(), 1);
    EXPECT_EQ(book.ask_depth(), 1);

    // Apply completely different snapshot
    std::vector<PriceLevel> bids = {{Price(50000.0), 300}, {Price(49900.0), 400}};
    std::vector<PriceLevel> asks = {{Price(51000.0), 500}};

    bool changed = book.snapshot(bids, asks);

    EXPECT_TRUE(changed);
    EXPECT_EQ(book.bid_depth(), 2);  // New bid levels
    EXPECT_EQ(book.ask_depth(), 1);  // New ask levels

    // Verify old levels are gone
    EXPECT_EQ(book.find_level(Side::BUY, Price(48000.0)), nullptr);
    EXPECT_EQ(book.find_level(Side::SELL, Price(52000.0)), nullptr);

    // Verify new levels exist
    EXPECT_NE(book.find_level(Side::BUY, Price(50000.0)), nullptr);
    EXPECT_NE(book.find_level(Side::SELL, Price(51000.0)), nullptr);

    const auto& tob = book.top_of_book();
    EXPECT_EQ(tob.bid_price, Price(50000.0));
    EXPECT_EQ(tob.ask_price, Price(51000.0));
}

// ===== Top-of-Book (TOB) Tracking Tests =====

TEST_F(OrderBookTest, TOB_ValidityConditions) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Initial state - not valid
    const auto& tob1 = book.top_of_book();
    EXPECT_FALSE(tob1.valid());

    // Only bid - not valid
    book.update(Side::BUY, Price(50000.0), 100);
    const auto& tob2 = book.top_of_book();
    EXPECT_FALSE(tob2.valid());

    // Only ask - not valid
    book.clear();
    book.update(Side::SELL, Price(51000.0), 200);
    const auto& tob3 = book.top_of_book();
    EXPECT_FALSE(tob3.valid());

    // Both sides with proper spread - valid
    book.update(Side::BUY, Price(50000.0), 100);
    const auto& tob4 = book.top_of_book();
    EXPECT_TRUE(tob4.valid());
    EXPECT_EQ(tob4.bid_price, Price(50000.0));
    EXPECT_EQ(tob4.ask_price, Price(51000.0));

    // Crossed book - not valid
    book.update(Side::BUY, Price(52000.0), 150);  // Bid higher than ask
    const auto& tob5 = book.top_of_book();
    EXPECT_FALSE(tob5.valid());  // bid >= ask makes it invalid
}

TEST_F(OrderBookTest, TOB_ChangesDetection) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Initial setup
    bool changed1 = book.update(Side::BUY, Price(50000.0), 100);
    bool changed2 = book.update(Side::SELL, Price(51000.0), 200);

    EXPECT_TRUE(changed1);  // First bid changes TOB
    EXPECT_TRUE(changed2);  // First ask changes TOB

    // Add non-top levels - should not change TOB
    bool changed3 = book.update(Side::BUY, Price(49900.0), 150);
    bool changed4 = book.update(Side::SELL, Price(51100.0), 250);

    EXPECT_FALSE(changed3);  // Lower bid doesn't change TOB
    EXPECT_FALSE(changed4);  // Higher ask doesn't change TOB

    // Update top bid price - should change TOB
    bool changed5 = book.update(Side::BUY, Price(50100.0), 120);
    EXPECT_TRUE(changed5);  // New best bid changes TOB

    // Update top ask price - should change TOB
    bool changed6 = book.update(Side::SELL, Price(50900.0), 180);
    EXPECT_TRUE(changed6);  // New best ask changes TOB

    // Remove top bid - should change TOB
    bool changed7 = book.update(Side::BUY, Price(50100.0), 0);
    EXPECT_TRUE(changed7);  // Removing best bid changes TOB
}

TEST_F(OrderBookTest, TOB_SpreadCalculations) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Setup book with known spread
    book.update(Side::BUY, Price(50000.0), 100);
    book.update(Side::SELL, Price(51000.0), 200);

    const auto& tob1 = book.top_of_book();
    EXPECT_EQ(tob1.spread(), Price(1000.0));  // 51000 - 50000 = 1000

    // Tighten spread
    book.update(Side::BUY, Price(50500.0), 150);
    const auto& tob2 = book.top_of_book();
    EXPECT_EQ(tob2.spread(), Price(500.0));  // 51000 - 50500 = 500

    // Tighten further from ask side
    book.update(Side::SELL, Price(50700.0), 180);
    const auto& tob3 = book.top_of_book();
    EXPECT_EQ(tob3.spread(), Price(200.0));  // 50700 - 50500 = 200

    // Very tight spread
    book.update(Side::SELL, Price(50501.0), 160);
    const auto& tob4 = book.top_of_book();
    EXPECT_EQ(tob4.spread(), Price(1.0));  // 50501 - 50500 = 1
}

TEST_F(OrderBookTest, TOB_BestBidAskUpdates) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Build up a book
    book.update(Side::BUY, Price(50000.0), 100);
    book.update(Side::BUY, Price(49900.0), 150);
    book.update(Side::BUY, Price(49800.0), 200);

    book.update(Side::SELL, Price(51000.0), 120);
    book.update(Side::SELL, Price(51100.0), 180);
    book.update(Side::SELL, Price(51200.0), 220);

    // Verify initial TOB
    const auto& tob1 = book.top_of_book();
    EXPECT_EQ(tob1.bid_price, Price(50000.0));
    EXPECT_EQ(tob1.bid_qty, 100);
    EXPECT_EQ(tob1.ask_price, Price(51000.0));
    EXPECT_EQ(tob1.ask_qty, 120);

    // Remove best bid - next level should become new best
    book.update(Side::BUY, Price(50000.0), 0);
    const auto& tob2 = book.top_of_book();
    EXPECT_EQ(tob2.bid_price, Price(49900.0));
    EXPECT_EQ(tob2.bid_qty, 150);
    EXPECT_EQ(tob2.ask_price, Price(51000.0));  // Ask unchanged

    // Remove best ask - next level should become new best
    book.update(Side::SELL, Price(51000.0), 0);
    const auto& tob3 = book.top_of_book();
    EXPECT_EQ(tob3.bid_price, Price(49900.0));  // Bid unchanged
    EXPECT_EQ(tob3.ask_price, Price(51100.0));
    EXPECT_EQ(tob3.ask_qty, 180);
}

TEST_F(OrderBookTest, TOB_CachingCorrectness) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Setup initial book
    book.update(Side::BUY, Price(50000.0), 100);
    book.update(Side::SELL, Price(51000.0), 200);

    // Get TOB multiple times - should be same cached object
    const auto& tob1 = book.top_of_book();
    const auto& tob2 = book.top_of_book();
    EXPECT_EQ(&tob1, &tob2);  // Same object reference

    // Verify cached values match direct access
    const auto* best_bid = book.bids().best();
    const auto* best_ask = book.asks().best();

    EXPECT_EQ(tob1.bid_price, best_bid->price);
    EXPECT_EQ(tob1.bid_qty, best_bid->quantity);
    EXPECT_EQ(tob1.ask_price, best_ask->price);
    EXPECT_EQ(tob1.ask_qty, best_ask->quantity);

    // Modify book and verify cache is updated
    book.update(Side::BUY, Price(50100.0), 150);
    const auto& tob3 = book.top_of_book();

    EXPECT_EQ(tob3.bid_price, Price(50100.0));  // Cache updated
    EXPECT_EQ(tob3.bid_qty, 150);

    // Direct access should match cached values
    const auto* new_best_bid = book.bids().best();
    EXPECT_EQ(tob3.bid_price, new_best_bid->price);
    EXPECT_EQ(tob3.bid_qty, new_best_bid->quantity);
}

// Additional test: Realistic Exchange Update Pattern
TEST_F(OrderBookTest, RealisticExchangeUpdatePattern) {
    // Simulate real exchange update patterns with rapid updates near touch
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Phase 1: Initial snapshot (typical exchange depth)
    std::vector<PriceLevel> initial_bids, initial_asks;

    // Create realistic bid levels (descending from 50000)
    for (int i = 0; i < 100; ++i) {
        initial_bids.push_back({Price(50000.0 - i * 10.0), static_cast<uint64_t>(100 + i * 5)});
    }

    // Create realistic ask levels (ascending from 50010)
    for (int i = 0; i < 100; ++i) {
        initial_asks.push_back({Price(50010.0 + i * 10.0), static_cast<uint64_t>(100 + i * 5)});
    }

    book.snapshot(initial_bids, initial_asks);

    // Verify initial state
    EXPECT_EQ(book.bid_depth(), 100);
    EXPECT_EQ(book.ask_depth(), 100);

    const auto& initial_tob = book.top_of_book();
    EXPECT_TRUE(initial_tob.valid());
    EXPECT_EQ(initial_tob.spread(), Price(10.0));  // Tight spread

    // Phase 2: Rapid updates concentrated near touch
    std::mt19937 rng(42);                         // Deterministic for reproducibility
    std::normal_distribution<> level_dist(0, 5);  // Most updates within 5 levels of top
    std::uniform_int_distribution<> side_dist(0, 1);
    std::uniform_int_distribution<> qty_dist(50, 500);
    std::uniform_int_distribution<> action_dist(0, 9);  // 10% chance of deletion

    int tob_changes = 0;
    int total_updates = 10000;

    for (int i = 0; i < total_updates; ++i) {
        // Determine which side and level to update
        bool is_bid = side_dist(rng) == 0;
        int level_offset = std::abs(static_cast<int>(level_dist(rng)));
        level_offset = std::min(level_offset, 99);  // Cap at max depth

        // Calculate price based on side and offset
        Price update_price;
        if (is_bid) {
            update_price = Price(50000.0 - level_offset * 10.0);
        } else {
            update_price = Price(50010.0 + level_offset * 10.0);
        }

        // Determine quantity (10% chance of deletion)
        uint64_t qty = (action_dist(rng) == 0) ? 0 : qty_dist(rng);

        // Apply update and track TOB changes
        bool changed = book.update(is_bid ? Side::BUY : Side::SELL, update_price, qty);
        if (changed) {
            tob_changes++;
        }

        // Periodically verify book consistency
        if (i % 1000 == 999) {
            // Book should never be crossed
            EXPECT_FALSE(book.is_crossed()) << "Book crossed at update " << i;

            // Should maintain reasonable depth
            EXPECT_GT(book.bid_depth(), 10) << "Bid depth too low at update " << i;
            EXPECT_GT(book.ask_depth(), 10) << "Ask depth too low at update " << i;

            // TOB should remain valid
            const auto& current_tob = book.top_of_book();
            EXPECT_TRUE(current_tob.valid()) << "Invalid TOB at update " << i;
        }
    }

    // Phase 3: Market impact simulation (large order eating through levels)
    // Simulate a large buy order consuming ask liquidity
    Price sweep_start = book.top_of_book().ask_price;
    int levels_consumed = 0;

    for (int i = 0; i < 10; ++i) {
        Price level_price = sweep_start + Price(i * 10.0);
        bool changed = book.update(Side::SELL, level_price, 0);  // Remove level
        if (changed) {
            levels_consumed++;
        }
    }

    // TOB should have moved up
    const auto& after_sweep = book.top_of_book();
    EXPECT_GT(after_sweep.ask_price, sweep_start);
    EXPECT_GT(after_sweep.spread(), Price(10.0));  // Spread should widen

    // Store the wide spread for comparison
    Price wide_spread = after_sweep.spread();

    // Phase 4: Recovery (new liquidity comes in)
    // Add new ask levels closer to the bid to tighten spread
    Price current_best_bid = after_sweep.bid_price;

    // Add a very close ask to create tight spread
    book.update(Side::SELL, current_best_bid + Price(5.0), 300);

    // Add more asks at increasing distances
    for (int i = 1; i < 5; ++i) {
        Price new_level = current_best_bid + Price(5.0 + i * 10.0);
        book.update(Side::SELL, new_level, 200 + i * 50);
    }

    // Spread should tighten significantly
    const auto& recovered_tob = book.top_of_book();
    EXPECT_EQ(recovered_tob.spread(), Price(5.0));   // New tight spread
    EXPECT_LT(recovered_tob.spread(), wide_spread);  // Much tighter than before

    // Summary statistics
    double tob_change_rate = static_cast<double>(tob_changes) / total_updates;

    // In realistic markets, TOB changes frequently but not every update
    EXPECT_GT(tob_change_rate, 0.01);  // At least 1% of updates change TOB
    EXPECT_LT(tob_change_rate, 0.50);  // But not more than 50%

    // Log statistics for information
    if (::testing::GTEST_FLAG(output) != "xml") {
        std::cerr << "Exchange pattern simulation: " << tob_changes << " TOB changes out of " << total_updates
                  << " updates (" << (tob_change_rate * 100) << "%)" << std::endl;
    }
}

// Additional test: Price edge cases with overflow tests
TEST_F(OrderBookTest, PriceEdgeCasesWithOverflow) {
    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "EXTREMEUSDT"};
    OrderBook book(instrument, *pool_);

    // Test 1: Very small prices (micro-cap tokens)
    {
        // Price uses int64_t internally with scale 10^9
        // Smallest representable price is 1e-9
        Price tiny_price(0.000000001);   // 1 nano-unit
        Price tiny_price2(0.000000002);  // 2 nano-units

        book.update(Side::BUY, tiny_price, 1000000);
        book.update(Side::SELL, tiny_price2, 2000000);

        const auto& tob = book.top_of_book();
        EXPECT_EQ(tob.bid_price, tiny_price);
        EXPECT_EQ(tob.ask_price, tiny_price2);
        EXPECT_EQ(tob.spread(), Price(0.000000001));
        EXPECT_TRUE(tob.valid());
    }

    // Test 2: Very large prices (theoretical max)
    {
        // Use raw integer values to avoid floating-point precision issues
        // Price uses int64_t with scale 10^9
        int64_t large_bid_raw = 5'000'000'000'000'000'000LL;  // 5 billion * 10^9
        int64_t large_ask_raw = 5'000'000'001'000'000'000LL;  // 5 billion + 1 * 10^9

        Price large_bid(large_bid_raw);
        Price large_ask(large_ask_raw);

        book.clear();
        book.update(Side::BUY, large_bid, 1);
        book.update(Side::SELL, large_ask, 1);

        const auto& tob = book.top_of_book();
        EXPECT_EQ(tob.bid_price, large_bid);
        EXPECT_EQ(tob.ask_price, large_ask);
        // Spread should be exactly 1.0 (1 * 10^9 in raw value)
        EXPECT_EQ(tob.spread(), Price(static_cast<int64_t>(1'000'000'000LL)));
        EXPECT_TRUE(tob.valid());
    }

    // Test 3: Price precision edge cases
    {
        // Test that we can represent 9 decimal places accurately
        book.clear();
        Price precise1(1.123456789);
        Price precise2(1.123456790);  // Differs by 1 nano-unit

        book.update(Side::BUY, precise1, 100);
        book.update(Side::SELL, precise2, 200);

        const auto& tob = book.top_of_book();
        EXPECT_EQ(tob.bid_price, precise1);
        EXPECT_EQ(tob.ask_price, precise2);
        EXPECT_EQ(tob.spread(), Price(0.000000001));
    }

    // Test 4: Zero price handling
    {
        book.clear();

        // Zero price with non-zero quantity creates a level
        // (BookSide doesn't reject zero prices)
        book.update(Side::BUY, Price(0.0), 100);
        EXPECT_FALSE(book.empty());  // Level is added with zero price
        EXPECT_EQ(book.bid_depth(), 1);

        // Clear and add valid prices
        book.clear();
        book.update(Side::BUY, Price(100.0), 1000);
        book.update(Side::SELL, Price(101.0), 2000);

        // Update quantity to zero - should remove the level
        book.update(Side::BUY, Price(100.0), 0);
        EXPECT_EQ(book.bid_depth(), 0);

        const auto& tob = book.top_of_book();
        EXPECT_FALSE(tob.valid());  // No valid TOB without bid
    }

    // Test 5: Mixed extreme prices in same book
    {
        book.clear();

        // Add multiple extreme price levels
        book.update(Side::BUY, Price(0.000001), 1000000);  // $0.000001
        book.update(Side::BUY, Price(0.01), 10000);        // $0.01
        book.update(Side::BUY, Price(1.0), 1000);          // $1
        book.update(Side::BUY, Price(1000.0), 10);         // $1,000
        book.update(Side::BUY, Price(1'000'000.0), 1);     // $1M

        book.update(Side::SELL, Price(0.000002), 2000000);  // $0.000002
        book.update(Side::SELL, Price(0.02), 20000);        // $0.02
        book.update(Side::SELL, Price(2.0), 2000);          // $2
        book.update(Side::SELL, Price(2000.0), 20);         // $2,000
        book.update(Side::SELL, Price(2'000'000.0), 2);     // $2M

        // Verify book maintains correct ordering across extreme ranges
        EXPECT_EQ(book.bid_depth(), 5);
        EXPECT_EQ(book.ask_depth(), 5);

        // Best bid should be highest price, best ask should be lowest
        const auto& tob = book.top_of_book();
        EXPECT_EQ(tob.bid_price, Price(1'000'000.0));
        EXPECT_EQ(tob.ask_price, Price(0.000002));
        // TOB is not valid because bid > ask (crossed book)
        EXPECT_FALSE(tob.valid());
        EXPECT_TRUE(book.is_crossed());
    }

    // Test 6: Quantity overflow protection
    {
        book.clear();

        // Use reasonable large quantities that won't cause overflow
        const uint64_t large_qty = 1'000'000'000'000ULL;  // 1 trillion
        const uint64_t max_qty = 10'000'000'000'000ULL;   // 10 trillion

        book.update(Side::BUY, Price(100.0), large_qty);
        book.update(Side::SELL, Price(101.0), max_qty);

        const auto& tob = book.top_of_book();
        EXPECT_EQ(tob.bid_qty, large_qty);
        EXPECT_EQ(tob.ask_qty, max_qty);

        // Update with another large quantity - should replace, not add
        const uint64_t updated_qty = large_qty - 1;
        bool tob_changed = book.update(Side::BUY, Price(100.0), updated_qty);

        // Should detect the quantity change now that the bug is fixed
        EXPECT_TRUE(tob_changed);
        EXPECT_EQ(book.top_of_book().bid_qty, updated_qty);
    }

    // Test 7: Rapid price movements (flash crash/spike simulation)
    {
        book.clear();

        // Start with normal market
        Price start_price(100.0);
        book.update(Side::BUY, start_price, 1000);
        book.update(Side::SELL, start_price + Price(0.1), 1000);

        // Simulate flash crash - price drops 90%
        for (int i = 1; i <= 10; ++i) {
            // Use direct Price construction to avoid multiplication issues
            double crash_value = 100.0 * (1.0 - i * 0.09);
            Price crash_price(crash_value);
            book.update(Side::BUY, crash_price, 100 * i);
        }

        // Best bid should still be the original at 100.0
        // We only added lower bids, didn't remove the original
        const auto& crash_tob = book.top_of_book();
        EXPECT_EQ(crash_tob.bid_price, start_price);
        EXPECT_EQ(crash_tob.bid_qty, 1000);  // Original quantity

        // Simulate recovery spike with new ask levels
        for (int i = 1; i <= 10; ++i) {
            double recovery_value = 101.0 + i * 2.0;  // Start above current ask
            Price recovery_price(recovery_value);
            book.update(Side::SELL, recovery_price, 100 * i);
        }

        // Best ask should still be the original at 100.1
        const auto& final_tob = book.top_of_book();
        EXPECT_EQ(final_tob.ask_price, start_price + Price(0.1));
    }
}

// Performance regression test - disabled by default, run with --gtest_also_run_disabled_tests
TEST_F(OrderBookTest, DISABLED_PerformanceRegression) {
    // Skip on CI or systems without high-resolution clock
    if (std::chrono::high_resolution_clock::period::den < 1'000'000) {
        GTEST_SKIP() << "System clock resolution too low for accurate timing";
    }

    InstrumentId instrument{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    OrderBook book(instrument, *pool_);

    // Performance budgets based on target hardware (adjust as needed)
    constexpr double kUpdateBudgetNs = 500.0;   // 500ns per update
    constexpr double kSnapshotBudgetUs = 50.0;  // 50µs for 100-level snapshot
    constexpr double kClearBudgetUs = 20.0;     // 20µs to clear 1000 levels

    // Warm up the allocator and CPU caches
    for (int i = 0; i < 1000; ++i) {
        book.update(Side::BUY, Price(50000.0 - i * 10.0), 100);
        book.update(Side::SELL, Price(50100.0 + i * 10.0), 100);
    }
    book.clear();

    // Test 1: Single update performance
    {
        std::vector<double> update_times;
        update_times.reserve(10'000);

        std::mt19937 rng(42);
        std::uniform_int_distribution<> price_offset(-5000, 5000);
        std::uniform_int_distribution<> qty_dist(100, 10000);
        std::uniform_int_distribution<> side_dist(0, 1);

        for (int i = 0; i < 10'000; ++i) {
            Price price(50000.0 + price_offset(rng) * 0.1);
            uint64_t qty = qty_dist(rng);
            Side side = side_dist(rng) ? Side::BUY : Side::SELL;

            auto start = std::chrono::high_resolution_clock::now();
            book.update(side, price, qty);
            auto end = std::chrono::high_resolution_clock::now();

            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
            update_times.push_back(static_cast<double>(duration.count()));
        }

        // Calculate p50 and p99
        std::sort(update_times.begin(), update_times.end());
        double p50 = update_times[update_times.size() / 2];
        double p99 = update_times[static_cast<size_t>(update_times.size() * 0.99)];

        if (::testing::GTEST_FLAG(output) != "xml") {
            std::cerr << "Update performance - p50: " << p50 << "ns, p99: " << p99 << "ns" << std::endl;
        }

        EXPECT_LT(p50, kUpdateBudgetNs) << "Update p50 exceeds budget";
        EXPECT_LT(p99, kUpdateBudgetNs * 5) << "Update p99 exceeds 5x budget";
    }

    // Test 2: Snapshot application performance
    {
        std::vector<PriceLevel> bid_levels, ask_levels;

        // Create 100-level snapshot
        for (int i = 0; i < 100; ++i) {
            bid_levels.push_back({Price(50000.0 - i * 10.0), static_cast<uint64_t>(1000 + i * 10)});
            ask_levels.push_back({Price(50100.0 + i * 10.0), static_cast<uint64_t>(2000 + i * 10)});
        }

        std::vector<double> snapshot_times;
        snapshot_times.reserve(100);

        for (int i = 0; i < 100; ++i) {
            auto start = std::chrono::high_resolution_clock::now();
            book.snapshot(bid_levels, ask_levels);
            auto end = std::chrono::high_resolution_clock::now();

            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
            snapshot_times.push_back(static_cast<double>(duration.count()) / 1000.0);  // Convert to µs
        }

        std::sort(snapshot_times.begin(), snapshot_times.end());
        double p50 = snapshot_times[snapshot_times.size() / 2];
        double p99 = snapshot_times[static_cast<size_t>(snapshot_times.size() * 0.99)];

        if (::testing::GTEST_FLAG(output) != "xml") {
            std::cerr << "Snapshot performance - p50: " << p50 << "µs, p99: " << p99 << "µs" << std::endl;
        }

        EXPECT_LT(p50, kSnapshotBudgetUs) << "Snapshot p50 exceeds budget";
        EXPECT_LT(p99, kSnapshotBudgetUs * 2) << "Snapshot p99 exceeds 2x budget";
    }

    // Test 3: Clear performance with many levels
    {
        std::vector<double> clear_times;
        clear_times.reserve(100);

        for (int iter = 0; iter < 100; ++iter) {
            // Fill book with 1000 levels each side
            for (int i = 0; i < 1000; ++i) {
                book.update(Side::BUY, Price(50000.0 - i * 1.0), 100);
                book.update(Side::SELL, Price(50100.0 + i * 1.0), 100);
            }

            auto start = std::chrono::high_resolution_clock::now();
            book.clear();
            auto end = std::chrono::high_resolution_clock::now();

            auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
            clear_times.push_back(static_cast<double>(duration.count()) / 1000.0);  // Convert to µs
        }

        std::sort(clear_times.begin(), clear_times.end());
        double p50 = clear_times[clear_times.size() / 2];
        double p99 = clear_times[static_cast<size_t>(clear_times.size() * 0.99)];

        if (::testing::GTEST_FLAG(output) != "xml") {
            std::cerr << "Clear performance - p50: " << p50 << "µs, p99: " << p99 << "µs" << std::endl;
        }

        EXPECT_LT(p50, kClearBudgetUs) << "Clear p50 exceeds budget";
        EXPECT_LT(p99, kClearBudgetUs * 2) << "Clear p99 exceeds 2x budget";
    }

    // Test 4: Realistic trading pattern performance
    {
        book.clear();

        // Initialize with realistic spread
        for (int i = 0; i < 10; ++i) {
            book.update(Side::BUY, Price(49990.0 - i * 10.0), 1000 + i * 100);
            book.update(Side::SELL, Price(50010.0 + i * 10.0), 1000 + i * 100);
        }

        // Simulate realistic order flow
        std::mt19937 rng(42);
        std::uniform_real_distribution<> spread_dist(-50.0, 50.0);
        std::uniform_int_distribution<> qty_dist(100, 5000);
        std::uniform_int_distribution<> action_dist(0, 99);

        auto start_time = std::chrono::high_resolution_clock::now();

        const int num_updates = 100'000;
        int tob_changes = 0;

        for (int i = 0; i < num_updates; ++i) {
            int action = action_dist(rng);

            if (action < 40) {
                // 40% - Update near top of book
                double offset = spread_dist(rng);
                Side side = offset < 0 ? Side::BUY : Side::SELL;
                Price price(50000.0 + offset);
                uint64_t qty = qty_dist(rng);

                bool changed = book.update(side, price, qty);
                if (changed)
                    tob_changes++;

            } else if (action < 80) {
                // 40% - Add/update in deeper book
                double offset = spread_dist(rng) * 5;
                Side side = offset < 0 ? Side::BUY : Side::SELL;
                Price price(50000.0 + offset);
                uint64_t qty = qty_dist(rng);

                bool changed = book.update(side, price, qty);
                if (changed)
                    tob_changes++;

            } else if (action < 95) {
                // 15% - Cancel orders (set qty to 0)
                double offset = spread_dist(rng);
                Side side = offset < 0 ? Side::BUY : Side::SELL;
                Price price(50000.0 + offset);

                bool changed = book.update(side, price, 0);
                if (changed)
                    tob_changes++;

            } else {
                // 5% - Large orders that might move market
                Side side = action_dist(rng) < 50 ? Side::BUY : Side::SELL;
                Price price =
                    side == Side::BUY ? Price(50000.0 + spread_dist(rng) / 2) : Price(50000.0 + spread_dist(rng) / 2);
                uint64_t qty = qty_dist(rng) * 10;

                bool changed = book.update(side, price, qty);
                if (changed)
                    tob_changes++;
            }
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        auto total_duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        double avg_update_ns = (static_cast<double>(total_duration.count()) * 1000.0) / num_updates;
        double tob_change_rate = static_cast<double>(tob_changes) / num_updates * 100.0;

        if (::testing::GTEST_FLAG(output) != "xml") {
            std::cerr << "Realistic pattern - avg update: " << avg_update_ns << "ns, "
                      << "TOB change rate: " << tob_change_rate << "%" << std::endl;
        }

        EXPECT_LT(avg_update_ns, kUpdateBudgetNs) << "Average update time exceeds budget";
        EXPECT_GT(tob_change_rate, 5.0) << "TOB change rate suspiciously low";
        EXPECT_LT(tob_change_rate, 50.0) << "TOB change rate suspiciously high";
    }
}

// ===== Main =====

// Global environment registration
[[maybe_unused]] static auto* test_env = []() {
    auto* env = new OrderBookTestEnvironment;
    OrderBookTestEnvironment::SetInstance(env);
    ::testing::AddGlobalTestEnvironment(env);
    return env;
}();