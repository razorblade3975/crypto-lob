#include <vector>

#include <benchmark/benchmark.h>

#include "core/memory_pool.hpp"

using namespace crypto_lob::core;

// Test object sizes with proper alignment
struct alignas(64) SmallObject {
    char data[64];
};

struct alignas(64) MediumObject {
    char data[192];
};

// Benchmark individual allocations vs batch API
template <typename T>
static void BM_IndividualAllocations(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    MemoryPool<T> pool(batch_size * 10);  // 10x capacity

    for (auto _ : state) {
        std::vector<T*> ptrs;
        ptrs.reserve(batch_size);

        // Individual allocations
        for (size_t i = 0; i < batch_size; ++i) {
            ptrs.push_back(pool.allocate());
        }

        // Individual deallocations
        for (T* ptr : ptrs) {
            pool.deallocate(ptr);
        }

        pool.flush_thread_cache();
    }

    state.SetItemsProcessed(state.iterations() * batch_size);
}

template <typename T>
static void BM_BatchAllocation(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    MemoryPool<T> pool(batch_size * 10);  // 10x capacity

    for (auto _ : state) {
        // Batch allocation
        auto ptrs = pool.allocate_batch(batch_size);

        // Batch deallocation
        pool.deallocate_batch(ptrs);

        pool.flush_thread_cache();
    }

    state.SetItemsProcessed(state.iterations() * batch_size);
}

// Benchmark mixed workload
template <typename T>
static void BM_MixedWorkload(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    MemoryPool<T> pool(batch_size * 20);

    for (auto _ : state) {
        // Some individual allocations
        std::vector<T*> singles;
        for (size_t i = 0; i < 10; ++i) {
            singles.push_back(pool.allocate());
        }

        // Batch allocation
        auto batch = pool.allocate_batch(batch_size);

        // Deallocate in mixed order
        pool.deallocate_batch(batch);
        for (T* ptr : singles) {
            pool.deallocate(ptr);
        }

        pool.flush_thread_cache();
    }

    state.SetItemsProcessed(state.iterations() * (batch_size + 10));
}

// Register benchmarks
BENCHMARK_TEMPLATE(BM_IndividualAllocations, SmallObject)->Arg(10)->Arg(50)->Arg(100)->Arg(500);

BENCHMARK_TEMPLATE(BM_BatchAllocation, SmallObject)->Arg(10)->Arg(50)->Arg(100)->Arg(500);

BENCHMARK_TEMPLATE(BM_IndividualAllocations, MediumObject)->Arg(10)->Arg(50)->Arg(100)->Arg(500);

BENCHMARK_TEMPLATE(BM_BatchAllocation, MediumObject)->Arg(10)->Arg(50)->Arg(100)->Arg(500);

BENCHMARK_TEMPLATE(BM_MixedWorkload, SmallObject)->Arg(50)->Arg(100)->Arg(200);

BENCHMARK_MAIN();