#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/memory_pool.hpp"

using namespace crypto_lob::core;

// Test object sizes representing different HFT use cases
struct alignas(64) SmallObject {  // Align to cache line
    uint64_t id;
    double price;
    uint64_t timestamp;
    double value1;
    double value2;
    double value3;
    char padding[8];  // 64 bytes total
};

struct alignas(64) MediumObject {  // Align to cache line
    uint64_t id;
    double values[15];
    uint64_t timestamp;
    // 136 bytes of data, but aligned to 192 bytes (3 cache lines)
    char padding[56];
};

struct alignas(64) LargeObject {  // Align to cache line
    uint64_t id;
    double matrix[8][8];  // 512 bytes of data
    uint64_t timestamps[8];
    char metadata[56];  // Adjusted for proper alignment
    // 640 bytes total (10 cache lines)
};

// Ensure proper sizes
static_assert(sizeof(SmallObject) == 64);
static_assert(sizeof(MediumObject) == 192);
static_assert(sizeof(LargeObject) == 640);

// Helper to measure allocation latency in nanoseconds
template <typename Allocator>
class AllocationTimer {
    using clock = std::chrono::high_resolution_clock;

  public:
    template <typename T>
    static double measure_allocation(Allocator& alloc, size_t iterations) {
        auto start = clock::now();

        for (size_t i = 0; i < iterations; ++i) {
            auto* ptr = alloc.allocate(1);
            benchmark::DoNotOptimize(ptr);
            alloc.deallocate(ptr, 1);
        }

        auto end = clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        return static_cast<double>(duration.count()) / iterations;
    }
};

// Benchmark: Sequential allocation/deallocation pattern
template <typename T>
static void BM_SequentialAlloc_StdAllocator(benchmark::State& state) {
    std::allocator<T> alloc;

    for (auto _ : state) {
        auto* ptr = alloc.allocate(1);
        benchmark::DoNotOptimize(ptr);
        new (ptr) T{};
        ptr->~T();
        alloc.deallocate(ptr, 1);
    }

    state.SetItemsProcessed(state.iterations());
}

template <typename T>
static void BM_SequentialAlloc_MemoryPool(benchmark::State& state) {
    MemoryPool<T> pool(10000);

    for (auto _ : state) {
        auto* ptr = pool.allocate();
        benchmark::DoNotOptimize(ptr);
        new (ptr) T{};
        ptr->~T();
        pool.deallocate(ptr);
    }

    state.SetItemsProcessed(state.iterations());
}

// Benchmark: Bulk allocation pattern (allocate N, then deallocate all)
template <typename T>
static void BM_BulkAlloc_StdAllocator(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    std::allocator<T> alloc;
    std::vector<T*> ptrs;
    ptrs.reserve(batch_size);

    for (auto _ : state) {
        // Allocate batch
        for (size_t i = 0; i < batch_size; ++i) {
            auto* ptr = alloc.allocate(1);
            new (ptr) T{};
            ptrs.push_back(ptr);
        }

        // Deallocate batch
        for (auto* ptr : ptrs) {
            ptr->~T();
            alloc.deallocate(ptr, 1);
        }
        ptrs.clear();
    }

    state.SetItemsProcessed(state.iterations() * batch_size);
}

template <typename T>
static void BM_BulkAlloc_MemoryPool(benchmark::State& state) {
    const size_t batch_size = state.range(0);
    MemoryPool<T> pool(batch_size * 2);  // 2x capacity for safety
    std::vector<T*> ptrs;
    ptrs.reserve(batch_size);

    for (auto _ : state) {
        // Allocate batch
        for (size_t i = 0; i < batch_size; ++i) {
            auto* ptr = pool.construct();
            ptrs.push_back(ptr);
        }

        // Deallocate batch
        for (auto* ptr : ptrs) {
            pool.destroy(ptr);
        }
        ptrs.clear();
    }

    state.SetItemsProcessed(state.iterations() * batch_size);
}

// Benchmark: Random allocation/deallocation pattern (simulates real usage)
template <typename T>
static void BM_RandomPattern_StdAllocator(benchmark::State& state) {
    const size_t active_objects = state.range(0);
    std::allocator<T> alloc;
    std::vector<T*> ptrs(active_objects, nullptr);
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<size_t> dist(0, active_objects - 1);

    // Pre-allocate half the objects
    for (size_t i = 0; i < active_objects / 2; ++i) {
        ptrs[i] = alloc.allocate(1);
        new (ptrs[i]) T{};
    }

    for (auto _ : state) {
        size_t idx = dist(rng);

        if (ptrs[idx] == nullptr) {
            // Allocate
            ptrs[idx] = alloc.allocate(1);
            new (ptrs[idx]) T{};
        } else {
            // Deallocate
            ptrs[idx]->~T();
            alloc.deallocate(ptrs[idx], 1);
            ptrs[idx] = nullptr;
        }
    }

    // Cleanup
    for (auto* ptr : ptrs) {
        if (ptr) {
            ptr->~T();
            alloc.deallocate(ptr, 1);
        }
    }

    state.SetItemsProcessed(state.iterations());
}

template <typename T>
static void BM_RandomPattern_MemoryPool(benchmark::State& state) {
    const size_t active_objects = state.range(0);
    MemoryPool<T> pool(active_objects * 2);
    std::vector<T*> ptrs(active_objects, nullptr);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> dist(0, active_objects - 1);

    // Pre-allocate half the objects
    for (size_t i = 0; i < active_objects / 2; ++i) {
        ptrs[i] = pool.construct();
    }

    for (auto _ : state) {
        size_t idx = dist(rng);

        if (ptrs[idx] == nullptr) {
            // Allocate
            ptrs[idx] = pool.construct();
        } else {
            // Deallocate
            pool.destroy(ptrs[idx]);
            ptrs[idx] = nullptr;
        }
    }

    // Cleanup
    for (auto* ptr : ptrs) {
        if (ptr) {
            pool.destroy(ptr);
        }
    }

    state.SetItemsProcessed(state.iterations());
}

// Benchmark: Multi-threaded allocation stress test
template <typename T>
static void BM_Multithreaded_StdAllocator(benchmark::State& state) {
    const size_t num_threads = state.range(0);
    const size_t ops_per_thread = 1000;

    for (auto _ : state) {
        std::vector<std::thread> threads;
        std::atomic<size_t> total_ops{0};

        auto worker = [&total_ops, ops_per_thread]() {
            std::allocator<T> alloc;
            for (size_t i = 0; i < ops_per_thread; ++i) {
                auto* ptr = alloc.allocate(1);
                new (ptr) T{};
                benchmark::DoNotOptimize(ptr);
                ptr->~T();
                alloc.deallocate(ptr, 1);
                total_ops.fetch_add(1, std::memory_order_relaxed);
            }
        };

        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }

        for (auto& t : threads) {
            t.join();
        }

        state.counters["total_ops"] = total_ops.load();
    }

    state.SetItemsProcessed(state.iterations() * num_threads * ops_per_thread);
}

template <typename T>
static void BM_Multithreaded_MemoryPool(benchmark::State& state) {
    const size_t num_threads = state.range(0);
    const size_t ops_per_thread = 1000;

    for (auto _ : state) {
        MemoryPool<T> pool(num_threads * ops_per_thread * 2);
        std::vector<std::thread> threads;
        std::atomic<size_t> total_ops{0};

        auto worker = [&pool, &total_ops, ops_per_thread]() {
            for (size_t i = 0; i < ops_per_thread; ++i) {
                auto* ptr = pool.construct();
                benchmark::DoNotOptimize(ptr);
                pool.destroy(ptr);
                total_ops.fetch_add(1, std::memory_order_relaxed);
            }
        };

        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }

        for (auto& t : threads) {
            t.join();
        }

        state.counters["total_ops"] = total_ops.load();
    }

    state.SetItemsProcessed(state.iterations() * num_threads * ops_per_thread);
}

// Benchmark: Cache behavior test (allocate many, access in different patterns)
template <typename T>
static void BM_CacheBehavior_StdAllocator(benchmark::State& state) {
    const size_t num_objects = 1000;
    std::allocator<T> alloc;
    std::vector<T*> ptrs;
    ptrs.reserve(num_objects);

    // Allocate objects
    for (size_t i = 0; i < num_objects; ++i) {
        auto* ptr = alloc.allocate(1);
        new (ptr) T{};
        ptrs.push_back(ptr);
    }

    // Access pattern: stride through memory
    size_t sum = 0;
    for (auto _ : state) {
        for (size_t i = 0; i < num_objects; i += 16) {  // Cache line stride
            sum += reinterpret_cast<uintptr_t>(ptrs[i]);
        }
        benchmark::DoNotOptimize(sum);
    }

    // Cleanup
    for (auto* ptr : ptrs) {
        ptr->~T();
        alloc.deallocate(ptr, 1);
    }

    state.SetItemsProcessed(state.iterations() * (num_objects / 16));
}

template <typename T>
static void BM_CacheBehavior_MemoryPool(benchmark::State& state) {
    const size_t num_objects = 1000;
    MemoryPool<T> pool(num_objects);
    std::vector<T*> ptrs;
    ptrs.reserve(num_objects);

    // Allocate objects
    for (size_t i = 0; i < num_objects; ++i) {
        auto* ptr = pool.construct();
        ptrs.push_back(ptr);
    }

    // Access pattern: stride through memory
    size_t sum = 0;
    for (auto _ : state) {
        for (size_t i = 0; i < num_objects; i += 16) {  // Cache line stride
            sum += reinterpret_cast<uintptr_t>(ptrs[i]);
        }
        benchmark::DoNotOptimize(sum);
    }

    // Cleanup
    for (auto* ptr : ptrs) {
        pool.destroy(ptr);
    }

    state.SetItemsProcessed(state.iterations() * (num_objects / 16));
}

// Register benchmarks for different object sizes
#define REGISTER_BENCHMARKS(ObjectType)                                                    \
    BENCHMARK_TEMPLATE(BM_SequentialAlloc_StdAllocator, ObjectType)->Iterations(1000000);  \
    BENCHMARK_TEMPLATE(BM_SequentialAlloc_MemoryPool, ObjectType)->Iterations(1000000);    \
                                                                                           \
    BENCHMARK_TEMPLATE(BM_BulkAlloc_StdAllocator, ObjectType)->Arg(100)->Arg(1000);        \
    BENCHMARK_TEMPLATE(BM_BulkAlloc_MemoryPool, ObjectType)->Arg(100)->Arg(1000);          \
                                                                                           \
    BENCHMARK_TEMPLATE(BM_RandomPattern_StdAllocator, ObjectType)->Arg(1000);              \
    BENCHMARK_TEMPLATE(BM_RandomPattern_MemoryPool, ObjectType)->Arg(1000);                \
                                                                                           \
    BENCHMARK_TEMPLATE(BM_Multithreaded_StdAllocator, ObjectType)->Arg(1)->Arg(4)->Arg(8); \
    BENCHMARK_TEMPLATE(BM_Multithreaded_MemoryPool, ObjectType)->Arg(1)->Arg(4)->Arg(8);   \
                                                                                           \
    BENCHMARK_TEMPLATE(BM_CacheBehavior_StdAllocator, ObjectType);                         \
    BENCHMARK_TEMPLATE(BM_CacheBehavior_MemoryPool, ObjectType);

REGISTER_BENCHMARKS(SmallObject)
REGISTER_BENCHMARKS(MediumObject)
REGISTER_BENCHMARKS(LargeObject)

// Custom main to add HFT-specific setup
int main(int argc, char** argv) {
    // HFT-specific system setup
    // CPU affinity would be set here in production

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();

    return 0;
}