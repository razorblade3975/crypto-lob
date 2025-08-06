#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/timestamp.hpp"

using namespace crypto_lob::core;

class TimestampTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Warm up the calibrator
        Timestamp::calibrator();
    }

    // Helper function to calculate percentile
    template <typename T>
    T calculate_percentile(std::vector<T>& values, double percentile) {
        std::sort(values.begin(), values.end());
        size_t index = static_cast<size_t>(percentile * values.size() / 100.0);
        return values[std::min(index, values.size() - 1)];
    }

    // Helper function to verify monotonicity
    bool is_monotonic(const std::vector<uint64_t>& values) {
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] < values[i - 1]) {
                return false;
            }
        }
        return true;
    }
};

// Test basic RDTSC functionality
TEST_F(TimestampTest, RdtscReturnsNonZero) {
    uint64_t tsc = Timestamp::rdtsc();
    EXPECT_NE(tsc, 0u);
}

TEST_F(TimestampTest, RdtscpReturnsNonZero) {
    uint64_t tsc = Timestamp::rdtscp();
    EXPECT_NE(tsc, 0u);
}

TEST_F(TimestampTest, RdtscOrderedReturnsNonZero) {
    uint64_t tsc = Timestamp::rdtsc_ordered();
    EXPECT_NE(tsc, 0u);
}

// Test monotonicity - timestamps should always increase
TEST_F(TimestampTest, RdtscMonotonicity) {
    constexpr size_t num_samples = 10000;
    std::vector<uint64_t> timestamps;
    timestamps.reserve(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        timestamps.push_back(Timestamp::rdtsc());
    }

    // Allow for some out-of-order due to CPU reordering
    // But overall trend should be increasing
    uint64_t inversions = 0;
    for (size_t i = 1; i < timestamps.size(); ++i) {
        if (timestamps[i] < timestamps[i - 1]) {
            inversions++;
        }
    }

    // Allow up to 1% inversions due to CPU reordering
    double inversion_rate = static_cast<double>(inversions) / num_samples;
    EXPECT_LT(inversion_rate, 0.01) << "Too many timestamp inversions: " << inversion_rate;
}

TEST_F(TimestampTest, RdtscpMonotonicity) {
    constexpr size_t num_samples = 10000;
    std::vector<uint64_t> timestamps;
    timestamps.reserve(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        timestamps.push_back(Timestamp::rdtscp());
    }

    // RDTSCP should have better ordering guarantees
    EXPECT_TRUE(is_monotonic(timestamps)) << "RDTSCP timestamps should be strictly monotonic";
}

TEST_F(TimestampTest, RdtscOrderedMonotonicity) {
    constexpr size_t num_samples = 10000;
    std::vector<uint64_t> timestamps;
    timestamps.reserve(num_samples);

    for (size_t i = 0; i < num_samples; ++i) {
        timestamps.push_back(Timestamp::rdtsc_ordered());
    }

    // With memory fence, should be strictly monotonic
    EXPECT_TRUE(is_monotonic(timestamps)) << "RDTSC_ordered timestamps should be strictly monotonic";
}

// Test that different RDTSC variants maintain ordering
TEST_F(TimestampTest, RdtscVariantOrdering) {
    uint64_t tsc1 = Timestamp::rdtsc();
    uint64_t tscp = Timestamp::rdtscp();
    uint64_t tsc2 = Timestamp::rdtsc_ordered();

    // Basic sanity check - later calls should have higher values
    EXPECT_LE(tsc1, tscp);
    EXPECT_LE(tscp, tsc2);
}

// Test calibrator functionality
TEST_F(TimestampTest, CalibratorFrequency) {
    auto& calibrator = Timestamp::calibrator();
    uint64_t freq = calibrator.frequency();

    // Modern CPUs typically run between 1GHz and 5GHz
    EXPECT_GE(freq, 1000000000ULL);   // >= 1 GHz (changed from > to >=)
    EXPECT_LT(freq, 10000000000ULL);  // < 10 GHz
}

TEST_F(TimestampTest, CalibratorSingleton) {
    auto& cal1 = Timestamp::calibrator();
    auto& cal2 = Timestamp::calibrator();

    // Should return the same instance
    EXPECT_EQ(&cal1, &cal2);
}

TEST_F(TimestampTest, TscToNsConversion) {
    auto& calibrator = Timestamp::calibrator();
    uint64_t freq = calibrator.frequency();

    // Test 1 second worth of ticks
    uint64_t one_second_ticks = freq;
    uint64_t ns = calibrator.tsc_to_ns(one_second_ticks);

    // Should be approximately 1 billion nanoseconds (1 second)
    // Allow 5% tolerance for calibration error
    EXPECT_GT(ns, 950000000ULL);
    EXPECT_LT(ns, 1050000000ULL);
}

TEST_F(TimestampTest, TscToNsSmallValues) {
    auto& calibrator = Timestamp::calibrator();

    // Test small tick counts
    uint64_t ns_0 = calibrator.tsc_to_ns(0);
    EXPECT_EQ(ns_0, 0u);

    uint64_t ns_1 = calibrator.tsc_to_ns(1);
    EXPECT_GE(ns_1, 0u);
    EXPECT_LE(ns_1, 1u);  // 1 tick should be <= 1ns on modern CPUs
}

// Test timing accuracy
TEST_F(TimestampTest, TimingAccuracy) {
    auto& calibrator = Timestamp::calibrator();

    // Measure a known duration
    auto start_time = std::chrono::steady_clock::now();
    uint64_t start_tsc = Timestamp::rdtsc();

    // Sleep for a known duration
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint64_t end_tsc = Timestamp::rdtsc();
    auto end_time = std::chrono::steady_clock::now();

    // Calculate durations
    uint64_t tsc_delta = end_tsc - start_tsc;
    uint64_t tsc_ns = calibrator.tsc_to_ns(tsc_delta);
    auto chrono_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();

    // Should be within 10% of each other
    double ratio = static_cast<double>(tsc_ns) / static_cast<double>(chrono_ns);
    EXPECT_GT(ratio, 0.9);
    EXPECT_LT(ratio, 1.1);
}

// Test convenience function
TEST_F(TimestampTest, ConvenienceFunction) {
    uint64_t tsc1 = rdtsc();
    uint64_t tsc2 = Timestamp::rdtsc();

    // Both should return valid timestamps
    EXPECT_NE(tsc1, 0u);
    EXPECT_NE(tsc2, 0u);

    // Second call should be >= first (allowing for CPU reordering)
    EXPECT_GE(tsc2, tsc1 - 1000);  // Allow small backward jump
}

// Test cross-thread consistency
TEST_F(TimestampTest, CrossThreadConsistency) {
    constexpr size_t num_threads = 4;
    constexpr size_t samples_per_thread = 1000;

    std::vector<std::thread> threads;
    std::vector<std::vector<uint64_t>> thread_timestamps(num_threads);

    std::atomic<bool> start_flag{false};

    // Launch threads
    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&, i]() {
            thread_timestamps[i].reserve(samples_per_thread);

            // Wait for start signal
            while (!start_flag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            // Collect timestamps
            for (size_t j = 0; j < samples_per_thread; ++j) {
                thread_timestamps[i].push_back(Timestamp::rdtsc());
            }
        });
    }

    // Start all threads simultaneously
    start_flag.store(true, std::memory_order_release);

    // Wait for completion
    for (auto& t : threads) {
        t.join();
    }

    // Verify each thread's timestamps are mostly monotonic
    for (size_t i = 0; i < num_threads; ++i) {
        uint64_t inversions = 0;
        for (size_t j = 1; j < thread_timestamps[i].size(); ++j) {
            if (thread_timestamps[i][j] < thread_timestamps[i][j - 1]) {
                inversions++;
            }
        }

        double inversion_rate = static_cast<double>(inversions) / samples_per_thread;
        EXPECT_LT(inversion_rate, 0.05) << "Thread " << i << " has too many inversions";
    }
}

// Test latency characteristics
TEST_F(TimestampTest, LatencyCharacteristics) {
    constexpr size_t num_samples = 100000;
    std::vector<uint64_t> latencies;
    latencies.reserve(num_samples);

    // Warm up
    for (size_t i = 0; i < 1000; ++i) {
        volatile auto dummy = Timestamp::rdtsc();
        (void)dummy;
    }

    // Measure RDTSC latency
    for (size_t i = 0; i < num_samples; ++i) {
        uint64_t start = Timestamp::rdtsc();
        uint64_t end = Timestamp::rdtsc();
        latencies.push_back(end - start);
    }

    // Calculate statistics
    uint64_t p50 = calculate_percentile(latencies, 50);
    uint64_t p90 = calculate_percentile(latencies, 90);
    uint64_t p99 = calculate_percentile(latencies, 99);

    // RDTSC should be very fast - typically under 100 cycles
    // These are loose bounds to account for different CPUs
    EXPECT_LT(p50, 200u) << "Median RDTSC latency too high";
    EXPECT_LT(p90, 500u) << "P90 RDTSC latency too high";
    EXPECT_LT(p99, 1000u) << "P99 RDTSC latency too high";
}

// Test RDTSCP vs RDTSC latency
TEST_F(TimestampTest, RdtscpVsRdtscLatency) {
    constexpr size_t num_samples = 10000;
    std::vector<uint64_t> rdtsc_latencies;
    std::vector<uint64_t> rdtscp_latencies;

    rdtsc_latencies.reserve(num_samples);
    rdtscp_latencies.reserve(num_samples);

    // Measure RDTSC latency
    for (size_t i = 0; i < num_samples; ++i) {
        uint64_t start = Timestamp::rdtsc();
        uint64_t end = Timestamp::rdtsc();
        rdtsc_latencies.push_back(end - start);
    }

    // Measure RDTSCP latency
    for (size_t i = 0; i < num_samples; ++i) {
        uint64_t start = Timestamp::rdtsc();
        uint64_t end = Timestamp::rdtscp();
        rdtscp_latencies.push_back(end - start);
    }

    uint64_t rdtsc_p50 = calculate_percentile(rdtsc_latencies, 50);
    uint64_t rdtscp_p50 = calculate_percentile(rdtscp_latencies, 50);

    // RDTSCP should be slightly slower due to serialization
    // But both should be very fast
    EXPECT_GE(rdtscp_p50, rdtsc_p50);
    EXPECT_LT(rdtscp_p50, rdtsc_p50 * 3);  // Should not be more than 3x slower
}

// Test edge cases
TEST_F(TimestampTest, LargeTscDelta) {
    auto& calibrator = Timestamp::calibrator();

    // Test with maximum possible delta
    uint64_t max_delta = UINT64_MAX / 2;  // Avoid overflow
    uint64_t ns = calibrator.tsc_to_ns(max_delta);

    // Should not overflow or crash
    EXPECT_GT(ns, 0u);
}

// Test that TSC values are reasonable
TEST_F(TimestampTest, TscValueRange) {
    uint64_t tsc1 = Timestamp::rdtsc();
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    uint64_t tsc2 = Timestamp::rdtsc();

    uint64_t delta = tsc2 - tsc1;

    // For a 10 microsecond sleep on a 3GHz CPU, expect roughly 30,000 cycles
    // Allow wide range for different CPUs and sleep accuracy
    EXPECT_GT(delta, 1000u);     // At least 1000 cycles
    EXPECT_LT(delta, 1000000u);  // Less than 1M cycles for 10us
}

// Stress test for concurrent access
TEST_F(TimestampTest, ConcurrentStress) {
    constexpr size_t num_threads = 8;
    constexpr size_t iterations = 100000;
    std::atomic<uint64_t> total_calls{0};
    std::vector<std::thread> threads;

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < iterations; ++j) {
                volatile auto tsc = Timestamp::rdtsc();
                (void)tsc;
                total_calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(total_calls.load(), num_threads * iterations);
}

// Test calibrator thread safety
TEST_F(TimestampTest, CalibratorThreadSafety) {
    constexpr size_t num_threads = 4;
    std::vector<std::thread> threads;
    std::atomic<bool> all_same{true};

    auto& expected_calibrator = Timestamp::calibrator();
    uint64_t expected_freq = expected_calibrator.frequency();

    for (size_t i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            auto& cal = Timestamp::calibrator();
            uint64_t freq = cal.frequency();

            if (&cal != &expected_calibrator || freq != expected_freq) {
                all_same.store(false, std::memory_order_relaxed);
            }

            // Use the calibrator
            for (size_t j = 0; j < 1000; ++j) {
                uint64_t tsc = Timestamp::rdtsc();
                volatile auto ns = cal.tsc_to_ns(tsc);
                (void)ns;
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_TRUE(all_same.load()) << "Calibrator not thread-safe";
}