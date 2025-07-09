#pragma once

#include <cstdint>
#include <concepts>
#include <chrono>

namespace crypto_lob::core {

// Timestamp type (microseconds since epoch)
using Timestamp = uint64_t;

// Concept for type safety
template<typename T>
concept TimestampType = std::same_as<T, Timestamp>;

// Utility function to get current timestamp
inline Timestamp current_timestamp() noexcept {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

}  // namespace crypto_lob::core