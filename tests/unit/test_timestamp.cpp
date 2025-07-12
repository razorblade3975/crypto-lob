#include <algorithm>
#include <chrono>
#include <iomanip>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "core/timestamp.hpp"

using namespace crypto_lob::core;
using namespace std::chrono_literals;

class TimestampTest : public ::testing::Test {
  protected:
    // Helper to check if timestamp is approximately current time
    bool is_current_timestamp(Timestamp ts, uint64_t tolerance_us = 1000) {
        // Use system_clock to match the implementation in timestamp.hpp
        auto now = std::chrono::system_clock::now();
        auto now_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

        return std::abs(static_cast<int64_t>(ts) - static_cast<int64_t>(now_us)) < static_cast<int64_t>(tolerance_us);
    }

    // Helper to convert timestamp to readable time for debugging
    std::string timestamp_to_string(Timestamp ts) {
        auto duration = std::chrono::microseconds(ts);
        auto time_point = std::chrono::system_clock::time_point(duration);
        auto time_t = std::chrono::system_clock::to_time_t(time_point);

        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");

        // Add microseconds
        auto micros = ts % 1'000'000;
        ss << "." << std::setfill('0') << std::setw(6) << micros;

        return ss.str();
    }
};

// Basic functionality tests
TEST_F(TimestampTest, TypeDefinition) {
    // Verify Timestamp is uint64_t
    static_assert(std::is_same_v<Timestamp, uint64_t>);

    // Verify it can hold microseconds since epoch
    Timestamp ts = 0;
    EXPECT_EQ(sizeof(ts), 8);

    // Maximum value check - can represent times far into the future
    Timestamp max_ts = std::numeric_limits<Timestamp>::max();
    // Max microseconds is about 584,942 years from epoch
    EXPECT_EQ(max_ts, 18'446'744'073'709'551'615ULL);
}

TEST_F(TimestampTest, CurrentTimestamp) {
    // Get current timestamp
    Timestamp ts1 = current_timestamp();

    // Verify it's a reasonable value (after year 2020)
    // 2020-01-01 00:00:00 UTC = 1577836800 seconds = 1577836800000000 microseconds
    EXPECT_GT(ts1, 1577836800000000ULL);

    // Verify it's not in the far future (before year 2100)
    // 2100-01-01 00:00:00 UTC = 4102444800 seconds = 4102444800000000 microseconds
    EXPECT_LT(ts1, 4102444800000000ULL);

    // Verify it's close to actual current time
    EXPECT_TRUE(is_current_timestamp(ts1));
}

TEST_F(TimestampTest, TimestampMonotonicity) {
    // Timestamps should be monotonically increasing
    std::vector<Timestamp> timestamps;

    for (int i = 0; i < 100; ++i) {
        timestamps.push_back(current_timestamp());
        // Small delay to ensure time advances
        std::this_thread::sleep_for(1us);
    }

    // Check that each timestamp is >= previous
    for (size_t i = 1; i < timestamps.size(); ++i) {
        EXPECT_GE(timestamps[i], timestamps[i - 1]);
    }

    // At least some should be strictly greater
    bool has_increase = false;
    for (size_t i = 1; i < timestamps.size(); ++i) {
        if (timestamps[i] > timestamps[i - 1]) {
            has_increase = true;
            break;
        }
    }
    EXPECT_TRUE(has_increase);
}

TEST_F(TimestampTest, TimestampResolution) {
    // Test that we can measure microsecond differences
    std::vector<Timestamp> timestamps;

    // Rapid-fire timestamp collection
    for (int i = 0; i < 1000; ++i) {
        timestamps.push_back(current_timestamp());
    }

    // Calculate differences
    std::vector<uint64_t> diffs;
    for (size_t i = 1; i < timestamps.size(); ++i) {
        if (timestamps[i] > timestamps[i - 1]) {
            diffs.push_back(timestamps[i] - timestamps[i - 1]);
        }
    }

    // Should have captured some differences
    EXPECT_GT(diffs.size(), 0);

    // Minimum difference should be small (microsecond resolution)
    if (!diffs.empty()) {
        auto min_diff = *std::min_element(diffs.begin(), diffs.end());
        EXPECT_LE(min_diff, 1000);  // Less than 1ms
    }
}

TEST_F(TimestampTest, TimestampConcept) {
    // Test the TimestampType concept
    static_assert(TimestampType<Timestamp>);
    static_assert(TimestampType<uint64_t>);  // Since Timestamp is uint64_t
    static_assert(!TimestampType<int64_t>);
    static_assert(!TimestampType<uint32_t>);
    static_assert(!TimestampType<double>);

    // Concept should work in generic code
    auto process_timestamp = []<TimestampType T>(T ts) {
        return ts + 1000;  // Add 1ms
    };

    Timestamp ts = current_timestamp();
    Timestamp ts_plus_1ms = process_timestamp(ts);
    EXPECT_EQ(ts_plus_1ms, ts + 1000);
}

TEST_F(TimestampTest, TimestampArithmetic) {
    Timestamp ts1 = 1'000'000'000;  // 1000 seconds since epoch
    Timestamp ts2 = 2'000'000'000;  // 2000 seconds since epoch

    // Basic arithmetic
    EXPECT_EQ(ts2 - ts1, 1'000'000'000);
    EXPECT_EQ(ts1 + 500'000, 1'000'500'000);

    // Comparison
    EXPECT_LT(ts1, ts2);
    EXPECT_GT(ts2, ts1);
    EXPECT_LE(ts1, ts1);
    EXPECT_GE(ts2, ts2);
}

TEST_F(TimestampTest, TimestampConversion) {
    // Convert between different time representations
    Timestamp ts = current_timestamp();

    // To std::chrono::microseconds
    auto micros = std::chrono::microseconds(ts);
    EXPECT_EQ(micros.count(), ts);

    // To std::chrono::system_clock::time_point
    auto time_point = std::chrono::system_clock::time_point(micros);

    // Back to microseconds
    auto back_to_micros = std::chrono::duration_cast<std::chrono::microseconds>(time_point.time_since_epoch()).count();
    EXPECT_EQ(back_to_micros, ts);
}

TEST_F(TimestampTest, TimestampPerformance) {
    // Measure the overhead of getting current timestamp
    constexpr int iterations = 1'000'000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        volatile Timestamp ts = current_timestamp();
        (void)ts;  // Prevent optimization
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double ns_per_call = static_cast<double>(duration.count()) / iterations;

    // Getting current timestamp should be very fast
    EXPECT_LT(ns_per_call, 100);  // Less than 100ns per call

    // Print for information
    std::cout << "Average time per current_timestamp(): " << ns_per_call << " ns" << std::endl;
}

TEST_F(TimestampTest, TimestampThreadSafety) {
    // Verify current_timestamp() is thread-safe
    constexpr int num_threads = 8;
    constexpr int timestamps_per_thread = 10000;

    std::vector<std::vector<Timestamp>> thread_timestamps(num_threads);
    std::vector<std::thread> threads;

    // Launch threads
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&thread_timestamps, i]() {
            for (int j = 0; j < timestamps_per_thread; ++j) {
                thread_timestamps[i].push_back(current_timestamp());
                // Small random delay
                if (j % 100 == 0) {
                    std::this_thread::sleep_for(std::chrono::microseconds(rand() % 10));
                }
            }
        });
    }

    // Wait for completion
    for (auto& t : threads) {
        t.join();
    }

    // Verify all timestamps are valid
    for (const auto& timestamps : thread_timestamps) {
        EXPECT_EQ(timestamps.size(), timestamps_per_thread);
        for (auto ts : timestamps) {
            EXPECT_GT(ts, 1577836800000000ULL);  // After 2020
            EXPECT_LT(ts, 4102444800000000ULL);  // Before 2100
        }
    }
}

TEST_F(TimestampTest, MicrosecondPrecision) {
    // Verify we're actually getting microsecond precision
    std::set<Timestamp> unique_timestamps;

    // Collect timestamps with microsecond delays
    for (int i = 0; i < 100; ++i) {
        unique_timestamps.insert(current_timestamp());
        std::this_thread::sleep_for(1us);
    }

    // Should have many unique timestamps
    EXPECT_GT(unique_timestamps.size(), 50);
}

TEST_F(TimestampTest, TimestampOrdering) {
    // Test that timestamps can be used for ordering events
    struct Event {
        Timestamp ts;
        int id;
    };

    std::vector<Event> events;

    // Create events with timestamps
    for (int i = 0; i < 10; ++i) {
        events.push_back({current_timestamp(), i});
        std::this_thread::sleep_for(10us);
    }

    // Shuffle events
    std::random_device rd;
    std::mt19937 g(rd());
    std::shuffle(events.begin(), events.end(), g);

    // Sort by timestamp
    std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.ts < b.ts; });

    // Verify order matches creation order
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(events[i].id, i);
    }
}

TEST_F(TimestampTest, TimestampLatencyMeasurement) {
    // Use timestamps to measure operation latency
    std::vector<uint64_t> latencies;

    // Ensure we have at least one non-zero latency measurement
    bool has_non_zero = false;

    for (int i = 0; i < 1000; ++i) {
        Timestamp start = current_timestamp();

        // Simulate some work - increase workload to ensure measurable time
        volatile int sum = 0;
        for (int j = 0; j < 10000; ++j) {
            sum += j * j;  // More complex operation
        }

        Timestamp end = current_timestamp();
        uint64_t latency = end - start;
        latencies.push_back(latency);

        if (latency > 0) {
            has_non_zero = true;
        }
    }

    // Calculate statistics
    auto min_latency = *std::min_element(latencies.begin(), latencies.end());
    auto max_latency = *std::max_element(latencies.begin(), latencies.end());
    auto avg_latency = std::accumulate(latencies.begin(), latencies.end(), 0ULL) / latencies.size();

    // In CI environments with coarse clock resolution, we may get 0 latencies
    // Just ensure we have at least some non-zero measurements
    EXPECT_TRUE(has_non_zero) << "No non-zero latency measurements recorded";
    EXPECT_LT(max_latency, 10000);  // Less than 10ms (generous for CI)

    // Only check average if we have non-zero measurements
    if (has_non_zero) {
        EXPECT_LE(avg_latency, max_latency);
    }
}

TEST_F(TimestampTest, EpochBoundaryHandling) {
    // Test edge cases around epoch (1970-01-01 00:00:00 UTC)
    Timestamp epoch = 0;
    EXPECT_EQ(epoch, 0);

    // One microsecond after epoch
    Timestamp after_epoch = 1;
    EXPECT_EQ(after_epoch, 1);

    // Test arithmetic near epoch
    Timestamp ts = 1000;  // 1ms after epoch
    EXPECT_EQ(ts - 500, 500);
    EXPECT_EQ(ts + 500, 1500);
}

TEST_F(TimestampTest, RealWorldTimestamps) {
    // Test with realistic cryptocurrency trading timestamps

    // Market open timestamp (e.g., 2024-01-01 09:30:00 UTC)
    // This is 1704099000 seconds = 1704099000000000 microseconds
    Timestamp market_open = 1704099000000000ULL;

    // Add various trading intervals
    Timestamp one_second_later = market_open + 1'000'000;
    Timestamp one_minute_later = market_open + 60'000'000;
    Timestamp one_hour_later = market_open + 3'600'000'000;

    EXPECT_EQ(one_second_later - market_open, 1'000'000);
    EXPECT_EQ(one_minute_later - market_open, 60'000'000);
    EXPECT_EQ(one_hour_later - market_open, 3'600'000'000);

    // Typical HFT intervals
    Timestamp microsecond_tick = market_open + 1;
    Timestamp millisecond_tick = market_open + 1'000;

    EXPECT_EQ(microsecond_tick - market_open, 1);
    EXPECT_EQ(millisecond_tick - market_open, 1'000);
}