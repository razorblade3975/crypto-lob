#pragma once

#include <cstdint>

#include <boost/intrusive/set.hpp>

#include "core/price.hpp"

namespace crypto_lob::core {

/// Price level node for intrusive RB-tree with order statistics
/// Memory layout optimized: hot data in first 32 bytes
/// NOTE: Keep hot members <= 32B; if expanded consider alignas(64)
struct alignas(64) PriceLevelNode
    : boost::intrusive::set_base_hook<boost::intrusive::optimize_size<false>,  // Better for 64-bit platforms
                                      boost::intrusive::link_mode<boost::intrusive::normal_link>> {
    Price price;                     // 16 bytes - 128-bit fixed point
    std::uint64_t quantity;          // 8 bytes - aggregated size at this price
    std::uint32_t update_seq;        // 4 bytes - last exchange sequence (relative)
    std::uint32_t subtree_size = 1;  // 4 bytes - nodes in subtree (for rank queries)
    // Total: 32 bytes (half cache line) - hot data ends here

    // Intrusive RB-tree hooks (24 bytes) inherited from base - cold data

    PriceLevelNode(Price p, std::uint64_t q, std::uint32_t seq) noexcept
        : price(p), quantity(q), update_seq(seq), subtree_size(1) {}

    PriceLevelNode() noexcept : price(0), quantity(0), update_seq(0), subtree_size(1) {}
};

// Verify memory layout assumptions
static_assert(sizeof(PriceLevelNode) == 128, "Node is 128B (cache-aligned)");
static_assert(alignof(PriceLevelNode) == 64, "Node must be cache-aligned");

/// Comparator for ask side (ascending price order)
struct AskComparator {
    // Enable heterogeneous lookup to avoid node construction
    using is_transparent = void;

    bool operator()(const Price& a, const Price& b) const noexcept {
        return a < b;
    }

    bool operator()(const PriceLevelNode& a, const PriceLevelNode& b) const noexcept {
        return a.price < b.price;
    }

    // Mixed comparisons for efficient key-based searches
    bool operator()(const Price& a, const PriceLevelNode& b) const noexcept {
        return a < b.price;
    }

    bool operator()(const PriceLevelNode& a, const Price& b) const noexcept {
        return a.price < b;
    }
};

/// Comparator for bid side (descending price order)
struct BidComparator {
    // Enable heterogeneous lookup to avoid node construction
    using is_transparent = void;

    bool operator()(const Price& a, const Price& b) const noexcept {
        return a > b;
    }

    bool operator()(const PriceLevelNode& a, const PriceLevelNode& b) const noexcept {
        return a.price > b.price;
    }

    // Mixed comparisons for efficient key-based searches
    bool operator()(const Price& a, const PriceLevelNode& b) const noexcept {
        return a > b.price;
    }

    bool operator()(const PriceLevelNode& a, const Price& b) const noexcept {
        return a.price > b;
    }
};

/// Simple price/quantity pair for snapshot events
struct PriceLevel {
    Price price;
    std::uint64_t quantity;

    [[nodiscard]] constexpr bool operator==(const PriceLevel& other) const noexcept {
        return price == other.price && quantity == other.quantity;
    }

    [[nodiscard]] constexpr bool operator!=(const PriceLevel& other) const noexcept {
        return !(*this == other);
    }
};

}  // namespace crypto_lob::core