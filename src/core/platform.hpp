#pragma once

#include <cstddef>

// Platform detection and requirements
#ifndef __linux__
    #error "This codebase requires Linux for huge pages, NUMA support, and epoll"
#endif

#ifndef __clang__
    #error "This codebase requires Clang compiler for optimal HFT performance"
#endif

#if __cplusplus < 202002L
    #error "This codebase requires C++20 or later"
#endif

// Verify 128-bit integer support (required for Price class)
#if !defined(__SIZEOF_INT128__)
    #error "128-bit integer support required for fixed-point price arithmetic"
#endif

// Fallback for hardware interference size
// libc++ doesn't provide these due to ABI stability concerns
#ifdef __cpp_lib_hardware_interference_size
    // Standard library provides it
#else
    // Provide our own fallback based on common x86-64 cache line size
    namespace std {
        inline constexpr std::size_t hardware_destructive_interference_size = 64;
        inline constexpr std::size_t hardware_constructive_interference_size = 64;
    }
#endif

namespace crypto_lob::core {

// Platform-specific constants
constexpr bool IS_LINUX = true;
constexpr bool HAS_HUGE_PAGES = true;
constexpr bool HAS_NUMA_SUPPORT = true;

}  // namespace crypto_lob::core