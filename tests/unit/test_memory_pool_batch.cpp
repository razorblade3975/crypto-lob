#include <atomic>
#include <chrono>
#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/memory_pool.hpp"

using namespace crypto_lob::core;

struct alignas(64) BatchTestObject {
    char data[64];
};

class MemoryPoolBatchTest : public ::testing::Test {
  protected:
    static constexpr size_t POOL_SIZE = 1000;
    std::unique_ptr<MemoryPool<BatchTestObject>> pool;

    void SetUp() override {
        pool = std::make_unique<MemoryPool<BatchTestObject>>(POOL_SIZE);
    }
};

TEST_F(MemoryPoolBatchTest, BasicBatchAllocation) {
    // Test batch allocation
    auto batch = pool->allocate_batch(10);
    EXPECT_EQ(batch.size(), 10);

    // Verify all pointers are unique
    std::set<BatchTestObject*> unique_ptrs(batch.begin(), batch.end());
    EXPECT_EQ(unique_ptrs.size(), 10);

    // Deallocate batch
    pool->deallocate_batch(batch);

    // After flush, should have 0 allocated
    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, BatchConstructDestroy) {
    // Test batch construction
    auto batch = pool->construct_batch(20, BatchTestObject{});
    EXPECT_EQ(batch.size(), 20);

    // All objects should be constructed
    for (auto* obj : batch) {
        EXPECT_NE(obj, nullptr);
    }

    // Destroy batch
    pool->destroy_batch(batch);

    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, LargeBatchAllocation) {
    // Allocate more than typical cache size
    auto batch = pool->allocate_batch(200);
    EXPECT_EQ(batch.size(), 200);

    // Should use both cache and direct pool allocation
    // Note: allocated_objects() may be higher due to thread-local cache prefetching
    EXPECT_GE(pool->allocated_objects(), 200);
    EXPECT_LE(pool->allocated_objects(), 300);  // Allow for some cache overhead

    pool->deallocate_batch(batch);
    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, BatchAllocationExhaustion) {
    // Try to allocate more than pool capacity
    auto batch = pool->allocate_batch(POOL_SIZE + 100);

    // Should get less than requested
    EXPECT_LE(batch.size(), POOL_SIZE);
    EXPECT_GT(batch.size(), 0);  // But should get something

    pool->deallocate_batch(batch);
    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, MixedBatchAndSingleAllocation) {
    // Allocate some singles
    std::vector<BatchTestObject*> singles;
    for (int i = 0; i < 10; ++i) {
        singles.push_back(pool->allocate());
    }

    // Allocate batch
    auto batch = pool->allocate_batch(30);
    EXPECT_EQ(batch.size(), 30);

    // Total should be 40
    EXPECT_EQ(pool->allocated_objects(), 40);

    // Deallocate all
    for (auto* obj : singles) {
        pool->deallocate(obj);
    }
    pool->deallocate_batch(batch);

    pool->flush_thread_cache();
    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, BatchPerformanceComparison) {
    const size_t BATCH_SIZE = 100;
    const size_t ITERATIONS = 1000;

    // Time individual allocations
    auto start_individual = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        std::vector<BatchTestObject*> ptrs;
        for (size_t j = 0; j < BATCH_SIZE; ++j) {
            ptrs.push_back(pool->allocate());
        }
        for (auto* ptr : ptrs) {
            pool->deallocate(ptr);
        }
    }
    auto end_individual = std::chrono::high_resolution_clock::now();

    pool->flush_thread_cache();

    // Time batch allocations
    auto start_batch = std::chrono::high_resolution_clock::now();
    for (size_t i = 0; i < ITERATIONS; ++i) {
        auto batch = pool->allocate_batch(BATCH_SIZE);
        pool->deallocate_batch(batch);
    }
    auto end_batch = std::chrono::high_resolution_clock::now();

    auto individual_time =
        std::chrono::duration_cast<std::chrono::microseconds>(end_individual - start_individual).count();
    auto batch_time = std::chrono::duration_cast<std::chrono::microseconds>(end_batch - start_batch).count();

    // Batch should be faster (or at least not significantly slower)
    std::cout << "Individual time: " << individual_time << " us\n";
    std::cout << "Batch time: " << batch_time << " us\n";
    std::cout << "Speedup: " << static_cast<double>(individual_time) / batch_time << "x\n";

    // We expect batch to be at least as fast, if not faster
    EXPECT_LE(batch_time, individual_time * 1.1);  // Allow 10% margin
}

TEST_F(MemoryPoolBatchTest, EmptyBatchOperations) {
    // Empty batch allocation
    auto empty_batch = pool->allocate_batch(0);
    EXPECT_EQ(empty_batch.size(), 0);

    // Empty batch deallocation (should not crash)
    pool->deallocate_batch(empty_batch);

    // Empty vector deallocation
    std::vector<BatchTestObject*> empty_vec;
    pool->deallocate_batch(empty_vec);

    EXPECT_EQ(pool->allocated_objects(), 0);
}

TEST_F(MemoryPoolBatchTest, ThreadedBatchOperations) {
    const size_t NUM_THREADS = 4;
    const size_t BATCH_SIZE = 50;
    const size_t ITERATIONS = 100;

    std::vector<std::thread> threads;
    std::atomic<size_t> total_allocated{0};

    for (size_t t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([this, &total_allocated]() {
            for (size_t i = 0; i < ITERATIONS; ++i) {
                auto batch = pool->allocate_batch(BATCH_SIZE);
                total_allocated.fetch_add(batch.size());

                // Do some work
                std::this_thread::yield();

                pool->deallocate_batch(batch);
                total_allocated.fetch_sub(batch.size());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Flush all thread caches
    pool->flush_thread_cache();

    EXPECT_EQ(pool->allocated_objects(), 0);
    EXPECT_EQ(total_allocated.load(), 0);
}