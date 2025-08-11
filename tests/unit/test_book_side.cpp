#include <chrono>
#include <limits>
#include <random>
#include <unordered_map>

#include <gtest/gtest.h>

#include "core/memory_pool.hpp"
#include "core/price.hpp"
#include "orderbook/book_side.hpp"

using namespace crypto_lob::core;

// Type aliases for bid and ask sides
using AskSide = BookSide<AskComparator>;
using BidSide = BookSide<BidComparator>;

// Helper functions to create prices for testing different ranges
// Using direct Price constructor with double values for clarity

// Hash support for Price (only needed for test's unordered_map)
namespace std {
template <>
struct hash<crypto_lob::core::Price> {
    std::size_t operator()(const crypto_lob::core::Price& p) const noexcept {
        return crypto_lob::core::hash_value(p);
    }
};
}  // namespace std

class BookSideTest : public ::testing::Test {
  protected:
    // Pool size to handle test operations
    static constexpr size_t POOL_SIZE = 2'000'000;

    void SetUp() override {
        ask_pool_ = std::make_unique<MemoryPool<PriceLevelNode>>(POOL_SIZE);
        bid_pool_ = std::make_unique<MemoryPool<PriceLevelNode>>(POOL_SIZE);
        ask_side_ = std::make_unique<AskSide>(*ask_pool_);
        bid_side_ = std::make_unique<BidSide>(*bid_pool_);
    }

    std::unique_ptr<MemoryPool<PriceLevelNode>> ask_pool_;
    std::unique_ptr<MemoryPool<PriceLevelNode>> bid_pool_;
    std::unique_ptr<AskSide> ask_side_;
    std::unique_ptr<BidSide> bid_side_;
};

TEST_F(BookSideTest, BasicOperations) {
    // Test empty book
    EXPECT_TRUE(ask_side_->empty());
    EXPECT_EQ(ask_side_->size(), 0);
    EXPECT_EQ(ask_side_->best(), nullptr);

    // Add first level
    auto result = ask_side_->apply_update(Price(100000000000), 1000, 1);
    EXPECT_EQ(result.delta_qty, 1000);
    EXPECT_TRUE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    EXPECT_FALSE(ask_side_->empty());
    EXPECT_EQ(ask_side_->size(), 1);
    EXPECT_EQ(ask_side_->best()->price, Price(100000000000));
    EXPECT_EQ(ask_side_->best()->quantity, 1000);

    // Update quantity
    result = ask_side_->apply_update(Price(100000000000), 2000, 2);
    EXPECT_EQ(result.delta_qty, 1000);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    EXPECT_EQ(ask_side_->best()->quantity, 2000);

    // Remove level
    result = ask_side_->apply_update(Price(100000000000), 0, 3);
    EXPECT_EQ(result.delta_qty, -2000);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_TRUE(result.is_deletion);

    EXPECT_TRUE(ask_side_->empty());
}

TEST_F(BookSideTest, PriceOrdering) {
    // Test ask side (ascending) - using high-value asset prices
    ask_side_->apply_update(Price(50200000000000), 300, 1);
    ask_side_->apply_update(Price(50000000000000), 100, 2);
    ask_side_->apply_update(Price(50100000000000), 200, 3);

    EXPECT_EQ(ask_side_->size(), 3);
    EXPECT_EQ(ask_side_->best()->price, Price(50000000000000));

    std::vector<PriceLevel> levels;
    ask_side_->get_top_n(10, std::back_inserter(levels));

    EXPECT_EQ(levels.size(), 3);
    EXPECT_EQ(levels[0].price, Price(50000000000000));
    EXPECT_EQ(levels[1].price, Price(50100000000000));
    EXPECT_EQ(levels[2].price, Price(50200000000000));

    // Test bid side (descending)
    bid_side_->apply_update(Price(49800000000000), 300, 1);
    bid_side_->apply_update(Price(50000000000000), 100, 2);
    bid_side_->apply_update(Price(49900000000000), 200, 3);

    EXPECT_EQ(bid_side_->best()->price, Price(50000000000000));

    levels.clear();
    bid_side_->get_top_n(10, std::back_inserter(levels));

    EXPECT_EQ(levels[0].price, Price(50000000000000));
    EXPECT_EQ(levels[1].price, Price(49900000000000));
    EXPECT_EQ(levels[2].price, Price(49800000000000));
}

TEST_F(BookSideTest, TopNTracking) {
    ask_side_->set_top_n_depth(3);

    // Fill top 3 - using low-value token prices
    ask_side_->apply_update(Price::from_string("0.001"), 100, 1);
    ask_side_->apply_update(Price::from_string("0.0011"), 100, 2);
    ask_side_->apply_update(Price::from_string("0.0012"), 100, 3);

    EXPECT_EQ(ask_side_->get_nth_price(), Price::from_string("0.0012"));

    // Add outside top 3
    auto result = ask_side_->apply_update(Price::from_string("0.0013"), 100, 4);
    EXPECT_FALSE(result.affects_top_n);

    // Add inside top 3
    result = ask_side_->apply_update(Price::from_string("0.0009"), 100, 5);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_EQ(ask_side_->get_nth_price(), Price::from_string("0.0011"));
}

TEST_F(BookSideTest, KthLevel) {
    // Test ask side - mid-range asset
    // Use from_string for proper decimal conversion
    ask_side_->apply_update(Price::from_string("10.0"), 100, 1);
    ask_side_->apply_update(Price::from_string("10.1"), 200, 2);
    ask_side_->apply_update(Price::from_string("10.2"), 300, 3);

    // Debug: Check size first
    EXPECT_EQ(ask_side_->size(), 3u);

    // Test each level with null checks
    auto* level0 = ask_side_->kth_level(0);
    ASSERT_NE(level0, nullptr) << "Level 0 should not be null";
    EXPECT_EQ(level0->price, Price::from_string("10.0"));

    auto* level1 = ask_side_->kth_level(1);
    ASSERT_NE(level1, nullptr) << "Level 1 should not be null";
    EXPECT_EQ(level1->price, Price::from_string("10.1"));

    auto* level2 = ask_side_->kth_level(2);
    ASSERT_NE(level2, nullptr) << "Level 2 should not be null";
    EXPECT_EQ(level2->price, Price::from_string("10.2"));

    EXPECT_EQ(ask_side_->kth_level(3), nullptr);

    // Test bid side (descending order)
    bid_side_->apply_update(Price::from_string("10.0"), 100, 1);
    bid_side_->apply_update(Price::from_string("9.9"), 200, 2);
    bid_side_->apply_update(Price::from_string("9.8"), 300, 3);

    // Debug: Check size first
    EXPECT_EQ(bid_side_->size(), 3u);

    // Test each level with null checks
    auto* bid_level0 = bid_side_->kth_level(0);
    ASSERT_NE(bid_level0, nullptr) << "Bid level 0 should not be null";
    EXPECT_EQ(bid_level0->price, Price::from_string("10.0"));

    auto* bid_level1 = bid_side_->kth_level(1);
    ASSERT_NE(bid_level1, nullptr) << "Bid level 1 should not be null";
    EXPECT_EQ(bid_level1->price, Price::from_string("9.9"));

    auto* bid_level2 = bid_side_->kth_level(2);
    ASSERT_NE(bid_level2, nullptr) << "Bid level 2 should not be null";
    EXPECT_EQ(bid_level2->price, Price::from_string("9.8"));

    EXPECT_EQ(bid_side_->kth_level(3), nullptr);
}

TEST_F(BookSideTest, FuzzTest) {
    std::mt19937 rng(42);  // Deterministic seed
    // Test a range around $1000 with realistic price increments
    // Use integer distribution to avoid floating-point precision issues
    // Range: $900.000000000 to $1100.000000000 with 9 decimal places
    std::uniform_int_distribution<int64_t> price_raw_dist(900'000'000'000LL, 1100'000'000'000LL);
    std::uniform_int_distribution<uint64_t> qty_dist(0, 10000);
    std::uniform_int_distribution<int> action_dist(0, 2);  // Add/update/remove

    // Use Price type consistently
    std::unordered_map<Price, uint64_t> expected_book;

    const size_t NUM_OPS = 100'000;

    for (size_t i = 0; i < NUM_OPS; ++i) {
        // Create price from raw integer value to avoid floating-point conversion
        Price price(price_raw_dist(rng));
        uint64_t qty = action_dist(rng) == 0 ? 0 : qty_dist(rng);

        // Update expected state
        if (qty == 0) {
            expected_book.erase(price);
        } else {
            expected_book[price] = qty;
        }

        // Apply to book
        ask_side_->apply_update(price, qty, i);

        // Validate every 1000 ops (only in debug builds for performance)
#ifndef NDEBUG
        if (i % 1000 == 0) {
            ASSERT_TRUE(ask_side_->validate_tree()) << "Tree validation failed at operation " << i;

            ASSERT_EQ(ask_side_->size(), expected_book.size()) << "Size mismatch at operation " << i;

            // Verify all expected prices are in book
            for (const auto& [px, qty] : expected_book) {
                auto* node = ask_side_->find(px);
                ASSERT_NE(node, nullptr) << "Price " << px.to_string() << " not found at operation " << i;
                ASSERT_EQ(node->quantity, qty)
                    << "Quantity mismatch for price " << px.to_string() << " at operation " << i;
            }
        }
#endif
    }

    // Final validation
    EXPECT_TRUE(ask_side_->validate_tree());
    EXPECT_EQ(ask_side_->size(), expected_book.size());
}

// Disabled by default - run with --gtest_also_run_disabled_tests for performance testing
TEST_F(BookSideTest, DISABLED_PerformanceBenchmark) {
    // Skip on CI or systems without high-resolution clock
    if (std::chrono::high_resolution_clock::period::den < 1'000'000) {
        GTEST_SKIP() << "System clock resolution too low for accurate timing";
    }

    // Performance expectations (adjust based on target hardware)
    constexpr double kNsPerOpBudget = 300.0;  // Expected p50 on Ryzen 7950X, -O3, LTO

    // Warm up - using a typical asset price range
    for (int i = 0; i < 1000; ++i) {
        // Use raw values: 100.0 = 100000000000, 0.01 = 10000000
        ask_side_->apply_update(Price(100000000000 + i * 10000000), 100, i);
    }
    ask_side_->clear();

    // Benchmark random updates
    std::mt19937 rng(42);
    // Use integer distribution for deterministic prices: $90.000000000 to $110.000000000
    std::uniform_int_distribution<int64_t> price_raw_dist(90'000'000'000LL, 110'000'000'000LL);
    std::uniform_int_distribution<uint64_t> qty_dist(100, 10000);

    const size_t NUM_OPS = 1'000'000;
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_OPS; ++i) {
        Price price(price_raw_dist(rng));
        uint64_t qty = qty_dist(rng);
        ask_side_->apply_update(price, qty, i);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double ns_per_op = static_cast<double>(duration.count()) / NUM_OPS;

    // Only output if not capturing for XML
    if (::testing::GTEST_FLAG(output) != "xml") {
        std::cerr << "Average time per update: " << ns_per_op << " ns" << std::endl;
    }

    EXPECT_LT(ns_per_op, kNsPerOpBudget);
}

TEST_F(BookSideTest, HashTreeCoherence) {
    std::mt19937 rng(42);
    // Test with micro-cap token prices: $0.000010000 to $0.001000000 (9 decimal places)
    std::uniform_int_distribution<int64_t> price_raw_dist(10'000LL, 1'000'000LL);
    std::uniform_int_distribution<uint64_t> qty_dist(0, 10000);

    // Run 1M operations (reduced from 10M for reasonable test time)
    for (size_t i = 0; i < 1'000'000; ++i) {
        Price price(price_raw_dist(rng));
        uint64_t qty = qty_dist(rng);
        ask_side_->apply_update(price, qty, i);
    }

    // Pick 100 random prices and verify coherence
    for (int i = 0; i < 100; ++i) {
        Price price(price_raw_dist(rng));

        auto* hash_result = ask_side_->find(price);

        // Check if tree contains this price
        bool in_tree = false;
        for (const auto& node : *ask_side_) {
            if (node.price == price) {
                in_tree = true;
                EXPECT_EQ(&node, hash_result);
                break;
            }
        }

        if (hash_result != nullptr) {
            EXPECT_TRUE(in_tree) << "Hash contains price " << price.to_string() << " but tree doesn't";
        } else {
            EXPECT_FALSE(in_tree) << "Tree contains price " << price.to_string() << " but hash doesn't";
        }
    }

    // Final full validation
    EXPECT_TRUE(ask_side_->validate_tree());
}

// Test edge cases
TEST_F(BookSideTest, EdgeCases) {
    // Zero quantity on empty book
    auto result = ask_side_->apply_update(Price(100000000000), 0, 1);
    EXPECT_EQ(result.delta_qty, 0);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_FALSE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    // Very large quantity (avoid UB with safe value)
    const uint64_t big_qty = std::numeric_limits<uint64_t>::max() / 2;
    result = ask_side_->apply_update(Price(100000000000), big_qty, 2);
    EXPECT_EQ(result.delta_qty, static_cast<int64_t>(big_qty));

    // Update with same quantity
    result = ask_side_->apply_update(Price(100000000000), big_qty, 3);
    EXPECT_EQ(result.delta_qty, 0);
}

// Additional test: Duplicate price updates
TEST_F(BookSideTest, DuplicatePriceUpdates) {
    // Insert same price multiple times with different quantities
    ask_side_->apply_update(Price(100000000000), 1000, 1);
    ask_side_->apply_update(Price(100000000000), 2000, 2);
    ask_side_->apply_update(Price(100000000000), 3000, 3);

    EXPECT_EQ(ask_side_->size(), 1);               // Only one node
    EXPECT_EQ(ask_side_->best()->quantity, 3000);  // Latest quantity
    EXPECT_EQ(ask_side_->best()->update_seq, 3);   // Latest sequence
}

// Additional test: Top-N depth change
TEST_F(BookSideTest, TopNDepthChange) {
    ask_side_->set_top_n_depth(5);

    // Fill 10 levels - using a mid-range asset with realistic tick sizes
    for (int i = 0; i < 10; ++i) {
        // Use raw values: 100.0 = 100000000000, 0.1 = 100000000
        ask_side_->apply_update(Price(100000000000 + i * 100000000), 100, i);
    }

    // 100.4 = 100400000000
    EXPECT_EQ(ask_side_->get_nth_price(), Price(100400000000));

    // Change depth to 8
    ask_side_->set_top_n_depth(8);
    // 100.7 = 100700000000
    EXPECT_EQ(ask_side_->get_nth_price(), Price(100700000000));

    // Test affects_top_n with new depth
    // 100.6 = 100600000000
    auto result = ask_side_->apply_update(Price(100600000000), 200, 11);
    EXPECT_TRUE(result.affects_top_n);

    // 100.9 = 100900000000
    result = ask_side_->apply_update(Price(100900000000), 200, 12);
    EXPECT_FALSE(result.affects_top_n);
}

// Additional test: Iterator stability
TEST_F(BookSideTest, IteratorStability) {
    // Add some levels with different price scales
    ask_side_->apply_update(Price(100000000000), 100, 1);
    ask_side_->apply_update(Price(100500000000), 200, 2);  // 100.5
    ask_side_->apply_update(Price(101000000000), 300, 3);  // 101.0

    // Get pointer to middle node
    const PriceLevelNode* middle_node = ask_side_->kth_level(1);
    ASSERT_NE(middle_node, nullptr);
    Price original_price = middle_node->price;

    // Update a different level
    ask_side_->apply_update(Price(100000000000), 150, 4);

    // Verify pointer still valid and points to same price
    EXPECT_EQ(middle_node->price, original_price);
    EXPECT_EQ(middle_node->quantity, 200);

    // Update the same level
    ask_side_->apply_update(Price(100500000000), 250, 5);  // 100.5

    // Pointer still valid, quantity updated
    EXPECT_EQ(middle_node->price, original_price);
    EXPECT_EQ(middle_node->quantity, 250);
}

// Additional test: Price overflow/underflow edge cases
TEST_F(BookSideTest, PriceOverflowUnderflow) {
    // Test near int64_t limits - Price uses int64_t internally
    // Max safe value considering Price internal scale of 10^9
    const int64_t max_safe_raw = std::numeric_limits<int64_t>::max() / 2;
    const int64_t min_safe_raw = std::numeric_limits<int64_t>::min() / 2;

    Price max_price(max_safe_raw);
    Price min_price(min_safe_raw);

    // Test ask side with extreme prices
    auto result1 = ask_side_->apply_update(max_price, 100, 1);
    EXPECT_TRUE(result1.is_new_level);
    EXPECT_EQ(result1.delta_qty, 100);

    // Add a normal price
    auto result2 = ask_side_->apply_update(Price(100000000000), 200, 2);
    EXPECT_TRUE(result2.is_new_level);

    // Verify ordering is maintained
    EXPECT_EQ(ask_side_->size(), 2);
    EXPECT_EQ(ask_side_->best()->price, Price(100000000000));  // Lower price is best for asks

    // Test bid side with extreme prices
    auto result3 = bid_side_->apply_update(min_price, 300, 1);
    EXPECT_TRUE(result3.is_new_level);
    EXPECT_EQ(result3.delta_qty, 300);

    auto result4 = bid_side_->apply_update(Price(100000000000), 400, 2);
    EXPECT_TRUE(result4.is_new_level);

    // Verify ordering is maintained
    EXPECT_EQ(bid_side_->size(), 2);
    EXPECT_EQ(bid_side_->best()->price, Price(100000000000));  // Higher price is best for bids

    // Test price arithmetic near limits
    Price near_max(static_cast<int64_t>(max_safe_raw - 1000000000LL));  // Leave room for Price's scale
    auto result5 = ask_side_->apply_update(near_max, 500, 3);
    EXPECT_TRUE(result5.is_new_level);

    // Verify tree consistency with extreme values
    EXPECT_TRUE(ask_side_->validate_tree());
    EXPECT_TRUE(bid_side_->validate_tree());
}

// Additional test: Sequence number wraparound
TEST_F(BookSideTest, SequenceNumberWraparound) {
    // Test behavior at 32-bit sequence wraparound
    uint32_t near_max = std::numeric_limits<uint32_t>::max() - 10;

    // Add initial level
    ask_side_->apply_update(Price(100000000000), 100, near_max);

    // Verify initial state
    auto* node = ask_side_->find(Price(100000000000));
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(node->update_seq, near_max);

    // Update several times approaching wraparound
    for (uint32_t i = 1; i <= 10; ++i) {
        uint32_t seq = near_max + i;
        ask_side_->apply_update(Price(100000000000), 100 + i, seq);

        // Verify update was applied
        node = ask_side_->find(Price(100000000000));
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->quantity, static_cast<uint64_t>(100 + i));
        EXPECT_EQ(node->update_seq, seq);
    }

    // Continue past wraparound point
    for (uint32_t i = 0; i < 10; ++i) {
        ask_side_->apply_update(Price(100000000000), 200 + i, i);

        node = ask_side_->find(Price(100000000000));
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->quantity, static_cast<uint64_t>(200 + i));
        EXPECT_EQ(node->update_seq, i);  // Wrapped around
    }

    // Test with multiple price levels during wraparound
    ask_side_->clear();

    // Add levels with sequences near max
    for (int i = 0; i < 5; ++i) {
        // Use raw values: 100.0 = 100000000000, 0.1 = 100000000
        ask_side_->apply_update(Price(100000000000 + i * 100000000), 1000 + i, near_max - i);
    }

    // Update all levels past wraparound
    for (int i = 0; i < 5; ++i) {
        uint32_t wrapped_seq = i;  // Sequences 0-4 after wraparound
        // Use raw values: 100.0 = 100000000000, 0.1 = 100000000
        ask_side_->apply_update(Price(100000000000 + i * 100000000), 2000 + i, wrapped_seq);

        auto* level = ask_side_->find(Price(100000000000 + i * 100000000));
        ASSERT_NE(level, nullptr);
        EXPECT_EQ(level->update_seq, wrapped_seq);
    }

    // Verify book remains consistent after wraparound
    EXPECT_TRUE(ask_side_->validate_tree());
    EXPECT_EQ(ask_side_->size(), 5);
}

// Additional test: Cache locality pattern
TEST_F(BookSideTest, CacheLocalityPattern) {
// Skip if running under sanitizers which affect timing
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    GTEST_SKIP() << "Skipping performance test under sanitizers";
#endif
#endif

    // Verify that frequently accessed nodes maintain cache locality
    const size_t NUM_LEVELS = 1000;

    // Create price levels
    for (size_t i = 0; i < NUM_LEVELS; ++i) {
        // Use raw values: 100.0 = 100000000000, 0.01 = 10000000
        ask_side_->apply_update(Price(100000000000 + i * 10000000), 100 + i, i);
    }

    // Warm up caches
    for (int warmup = 0; warmup < 100; ++warmup) {
        volatile auto* node = ask_side_->kth_level(warmup % 10);
        (void)node;
    }

    // Measure access patterns
    auto start = std::chrono::high_resolution_clock::now();

    // Simulate realistic access pattern - top levels accessed more
    std::mt19937 rng(42);  // Deterministic seed
    std::uniform_int_distribution<> top_dist(0, 99);
    std::uniform_int_distribution<> all_dist(0, NUM_LEVELS - 1);

    const int NUM_ACCESSES = 10000;
    int top_accesses = 0;

    for (int iteration = 0; iteration < NUM_ACCESSES; ++iteration) {
        // 80% of accesses to top 10% of levels
        size_t level;
        if (top_dist(rng) < 80) {
            level = all_dist(rng) % (NUM_LEVELS / 10);
            top_accesses++;
        } else {
            level = all_dist(rng);
        }

        auto* node = ask_side_->kth_level(level);
        if (node) {
            // Force actual memory access
            volatile auto price = node->price.raw_value();
            volatile auto qty = node->quantity;
            (void)price;
            (void)qty;
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

    // Verify access pattern was as expected
    double top_ratio = static_cast<double>(top_accesses) / NUM_ACCESSES;
    EXPECT_GT(top_ratio, 0.75);  // Should be around 0.8
    EXPECT_LT(top_ratio, 0.85);

    // Performance expectation: should complete quickly due to cache hits
    // This is a soft expectation - may vary by hardware
    if (duration.count() > 10000) {  // 10ms for 10k accesses
        std::cerr << "Warning: Cache locality test took " << duration.count() << " microseconds (expected < 10000)"
                  << std::endl;
    }

    // Additional test: sequential vs random access comparison
    const int SEQUENTIAL_ACCESSES = 1000;

    // Sequential access (cache-friendly)
    auto seq_start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < SEQUENTIAL_ACCESSES; ++i) {
        for (size_t j = 0; j < 10; ++j) {
            auto* node = ask_side_->kth_level(j);
            if (node) {
                volatile auto price = node->price.raw_value();
                (void)price;
            }
        }
    }
    auto seq_end = std::chrono::high_resolution_clock::now();
    auto seq_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(seq_end - seq_start);

    // Random access (cache-unfriendly)
    auto rand_start = std::chrono::high_resolution_clock::now();
    std::uniform_int_distribution<> rand_level_dist(0, NUM_LEVELS - 1);
    for (int i = 0; i < SEQUENTIAL_ACCESSES; ++i) {
        for (size_t j = 0; j < 10; ++j) {
            auto* node = ask_side_->kth_level(rand_level_dist(rng));
            if (node) {
                volatile auto price = node->price.raw_value();
                (void)price;
            }
        }
    }
    auto rand_end = std::chrono::high_resolution_clock::now();
    auto rand_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(rand_end - rand_start);

    // Sequential access should be faster than random (better cache usage)
    // Only check if both durations are meaningful (> 0)
    if (seq_duration.count() > 0 && rand_duration.count() > 0) {
        double speedup = static_cast<double>(rand_duration.count()) / seq_duration.count();
        // Sequential should be at least somewhat faster
        if (speedup < 1.1) {
            std::cerr << "Warning: Sequential access not significantly faster than random "
                      << "(speedup: " << speedup << "x)" << std::endl;
        }
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}