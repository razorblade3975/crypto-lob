#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
// Note: <new> would be for std::hardware_destructive_interference_size, but we use fixed 64

namespace crypto_lob::core {

// Cache line size constant - centralized to prevent magic number sprawl
static constexpr std::size_t CACHELINE_SIZE = 64;

// Cache line alignment macro for structures
#define CACHE_ALIGNED alignas(CACHELINE_SIZE)

// Struct wrapper for cache-aligned types (alignment only)
template <typename T>
struct CACHE_ALIGNED Aligned64 {
    T value;
};

// Struct wrapper for cache-aligned types with full 64-byte size
template <typename T>
struct CACHE_ALIGNED Sized64 {
    T value;
    std::array<std::byte, CACHELINE_SIZE - sizeof(T)> padding{};

    static_assert(sizeof(T) <= CACHELINE_SIZE, "Type too large for cache line");
};

// Cache-aligned padding struct for preventing false sharing
template <std::size_t N>
struct CACHE_ALIGNED CachePad {
    std::array<std::byte, N> data{};
};

// Align size to cache line boundary using bit-math for hot path performance
[[nodiscard]] constexpr std::size_t align_to_cacheline(std::size_t size) noexcept {
    return (size + CACHELINE_SIZE - 1) & ~std::size_t(CACHELINE_SIZE - 1);
}

// Check if size is cache line aligned
[[nodiscard]] constexpr bool is_cacheline_aligned(std::size_t size) noexcept {
    return (size & (CACHELINE_SIZE - 1)) == 0;
}

// Check if pointer is cache line aligned
// Note: Cannot be constexpr because reinterpret_cast is not allowed in constant expressions
[[nodiscard]] inline bool is_cacheline_aligned(const void* ptr) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return (reinterpret_cast<std::uintptr_t>(ptr) & (CACHELINE_SIZE - 1)) == 0;
}

// Software prefetch hints for predictable memory access patterns
enum class PrefetchHint : int {
    READ_TEMPORAL = 3,      // High temporal locality (will be accessed again soon)
    READ_NON_TEMPORAL = 0,  // Low temporal locality (streaming access)
    WRITE_TEMPORAL = 1,     // Prepare for write with temporal locality
    WRITE_NON_TEMPORAL = 2  // Prepare for write, low temporal locality
};

// Prefetch memory location to reduce cache miss latency
// addr: Memory address to prefetch
// hint: Temporal locality hint for the CPU cache hierarchy
inline void cache_prefetch(const void* addr, PrefetchHint hint = PrefetchHint::READ_TEMPORAL) noexcept {
    // __builtin_prefetch requires compile-time constant arguments, so we use a switch
    switch (hint) {
        case PrefetchHint::READ_TEMPORAL:
            __builtin_prefetch(static_cast<const char*>(addr), 0, 3);
            break;
        case PrefetchHint::READ_NON_TEMPORAL:
            __builtin_prefetch(static_cast<const char*>(addr), 0, 0);
            break;
        case PrefetchHint::WRITE_TEMPORAL:
            __builtin_prefetch(static_cast<const char*>(addr), 1, 1);
            break;
        case PrefetchHint::WRITE_NON_TEMPORAL:
            __builtin_prefetch(static_cast<const char*>(addr), 1, 2);
            break;
    }
}

// Utility to calculate optimal padding between two members
template <typename T1, typename T2>
[[nodiscard]] constexpr std::size_t padding_between() noexcept {
    static_assert(sizeof(T1) <= CACHELINE_SIZE && sizeof(T2) <= CACHELINE_SIZE,
                  "Types larger than cache line may cause padding wraparound");

    std::size_t total_size = sizeof(T1) + sizeof(T2);
    const std::size_t aligned_size = align_to_cacheline(total_size);
    return aligned_size - total_size;
}

// Cache-friendly structure layout verification
template <typename T>
[[nodiscard]] constexpr bool is_cache_friendly() noexcept {
    return sizeof(T) <= CACHELINE_SIZE && alignof(T) >= CACHELINE_SIZE;
}

// Portable compiler barrier - does NOT flush CPU cache, only prevents reordering
inline void memory_barrier() noexcept {
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

// Branch prediction hints for hot paths - guard against libc conflicts
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif

#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

}  // namespace crypto_lob::core