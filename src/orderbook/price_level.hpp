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

/// Comparator for ask side (ascending price order)
struct AskComparator {
    // Enable heterogeneous lookup to avoid node construction
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

/// Comparator for bid side (descending price order)
struct BidComparator {
    // Enable heterogeneous lookup to avoid node construction
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