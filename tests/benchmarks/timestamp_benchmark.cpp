#include <chrono>
#include <vector>

#include <benchmark/benchmark.h>

#include "core/timestamp.hpp"

using namespace crypto_lob::core;

// Benchmark raw RDTSC performance
static void BM_Rdtsc(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = Timestamp::rdtsc();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark RDTSCP performance
static void BM_Rdtscp(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = Timestamp::rdtscp();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark RDTSC with memory fence
static void BM_RdtscOrdered(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = Timestamp::rdtsc_ordered();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark convenience function
static void BM_RdtscConvenience(benchmark::State& state) {
    for (auto _ : state) {
        auto tsc = rdtsc();
        benchmark::DoNotOptimize(tsc);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark TSC to nanoseconds conversion
static void BM_TscToNs(benchmark::State& state) {
    auto& calibrator = Timestamp::calibrator();
    uint64_t tsc_delta = 1000000;  // Arbitrary delta

    for (auto _ : state) {
        auto ns = calibrator.tsc_to_ns(tsc_delta);
        benchmark::DoNotOptimize(ns);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark std::chrono::steady_clock for comparison
static void BM_ChronoSteadyClock(benchmark::State& state) {
    for (auto _ : state) {
        auto now = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(now);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark std::chrono::high_resolution_clock for comparison
static void BM_ChronoHighResClock(benchmark::State& state) {
    for (auto _ : state) {
        auto now = std::chrono::high_resolution_clock::now();
        benchmark::DoNotOptimize(now);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark std::chrono::system_clock for comparison
static void BM_ChronoSystemClock(benchmark::State& state) {
    for (auto _ : state) {
        auto now = std::chrono::system_clock::now();
        benchmark::DoNotOptimize(now);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark timestamp delta calculation
static void BM_TimestampDelta(benchmark::State& state) {
    for (auto _ : state) {
        uint64_t start = Timestamp::rdtsc();
        benchmark::DoNotOptimize(start);
        uint64_t end = Timestamp::rdtsc();
        benchmark::DoNotOptimize(end);
        uint64_t delta = end - start;
        benchmark::DoNotOptimize(delta);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark full timing operation (start, end, convert to ns)
static void BM_FullTimingOperation(benchmark::State& state) {
    auto& calibrator = Timestamp::calibrator();

    for (auto _ : state) {
        uint64_t start = Timestamp::rdtsc();
        benchmark::DoNotOptimize(start);

        // Simulate some work
        volatile int dummy = 0;
        for (int i = 0; i < 10; ++i) {
            dummy += i;
        }

        uint64_t end = Timestamp::rdtsc();
        uint64_t delta = end - start;
        uint64_t ns = calibrator.tsc_to_ns(delta);
        benchmark::DoNotOptimize(ns);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark sequential timestamp reads to measure overhead
static void BM_SequentialReads(benchmark::State& state) {
    const int num_reads = state.range(0);
    std::vector<uint64_t> timestamps(num_reads);

    for (auto _ : state) {
        for (int i = 0; i < num_reads; ++i) {
            timestamps[i] = Timestamp::rdtsc();
        }
        benchmark::DoNotOptimize(timestamps.data());
    }
    state.SetItemsProcessed(state.iterations() * num_reads);
}

// Benchmark calibrator access
static void BM_CalibratorAccess(benchmark::State& state) {
    for (auto _ : state) {
        auto& calibrator = Timestamp::calibrator();
        benchmark::DoNotOptimize(&calibrator);
    }
    state.SetItemsProcessed(state.iterations());
}

// Benchmark calibrator frequency access
static void BM_CalibratorFrequency(benchmark::State& state) {
    auto& calibrator = Timestamp::calibrator();

    for (auto _ : state) {
        auto freq = calibrator.frequency();
        benchmark::DoNotOptimize(freq);
    }
    state.SetItemsProcessed(state.iterations());
}

// Register benchmarks
BENCHMARK(BM_Rdtsc);
BENCHMARK(BM_Rdtscp);
BENCHMARK(BM_RdtscOrdered);
BENCHMARK(BM_RdtscConvenience);
BENCHMARK(BM_TscToNs);
BENCHMARK(BM_ChronoSteadyClock);
BENCHMARK(BM_ChronoHighResClock);
BENCHMARK(BM_ChronoSystemClock);
BENCHMARK(BM_TimestampDelta);
BENCHMARK(BM_FullTimingOperation);
BENCHMARK(BM_SequentialReads)->Range(1, 1000);
BENCHMARK(BM_CalibratorAccess);
BENCHMARK(BM_CalibratorFrequency);

BENCHMARK_MAIN();