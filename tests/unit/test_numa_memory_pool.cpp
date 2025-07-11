#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/numa_memory_pool.hpp"

using namespace crypto_lob::core;

struct alignas(64) NumaTestObject {
    char data[192];
    NumaTestObject() = default;
};

class NumaMemoryPoolTest : public ::testing::Test {
  protected:
    static constexpr size_t POOL_SIZE = 10000;
    std::unique_ptr<NumaMemoryPool<NumaTestObject>> pool;

    void SetUp() override {
        pool = std::make_unique<NumaMemoryPool<NumaTestObject>>(POOL_SIZE);
    }
};

TEST_F(NumaMemoryPoolTest, BasicAllocation) {
    auto* obj = pool->allocate();
    ASSERT_NE(obj, nullptr);

    pool->deallocate(obj);
    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(NumaMemoryPoolTest, ConstructDestroy) {
    auto* obj = pool->construct();
    ASSERT_NE(obj, nullptr);

    pool->destroy(obj);
    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(NumaMemoryPoolTest, BatchOperations) {
    auto batch = pool->allocate_batch(100);
    EXPECT_EQ(batch.size(), 100);

    // Verify all pointers are unique
    std::set<NumaTestObject*> unique_ptrs(batch.begin(), batch.end());
    EXPECT_EQ(unique_ptrs.size(), 100);

    pool->deallocate_batch(batch);
    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(NumaMemoryPoolTest, CapacityAndUtilization) {
    EXPECT_EQ(pool->object_capacity(), POOL_SIZE);
    EXPECT_EQ(pool->utilization(), 0.0);

    std::vector<NumaTestObject*> objects;
    for (size_t i = 0; i < 100; ++i) {
        objects.push_back(pool->allocate());
    }

    // Allow for thread-local cache overhead
    EXPECT_GE(pool->allocated_objects(), 100);
    EXPECT_LE(pool->allocated_objects(), 200);
    EXPECT_GT(pool->utilization(), 0.0);
    EXPECT_LT(pool->utilization(), 0.02);  // Less than 2%

    for (auto* obj : objects) {
        pool->deallocate(obj);
    }

    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(NumaMemoryPoolTest, NodeStats) {
    auto stats = pool->get_node_stats();
    EXPECT_GE(stats.size(), 1);  // At least one node

    size_t total_capacity = 0;
    for (const auto& node_stat : stats) {
        EXPECT_GE(node_stat.node_id, 0);
        EXPECT_EQ(node_stat.allocated, 0);
        EXPECT_GT(node_stat.capacity, 0);
        EXPECT_EQ(node_stat.utilization, 0.0);
        total_capacity += node_stat.capacity;
    }

    EXPECT_EQ(total_capacity, POOL_SIZE);
}

TEST_F(NumaMemoryPoolTest, NumaAwareness) {
    // This test just checks the API, actual NUMA behavior depends on hardware
    size_t num_nodes = pool->num_nodes();
    EXPECT_GE(num_nodes, 1);

    // On non-NUMA systems or in containers, we expect a single node
    if (!pool->is_numa_aware()) {
        EXPECT_EQ(num_nodes, 1);
    }

    std::cout << "NUMA awareness: " << (pool->is_numa_aware() ? "Yes" : "No") << std::endl;
    std::cout << "Number of NUMA nodes: " << num_nodes << std::endl;
}

TEST_F(NumaMemoryPoolTest, CrossNodeDeallocation) {
    // Allocate objects
    std::vector<NumaTestObject*> objects;
    for (size_t i = 0; i < 50; ++i) {
        objects.push_back(pool->allocate());
    }

    // Deallocate from a different thread (potentially different NUMA node)
    std::thread deallocator([this, &objects]() {
        for (auto* obj : objects) {
            pool->deallocate(obj);
        }
    });

    deallocator.join();
    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(NumaMemoryPoolTest, MultithreadedStress) {
    const size_t NUM_THREADS = 4;
    const size_t OPS_PER_THREAD = 1000;

    std::vector<std::thread> threads;
    std::atomic<size_t> total_allocated{0};

    for (size_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, &total_allocated]() {
            // Optionally pin to NUMA node
            // NumaMemoryPool<NumaTestObject>::pin_thread_to_node(t % pool->num_nodes());

            std::vector<NumaTestObject*> local_objects;

            for (size_t i = 0; i < OPS_PER_THREAD; ++i) {
                if (i % 2 == 0) {
                    // Allocate
                    auto* obj = pool->allocate();
                    if (obj) {
                        local_objects.push_back(obj);
                        total_allocated.fetch_add(1);
                    }
                } else if (!local_objects.empty()) {
                    // Deallocate
                    pool->deallocate(local_objects.back());
                    local_objects.pop_back();
                    total_allocated.fetch_sub(1);
                }
            }

            // Clean up remaining
            for (auto* obj : local_objects) {
                pool->deallocate(obj);
                total_allocated.fetch_sub(1);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
    EXPECT_EQ(total_allocated.load(), 0);
}

TEST_F(NumaMemoryPoolTest, MixedBatchAndSingle) {
    // Single allocations
    std::vector<NumaTestObject*> singles;
    for (int i = 0; i < 20; ++i) {
        singles.push_back(pool->allocate());
    }

    // Batch allocation
    auto batch = pool->allocate_batch(50);
    EXPECT_EQ(batch.size(), 50);

    // Total should be 70
    EXPECT_GE(pool->allocated_objects(), 70);

    // Deallocate all
    for (auto* obj : singles) {
        pool->deallocate(obj);
    }
    pool->deallocate_batch(batch);

    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

// Performance comparison test (optional, only runs with --gtest_also_run_disabled_tests)
TEST_F(NumaMemoryPoolTest, DISABLED_PerformanceComparison) {
    const size_t ITERATIONS = 100000;

    // Time NUMA-aware pool
    auto start_numa = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        auto* obj = pool->allocate();
        pool->deallocate(obj);
    }
    auto end_numa = std::chrono::high_resolution_clock::now();

    // Time regular pool
    MemoryPool<NumaTestObject> regular_pool(POOL_SIZE);
    auto start_regular = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        auto* obj = regular_pool.allocate();
        regular_pool.deallocate(obj);
    }
    auto end_regular = std::chrono::high_resolution_clock::now();

    auto numa_time = std::chrono::duration_cast<std::chrono::microseconds>(end_numa - start_numa).count();
    auto regular_time = std::chrono::duration_cast<std::chrono::microseconds>(end_regular - start_regular).count();

    std::cout << "NUMA-aware pool time: " << numa_time << " us\n";
    std::cout << "Regular pool time: " << regular_time << " us\n";
    std::cout << "Overhead: " << (numa_time > regular_time ? "+" : "")
              << (static_cast<double>(numa_time - regular_time) / regular_time * 100) << "%\n";
}