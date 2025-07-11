#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>

namespace crypto_lob::core {

// Timestamp type (microseconds since epoch)
using Timestamp = uint64_t;

// Concept for type safety
template <typename T>
concept TimestampType = std::same_as<T, Timestamp>;

// Utility function to get current timestamp
inline Timestamp current_timestamp() noexcept {
    // Use system_clock for wall-clock time (not high_resolution_clock which may be monotonic)
    // This is critical for HFT systems that need real timestamps for market data
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

}  // namespace crypto_lob::core