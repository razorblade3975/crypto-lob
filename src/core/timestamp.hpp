#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace crypto_lob::core {

/**
 * HFT-optimized timestamp utilities using RDTSC
 *
 * RDTSC (Read Time-Stamp Counter) provides ~3ns latency vs ~30-50ns for clock_gettime
 * Critical for HFT where every nanosecond matters
 */
class Timestamp {
  public:
    // Get raw TSC value - fastest possible timestamp (~3ns)
    [[gnu::always_inline, gnu::hot]] static inline uint64_t rdtsc() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        uint32_t lo, hi;
        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        // Fallback for non-x86 architectures
        return std::chrono::steady_clock::now().time_since_epoch().count();
#endif
    }

    // Get TSC with serialization - ensures all previous instructions complete
    // Use when you need guaranteed ordering (slightly slower ~10ns)
    [[gnu::always_inline]] static inline uint64_t rdtscp() noexcept {
#if defined(__x86_64__)
        uint32_t lo, hi, aux;
        __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(aux));
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return rdtsc();
#endif
    }

    // Memory fence + RDTSC for strict ordering
    [[gnu::always_inline]] static inline uint64_t rdtsc_ordered() noexcept {
#if defined(__x86_64__)
        uint32_t lo, hi;
        __asm__ volatile("mfence\n\t"
                         "rdtsc"
                         : "=a"(lo), "=d"(hi)
                         :
                         : "memory");
        return (static_cast<uint64_t>(hi) << 32) | lo;
#else
        return rdtsc();
#endif
    }

    // Calibration support - convert TSC to nanoseconds
    class Calibrator {
      public:
        Calibrator() {
            calibrate();
        }

        // Convert TSC ticks to nanoseconds
        [[gnu::always_inline]] uint64_t tsc_to_ns(uint64_t tsc_delta) const noexcept {
            return (tsc_delta * 1000000000ULL) / tsc_frequency_;
        }

        // Get TSC frequency in Hz
        uint64_t frequency() const noexcept {
            return tsc_frequency_;
        }

      private:
        void calibrate() {
            // Calibrate TSC frequency using steady_clock
            auto start_time = std::chrono::steady_clock::now();
            uint64_t start_tsc = rdtsc();

            // Calibration period - 10ms should be enough
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            auto end_time = std::chrono::steady_clock::now();
            uint64_t end_tsc = rdtsc();

            auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count();
            uint64_t tsc_delta = end_tsc - start_tsc;

            // Calculate frequency: ticks per second
            tsc_frequency_ = (tsc_delta * 1000000000ULL) / elapsed_ns;
        }

        std::atomic<uint64_t> tsc_frequency_{3000000000ULL};  // Default 3GHz
    };

    // Global calibrator instance (thread-safe initialization)
    static Calibrator& calibrator() {
        static Calibrator instance;
        return instance;
    }
};

// Convenience function for hot path
[[gnu::always_inline, gnu::hot]] inline uint64_t rdtsc() noexcept {
    return Timestamp::rdtsc();
}

}  // namespace crypto_lob::core