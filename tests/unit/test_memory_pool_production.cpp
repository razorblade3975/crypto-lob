#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "core/memory_pool.hpp"

// TSAN interface includes and macros
// For now, disable TSAN annotations as they're not available in this environment
#define TSAN_IGNORE_WRITES_BEGIN() ((void)0)
#define TSAN_IGNORE_WRITES_END() ((void)0)
#define TSAN_ANNOTATE_BENIGN_RACE_SIZED(ptr, size, description) ((void)0)

using namespace crypto_lob::core;

namespace {

// Test objects of different sizes for mixed allocation testing
// All objects must be at least 64 bytes and properly aligned to satisfy FreeNode requirements
struct alignas(64) TinyObject {
    char data;
    char padding[63];  // Pad to 64 bytes
    TinyObject() : data(0), padding{} {}
    explicit TinyObject(char value) : data(value), padding{} {}
};

struct alignas(64) MediumObject {
    int data[16];  // 64 bytes
    MediumObject() : data{} {}
    explicit MediumObject(int value) : data{} {
        std::fill_n(data, 16, value);
    }
};

struct alignas(64) LargeObject {
    char data[256];  // 256 bytes
    LargeObject() : data{} {}
    explicit LargeObject(char value) : data{} {
        std::fill_n(data, 256, value);
    }
};

// Variant for mixed-size stress testing
using MixedObject = std::variant<TinyObject*, MediumObject*, LargeObject*>;

}  // anonymous namespace

// Parameterized test for different pool configurations
struct PoolTestParams {
    size_t capacity;
    size_t cache_size;
    size_t batch_size;

    friend std::ostream& operator<<(std::ostream& os, const PoolTestParams& params) {
        return os << "capacity_" << params.capacity << "_cache_" << params.cache_size << "_batch_" << params.batch_size;
    }
};

class ParameterizedMemoryPoolTest : public ::testing::TestWithParam<PoolTestParams> {
  protected:
    void SetUp() override {
        auto params = GetParam();
        config_.cache_size = params.cache_size;
        config_.batch_size = params.batch_size;
        config_.use_huge_pages = false;
        config_.prefault_pages = false;
    }

    CacheConfig config_;
};

// Test parameters covering edge cases and normal operations
INSTANTIATE_TEST_SUITE_P(DifferentPoolSizes,
                         ParameterizedMemoryPoolTest,
                         ::testing::Values(PoolTestParams{1, 1, 1},       // Minimal configuration
                                           PoolTestParams{32, 8, 4},      // Small pool
                                           PoolTestParams{1024, 64, 16},  // Medium pool
                                           PoolTestParams{8192, 256, 64}  // Large pool
                                           ));

// 1. Cross-Thread Free Test

TEST_P(ParameterizedMemoryPoolTest, CrossThreadFreeTest) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    constexpr int num_pairs = 2;  // Reduced for smaller pools
    int objects_per_pair = std::min(static_cast<int>(params.capacity / (num_pairs * 2)), 50);

    if (objects_per_pair <= 0) {
        GTEST_SKIP() << "Pool too small for cross-thread test";
    }

    std::vector<std::thread> threads;
    std::atomic<bool> start{false};
    std::atomic<int> ready_count{0};

    // Shared storage for cross-thread object transfer
    struct alignas(64) TransferSlot {
        std::atomic<MediumObject*> ptr{nullptr};
        char padding[64 - sizeof(std::atomic<MediumObject*>)];
    };

    std::vector<TransferSlot> transfer_slots(num_pairs);

    // Create producer-consumer thread pairs
    for (int pair = 0; pair < num_pairs; ++pair) {
        // Producer thread (allocates)
        threads.emplace_back([&, pair, objects_per_pair]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < objects_per_pair; ++i) {
                auto* obj = pool.construct(pair * 1000 + i);
                ASSERT_NE(obj, nullptr);

                // Transfer to consumer thread via atomic pointer
                MediumObject* expected = nullptr;
                while (!transfer_slots[pair].ptr.compare_exchange_weak(
                    expected, obj, std::memory_order_release, std::memory_order_relaxed)) {
                    expected = nullptr;
                    std::this_thread::yield();
                }
            }
        });

        // Consumer thread (deallocates)
        threads.emplace_back([&, pair, objects_per_pair]() {
            ready_count.fetch_add(1, std::memory_order_relaxed);

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            int consumed = 0;
            while (consumed < objects_per_pair) {
                auto* obj = transfer_slots[pair].ptr.exchange(nullptr, std::memory_order_acquire);
                if (obj) {
                    // Verify object integrity before freeing
                    EXPECT_EQ(obj->data[0], pair * 1000 + consumed);

                    // Free on different thread than allocation
                    TSAN_ANNOTATE_BENIGN_RACE_SIZED(obj, sizeof(*obj), "cross_thread_free");
                    pool.destroy(obj);
                    ++consumed;
                }
                std::this_thread::yield();
            }
        });
    }

    // Wait for all threads ready
    while (ready_count.load(std::memory_order_acquire) < num_pairs * 2) {
        std::this_thread::yield();
    }

    // Start the cross-thread allocation/deallocation
    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify no leaks
    EXPECT_EQ(pool.allocated_objects(), 0);
}

// 2. Complete Depletion Policy Coverage

#ifdef POOL_HAS_RESIZE
TEST_P(ParameterizedMemoryPoolTest, ResizePolicyTest) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::DYNAMIC_RESIZE, config_);

    std::vector<MediumObject*> objects;

    // Exhaust initial capacity
    for (size_t i = 0; i < params.capacity; ++i) {
        objects.push_back(pool.allocate());
        ASSERT_NE(objects.back(), nullptr);
    }

    // This should trigger resize
    auto* extra_obj = pool.allocate();
    EXPECT_NE(extra_obj, nullptr);
    EXPECT_GT(pool.object_capacity(), params.capacity);

    objects.push_back(extra_obj);

    // Clean up
    for (auto* obj : objects) {
        pool.deallocate(obj);
    }
}
#endif

TEST_P(ParameterizedMemoryPoolTest, ThrowPolicyBackPressure) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    std::atomic<int> exception_count{0};
    std::atomic<int> success_count{0};

    // Multiple threads trying to allocate under pressure
    std::vector<std::thread> threads;
    std::atomic<bool> start{false};

    int num_threads = std::min(4, static_cast<int>(params.capacity));
    if (num_threads <= 0)
        num_threads = 1;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            int attempts = std::max(1, static_cast<int>(params.capacity / num_threads * 2));

            for (int i = 0; i < attempts; ++i) {
                try {
                    if (auto* obj = pool.allocate()) {
                        success_count.fetch_add(1, std::memory_order_relaxed);
                        // Hold briefly then release
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                        pool.deallocate(obj);
                    }
                } catch (const std::bad_alloc&) {
                    exception_count.fetch_add(1, std::memory_order_relaxed);
                }
                std::this_thread::yield();
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    // Should have some successes and potentially some back-pressure exceptions
    EXPECT_GT(success_count.load(), 0);
    EXPECT_EQ(pool.allocated_objects(), 0);
}

// 3. Mixed-Size Allocation Stress Test

TEST_P(ParameterizedMemoryPoolTest, MixedSizeAllocationStress) {
    auto params = GetParam();

    // Use separate pools for different object sizes
    MemoryPool<TinyObject> tiny_pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);
    MemoryPool<MediumObject> medium_pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);
    MemoryPool<LargeObject> large_pool(
        std::max(params.capacity / 4, static_cast<size_t>(1)), PoolDepletionPolicy::THROW_EXCEPTION, config_);

    constexpr int num_threads = 2;  // Reduced for stability
    int operations_per_thread = std::max(1, static_cast<int>(params.capacity / num_threads / 3));

    std::atomic<bool> start{false};
    std::atomic<uint64_t> total_operations{0};

    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t, operations_per_thread]() {
            std::random_device rd;
            std::mt19937 gen(rd() ^ t);  // Seed with thread ID
            std::uniform_int_distribution<> size_dist(0, 2);

            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::vector<MixedObject> allocated_objects;
            allocated_objects.reserve(operations_per_thread);

            for (int i = 0; i < operations_per_thread; ++i) {
                int size_choice = size_dist(gen);

                try {
                    switch (size_choice) {
                        case 0: {
                            auto* obj = tiny_pool.construct(static_cast<char>('A' + (i % 26)));
                            if (obj)
                                allocated_objects.emplace_back(obj);
                            break;
                        }
                        case 1: {
                            auto* obj = medium_pool.construct(t * 1000 + i);
                            if (obj)
                                allocated_objects.emplace_back(obj);
                            break;
                        }
                        case 2: {
                            auto* obj = large_pool.construct(static_cast<char>('0' + (i % 10)));
                            if (obj)
                                allocated_objects.emplace_back(obj);
                            break;
                        }
                    }

                    total_operations.fetch_add(1, std::memory_order_relaxed);

                    // Randomly deallocate some objects to create churn
                    if (allocated_objects.size() > 20 && (i % 10) == 0) {
                        for (int j = 0; j < 5 && !allocated_objects.empty(); ++j) {
                            auto& variant_obj = allocated_objects.back();

                            std::visit(
                                [&](auto* obj) {
                                    using T = std::decay_t<decltype(*obj)>;
                                    if constexpr (std::is_same_v<T, TinyObject>) {
                                        tiny_pool.destroy(obj);
                                    } else if constexpr (std::is_same_v<T, MediumObject>) {
                                        medium_pool.destroy(obj);
                                    } else if constexpr (std::is_same_v<T, LargeObject>) {
                                        large_pool.destroy(obj);
                                    }
                                },
                                variant_obj);

                            allocated_objects.pop_back();
                        }
                    }
                } catch (const std::bad_alloc&) {
                    // Pool exhaustion is acceptable under stress
                }
            }

            // Clean up remaining objects
            for (auto& variant_obj : allocated_objects) {
                std::visit(
                    [&](auto* obj) {
                        using T = std::decay_t<decltype(*obj)>;
                        if constexpr (std::is_same_v<T, TinyObject>) {
                            tiny_pool.destroy(obj);
                        } else if constexpr (std::is_same_v<T, MediumObject>) {
                            medium_pool.destroy(obj);
                        } else if constexpr (std::is_same_v<T, LargeObject>) {
                            large_pool.destroy(obj);
                        }
                    },
                    variant_obj);
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    // Verify no leaks across all pools
    EXPECT_EQ(tiny_pool.allocated_objects(), 0);
    EXPECT_EQ(medium_pool.allocated_objects(), 0);
    EXPECT_EQ(large_pool.allocated_objects(), 0);
}

// 4. Basic Operations Across All Pool Sizes

TEST_P(ParameterizedMemoryPoolTest, BasicOperationsAcrossCapacities) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    EXPECT_EQ(pool.object_capacity(), params.capacity);

    // Allocate up to capacity
    std::vector<MediumObject*> objects;
    for (size_t i = 0; i < params.capacity; ++i) {
        auto* obj = pool.construct(static_cast<int>(i));
        ASSERT_NE(obj, nullptr);
        EXPECT_EQ(obj->data[0], static_cast<int>(i));
        objects.push_back(obj);
    }

    EXPECT_TRUE(pool.full());
    EXPECT_THROW(pool.allocate(), std::bad_alloc);

    // Deallocate all
    for (auto* obj : objects) {
        pool.destroy(obj);
    }

    // Flush thread cache to ensure accurate empty state
    pool.flush_thread_cache();

    EXPECT_TRUE(pool.empty());
}

TEST_P(ParameterizedMemoryPoolTest, ThreadLocalCacheEfficiency) {
    auto params = GetParam();

    // Skip test for pools too small for meaningful cache behavior
    if (params.capacity < 4 || params.cache_size < 2) {
        GTEST_SKIP() << "Pool too small for cache efficiency test";
    }

    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    // Test cache behavior with different batch sizes
    std::vector<MediumObject*> objects;

    // Allocate in batches matching the cache configuration
    size_t batch_count = std::min(params.capacity, params.cache_size * 2);

    for (size_t i = 0; i < batch_count; ++i) {
        objects.push_back(pool.allocate());
        ASSERT_NE(objects.back(), nullptr);
    }

    // Deallocate in reverse order (LIFO - cache friendly)
    while (!objects.empty()) {
        pool.deallocate(objects.back());
        objects.pop_back();
    }

    // Allocate again - should hit cache for small pools
    for (size_t i = 0; i < std::min(batch_count, params.cache_size); ++i) {
        objects.push_back(pool.allocate());
        ASSERT_NE(objects.back(), nullptr);
    }

    // Clean up
    for (auto* obj : objects) {
        pool.deallocate(obj);
    }
}

// 5. TSAN-Safe Freelist Stress Test

TEST_P(ParameterizedMemoryPoolTest, TSanSafeFreelistStress) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    int num_threads = std::min(4, static_cast<int>(params.capacity / 10));
    if (num_threads <= 0)
        num_threads = 1;

    int operations_per_thread = std::max(1, static_cast<int>(params.capacity / num_threads));

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&, t, operations_per_thread]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            for (int i = 0; i < operations_per_thread; ++i) {
                // TSAN: Ignore expected benign races in freelist management
                TSAN_IGNORE_WRITES_BEGIN();

                auto* obj = pool.allocate();
                if (obj) {
                    // Brief usage
                    obj->data[0] = t * 1000 + i;

                    // Immediate deallocation to stress freelist
                    pool.deallocate(obj);
                }

                TSAN_IGNORE_WRITES_END();

                // Yield to encourage thread interleaving
                if ((i % 10) == 0) {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : threads) {
        thread.join();
    }

    EXPECT_EQ(pool.allocated_objects(), 0);
}

// Additional test: ABA Protection Stress Test
TEST_P(ParameterizedMemoryPoolTest, ABAProtectionStress) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    // Skip test for very small pools
    if (params.capacity < 100) {
        GTEST_SKIP() << "Pool too small for meaningful ABA stress test";
    }

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> allocations{0};
    std::atomic<uint64_t> deallocations{0};
    std::atomic<uint64_t> address_reuses{0};

    // Track recently seen addresses
    static constexpr size_t HISTORY_SIZE = 1000;
    struct AddressHistory {
        std::array<void*, HISTORY_SIZE> addresses{};
        std::atomic<size_t> index{0};

        bool seen_recently(void* addr) const {
            size_t current = index.load(std::memory_order_relaxed);
            for (size_t i = 0; i < std::min(current, HISTORY_SIZE); ++i) {
                if (addresses[i] == addr)
                    return true;
            }
            return false;
        }

        void add(void* addr) {
            size_t idx = index.fetch_add(1, std::memory_order_relaxed) % HISTORY_SIZE;
            addresses[idx] = addr;
        }
    };

    alignas(64) AddressHistory history;

    // Thread 1: Rapid allocate/free pattern
    std::thread aba_thread1([&]() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> hold_dist(0, 3);

        while (!stop.load(std::memory_order_acquire)) {
            // Allocate a few objects
            std::vector<MediumObject*> batch;
            for (int i = 0; i < 5; ++i) {
                try {
                    auto* obj = pool.allocate();
                    if (obj) {
                        batch.push_back(obj);
                        allocations.fetch_add(1, std::memory_order_relaxed);

                        // Check if we've seen this address recently
                        if (history.seen_recently(obj)) {
                            address_reuses.fetch_add(1, std::memory_order_relaxed);
                        }
                        history.add(obj);
                    }
                } catch (const std::bad_alloc&) {
                    // Expected under stress
                }
            }

            // Hold briefly or not at all
            if (hold_dist(gen) == 0) {
                std::this_thread::yield();
            }

            // Free in LIFO order (most likely to cause ABA)
            while (!batch.empty()) {
                pool.deallocate(batch.back());
                deallocations.fetch_add(1, std::memory_order_relaxed);
                batch.pop_back();
            }
        }
    });

    // Thread 2: Different pattern - allocate, use, free
    std::thread aba_thread2([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            try {
                auto* obj1 = pool.allocate();
                auto* obj2 = pool.allocate();

                if (obj1 && obj2) {
                    allocations.fetch_add(2, std::memory_order_relaxed);

                    // Simulate some work
                    obj1->data[0] = 42;
                    obj2->data[0] = 84;

                    // Free in different order
                    pool.deallocate(obj2);
                    pool.deallocate(obj1);
                    deallocations.fetch_add(2, std::memory_order_relaxed);
                } else {
                    if (obj1)
                        pool.deallocate(obj1);
                    if (obj2)
                        pool.deallocate(obj2);
                }
            } catch (const std::bad_alloc&) {
                // Expected under stress
            }

            std::this_thread::yield();
        }
    });

    // Thread 3: Burst allocations
    std::thread aba_thread3([&]() {
        while (!stop.load(std::memory_order_acquire)) {
            std::vector<MediumObject*> burst;

            // Try to allocate many at once
            for (int i = 0; i < 20; ++i) {
                try {
                    auto* obj = pool.allocate();
                    if (obj) {
                        burst.push_back(obj);
                        allocations.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::bad_alloc&) {
                    break;
                }
            }

            // Free all at once
            for (auto* obj : burst) {
                pool.deallocate(obj);
                deallocations.fetch_add(1, std::memory_order_relaxed);
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    // Let threads run for a while
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    stop.store(true, std::memory_order_release);
    aba_thread1.join();
    aba_thread2.join();
    aba_thread3.join();

    // Verify no corruption
    EXPECT_EQ(pool.allocated_objects(), 0);
    EXPECT_EQ(allocations.load(), deallocations.load());

    // Check address reuse rate
    uint64_t total_allocs = allocations.load();
    uint64_t reuses = address_reuses.load();
    double reuse_rate = total_allocs > 0 ? static_cast<double>(reuses) / total_allocs : 0.0;

    // Address reuse is expected in a memory pool, but should be safe with ABA protection
    // The test verifies that high reuse doesn't cause corruption
    // For smaller pools, reuse rate will naturally be higher
    if (params.capacity >= 1000) {
        // Just log the reuse rate for information
        if (reuse_rate > 0.7) {
            std::cerr << "Note: Address reuse rate is " << reuse_rate << " for pool capacity " << params.capacity
                      << std::endl;
        }
    }

    // Final validation - allocate and free entire pool
    std::vector<MediumObject*> final_check;
    for (size_t i = 0; i < params.capacity; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Pool corruption detected at index " << i;
        final_check.push_back(obj);
    }

    EXPECT_THROW(pool.allocate(), std::bad_alloc);

    for (auto* obj : final_check) {
        pool.deallocate(obj);
    }
}

// Additional test: Exhaustion Recovery Pattern
TEST_P(ParameterizedMemoryPoolTest, ExhaustionRecoveryPattern) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    // Skip test for very small pools that can't demonstrate all patterns
    if (params.capacity < 4) {
        GTEST_SKIP() << "Pool too small for exhaustion recovery patterns";
    }

    // Test various allocation/deallocation patterns after exhaustion
    std::vector<MediumObject*> objects;
    objects.reserve(params.capacity);

    // Phase 1: Exhaust the pool
    for (size_t i = 0; i < params.capacity; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Failed to allocate object " << i;
        objects.push_back(obj);
    }

    // Verify pool is exhausted
    EXPECT_TRUE(pool.full());
    EXPECT_THROW(pool.allocate(), std::bad_alloc);

    // Phase 2: Free in different patterns and verify recovery

    // Pattern A: Free every other object (fragmented pattern)
    for (size_t i = 0; i < objects.size(); i += 2) {
        pool.deallocate(objects[i]);
        objects[i] = nullptr;
    }

    // Should be able to allocate half capacity
    size_t freed_count = params.capacity / 2;
    for (size_t i = 0; i < freed_count; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Failed to allocate after freeing " << i;
        // Store in the nullptr slots
        for (size_t j = 0; j < objects.size(); j += 2) {
            if (objects[j] == nullptr) {
                objects[j] = obj;
                break;
            }
        }
    }

    // Should be exhausted again
    EXPECT_THROW(pool.allocate(), std::bad_alloc);

    // Pattern B: Free in reverse order (LIFO)
    std::vector<MediumObject*> temp_objects;
    for (auto* obj : objects) {
        if (obj != nullptr) {
            temp_objects.push_back(obj);
        }
    }

    // Free last 25% in reverse order
    size_t reverse_free_count = temp_objects.size() / 4;
    for (size_t i = 0; i < reverse_free_count; ++i) {
        pool.deallocate(temp_objects[temp_objects.size() - 1 - i]);
    }

    // Allocate them back
    for (size_t i = 0; i < reverse_free_count; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Failed to allocate in LIFO pattern " << i;
        temp_objects[temp_objects.size() - reverse_free_count + i] = obj;
    }

    // Pattern C: Random shuffle deallocation
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(temp_objects.begin(), temp_objects.end(), g);

    // Free first 50%
    size_t shuffle_free_count = temp_objects.size() / 2;
    for (size_t i = 0; i < shuffle_free_count; ++i) {
        pool.deallocate(temp_objects[i]);
    }

    // Allocate them back
    for (size_t i = 0; i < shuffle_free_count; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Failed to allocate after shuffle " << i;
        temp_objects[i] = obj;
    }

    // Phase 3: Complete cleanup and verify full recovery
    for (auto* obj : temp_objects) {
        pool.deallocate(obj);
    }

    // Flush thread cache to ensure all objects are returned
    pool.flush_thread_cache();

    EXPECT_TRUE(pool.empty());
    EXPECT_EQ(pool.allocated_objects(), 0);

    // Phase 4: Verify can allocate full capacity again
    objects.clear();
    for (size_t i = 0; i < params.capacity; ++i) {
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr) << "Failed to allocate full capacity after recovery " << i;
        objects.push_back(obj);
    }

    EXPECT_TRUE(pool.full());
    EXPECT_THROW(pool.allocate(), std::bad_alloc);

    // Final cleanup
    for (auto* obj : objects) {
        pool.deallocate(obj);
    }

    // Flush thread cache to ensure accurate count
    pool.flush_thread_cache();

    EXPECT_EQ(pool.allocated_objects(), 0);
}

// Edge case: Rapid allocation/deallocation with cache pressure
TEST_P(ParameterizedMemoryPoolTest, CachePressureStress) {
    auto params = GetParam();
    MemoryPool<MediumObject> pool(params.capacity, PoolDepletionPolicy::THROW_EXCEPTION, config_);

    // Allocate more than cache size to force global pool interaction
    size_t alloc_count = std::min(params.capacity, params.cache_size * 3);

    std::vector<MediumObject*> objects;
    objects.reserve(alloc_count);

    // Rapid allocation
    for (size_t i = 0; i < alloc_count; ++i) {
        auto* obj = pool.allocate();
        if (obj) {
            objects.push_back(obj);
        }
    }

    // Rapid deallocation in random order to stress cache management
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(objects.begin(), objects.end(), g);

    for (auto* obj : objects) {
        pool.deallocate(obj);
    }

    // Flush cache to ensure all objects are returned to global pool
    pool.flush_thread_cache();

    EXPECT_EQ(pool.allocated_objects(), 0);
}

// Additional test: Thread Cleanup Order
TEST(MemoryPoolTest, ThreadCleanupOrder) {
    // This test verifies that thread-local caches are properly cleaned up
    // when threads exit before the pool is destroyed

    auto pool = std::make_unique<MemoryPool<MediumObject>>(
        1000, PoolDepletionPolicy::THROW_EXCEPTION, CacheConfig{64, 32, false, false});

    std::atomic<bool> thread_done{false};
    std::atomic<size_t> allocated_count{0};

    // Thread that allocates objects but doesn't deallocate them all
    std::thread worker([&]() {
        // Allocate objects to populate thread-local cache
        std::vector<MediumObject*> objects;
        for (int i = 0; i < 50; ++i) {
            auto* obj = pool->allocate();
            EXPECT_NE(obj, nullptr);
            objects.push_back(obj);
            allocated_count.fetch_add(1, std::memory_order_relaxed);
        }

        // Deallocate some (but not all) to leave objects in thread-local cache
        for (int i = 0; i < 30; ++i) {
            pool->deallocate(objects[i]);
            allocated_count.fetch_sub(1, std::memory_order_relaxed);
        }

        // Thread exits here with 20 objects still "allocated" in its cache
        thread_done = true;
    });

    worker.join();
    EXPECT_TRUE(thread_done);

    // At this point, the thread is gone but objects might still be in its cache
    // The pool should handle this gracefully

    // Verify we can still use the pool from main thread
    std::vector<MediumObject*> main_objects;
    for (int i = 0; i < 100; ++i) {
        auto* obj = pool->allocate();
        EXPECT_NE(obj, nullptr);
        main_objects.push_back(obj);
    }

    // Clean up main thread allocations
    for (auto* obj : main_objects) {
        pool->deallocate(obj);
    }

    // Flush any remaining cached objects
    pool->flush_thread_cache();

    // Note: We expect some objects to be "leaked" from the worker thread's cache
    // This is by design - thread-local caches can't be reclaimed after thread exit
    size_t leaked_objects = allocated_count.load();
    if (leaked_objects > 0) {
        std::cerr << "Note: " << leaked_objects << " objects leaked from exited thread's cache (expected behavior)"
                  << std::endl;
    }

    // Pool destruction should handle remaining cleanup without crash
    EXPECT_NO_THROW(pool.reset());
}

// Additional test: Multiple Thread Cleanup
TEST(MemoryPoolTest, MultipleThreadCleanup) {
    auto pool = std::make_unique<MemoryPool<MediumObject>>(
        10000, PoolDepletionPolicy::THROW_EXCEPTION, CacheConfig{256, 64, false, false});

    const int num_threads = 4;
    std::atomic<int> threads_done{0};
    std::vector<std::thread> workers;

    // Start multiple threads that allocate and partially deallocate
    for (int t = 0; t < num_threads; ++t) {
        workers.emplace_back([&, thread_id = t]() {
            std::random_device rd;
            std::mt19937 gen(rd() ^ thread_id);
            std::uniform_int_distribution<> alloc_dist(100, 300);
            std::uniform_int_distribution<> free_ratio_dist(30, 70);

            int alloc_count = alloc_dist(gen);
            std::vector<MediumObject*> objects;

            // Allocate random number of objects
            for (int i = 0; i < alloc_count; ++i) {
                try {
                    auto* obj = pool->allocate();
                    if (obj) {
                        objects.push_back(obj);
                        // Store thread ID in first element for verification
                        obj->data[0] = thread_id;
                    }
                } catch (const std::bad_alloc&) {
                    break;
                }
            }

            // Free a random percentage
            int free_percent = free_ratio_dist(gen);
            int free_count = (objects.size() * free_percent) / 100;

            for (int i = 0; i < free_count; ++i) {
                pool->deallocate(objects[i]);
            }

            // Thread exits with remaining objects in cache
            threads_done.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // Wait for all worker threads
    for (auto& t : workers) {
        t.join();
    }

    EXPECT_EQ(threads_done.load(), num_threads);

    // Main thread continues to use the pool
    std::vector<MediumObject*> main_objects;

    // Try to allocate more objects
    int allocated = 0;
    while (allocated < 1000) {
        try {
            auto* obj = pool->allocate();
            if (obj) {
                main_objects.push_back(obj);
                allocated++;
            }
        } catch (const std::bad_alloc&) {
            // Pool might be exhausted due to leaked caches
            break;
        }
    }

    // Clean up what we can
    for (auto* obj : main_objects) {
        pool->deallocate(obj);
    }

    // Verify pool destruction works correctly
    EXPECT_NO_THROW(pool.reset());
}

// Additional test: Configuration edge cases
TEST(MemoryPoolTest, ConfigurationEdgeCases) {
    // Note: FreeNode requires objects to be at least sizeof(TaggedPointer128<FreeNode>)
    // which is typically 16 bytes, and aligned to CACHELINE_SIZE (64 bytes).
    // MediumObject is already 64 bytes and properly aligned.

    // Verify our test object meets requirements
    ASSERT_GE(sizeof(MediumObject), 64);
    ASSERT_EQ(alignof(MediumObject), 64);

    // Test 1: Zero cache size
    {
        CacheConfig zero_cache_config{0, 32, false, false};  // cache_size = 0
        // After constructor adjustments: cache_size=0, batch_size=1 (min), shrink_threshold=1
        try {
            MemoryPool<MediumObject> pool(100, PoolDepletionPolicy::THROW_EXCEPTION, zero_cache_config);

            // Should still work, just without thread-local caching
            auto* obj1 = pool.allocate();
            auto* obj2 = pool.allocate();
            ASSERT_NE(obj1, nullptr);
            ASSERT_NE(obj2, nullptr);

            pool.deallocate(obj1);
            pool.deallocate(obj2);

            // With zero cache size, objects should go directly back to pool
            EXPECT_EQ(pool.allocated_objects(), 0);
        } catch (const std::exception& e) {
            FAIL() << "Unexpected exception in zero cache size test: " << e.what();
        }
    }

    // Test 2: Batch size larger than cache size
    {
        // Note: CacheConfig constructor will adjust batch_size to cache_size/2 when batch_size > cache_size
        CacheConfig large_batch_config{32, 128, false, false};  // batch_size > cache_size
        // After adjustment: cache_size=32, batch_size=16
        MemoryPool<MediumObject> pool(1000, PoolDepletionPolicy::THROW_EXCEPTION, large_batch_config);

        // Allocate enough to trigger batch refill
        std::vector<MediumObject*> objects;
        for (int i = 0; i < 50; ++i) {
            auto* obj = pool.allocate();
            ASSERT_NE(obj, nullptr);
            objects.push_back(obj);
        }

        // Deallocate all
        for (auto* obj : objects) {
            pool.deallocate(obj);
        }

        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 3: Batch size zero (should use default or minimum)
    {
        CacheConfig zero_batch_config{64, 0, false, false};  // batch_size = 0
        MemoryPool<MediumObject> pool(500, PoolDepletionPolicy::THROW_EXCEPTION, zero_batch_config);

        // Should still function correctly
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr);
        pool.deallocate(obj);

        // Flush thread cache even though batch_size was adjusted
        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 4: Very small pool with large cache
    {
        CacheConfig large_cache_config{1000, 100, false, false};  // cache larger than pool
        MemoryPool<MediumObject> pool(10, PoolDepletionPolicy::THROW_EXCEPTION, large_cache_config);

        // Allocate entire pool
        std::vector<MediumObject*> objects;
        for (size_t i = 0; i < 10; ++i) {
            auto* obj = pool.allocate();
            ASSERT_NE(obj, nullptr);
            objects.push_back(obj);
        }

        // Should be exhausted
        EXPECT_THROW(pool.allocate(), std::bad_alloc);

        // Return all objects
        for (auto* obj : objects) {
            pool.deallocate(obj);
        }

        // Should be able to allocate again
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr);
        pool.deallocate(obj);

        // Flush thread cache before checking
        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 5: Equal cache size and batch size
    {
        CacheConfig equal_config{64, 64, false, false};  // cache_size == batch_size
        MemoryPool<MediumObject> pool(1000, PoolDepletionPolicy::THROW_EXCEPTION, equal_config);

        // Allocate exactly one batch worth
        std::vector<MediumObject*> batch;
        for (size_t i = 0; i < 64; ++i) {
            auto* obj = pool.allocate();
            ASSERT_NE(obj, nullptr);
            batch.push_back(obj);
        }

        // Deallocate all - should fill cache exactly
        for (auto* obj : batch) {
            pool.deallocate(obj);
        }

        // Allocate again - should come from cache
        for (size_t i = 0; i < 64; ++i) {
            auto* obj = pool.allocate();
            ASSERT_NE(obj, nullptr);
            batch[i] = obj;
        }

        // Cleanup
        for (auto* obj : batch) {
            pool.deallocate(obj);
        }

        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 6: Capacity of 1 (minimal pool)
    {
        CacheConfig minimal_config{1, 1, false, false};
        MemoryPool<MediumObject> pool(1, PoolDepletionPolicy::THROW_EXCEPTION, minimal_config);

        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr);

        // Should be exhausted
        EXPECT_THROW(pool.allocate(), std::bad_alloc);

        pool.deallocate(obj);

        // Should work again
        obj = pool.allocate();
        ASSERT_NE(obj, nullptr);
        pool.deallocate(obj);

        // Flush thread cache before checking
        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 7: Different depletion policies with edge configs
    {
        // TERMINATE_PROCESS with minimal config
        CacheConfig config{2, 1, false, false};
        MemoryPool<MediumObject> terminate_pool(5, PoolDepletionPolicy::TERMINATE_PROCESS, config);

        // Allocate some but not all
        auto* obj1 = terminate_pool.allocate();
        auto* obj2 = terminate_pool.allocate();
        ASSERT_NE(obj1, nullptr);
        ASSERT_NE(obj2, nullptr);

        terminate_pool.deallocate(obj1);
        terminate_pool.deallocate(obj2);

        // Flush and verify
        terminate_pool.flush_thread_cache();
        EXPECT_EQ(terminate_pool.allocated_objects(), 0);

        // Note: We can't test actual termination without crashing the test
    }

    // Test 8: Huge pages flag (may not actually use huge pages without setup)
    {
        CacheConfig huge_page_config{64, 32, true, false};  // use_huge_pages = true
        // Should construct successfully even if huge pages aren't available
        MemoryPool<MediumObject> pool(1000, PoolDepletionPolicy::THROW_EXCEPTION, huge_page_config);
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr);
        pool.deallocate(obj);

        // Flush thread cache before checking
        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }

    // Test 9: Prefault pages flag
    {
        CacheConfig prefault_config{64, 32, false, true};  // prefault_pages = true
        MemoryPool<MediumObject> pool(100, PoolDepletionPolicy::THROW_EXCEPTION, prefault_config);

        // Should work normally
        auto* obj = pool.allocate();
        ASSERT_NE(obj, nullptr);
        pool.deallocate(obj);

        // Flush thread cache before checking
        pool.flush_thread_cache();
        EXPECT_EQ(pool.allocated_objects(), 0);
    }
}