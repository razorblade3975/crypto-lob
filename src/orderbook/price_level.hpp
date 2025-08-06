/**
 * @file price_level.hpp
 * @brief Price level data structures for high-frequency trading order book implementation
 *
 * This file defines the core data structures used to represent price levels in an order book:
 *
 * 1. PriceLevelNode: Cache-aligned intrusive RB-tree node optimized for HFT performance
 *    - 64-byte aligned for cache line optimization
 *    - Hot data (price, quantity) packed in first 32 bytes
 *    - Supports O(log k) order statistics via subtree_size
 *
 * 2. AskComparator/BidComparator: Transparent comparators for heterogeneous lookup
 *    - Enables direct Price-based searches without temporary node construction
 *    - Critical for maintaining sub-microsecond latency requirements
 *
 * 3. PriceLevel: Simple price/quantity pair for snapshot serialization
 *    - Lightweight structure for market data distribution
 *    - No intrusive hooks or alignment requirements
 *
 * Memory layout and cache optimization are critical for HFT performance.
 * All structures are designed to minimize cache misses and memory allocations.
 */

#pragma once

#include <cstdint>

#include <boost/intrusive/set.hpp>

#include "core/price.hpp"

namespace crypto_lob::core {

/**
 * @brief Cache-aligned price level node for intrusive RB-tree with order statistics
 *
 * Memory Layout Optimization:
 * - 64-byte cache line alignment prevents false sharing in multi-threaded access
 * - Hot data (price, quantity, seq, subtree_size) packed in first 24 bytes
 * - Intrusive RB-tree hooks (24 bytes) are considered "cold" data
 * - Total size: 64 bytes (exactly one cache line)
 *
 * Design Rationale:
 * - Does NOT inherit from generic intrusive node types to maintain optimal layout
 * - Uses boost::intrusive::set_base_hook for minimal overhead
 * - optimize_size<false> preferred on 64-bit platforms for better performance
 * - normal_link mode provides automatic cleanup and debugging support
 *
 * Performance Characteristics:
 * - Cache-friendly: hot path accesses stay within single cache line
 * - Lock-free: intrusive design eliminates memory allocation in critical path
 * - Order statistics: subtree_size enables O(log k) rank/select operations
 *
 * @warning Keep hot data members <= 32 bytes. If expansion needed, consider alignas(64)
 */
struct alignas(64) PriceLevelNode
    : boost::intrusive::set_base_hook<boost::intrusive::optimize_size<false>,  // Better for 64-bit platforms
                                      boost::intrusive::link_mode<boost::intrusive::normal_link>> {
    Price price;                     // 8 bytes - 64-bit fixed point
    std::uint64_t quantity;          // 8 bytes - aggregated size at this price
    std::uint32_t update_seq;        // 4 bytes - last exchange sequence (relative)
    std::uint32_t subtree_size = 1;  // 4 bytes - nodes in subtree (for rank queries)
    // Total: 24 bytes - hot data ends here

    // Intrusive RB-tree hooks (24 bytes) inherited from base - cold data

    PriceLevelNode(Price price_arg, std::uint64_t qty, std::uint32_t seq) noexcept
        : price(price_arg), quantity(qty), update_seq(seq), subtree_size(1) {}

    PriceLevelNode() noexcept : price(Price::zero()), quantity(0), update_seq(0), subtree_size(1) {}
};

// Verify memory layout assumptions
static_assert(sizeof(PriceLevelNode) == 64, "Node is 64B (cache-aligned)");
static_assert(alignof(PriceLevelNode) == 64, "Node must be cache-aligned");

/**
 * @brief Comparator for ask side with ascending price order (lowest price first)
 *
 * Sort Order:
 * - Ascending price order: 100.50 < 100.51 < 100.52
 * - Best ask (lowest price) appears first in the tree
 * - Consistent with market convention where best ask = lowest selling price
 *
 * Heterogeneous Lookup Support:
 * - is_transparent typedef enables C++14 transparent comparisons
 * - Allows direct Price-based find(), lower_bound(), upper_bound() operations
 * - Eliminates temporary PriceLevelNode construction in hot path
 * - Critical for sub-microsecond latency requirements in HFT
 *
 * Mixed Comparison Operators:
 * - Supports all combinations: Price vs Price, Node vs Node, Price vs Node, Node vs Price
 * - Enables efficient key-based searches without object conversion overhead
 * - All operators are noexcept for maximum performance
 */
struct AskComparator {
    /// Enable heterogeneous lookup to avoid temporary node construction
    /// Required for C++14 transparent comparisons in associative containers
    using is_transparent = void;

    bool operator()(const Price& lhs, const Price& rhs) const noexcept {
        return lhs < rhs;
    }

    bool operator()(const PriceLevelNode& lhs, const PriceLevelNode& rhs) const noexcept {
        return lhs.price < rhs.price;
    }

    // Mixed comparisons for efficient key-based searches
    bool operator()(const Price& lhs, const PriceLevelNode& rhs) const noexcept {
        return lhs < rhs.price;
    }

    bool operator()(const PriceLevelNode& lhs, const Price& rhs) const noexcept {
        return lhs.price < rhs;
    }
};

/**
 * @brief Comparator for bid side with descending price order (highest price first)
 *
 * Sort Order:
 * - Descending price order: 100.52 > 100.51 > 100.50
 * - Best bid (highest price) appears first in the tree
 * - Consistent with market convention where best bid = highest buying price
 *
 * Heterogeneous Lookup Support:
 * - is_transparent typedef enables C++14 transparent comparisons
 * - Allows direct Price-based find(), lower_bound(), upper_bound() operations
 * - Eliminates temporary PriceLevelNode construction in hot path
 * - Critical for sub-microsecond latency requirements in HFT
 *
 * Mixed Comparison Operators:
 * - Supports all combinations: Price vs Price, Node vs Node, Price vs Node, Node vs Price
 * - Enables efficient key-based searches without object conversion overhead
 * - All operators are noexcept for maximum performance
 * - Uses > instead of < to achieve descending order
 */
struct BidComparator {
    /// Enable heterogeneous lookup to avoid temporary node construction
    /// Required for C++14 transparent comparisons in associative containers
    using is_transparent = void;

    bool operator()(const Price& lhs, const Price& rhs) const noexcept {
        return lhs > rhs;
    }

    bool operator()(const PriceLevelNode& lhs, const PriceLevelNode& rhs) const noexcept {
        return lhs.price > rhs.price;
    }

    // Mixed comparisons for efficient key-based searches
    bool operator()(const Price& lhs, const PriceLevelNode& rhs) const noexcept {
        return lhs > rhs.price;
    }

    bool operator()(const PriceLevelNode& lhs, const Price& rhs) const noexcept {
        return lhs.price > rhs;
    }
};

/**
 * @brief Simple price/quantity pair for market data snapshots and serialization
 *
 * Design Purpose:
 * - Lightweight structure for snapshot events and market data distribution
 * - No intrusive hooks or cache alignment requirements (unlike PriceLevelNode)
 * - Plain Old Data (POD) type suitable for direct serialization
 * - Used in market data feeds, REST API responses, and inter-process communication
 *
 * Memory Layout:
 * - Price: 8 bytes (64-bit fixed-point value)
 * - Quantity: 8 bytes (64-bit unsigned integer)
 * - Total: 16 bytes (no padding, naturally aligned)
 * - Compact representation minimizes bandwidth in market data feeds
 *
 * Operations:
 * - Equality comparison for snapshot diffing and validation
 * - All operations are constexpr and noexcept for maximum performance
 * - No ordering operators (use external comparators if needed)
 */
struct PriceLevel {
    Price price;             ///< Price level (64-bit fixed-point)
    std::uint64_t quantity;  ///< Aggregated quantity at this price level

    /**
     * @brief Equality comparison for price level validation and diffing
     * @param other The price level to compare against
     * @return true if both price and quantity are equal, false otherwise
     *
     * Used for:
     * - Snapshot validation and consistency checking
     * - Market data diffing to detect changes
     * - Unit testing and verification
     */
    [[nodiscard]] constexpr bool operator==(const PriceLevel& other) const noexcept {
        return price == other.price && quantity == other.quantity;
    }

    /**
     * @brief Inequality comparison (negation of equality)
     * @param other The price level to compare against
     * @return true if price or quantity differs, false if equal
     *
     * Implemented as negation of operator== for consistency and performance.
     * Useful for change detection in market data processing pipelines.
     */
    [[nodiscard]] constexpr bool operator!=(const PriceLevel& other) const noexcept {
        return !(*this == other);
    }
};

/**
 * @note PriceKeyOfValue for boost::intrusive
 *
 * The PriceKeyOfValue struct is defined in book_side.hpp rather than this file.
 * It extracts the Price key from PriceLevelNode for boost::intrusive::set operations.
 * This separation follows the single responsibility principle:
 * - price_level.hpp: Core data structures and comparators
 * - book_side.hpp: Container configuration and key extraction utilities
 *
 * PriceKeyOfValue enables heterogeneous lookup by allowing the intrusive set
 * to extract the Price field from PriceLevelNode for comparison operations,
 * which is essential for the transparent comparison functionality provided
 * by AskComparator and BidComparator.
 */

}  // namespace crypto_lob::core