/**
 * @file order_book.hpp
 * @brief High-performance order book implementation optimized for HFT applications
 *
 * This file contains the core OrderBook class and related structures for maintaining
 * bid/ask price levels in a cryptocurrency limit order book. The implementation is
 * designed for ultra-low latency operations with the following key characteristics:
 *
 * Architecture Overview:
 * - Pure data structure with no state machine or event buffering
 * - Cache-aligned to prevent false sharing in multi-threaded environments
 * - Memory pool-based allocation to eliminate runtime heap allocations
 * - Optimized for read-heavy workloads with cached top-of-book access
 * - Single-threaded design for maximum performance within LOB worker threads
 *
 * Performance Characteristics:
 * - O(log n) insert/update/delete operations via red-black trees
 * - O(1) top-of-book access through intelligent caching
 * - Zero-allocation updates in steady state (pre-allocated memory pool)
 * - Cache-friendly data layout for optimal CPU utilization
 *
 * Thread Safety:
 * - NOT thread-safe by design - intended for single-threaded LOB workers
 * - Cache-aligned to prevent false sharing when multiple books exist
 * - External synchronization required for multi-threaded access
 *
 * Memory Management:
 * - Uses shared memory pool for all price level allocations
 * - Automatic memory reclamation when price levels are removed
 * - No per-update heap allocations in steady state operation
 * - Configurable initial capacity and depth limits
 */

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "core/cache.hpp"
#include "core/enums.hpp"
#include "core/memory_pool.hpp"
#include "core/price.hpp"
#include "core/timestamp.hpp"
#include "orderbook/book_side.hpp"

namespace crypto_lob::core {

// Type aliases for bid and ask sides
using AskSide = BookSide<AskComparator>;
using BidSide = BookSide<BidComparator>;

/**
 * @brief Top of book snapshot containing cached best bid/ask prices and quantities
 *
 * This structure provides ultra-fast access to the most critical order book data
 * without requiring tree traversals. It is cache-aligned to optimize memory access
 * patterns and prevent false sharing when accessed frequently.
 *
 * Cache Strategy:
 * - Updated only when best bid or ask changes (price or quantity)
 * - Eliminates need for tree traversals on every top-of-book query
 * - Provides O(1) access to spread calculations and validity checks
 *
 * Memory Layout:
 * - 64-byte aligned for optimal cache line utilization
 * - Fits entirely within a single cache line on most modern CPUs
 * - Sequential layout optimizes prefetch behavior
 *
 * Validity Semantics:
 * - Zero prices indicate no liquidity on that side
 * - Crossed book (bid >= ask) indicates invalid market state
 * - Empty book results in all-zero TopOfBook structure
 */
struct alignas(64) TopOfBook {
    Price bid_price{static_cast<int64_t>(0)};  ///< Best bid price (highest bid)
    Price ask_price{static_cast<int64_t>(0)};  ///< Best ask price (lowest ask)
    std::uint64_t bid_qty{0};                  ///< Quantity available at best bid
    std::uint64_t ask_qty{0};                  ///< Quantity available at best ask

    /**
     * @brief Check if the top of book represents a valid market state
     * @return true if both sides have valid prices and book is not crossed
     *
     * A valid top of book requires:
     * - Non-zero bid and ask prices (liquidity exists on both sides)
     * - Bid price < ask price (market is not crossed)
     */
    [[nodiscard]] constexpr bool valid() const noexcept {
        return !bid_price.is_zero() && !ask_price.is_zero() && bid_price < ask_price;
    }

    /**
     * @brief Calculate the bid-ask spread
     * @return Price difference between ask and bid
     * @note Returns zero or negative value if book is invalid or crossed
     */
    [[nodiscard]] constexpr Price spread() const noexcept {
        return ask_price - bid_price;
    }

    /**
     * @brief Compare two TopOfBook structures for exact equality
     * @param other The TopOfBook to compare against
     * @return true if all fields match exactly
     */
    [[nodiscard]] constexpr bool operator==(const TopOfBook& other) const noexcept {
        return bid_price == other.bid_price && ask_price == other.ask_price && bid_qty == other.bid_qty &&
               ask_qty == other.ask_qty;
    }
};

/// Pure order book data structure - no state machine, no buffering, no events
/// Thread-safety: NOT thread-safe, designed for single-threaded LOB worker
///
/// Design philosophy:
/// - This is a pure data structure focused solely on maintaining bid/ask levels
/// - update()/snapshot() return values tell you everything about BBO movement
/// - OrderBook itself stores no transient state (no flags, no counters)
/// - Cache-aligned to prevent false sharing when multiple books are processed by different threads
///
/// Performance notes:
/// - Top of book is cached and updated only when needed
/// - No per-update heap allocations (uses memory pool)
/// - Optimized for read-heavy access patterns
class alignas(CACHELINE_SIZE) OrderBook {
  public:
    using PoolType = MemoryPool<PriceLevelNode>;

    struct Config {
        std::size_t initial_capacity = 4096;
        std::size_t top_n_depth = 20;
    };

    explicit OrderBook(const InstrumentId& instrument, PoolType& pool)
        : instrument_(instrument), bids_(pool, 4096), asks_(pool, 4096) {
        bids_.set_top_n_depth(20);
        asks_.set_top_n_depth(20);
    }

    explicit OrderBook(const InstrumentId& instrument, PoolType& pool, const Config& config)
        : instrument_(instrument), bids_(pool, config.initial_capacity), asks_(pool, config.initial_capacity) {
        bids_.set_top_n_depth(config.top_n_depth);
        asks_.set_top_n_depth(config.top_n_depth);
    }

    ~OrderBook() = default;

    // Delete copy and move operations due to BidSide/AskSide constraints
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&) = delete;
    OrderBook& operator=(OrderBook&&) = delete;

    /// Clear all levels from both sides
    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        cached_tob_ = TopOfBook{};
    }

    /// Apply a single update to the book
    /// @return true if top of book changed, false otherwise
    /// Caller can use this return value to track BBO changes without storing state
    [[nodiscard]] bool update(Side side, Price price, std::uint64_t quantity) {
        // Cache old top and quantities for comparison
        // Note: we must cache quantities before apply_update modifies them in-place
        auto old_bid_top = bids_.best();
        auto old_ask_top = asks_.best();
        uint64_t old_bid_qty = old_bid_top ? old_bid_top->quantity : 0;
        uint64_t old_ask_qty = old_ask_top ? old_ask_top->quantity : 0;

        // Apply update to appropriate side
        if (side == Side::BUY) {
            bids_.apply_update(price, quantity, 0);
        } else {
            asks_.apply_update(price, quantity, 0);
        }

        // Check if top changed (price or quantity)
        auto new_bid_top = bids_.best();
        auto new_ask_top = asks_.best();

        bool bid_changed = (old_bid_top != new_bid_top) || (new_bid_top && old_bid_qty != new_bid_top->quantity);

        bool ask_changed = (old_ask_top != new_ask_top) || (new_ask_top && old_ask_qty != new_ask_top->quantity);

        // Update cached TOB if needed
        if (bid_changed || ask_changed) {
            update_cached_tob();
            return true;
        }

        return false;
    }

    /// Apply a full snapshot (clears book first)
    /// @return true (snapshot always changes the book state)
    /// Caller should treat snapshot as a full book replacement event
    bool snapshot(const std::vector<PriceLevel>& bid_levels, const std::vector<PriceLevel>& ask_levels) {
        clear();

        for (const auto& level : bid_levels) {
            if (level.quantity > 0) {
                bids_.apply_update(level.price, level.quantity, 0);
            }
        }

        for (const auto& level : ask_levels) {
            if (level.quantity > 0) {
                asks_.apply_update(level.price, level.quantity, 0);
            }
        }

        update_cached_tob();
        return true;  // Snapshot always represents a state change
    }

    /// Get current top of book (cached for performance)
    [[nodiscard]] const TopOfBook& top_of_book() const noexcept {
        return cached_tob_;
    }

    /**
     * @brief Extract the top N price levels from both sides of the book
     * @tparam OutputIterator Iterator type that accepts PriceLevelNode objects
     * @param depth Maximum number of levels to extract from each side
     * @param bid_out Output iterator for bid levels (highest to lowest price)
     * @param ask_out Output iterator for ask levels (lowest to highest price)
     *
     * Usage:
     * @code
     * std::vector<PriceLevelNode> bid_levels, ask_levels;
     * book.get_levels(10, std::back_inserter(bid_levels), std::back_inserter(ask_levels));
     * @endcode
     *
     * Performance:
     * - O(min(depth, actual_levels)) complexity
     * - No heap allocations, uses pre-computed top-N cache when possible
     * - Levels are returned in trading-priority order (bids: high->low, asks: low->high)
     *
     * Thread Safety:
     * - Read-only operation, safe to call from observer threads
     * - Must not be called concurrently with update operations
     */
    template <typename OutputIterator>
    void get_levels(std::size_t depth, OutputIterator bid_out, OutputIterator ask_out) const {
        bids_.get_top_n(depth, bid_out);
        asks_.get_top_n(depth, ask_out);
    }

    /**
     * @brief Check if the order book is in a crossed state
     * @return true if the best bid price is greater than or equal to the best ask price
     *
     * A crossed book occurs when:
     * - Best bid price >= best ask price
     * - This typically indicates a momentary market imbalance
     * - Can happen during rapid price movements or data feed issues
     * - May signal arbitrage opportunities or system errors
     *
     * Market Implications:
     * - Crossed books are generally invalid market states
     * - Should trigger immediate attention in trading systems
     * - May require book rebuild or special handling
     * - Can indicate stale or corrupted market data
     *
     * Performance:
     * - O(1) operation using cached best bid/ask pointers
     * - Returns false if either side is empty (no cross possible)
     *
     * Thread Safety:
     * - Read-only operation, safe from observer threads
     * - Must not be called concurrently with update operations
     */
    [[nodiscard]] bool is_crossed() const noexcept {
        const auto* best_bid = bids_.best();
        const auto* best_ask = asks_.best();

        if (!best_bid || !best_ask)
            return false;

        return best_bid->price >= best_ask->price;
    }

    /**
     * @name Basic Accessors
     * @brief Fundamental order book state and identification methods
     *
     * Thread Safety: All accessor methods are read-only and safe to call from
     * observer threads, but must not be called concurrently with update operations.
     * @{
     */

    /**
     * @brief Get the instrument identifier for this order book
     * @return Reference to the InstrumentId (e.g., "BTC-USD", "ETH-BTC")
     *
     * The instrument ID is immutable for the lifetime of the order book instance.
     * Used for routing, logging, and identifying which market this book represents.
     */
    [[nodiscard]] const InstrumentId& instrument() const noexcept {
        return instrument_;
    }

    /**
     * @brief Get the number of distinct bid price levels
     * @return Count of active bid levels with non-zero quantities
     *
     * Performance: O(1) - maintains cached size counter
     * This represents the depth of the bid side, not the total quantity.
     */
    [[nodiscard]] std::size_t bid_depth() const noexcept {
        return bids_.size();
    }

    /**
     * @brief Get the number of distinct ask price levels
     * @return Count of active ask levels with non-zero quantities
     *
     * Performance: O(1) - maintains cached size counter
     * This represents the depth of the ask side, not the total quantity.
     */
    [[nodiscard]] std::size_t ask_depth() const noexcept {
        return asks_.size();
    }

    /**
     * @brief Check if the order book contains no liquidity
     * @return true if both bid and ask sides are empty
     *
     * An empty book indicates no active orders or market makers.
     * This is different from a book with only one side populated.
     */
    [[nodiscard]] bool empty() const noexcept {
        return bids_.empty() && asks_.empty();
    }

    /** @} */  // End of Basic Accessors group

    /**
     * @name Direct Side Access
     * @brief Low-level access to bid and ask side implementations
     *
     * These methods provide direct access to the underlying BookSide implementations.
     * Primarily intended for testing, debugging, and advanced use cases.
     *
     * Thread Safety: Read-only access is safe from observer threads,
     * but external synchronization required during concurrent updates.
     * @{
     */

    /**
     * @brief Get direct read-only access to the bid side implementation
     * @return Reference to the BidSide (BookSide<BidComparator>) instance
     *
     * Provides access to all BookSide methods including iterators, tree operations,
     * and advanced queries. Use with caution in production code.
     */
    [[nodiscard]] const BidSide& bids() const noexcept {
        return bids_;
    }

    /**
     * @brief Get direct read-only access to the ask side implementation
     * @return Reference to the AskSide (BookSide<AskComparator>) instance
     *
     * Provides access to all BookSide methods including iterators, tree operations,
     * and advanced queries. Use with caution in production code.
     */
    [[nodiscard]] const AskSide& asks() const noexcept {
        return asks_;
    }

    /** @} */  // End of Direct Side Access group

    /**
     * @brief Find a specific price level on either side of the book
     * @param side Which side to search (Side::BUY for bids, Side::SELL for asks)
     * @param price The exact price to search for
     * @return Pointer to PriceLevelNode if found, nullptr if not found
     *
     * Performance: O(log n) tree search operation
     *
     * Usage:
     * @code
     * auto* level = book.find_level(Side::BUY, Price{100'000'000});
     * if (level) {
     *     std::cout << "Quantity at $1000.00: " << level->quantity << std::endl;
     * }
     * @endcode
     *
     * Thread Safety:
     * - Read-only operation, safe from observer threads
     * - Returned pointer may become invalid after subsequent updates
     * - Do not cache the returned pointer across update operations
     */
    [[nodiscard]] const PriceLevelNode* find_level(Side side, Price price) const noexcept {
        return (side == Side::BUY) ? bids_.find(price) : asks_.find(price);
    }

  private:
    /**
     * @brief Update the cached top-of-book snapshot from current best bid/ask
     *
     * Cache Management Strategy:
     * - Called only when best bid or ask changes (price or quantity)
     * - Avoids unnecessary cache updates when deeper levels change
     * - Maintains cache coherency without performance overhead
     * - Uses null-pointer checks to handle empty sides gracefully
     *
     * Performance Optimization:
     * - Single tree traversal to get best() pointers
     * - Direct assignment to cache-aligned structure
     * - No heap allocations or complex computations
     * - Optimized for frequent top-of-book queries
     *
     * Cache Invalidation:
     * - Zero prices indicate no liquidity on that side
     * - Cache remains valid until next update/snapshot operation
     * - Automatically maintained by update() and snapshot() methods
     */
    void update_cached_tob() noexcept {
        const auto* best_bid = bids_.best();
        const auto* best_ask = asks_.best();

        cached_tob_.bid_price = best_bid ? best_bid->price : Price{static_cast<int64_t>(0)};
        cached_tob_.bid_qty = best_bid ? best_bid->quantity : 0;
        cached_tob_.ask_price = best_ask ? best_ask->price : Price{static_cast<int64_t>(0)};
        cached_tob_.ask_qty = best_ask ? best_ask->quantity : 0;
    }

    InstrumentId instrument_;  ///< Immutable instrument identifier
    BidSide bids_;             ///< Bid side implementation (BookSide<BidComparator>)
    AskSide asks_;             ///< Ask side implementation (BookSide<AskComparator>)

    /**
     * @brief Cached top-of-book data for ultra-fast access
     *
     * Cache Strategy:
     * - Updated only when best bid/ask changes
     * - Eliminates tree traversal for frequent top-of-book queries
     * - Cache-aligned for optimal memory access patterns
     * - Provides O(1) access to most critical order book data
     */
    TopOfBook cached_tob_{};
};

// Verify cache alignment
static_assert(alignof(OrderBook) == CACHELINE_SIZE, "OrderBook must be cache-aligned");

}  // namespace crypto_lob::core