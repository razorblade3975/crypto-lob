#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <pthread.h>  // For pthread_setaffinity_np
#include <sched.h>    // For CPU affinity
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

#ifdef HAS_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

#include "core/memory_pool.hpp"

using namespace crypto_lob::core;

// Order structure typical in HFT
struct alignas(64) Order {
    uint64_t order_id;
    uint64_t instrument_id;
    double price;
    uint64_t quantity;
    uint64_t timestamp;
    uint8_t side;
    uint8_t order_type;
    char padding[22];  // Align to 64 bytes (cache line size)
};

static_assert(sizeof(Order) == 64, "Order should be 64 bytes (cache line size)");

// Latency measurement utilities
class LatencyRecorder {
  private:
    std::vector<double> latencies_;
    size_t capacity_;

  public:
    explicit LatencyRecorder(size_t capacity) : capacity_(capacity) {
        latencies_.reserve(capacity);
    }

    void record(double latency_ns) {
        if (latencies_.size() < capacity_) {
            latencies_.push_back(latency_ns);
        }
    }

    void compute_percentiles() {
        if (latencies_.empty())
            return;

        std::sort(latencies_.begin(), latencies_.end());

        auto percentile = [this](double p) {
            size_t idx = static_cast<size_t>(p * latencies_.size() / 100.0);
            return latencies_[std::min(idx, latencies_.size() - 1)];
        };

        std::cout << "\nLatency Percentiles (nanoseconds):\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  Min:    " << latencies_.front() << " ns\n";
        std::cout << "  50%:    " << percentile(50) << " ns\n";
        std::cout << "  90%:    " << percentile(90) << " ns\n";
        std::cout << "  99%:    " << percentile(99) << " ns\n";
        std::cout << "  99.9%:  " << percentile(99.9) << " ns\n";
        std::cout << "  99.99%: " << percentile(99.99) << " ns\n";
        std::cout << "  Max:    " << latencies_.back() << " ns\n";

        // Calculate mean and std dev
        double mean = std::accumulate(latencies_.begin(), latencies_.end(), 0.0) / latencies_.size();
        double sq_sum = 0;
        for (double lat : latencies_) {
            sq_sum += (lat - mean) * (lat - mean);
        }
        double std_dev = std::sqrt(sq_sum / latencies_.size());

        std::cout << "  Mean:   " << mean << " ns\n";
        std::cout << "  StdDev: " << std_dev << " ns\n";
    }
};

// High-precision timer using rdtsc on x86_64
class RdtscTimer {
  private:
    static inline uint64_t rdtsc() {
#if defined(__x86_64__) || defined(__i386__)
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        return ((uint64_t)hi << 32) | lo;
#else
        // Fallback for non-x86 architectures
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
#endif
    }

    static double cycles_per_ns_;

    static void calibrate() {
        auto start_time = std::chrono::high_resolution_clock::now();
        uint64_t start_cycles = rdtsc();

        // Busy wait for calibration period
        while (std::chrono::high_resolution_clock::now() - start_time < std::chrono::milliseconds(100)) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ __volatile__("pause");
#else
            // Yield CPU on non-x86 architectures
            std::this_thread::yield();
#endif
        }

        uint64_t end_cycles = rdtsc();
        auto end_time = std::chrono::high_resolution_clock::now();

        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
        cycles_per_ns_ = static_cast<double>(end_cycles - start_cycles) / duration_ns;
    }

  public:
    static void init() {
        calibrate();
        std::cout << "RDTSC calibrated: " << cycles_per_ns_ << " cycles/ns\n";
    }

    static double measure_ns(auto func) {
        // Warm up CPU pipeline
        func();

#if defined(__x86_64__) || defined(__i386__)
        // Serialize instruction stream
        __asm__ __volatile__("mfence" ::: "memory");
        uint64_t start = rdtsc();
        __asm__ __volatile__("lfence" ::: "memory");

        func();

        __asm__ __volatile__("mfence" ::: "memory");
        uint64_t end = rdtsc();
        __asm__ __volatile__("lfence" ::: "memory");

        return static_cast<double>(end - start) / cycles_per_ns_;
#else
        // Fallback for non-x86 architectures
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<double, std::nano>(end - start).count();
#endif
    }
};

double RdtscTimer::cycles_per_ns_ = 1.0;

// Benchmark different allocators with detailed latency analysis
template <typename Allocator>
void benchmark_allocator_latency(const std::string& name, Allocator& alloc, size_t iterations) {
    std::cout << "\n=== " << name << " ===\n";

    LatencyRecorder alloc_recorder(iterations);
    LatencyRecorder dealloc_recorder(iterations);
    LatencyRecorder combined_recorder(iterations);

    // Pre-allocate to avoid first-time overhead
    std::vector<Order*> warmup_ptrs;
    for (int i = 0; i < 100; ++i) {
        if constexpr (std::is_same_v<Allocator, std::allocator<Order>>) {
            auto* ptr = alloc.allocate(1);
            new (ptr) Order{};
            warmup_ptrs.push_back(ptr);
        } else if constexpr (std::is_same_v<Allocator, MemoryPool<Order>>) {
            auto* ptr = alloc.allocate();
            new (ptr) Order{};
            warmup_ptrs.push_back(ptr);
        }
    }

    // Clean up warmup
    for (auto* ptr : warmup_ptrs) {
        if constexpr (std::is_same_v<Allocator, std::allocator<Order>>) {
            ptr->~Order();
            alloc.deallocate(ptr, 1);
        } else if constexpr (std::is_same_v<Allocator, MemoryPool<Order>>) {
            ptr->~Order();
            alloc.deallocate(ptr);
        }
    }

    // Measure allocation latencies
    for (size_t i = 0; i < iterations; ++i) {
        Order* ptr = nullptr;

        double alloc_time = RdtscTimer::measure_ns([&]() {
            if constexpr (std::is_same_v<Allocator, std::allocator<Order>>) {
                ptr = alloc.allocate(1);
                new (ptr) Order{};
            } else if constexpr (std::is_same_v<Allocator, MemoryPool<Order>>) {
                ptr = alloc.allocate();
                new (ptr) Order{};
            }
        });

        alloc_recorder.record(alloc_time);

        // Small delay to prevent CPU thermal issues
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause");
#else
        std::this_thread::yield();
#endif

        double dealloc_time = RdtscTimer::measure_ns([&]() {
            if constexpr (std::is_same_v<Allocator, std::allocator<Order>>) {
                ptr->~Order();
                alloc.deallocate(ptr, 1);
            } else if constexpr (std::is_same_v<Allocator, MemoryPool<Order>>) {
                ptr->~Order();
                alloc.deallocate(ptr);
            }
        });

        dealloc_recorder.record(dealloc_time);
        combined_recorder.record(alloc_time + dealloc_time);
    }

    std::cout << "\nAllocation Latency:";
    alloc_recorder.compute_percentiles();

    std::cout << "\nDeallocation Latency:";
    dealloc_recorder.compute_percentiles();

    std::cout << "\nCombined (Alloc + Dealloc) Latency:";
    combined_recorder.compute_percentiles();
}

#ifdef HAS_JEMALLOC
// jemalloc custom allocator wrapper
template <typename T>
class JemallocAllocator {
  public:
    using value_type = T;

    JemallocAllocator() = default;

    template <typename U>
    JemallocAllocator(const JemallocAllocator<U>&) {}

    T* allocate(size_t n) {
        return static_cast<T*>(je_malloc(n * sizeof(T)));
    }

    void deallocate(T* p, size_t) {
        je_free(p);
    }

    T* construct() {
        T* ptr = allocate(1);
        new (ptr) T{};
        return ptr;
    }

    void destroy(T* ptr) {
        ptr->~T();
        deallocate(ptr, 1);
    }
};
#endif

// Measure cache miss patterns
void benchmark_cache_behavior() {
    std::cout << "\n\n=== Cache Behavior Analysis ===\n";

    const size_t num_objects = 10000;
    const size_t access_iterations = 100000;

    // Test 1: std::allocator (likely scattered memory)
    {
        std::allocator<Order> alloc;
        std::vector<Order*> ptrs;
        ptrs.reserve(num_objects);

        for (size_t i = 0; i < num_objects; ++i) {
            auto* ptr = alloc.allocate(1);
            new (ptr) Order{};
            ptrs.push_back(ptr);
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t sum = 0;

        for (size_t iter = 0; iter < access_iterations; ++iter) {
            for (size_t i = 0; i < num_objects; i += 64) {  // Stride by cache line
                sum += ptrs[i]->order_id;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        benchmark::DoNotOptimize(sum);

        std::cout << "std::allocator cache traverse: " << duration.count() / access_iterations << " ns/iteration\n";

        for (auto* ptr : ptrs) {
            ptr->~Order();
            alloc.deallocate(ptr, 1);
        }
    }

    // Test 2: MemoryPool (contiguous memory)
    {
        MemoryPool<Order> pool(num_objects);
        std::vector<Order*> ptrs;
        ptrs.reserve(num_objects);

        for (size_t i = 0; i < num_objects; ++i) {
            auto* ptr = pool.allocate();
            new (ptr) Order{};
            ptrs.push_back(ptr);
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint64_t sum = 0;

        for (size_t iter = 0; iter < access_iterations; ++iter) {
            for (size_t i = 0; i < num_objects; i += 64) {  // Stride by cache line
                sum += ptrs[i]->order_id;
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        benchmark::DoNotOptimize(sum);

        std::cout << "MemoryPool cache traverse: " << duration.count() / access_iterations << " ns/iteration\n";

        for (auto* ptr : ptrs) {
            ptr->~Order();
            pool.deallocate(ptr);
        }
    }
}

// Test thread-local cache effectiveness
void benchmark_thread_local_cache() {
    std::cout << "\n\n=== Thread-Local Cache Effectiveness ===\n";

    const size_t iterations = 1000000;

    // Test with different cache configurations
    struct CacheTest {
        const char* name;
        CacheConfig config;
    };

    std::vector<CacheTest> configs = {
        {"No Cache", CacheConfig{0, 0, true, true}},
        {"Small Cache (16)", CacheConfig{16, 8, true, true}},
        {"Default Cache (64)", CacheConfig{64, 32, true, true}},
        {"Large Cache (256)", CacheConfig{256, 128, true, true}},
    };

    for (const auto& test : configs) {
        MemoryPool<Order> pool(10000, PoolDepletionPolicy::TERMINATE_PROCESS, test.config);

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < iterations; ++i) {
            auto* order = pool.allocate();
            new (order) Order{};
            benchmark::DoNotOptimize(order);
            order->~Order();
            pool.deallocate(order);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

        double avg_latency = static_cast<double>(duration.count()) / iterations;
        std::cout << test.name << ": " << avg_latency << " ns/op\n";
    }
}

int main(int argc, char** argv) {
    // Initialize high-precision timer
    RdtscTimer::init();

    // Pin to CPU core for consistent measurements
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);  // Use core 2 to avoid system interrupts on core 0
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);

    const size_t iterations = 100000;

    // Test different allocators
    std::allocator<Order> std_alloc;
    benchmark_allocator_latency("std::allocator", std_alloc, iterations);

    MemoryPool<Order> mem_pool(iterations * 2, PoolDepletionPolicy::TERMINATE_PROCESS);
    benchmark_allocator_latency("MemoryPool", mem_pool, iterations);

#ifdef HAS_JEMALLOC
    JemallocAllocator<Order> jemalloc;
    benchmark_allocator_latency("jemalloc", jemalloc, iterations);
#endif

    // Additional tests
    benchmark_cache_behavior();
    benchmark_thread_local_cache();

    return 0;
}