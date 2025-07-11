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

/// Top of book snapshot with cached values
struct alignas(64) TopOfBook {
    Price bid_price{static_cast<int64_t>(0)};
    Price ask_price{static_cast<int64_t>(0)};
    std::uint64_t bid_qty{0};
    std::uint64_t ask_qty{0};

    [[nodiscard]] constexpr bool valid() const noexcept {
        return !bid_price.is_zero() && !ask_price.is_zero() && bid_price < ask_price;
    }

    [[nodiscard]] constexpr Price spread() const noexcept {
        return ask_price - bid_price;
    }

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
        // Cache old top for comparison
        auto old_bid_top = bids_.best();
        auto old_ask_top = asks_.best();

        // Apply update to appropriate side
        if (side == Side::BUY) {
            bids_.apply_update(price, quantity, 0);
        } else {
            asks_.apply_update(price, quantity, 0);
        }

        // Check if top changed
        auto new_bid_top = bids_.best();
        auto new_ask_top = asks_.best();

        bool bid_changed = (old_bid_top != new_bid_top) ||
                           (old_bid_top && new_bid_top && old_bid_top->quantity != new_bid_top->quantity);

        bool ask_changed = (old_ask_top != new_ask_top) ||
                           (old_ask_top && new_ask_top && old_ask_top->quantity != new_ask_top->quantity);

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

    /// Get top N levels for each side
    template <typename OutputIterator>
    void get_levels(std::size_t depth, OutputIterator bid_out, OutputIterator ask_out) const {
        bids_.get_top_n(depth, bid_out);
        asks_.get_top_n(depth, ask_out);
    }

    /// Check if spread is crossed
    [[nodiscard]] bool is_crossed() const noexcept {
        const auto* best_bid = bids_.best();
        const auto* best_ask = asks_.best();

        if (!best_bid || !best_ask)
            return false;

        return best_bid->price >= best_ask->price;
    }

    // Accessors
    [[nodiscard]] const InstrumentId& instrument() const noexcept {
        return instrument_;
    }
    [[nodiscard]] std::size_t bid_depth() const noexcept {
        return bids_.size();
    }
    [[nodiscard]] std::size_t ask_depth() const noexcept {
        return asks_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return bids_.empty() && asks_.empty();
    }

    // Direct access for testing
    [[nodiscard]] const BidSide& bids() const noexcept {
        return bids_;
    }
    [[nodiscard]] const AskSide& asks() const noexcept {
        return asks_;
    }

    // Find a specific price level
    [[nodiscard]] const PriceLevelNode* find_level(Side side, Price price) const noexcept {
        return (side == Side::BUY) ? bids_.find(price) : asks_.find(price);
    }

  private:
    void update_cached_tob() noexcept {
        const auto* best_bid = bids_.best();
        const auto* best_ask = asks_.best();

        cached_tob_.bid_price = best_bid ? best_bid->price : Price{static_cast<int64_t>(0)};
        cached_tob_.bid_qty = best_bid ? best_bid->quantity : 0;
        cached_tob_.ask_price = best_ask ? best_ask->price : Price{static_cast<int64_t>(0)};
        cached_tob_.ask_qty = best_ask ? best_ask->quantity : 0;
    }

    InstrumentId instrument_;
    BidSide bids_;
    AskSide asks_;

    // Cached top of book for ultra-fast access
    TopOfBook cached_tob_{};
};

// Verify cache alignment
static_assert(alignof(OrderBook) == CACHELINE_SIZE, "OrderBook must be cache-aligned");

}  // namespace crypto_lob::core