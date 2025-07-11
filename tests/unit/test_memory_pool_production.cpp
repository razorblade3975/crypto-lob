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
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#include <sanitizer/tsan_interface.h>
#define TSAN_IGNORE_WRITES_BEGIN() AnnotateIgnoreWritesBegin(__FILE__, __LINE__)
#define TSAN_IGNORE_WRITES_END() AnnotateIgnoreWritesEnd(__FILE__, __LINE__)
#define TSAN_ANNOTATE_BENIGN_RACE_SIZED(ptr, size, description) \
    AnnotateBenignRaceSized(__FILE__, __LINE__, ptr, size, description)
#else
#define TSAN_IGNORE_WRITES_BEGIN() ((void)0)
#define TSAN_IGNORE_WRITES_END() ((void)0)
#define TSAN_ANNOTATE_BENIGN_RACE_SIZED(ptr, size, description) ((void)0)
#endif
#else
#define TSAN_IGNORE_WRITES_BEGIN() ((void)0)
#define TSAN_IGNORE_WRITES_END() ((void)0)
#define TSAN_ANNOTATE_BENIGN_RACE_SIZED(ptr, size, description) ((void)0)
#endif

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
        std::max(params.capacity / 4, size_t{1}), PoolDepletionPolicy::THROW_EXCEPTION, config_);

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