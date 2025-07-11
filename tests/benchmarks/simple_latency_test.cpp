#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <vector>

#include "core/memory_pool.hpp"

using namespace crypto_lob::core;
using namespace std::chrono;

// Order structure typical in HFT
struct alignas(64) Order {
    uint64_t order_id;
    uint64_t instrument_id;
    double price;
    uint64_t quantity;
    uint64_t timestamp;
    uint8_t side;
    uint8_t order_type;
    char padding[22];  // Align to 64 bytes
};

static_assert(sizeof(Order) == 64, "Order should be 64 bytes");

class LatencyMeasurement {
  private:
    std::vector<double> samples_;

  public:
    explicit LatencyMeasurement(size_t capacity) {
        samples_.reserve(capacity);
    }

    void add_sample(double ns) {
        samples_.push_back(ns);
    }

    void print_statistics(const std::string& name) {
        if (samples_.empty())
            return;

        std::sort(samples_.begin(), samples_.end());

        auto percentile = [this](double p) {
            size_t idx = static_cast<size_t>((p / 100.0) * samples_.size());
            return samples_[std::min(idx, samples_.size() - 1)];
        };

        double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
        double mean = sum / samples_.size();

        double sq_sum = 0;
        for (double s : samples_) {
            sq_sum += (s - mean) * (s - mean);
        }
        double std_dev = std::sqrt(sq_sum / samples_.size());

        std::cout << "\n=== " << name << " ===\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "Samples: " << samples_.size() << "\n";
        std::cout << "Mean:    " << mean << " ns\n";
        std::cout << "StdDev:  " << std_dev << " ns\n";
        std::cout << "Min:     " << samples_.front() << " ns\n";
        std::cout << "50%:     " << percentile(50) << " ns\n";
        std::cout << "90%:     " << percentile(90) << " ns\n";
        std::cout << "99%:     " << percentile(99) << " ns\n";
        std::cout << "99.9%:   " << percentile(99.9) << " ns\n";
        std::cout << "Max:     " << samples_.back() << " ns\n";
    }
};

template <typename Func>
double measure_ns(Func&& func) {
    auto start = high_resolution_clock::now();
    func();
    auto end = high_resolution_clock::now();
    return duration<double, std::nano>(end - start).count();
}

void benchmark_allocator_performance() {
    const size_t iterations = 100'000;
    const size_t warmup = 1'000;

    std::cout << "Running allocation latency benchmarks...\n";
    std::cout << "Iterations: " << iterations << "\n";

    // Warm up to stabilize CPU
    {
        std::allocator<Order> alloc;
        for (size_t i = 0; i < warmup; ++i) {
            auto* ptr = alloc.allocate(1);
            new (ptr) Order{};
            ptr->~Order();
            alloc.deallocate(ptr, 1);
        }
    }

    // Test std::allocator
    {
        LatencyMeasurement alloc_latency(iterations);
        LatencyMeasurement dealloc_latency(iterations);
        LatencyMeasurement combined_latency(iterations);

        std::allocator<Order> alloc;

        for (size_t i = 0; i < iterations; ++i) {
            Order* ptr = nullptr;

            double alloc_time = measure_ns([&]() {
                ptr = alloc.allocate(1);
                new (ptr) Order{};
            });

            double dealloc_time = measure_ns([&]() {
                ptr->~Order();
                alloc.deallocate(ptr, 1);
            });

            alloc_latency.add_sample(alloc_time);
            dealloc_latency.add_sample(dealloc_time);
            combined_latency.add_sample(alloc_time + dealloc_time);
        }

        alloc_latency.print_statistics("std::allocator - Allocation");
        dealloc_latency.print_statistics("std::allocator - Deallocation");
        combined_latency.print_statistics("std::allocator - Combined");
    }

    // Test MemoryPool
    {
        LatencyMeasurement alloc_latency(iterations);
        LatencyMeasurement dealloc_latency(iterations);
        LatencyMeasurement combined_latency(iterations);

        MemoryPool<Order> pool(iterations * 2);

        // Warm up the pool
        for (size_t i = 0; i < warmup; ++i) {
            auto* ptr = pool.construct();
            pool.destroy(ptr);
        }

        for (size_t i = 0; i < iterations; ++i) {
            Order* ptr = nullptr;

            double alloc_time = measure_ns([&]() { ptr = pool.construct(); });

            double dealloc_time = measure_ns([&]() { pool.destroy(ptr); });

            alloc_latency.add_sample(alloc_time);
            dealloc_latency.add_sample(dealloc_time);
            combined_latency.add_sample(alloc_time + dealloc_time);
        }

        alloc_latency.print_statistics("MemoryPool - Allocation");
        dealloc_latency.print_statistics("MemoryPool - Deallocation");
        combined_latency.print_statistics("MemoryPool - Combined");
    }

    // Test thread-local cache effectiveness
    std::cout << "\n\n=== Thread-Local Cache Analysis ===\n";

    struct TestConfig {
        const char* name;
        CacheConfig config;
    };

    std::vector<TestConfig> configs = {
        {"Minimal Cache (1)", CacheConfig{1, 1, true, true}},  // Changed from 0 to avoid depletion
        {"Small Cache (16)", CacheConfig{16, 8, true, true}},
        {"Default Cache (64)", CacheConfig{64, 32, true, true}},
        {"Large Cache (256)", CacheConfig{256, 128, true, true}},
    };

    for (const auto& test : configs) {
        MemoryPool<Order> pool(iterations * 2, PoolDepletionPolicy::TERMINATE_PROCESS, test.config);

        // Warm up
        for (size_t i = 0; i < warmup; ++i) {
            auto* ptr = pool.construct();
            pool.destroy(ptr);
        }

        auto start = high_resolution_clock::now();

        for (size_t i = 0; i < iterations; ++i) {
            auto* order = pool.construct();
            pool.destroy(order);
        }

        auto end = high_resolution_clock::now();
        auto duration_ns = duration_cast<nanoseconds>(end - start).count();
        double avg_latency = static_cast<double>(duration_ns) / iterations;

        std::cout << test.name << ": " << avg_latency << " ns/op\n";
    }
}

void benchmark_memory_patterns() {
    std::cout << "\n\n=== Memory Access Pattern Analysis ===\n";

    const size_t num_objects = 10'000;
    const size_t access_iterations = 100'000;

    // Test std::allocator (scattered memory)
    {
        std::allocator<Order> alloc;
        std::vector<Order*> ptrs;
        ptrs.reserve(num_objects);

        for (size_t i = 0; i < num_objects; ++i) {
            auto* ptr = alloc.allocate(1);
            new (ptr) Order{};
            ptr->order_id = i;
            ptrs.push_back(ptr);
        }

        auto start = high_resolution_clock::now();
        uint64_t sum = 0;

        for (size_t iter = 0; iter < access_iterations; ++iter) {
            // Stride through memory by cache lines
            for (size_t i = 0; i < num_objects; i += 16) {
                sum += ptrs[i]->order_id;
            }
        }

        auto end = high_resolution_clock::now();
        auto duration_ns = duration_cast<nanoseconds>(end - start).count();

        // Prevent optimization
        if (sum == 0)
            std::cout << "Error";

        std::cout << "std::allocator traverse time: " << duration_ns / access_iterations << " ns/iteration\n";

        // Cleanup
        for (auto* ptr : ptrs) {
            ptr->~Order();
            alloc.deallocate(ptr, 1);
        }
    }

    // Test MemoryPool (contiguous memory)
    {
        MemoryPool<Order> pool(num_objects);
        std::vector<Order*> ptrs;
        ptrs.reserve(num_objects);

        for (size_t i = 0; i < num_objects; ++i) {
            auto* ptr = pool.construct();
            ptr->order_id = i;
            ptrs.push_back(ptr);
        }

        auto start = high_resolution_clock::now();
        uint64_t sum = 0;

        for (size_t iter = 0; iter < access_iterations; ++iter) {
            // Stride through memory by cache lines
            for (size_t i = 0; i < num_objects; i += 16) {
                sum += ptrs[i]->order_id;
            }
        }

        auto end = high_resolution_clock::now();
        auto duration_ns = duration_cast<nanoseconds>(end - start).count();

        // Prevent optimization
        if (sum == 0)
            std::cout << "Error";

        std::cout << "MemoryPool traverse time: " << duration_ns / access_iterations << " ns/iteration\n";

        // Calculate improvement
        std::cout << "\nMemory Locality Analysis:\n";

        // Check if allocations are contiguous
        bool contiguous = true;
        for (size_t i = 1; i < ptrs.size(); ++i) {
            if (reinterpret_cast<uintptr_t>(ptrs[i]) != reinterpret_cast<uintptr_t>(ptrs[i - 1]) + sizeof(Order)) {
                contiguous = false;
                break;
            }
        }

        std::cout << "Allocations are " << (contiguous ? "contiguous" : "scattered") << "\n";

        // Cleanup
        for (auto* ptr : ptrs) {
            pool.destroy(ptr);
        }
    }
}

int main() {
    std::cout << "Memory Pool Latency Analysis\n";
    std::cout << "============================\n\n";

    benchmark_allocator_performance();
    benchmark_memory_patterns();

    std::cout << "\n\nAnalysis complete.\n";

    return 0;
}