#include <chrono>
#include <limits>
#include <random>
#include <unordered_map>

#include <gtest/gtest.h>

#include "core/price.hpp"
#include "core/memory_pool.hpp"
#include "orderbook/book_side.hpp"

using namespace crypto_lob::core;

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
    auto result = ask_side_->apply_update(Price(100.0), 1000, 1);
    EXPECT_EQ(result.delta_qty, 1000);
    EXPECT_TRUE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    EXPECT_FALSE(ask_side_->empty());
    EXPECT_EQ(ask_side_->size(), 1);
    EXPECT_EQ(ask_side_->best()->price, Price(100.0));
    EXPECT_EQ(ask_side_->best()->quantity, 1000);

    // Update quantity
    result = ask_side_->apply_update(Price(100.0), 2000, 2);
    EXPECT_EQ(result.delta_qty, 1000);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    EXPECT_EQ(ask_side_->best()->quantity, 2000);

    // Remove level
    result = ask_side_->apply_update(Price(100.0), 0, 3);
    EXPECT_EQ(result.delta_qty, -2000);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_TRUE(result.is_deletion);

    EXPECT_TRUE(ask_side_->empty());
}

TEST_F(BookSideTest, PriceOrdering) {
    // Test ask side (ascending) - using high-value asset prices
    ask_side_->apply_update(Price(50200.0), 300, 1);
    ask_side_->apply_update(Price(50000.0), 100, 2);
    ask_side_->apply_update(Price(50100.0), 200, 3);

    EXPECT_EQ(ask_side_->size(), 3);
    EXPECT_EQ(ask_side_->best()->price, Price(50000.0));

    std::vector<PriceLevel> levels;
    ask_side_->get_top_n(10, std::back_inserter(levels));

    EXPECT_EQ(levels.size(), 3);
    EXPECT_EQ(levels[0].price, Price(50000.0));
    EXPECT_EQ(levels[1].price, Price(50100.0));
    EXPECT_EQ(levels[2].price, Price(50200.0));

    // Test bid side (descending)
    bid_side_->apply_update(Price(49800.0), 300, 1);
    bid_side_->apply_update(Price(50000.0), 100, 2);
    bid_side_->apply_update(Price(49900.0), 200, 3);

    EXPECT_EQ(bid_side_->best()->price, Price(50000.0));

    levels.clear();
    bid_side_->get_top_n(10, std::back_inserter(levels));

    EXPECT_EQ(levels[0].price, Price(50000.0));
    EXPECT_EQ(levels[1].price, Price(49900.0));
    EXPECT_EQ(levels[2].price, Price(49800.0));
}

TEST_F(BookSideTest, TopNTracking) {
    ask_side_->set_top_n_depth(3);

    // Fill top 3 - using low-value token prices to test LOW_SCALE
    ask_side_->apply_update(Price(0.001), 100, 1);
    ask_side_->apply_update(Price(0.0011), 100, 2);
    ask_side_->apply_update(Price(0.0012), 100, 3);

    EXPECT_EQ(ask_side_->get_nth_price(), Price(0.0012));

    // Add outside top 3
    auto result = ask_side_->apply_update(Price(0.0013), 100, 4);
    EXPECT_FALSE(result.affects_top_n);

    // Add inside top 3
    result = ask_side_->apply_update(Price(0.0009), 100, 5);
    EXPECT_TRUE(result.affects_top_n);
    EXPECT_EQ(ask_side_->get_nth_price(), Price(0.0011));
}

TEST_F(BookSideTest, KthLevel) {
    // Test ask side - mid-range asset
    ask_side_->apply_update(Price(10.0), 100, 1);
    ask_side_->apply_update(Price(10.1), 200, 2);
    ask_side_->apply_update(Price(10.2), 300, 3);

    EXPECT_EQ(ask_side_->kth_level(0)->price, Price(10.0));
    EXPECT_EQ(ask_side_->kth_level(1)->price, Price(10.1));
    EXPECT_EQ(ask_side_->kth_level(2)->price, Price(10.2));
    EXPECT_EQ(ask_side_->kth_level(3), nullptr);

    // Test bid side (descending order)
    bid_side_->apply_update(Price(10.0), 100, 1);
    bid_side_->apply_update(Price(9.9), 200, 2);
    bid_side_->apply_update(Price(9.8), 300, 3);

    EXPECT_EQ(bid_side_->kth_level(0)->price, Price(10.0));
    EXPECT_EQ(bid_side_->kth_level(1)->price, Price(9.9));
    EXPECT_EQ(bid_side_->kth_level(2)->price, Price(9.8));
    EXPECT_EQ(bid_side_->kth_level(3), nullptr);
}

TEST_F(BookSideTest, FuzzTest) {
    std::mt19937 rng(42);  // Deterministic seed
    // Test a range around $1000 with realistic price increments
    std::uniform_real_distribution<double> price_dist(900.0, 1100.0);
    std::uniform_int_distribution<uint64_t> qty_dist(0, 10000);
    std::uniform_int_distribution<int> action_dist(0, 2);  // Add/update/remove

    // Use Price type consistently
    std::unordered_map<Price, uint64_t> expected_book;

    const size_t NUM_OPS = 100'000;

    for (size_t i = 0; i < NUM_OPS; ++i) {
        Price price = Price(price_dist(rng));
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
                ASSERT_EQ(node->quantity, qty) << "Quantity mismatch for price " << px.to_string() << " at operation " << i;
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
        ask_side_->apply_update(Price(100.0 + i * 0.01), 100, i);
    }
    ask_side_->clear();

    // Benchmark random updates
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> price_dist(90.0, 110.0);
    std::uniform_int_distribution<uint64_t> qty_dist(100, 10000);

    const size_t NUM_OPS = 1'000'000;
    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < NUM_OPS; ++i) {
        Price price = Price(price_dist(rng));
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
    // Test with micro-cap token prices to ensure LOW_SCALE is tested
    std::uniform_real_distribution<double> price_dist(0.00001, 0.001);
    std::uniform_int_distribution<uint64_t> qty_dist(0, 10000);

    // Run 1M operations (reduced from 10M for reasonable test time)
    for (size_t i = 0; i < 1'000'000; ++i) {
        Price price = Price(price_dist(rng));
        uint64_t qty = qty_dist(rng);
        ask_side_->apply_update(price, qty, i);
    }

    // Pick 100 random prices and verify coherence
    for (int i = 0; i < 100; ++i) {
        Price price = Price(price_dist(rng));

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
    auto result = ask_side_->apply_update(Price(100.0), 0, 1);
    EXPECT_EQ(result.delta_qty, 0);
    EXPECT_FALSE(result.is_new_level);
    EXPECT_FALSE(result.affects_top_n);
    EXPECT_FALSE(result.is_deletion);

    // Very large quantity (avoid UB with safe value)
    const uint64_t big_qty = std::numeric_limits<uint64_t>::max() / 2;
    result = ask_side_->apply_update(Price(100.0), big_qty, 2);
    EXPECT_EQ(result.delta_qty, static_cast<int64_t>(big_qty));

    // Update with same quantity
    result = ask_side_->apply_update(Price(100.0), big_qty, 3);
    EXPECT_EQ(result.delta_qty, 0);
}

// Additional test: Duplicate price updates
TEST_F(BookSideTest, DuplicatePriceUpdates) {
    // Insert same price multiple times with different quantities
    ask_side_->apply_update(Price(100.0), 1000, 1);
    ask_side_->apply_update(Price(100.0), 2000, 2);
    ask_side_->apply_update(Price(100.0), 3000, 3);

    EXPECT_EQ(ask_side_->size(), 1);               // Only one node
    EXPECT_EQ(ask_side_->best()->quantity, 3000);  // Latest quantity
    EXPECT_EQ(ask_side_->best()->update_seq, 3);   // Latest sequence
}

// Additional test: Top-N depth change
TEST_F(BookSideTest, TopNDepthChange) {
    ask_side_->set_top_n_depth(5);

    // Fill 10 levels - using a mid-range asset with realistic tick sizes
    for (int i = 0; i < 10; ++i) {
        ask_side_->apply_update(Price(100.0 + i * 0.1), 100, i);
    }

    EXPECT_EQ(ask_side_->get_nth_price(), Price(100.4));

    // Change depth to 8
    ask_side_->set_top_n_depth(8);
    EXPECT_EQ(ask_side_->get_nth_price(), Price(100.7));

    // Test affects_top_n with new depth
    auto result = ask_side_->apply_update(Price(100.6), 200, 11);
    EXPECT_TRUE(result.affects_top_n);

    result = ask_side_->apply_update(Price(100.9), 200, 12);
    EXPECT_FALSE(result.affects_top_n);
}

// Additional test: Iterator stability
TEST_F(BookSideTest, IteratorStability) {
    // Add some levels with different price scales
    ask_side_->apply_update(Price(100.0), 100, 1);
    ask_side_->apply_update(Price(100.5), 200, 2);
    ask_side_->apply_update(Price(101.0), 300, 3);

    // Get pointer to middle node
    const PriceLevelNode* middle_node = ask_side_->kth_level(1);
    ASSERT_NE(middle_node, nullptr);
    Price original_price = middle_node->price;

    // Update a different level
    ask_side_->apply_update(Price(100.0), 150, 4);

    // Verify pointer still valid and points to same price
    EXPECT_EQ(middle_node->price, original_price);
    EXPECT_EQ(middle_node->quantity, 200);

    // Update the same level
    ask_side_->apply_update(Price(100.5), 250, 5);

    // Pointer still valid, quantity updated
    EXPECT_EQ(middle_node->price, original_price);
    EXPECT_EQ(middle_node->quantity, 250);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}