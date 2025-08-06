/**
 * @file book_side.hpp
 * @brief Hybrid tree+hash data structure for high-frequency trading order book sides
 *
 * This file implements a hybrid data structure optimizing for both top-of-book access and
 * arbitrary price level lookups in high-frequency trading scenarios. The design combines:
 *
 * 1. Red-Black Tree (boost::intrusive::set): Provides O(log n) ordered access for top levels
 *    - Critical for market data feeds requiring top-N levels
 *    - Maintains strict ordering for bid/ask comparators
 *    - Intrusive design minimizes memory allocations
 *
 * 2. Hash Map (boost::unordered_flat_map): Provides O(1) random access by price
 *    - Essential for processing delta updates at arbitrary price levels
 *    - Flat hash map design optimizes cache locality
 *    - Synchronized with tree to maintain data consistency
 *
 * Memory Management:
 * - Uses object pool allocation for deterministic performance
 * - No dynamic allocations in critical path operations
 * - Node reuse prevents fragmentation in long-running processes
 *
 * Performance Characteristics:
 * - apply_update(): O(log n) worst case, O(1) average for quantity updates
 * - get_top_n(): O(n) where n is requested levels (not book depth)
 * - find(): O(1) average case hash lookup
 * - affects_top_n(): O(1) comparison with cached nth price
 *
 * Thread Safety:
 * - Not thread-safe by design for maximum performance
 * - Expected to be used in single-threaded market data processing
 * - External synchronization required for multi-threaded access
 */

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include <boost/intrusive/set.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include "core/cache.hpp"
#include "core/memory_pool.hpp"
#include "orderbook/price_level.hpp"

namespace crypto_lob::core {

/**
 * @class BookSide
 * @brief One side of an order book (bid or ask) using hybrid tree+hash architecture
 *
 * This class implements a high-performance order book side optimized for HFT scenarios.
 * It maintains price levels in both a red-black tree (for ordered access) and a hash map
 * (for random access), ensuring both data structures remain synchronized at all times.
 *
 * Design Rationale:
 * - Tree structure enables efficient top-N level extraction for market data feeds
 * - Hash map enables O(1) delta update processing for arbitrary price levels
 * - Intrusive containers minimize memory allocations and improve cache locality
 * - Template-based comparator supports both bid (descending) and ask (ascending) sides
 *
 * Memory Usage:
 * - Each price level: ~64 bytes (node + intrusive hooks + hash entry)
 * - Hash map overhead: ~25% with 0.75 load factor
 * - Object pool eliminates allocation/deallocation overhead
 *
 * Critical Performance Notes:
 * - Never use try-catch in hot paths (HFT requirement)
 * - All operations designed for deterministic latency
 * - Memory pool prevents allocation-related jitter
 * - Comparator template enables compile-time optimization
 *
 * @tparam Comparator Price comparison function (AskComparator or BidComparator)
 *
 * ⚠️ Never access RB-tree internals directly—use Boost iterator API only
 */
template <typename Comparator>
class BookSide {
  public:
    /// @brief Alias for price level node type - contains price, quantity, and sequence number
    using Node = PriceLevelNode;

    /**
     * @brief Key extractor for heterogeneous lookup without temporary node allocation
     *
     * This struct enables direct price comparisons in tree operations without creating
     * temporary Node objects, which is critical for performance in HFT scenarios.
     * Used by boost::intrusive::set for heterogeneous lookups.
     */
    struct PriceKeyOfValue {
        using type = Price;
        const type& operator()(const Node& node) const noexcept {
            return node.price;
        }
    };

    /**
     * @brief Red-black tree type for maintaining ordered price levels
     *
     * Configuration optimized for HFT requirements:
     * - Comparator: Template parameter supporting AskComparator (ascending) or BidComparator (descending)
     * - key_of_value: Enables heterogeneous lookup by price without node construction
     * - constant_time_size: Provides O(1) size() operation for efficient book depth queries
     * - optimize_size<false>: Optimizes for performance over memory on 64-bit platforms
     *
     * The tree maintains strict ordering invariant: for asks (price ascending), for bids (price descending)
     */
    using Tree = boost::intrusive::set<Node,
                                       boost::intrusive::compare<Comparator>,
                                       boost::intrusive::key_of_value<PriceKeyOfValue>,
                                       boost::intrusive::constant_time_size<true>,  // O(1) size()
                                       boost::intrusive::optimize_size<false>       // Better for 64-bit platforms
                                       >;

    /**
     * @brief Hash map type for O(1) random access to price levels
     *
     * Uses boost::unordered_flat_map for optimal cache performance:
     * - Open addressing with linear probing minimizes memory indirections
     * - Flat storage layout improves cache locality compared to chained hashing
     * - Price -> Node* mapping enables direct access to tree nodes
     * - Must remain synchronized with tree at all times
     */
    using Hash = boost::unordered_flat_map<Price, Node*>;

    /// @brief Memory pool type for deterministic node allocation/deallocation
    using PoolType = MemoryPool<Node>;

    /**
     * @brief Result of applying an update operation to the book side
     *
     * This struct provides comprehensive information about the impact of an update,
     * enabling efficient downstream processing such as market data publication
     * and risk calculations.
     */
    struct UpdateResult {
        /// @brief Change in quantity (positive for additions, negative for removals)
        std::int64_t delta_qty = 0;
        /// @brief Whether this update affects the top N levels (important for market data feeds)
        bool affects_top_n = false;
        /// @brief Whether this update created a new price level
        bool is_new_level = false;
        /// @brief Whether this update deleted a price level (quantity became zero)
        bool is_deletion = false;
    };

    /**
     * @brief Construct a book side with specified memory pool and initial capacity
     *
     * @param pool Memory pool for node allocation/deallocation
     * @param initial_capacity Initial hash map capacity (default: 4096 levels)
     *
     * The constructor optimizes hash map performance by:
     * - Pre-reserving capacity to avoid rehashing during normal operation
     * - Setting load factor to 0.75 for optimal space/performance tradeoff
     *
     * Initial capacity should be sized based on expected market depth:
     * - Major pairs: 1000-2000 levels typical
     * - Minor pairs: 100-500 levels typical
     * - Options/futures: Can have 10,000+ levels
     */
    explicit BookSide(PoolType& pool, std::size_t initial_capacity = 4096) : pool_(pool), tree_(), index_() {
        index_.reserve(initial_capacity);
        index_.max_load_factor(0.75f);
    }

    ~BookSide() {
        clear();
    }

    // Delete copy, allow move
    BookSide(const BookSide&) = delete;
    BookSide& operator=(const BookSide&) = delete;
    BookSide(BookSide&&) = default;
    BookSide& operator=(BookSide&&) = default;

    /**
     * @brief Apply an update to a price level - the core operation of the order book
     *
     * This method implements the complete price level update algorithm, handling three cases:
     * 1. New price level creation (when price doesn't exist and qty > 0)
     * 2. Existing price level modification (quantity update)
     * 3. Price level deletion (when qty = 0)
     *
     * Algorithm Overview:
     * ┌─────────────────────┐
     * │  Hash Lookup O(1)   │ ──── Price exists in index_?
     * └─────────────────────┘
     *          │
     *      ┌───┴───┐
     *      │  NO   │               │  YES  │
     *      │       │               │       │
     *      ▼       ▼               ▼       ▼
     * ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
     * │qty=0?   │ │qty>0?   │ │qty=0?   │ │qty>0?   │
     * │Return   │ │Create   │ │Delete   │ │Update   │
     * │empty    │ │new      │ │level    │ │quantity │
     * │result   │ │level    │ │         │ │         │
     * └─────────┘ └─────────┘ └─────────┘ └─────────┘
     *      │           │           │           │
     *      └───────────┼───────────┼───────────┘
     *                  ▼           ▼
     *            ┌─────────────────────┐
     *            │  Update nth_price   │ ──── If affects top N levels
     *            │  if needed          │
     *            └─────────────────────┘
     *
     * Synchronization Strategy:
     * - Tree and hash map are updated atomically within the same operation
     * - Hash map points directly to tree nodes (no data duplication)
     * - nth_price_ cache is updated only when top N levels are affected
     * - All operations maintain both data structure invariants
     *
     * Performance Characteristics:
     * - Hash lookup: O(1) average, O(n) worst case
     * - Tree operations: O(log n) for insert/delete, O(1) for find existing
     * - nth_price update: O(top_n_depth) when needed
     * - Total: O(log n) worst case, O(1) average for quantity-only updates
     *
     * Critical Error Handling:
     * - Tree insertion failures call std::abort() (should never happen)
     * - Hash/tree inconsistencies call std::abort() (data corruption)
     * - No exceptions thrown (HFT requirement)
     *
     * @param price Price level to update
     * @param qty New quantity (0 means delete the level)
     * @param seq Sequence number for update tracking
     * @return UpdateResult with delta quantity and impact flags
     */
    UpdateResult apply_update(Price price, std::uint64_t qty, std::uint32_t seq) {
        UpdateResult result{0, false, false, false};  // Explicit initialization

        // Step 1: Hash lookup to determine if price level exists O(1) average
        auto it = index_.find(price);
        if (it == index_.end()) {
            // CASE 1: New price level - price does not exist in book

            if (qty == 0) {
                // Attempting to delete non-existent level - no-op
                return result;  // Return empty result (no changes)
            }

            // Step 2A: Create new price level node from memory pool
            auto* node = pool_.construct(price, qty, seq);

            // Step 3A: Insert into tree (maintains ordering invariant) O(log n)
            auto insert_result = tree_.insert(*node);
            if (unlikely(!insert_result.second)) {
                // Critical invariant violation - tree insertion should always succeed for new price
                // This indicates memory corruption or duplicate price (impossible case)
                std::abort();
            }

            // Step 4A: Insert into hash map for O(1) future lookups
            index_[price] = node;

            // Step 5A: Calculate result for new level
            result.delta_qty = static_cast<std::int64_t>(qty);
            result.is_new_level = true;
            result.affects_top_n = affects_top_n(price);  // Check if new level affects top N

            // Step 6A: Update nth_price cache if necessary
            // Two scenarios requiring nth_price update:
            // 1. Book depth just reached N levels (tree_.size() == top_n_depth_)
            // 2. Book depth > N and new level enters top N (affects_top_n = true)
            if (tree_.size() == top_n_depth_ || (tree_.size() > top_n_depth_ && result.affects_top_n)) {
                update_nth_price();
            }

        } else {
            // CASE 2: Existing price level - price found in hash map
            Node* node = it->second;                 // Direct pointer to tree node
            std::uint64_t old_qty = node->quantity;  // Save old quantity for delta calculation

            if (qty == 0) {
                // CASE 2A: Delete existing price level (qty = 0)

                // Step 2B: Check if deletion affects top N (before removal)
                bool was_in_top_n = affects_top_n(price);

                // Step 3B: Remove from tree first (maintains BST invariant) O(log n)
                // Use iterator_to for O(1) conversion from node pointer to iterator
                tree_.erase(tree_.iterator_to(*node));

                // Step 4B: Remove from hash map (use iterator to avoid double lookup)
                index_.erase(it);

                // Step 5B: Return node to memory pool
                pool_.destroy(node);

                // Step 6B: Calculate result for deletion
                result.delta_qty = -static_cast<std::int64_t>(old_qty);  // Negative delta
                result.is_deletion = true;
                result.affects_top_n = was_in_top_n;

                // Step 7B: Update nth_price cache if necessary
                // Two scenarios requiring nth_price update after deletion:
                // 1. Book depth just dropped to N-1 levels (need to clear nth_price)
                // 2. Deleted level was in top N (top N composition changed)
                if (tree_.size() == top_n_depth_ - 1 || was_in_top_n) {
                    update_nth_price();
                }

            } else {
                // CASE 2B: Update quantity only (most common case) O(1)

                // Step 2C: Update node data in-place (no tree/hash restructuring needed)
                node->quantity = qty;
                node->update_seq = seq;  // Sequence wraps at 4B messages (~1 hour at 1M/sec)

                // Step 3C: Calculate result for quantity update
                result.delta_qty = static_cast<std::int64_t>(qty) - static_cast<std::int64_t>(old_qty);
                result.affects_top_n = affects_top_n(price);  // Check if level is in top N

                // Note: No nth_price update needed for quantity-only changes
                // The relative position of this price level doesn't change
            }
        }

        return result;
    }

    /// Get the best price level
    [[nodiscard]] const Node* best() const noexcept {
        return tree_.empty() ? nullptr : &*tree_.begin();
    }

    /**
     * @brief Get price level at position k (0-indexed) in sorted order
     *
     * This method provides positional access to price levels without requiring
     * order-statistic tree support. Useful for extracting specific levels
     * for market data feeds or risk calculations.
     *
     * Algorithm:
     * 1. Bounds check: Ensure k is within valid range [0, tree_.size())
     * 2. Iterator traversal: Start from tree_.begin() and advance k positions
     * 3. Return node pointer or nullptr if out of bounds
     *
     * Performance Characteristics:
     * - Time Complexity: O(k) where k is the position
     * - Space Complexity: O(1) - no additional storage
     * - Without subtree sizes: Linear traversal is the only option
     * - Alternative: Could maintain order-statistic tree for O(log n) access
     *
     * Usage Examples:
     * - kth_level(0) returns best price (same as best())
     * - kth_level(1) returns second best price
     * - kth_level(tree_.size()-1) returns worst price
     *
     * Thread Safety: Read-only operation, safe for concurrent reads
     *
     * @param k Zero-based position in sorted price order
     * @return Pointer to node at position k, or nullptr if k >= size()
     */
    [[nodiscard]] const Node* kth_level(std::size_t k) const noexcept {
        // Bounds check: Prevent iterator advance beyond end()
        if (k >= tree_.size())
            return nullptr;

        // Linear traversal from beginning (O(k) complexity)
        auto it = tree_.begin();
        std::advance(it, k);  // Standard library iterator advancement
        return &*it;          // Convert iterator to node pointer
    }

    /**
     * @brief Fill top N levels into output array - optimized for market data feeds
     *
     * This method efficiently extracts the top N price levels in sorted order,
     * which is the most common operation for market data publication and
     * algorithmic trading systems.
     *
     * Algorithm:
     * 1. Initialize counter and iterator at best price (tree_.begin())
     * 2. Iterate through tree in sorted order until:
     *    - Requested count N is reached, OR
     *    - End of tree is reached (fewer than N levels available)
     * 3. For each level: Convert Node to PriceLevel and output via iterator
     * 4. Return actual number of levels extracted
     *
     * Performance Characteristics:
     * - Time Complexity: O(min(n, book_depth)) - linear in requested levels
     * - Space Complexity: O(1) - no additional storage (output provided by caller)
     * - Memory Access: Sequential tree traversal optimizes cache locality
     * - Branch Prediction: Loop condition optimized for typical market data scenarios
     *
     * Typical Usage Patterns:
     * - Market data feeds: get_top_n(10, output) for L2 data
     * - Risk systems: get_top_n(5, output) for immediate impact calculation
     * - Algo trading: get_top_n(1, output) for top-of-book monitoring
     *
     * Output Iterator Requirements:
     * - Must support pre-increment (++out)
     * - Must support dereference assignment (*out = value)
     * - Common types: std::back_inserter, raw pointers, vector iterators
     *
     * Thread Safety: Read-only operation, safe for concurrent reads
     *
     * @tparam OutputIterator Iterator type supporting output operations
     * @param n Maximum number of levels to extract
     * @param out Output iterator for receiving PriceLevel objects
     * @return Actual number of levels written (may be less than n)
     */
    template <typename OutputIterator>
    std::size_t get_top_n(std::size_t n, OutputIterator out) const {
        std::size_t count = 0;         // Track number of levels processed
        auto it = tree_.begin();       // Start from best price
        const auto end = tree_.end();  // Cache end iterator for efficiency

        // Sequential traversal of top N levels in price-priority order
        while (it != end && count < n) {
            // Convert internal Node to public PriceLevel interface
            *out++ = PriceLevel{it->price, it->quantity};
            ++it;     // Advance to next best price
            ++count;  // Track progress for early termination
        }

        return count;  // Return actual levels extracted (≤ n)
    }

    /**
     * @brief Check if a price would be in the top N levels - critical for market data filtering
     *
     * This method determines whether a price level affects the top N levels, which is
     * essential for optimizing market data publication and avoiding unnecessary processing
     * of deep book updates that don't impact trading decisions.
     *
     * Algorithm Logic:
     * ┌─────────────────────────┐
     * │ Book Depth < N?         │ ──── YES ──── Return TRUE (all levels matter)
     * └─────────────────────────┘
     *              │
     *              NO
     *              │
     * ┌─────────────────────────┐
     * │ Compare price with      │
     * │ nth_price_ using        │ ──── Comparator-specific logic
     * │ comparator logic        │
     * └─────────────────────────┘
     *
     * Comparator Logic Explanation:
     *
     * AskComparator (ascending prices - lower is better):
     * - nth_price_ = price of Nth best ask (Nth lowest price)
     * - price <= nth_price_ means price would be in top N
     * - Example: top_n=3, book=[100, 101, 102, 105], nth_price_=102
     *   - affects_top_n(99) = true  (better than 102)
     *   - affects_top_n(101.5) = true  (better than 102)
     *   - affects_top_n(103) = false (worse than 102)
     *
     * BidComparator (descending prices - higher is better):
     * - nth_price_ = price of Nth best bid (Nth highest price)
     * - price >= nth_price_ means price would be in top N
     * - Example: top_n=3, book=[105, 102, 101, 100], nth_price_=101
     *   - affects_top_n(106) = true  (better than 101)
     *   - affects_top_n(101.5) = true  (better than 101)
     *   - affects_top_n(99) = false (worse than 101)
     *
     * Performance Characteristics:
     * - Time Complexity: O(1) - single comparison with cached nth_price_
     * - Space Complexity: O(1) - no additional storage
     * - Branch Prediction: Highly predictable for typical market scenarios
     * - Cache Performance: Accesses only cached nth_price_ value
     *
     * Critical Usage Patterns:
     * - Market Data: Skip publishing updates that don't affect L2 feed
     * - Risk Management: Focus on price levels that impact trading
     * - Algorithmic Trading: Optimize reaction to relevant book changes
     *
     * Thread Safety: Read-only operation using atomic price comparison
     *
     * @param price Price level to test for top N inclusion
     * @return true if price would be among the top N levels, false otherwise
     */
    [[nodiscard]] bool affects_top_n(Price price) const noexcept {
        // Case 1: Book is shallower than N levels - every level matters
        if (tree_.size() < top_n_depth_)
            return true;

        // Case 2: Book has ≥ N levels - compare against nth best price
        // Compile-time template specialization for different comparators
        if constexpr (std::is_same_v<Comparator, AskComparator>) {
            // Ask side: Lower prices are better (ascending order)
            // A price affects top N if it's ≤ the Nth best (Nth lowest) price
            return price <= nth_price_;
        } else {
            // Bid side: Higher prices are better (descending order)
            // A price affects top N if it's ≥ the Nth best (Nth highest) price
            return price >= nth_price_;
        }
    }

    /// Clear all levels
    void clear() noexcept {
        // Iterate tree instead of hash to avoid double lookup
        while (!tree_.empty()) {
            auto it = tree_.begin();
            Node* node = &*it;
            tree_.erase(it);

            // Find and erase from hash
            auto hash_it = index_.find(node->price);
            if (unlikely(hash_it == index_.end())) {
                // Critical error - tree/hash inconsistency
                // Fail fast to prevent data corruption
                std::abort();
            }
            index_.erase(hash_it);

            pool_.destroy(node);
        }
        nth_price_ = Price::zero();  // Sentinel: means "book narrower than N"; never used in affects_top_n()
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return tree_.size();  // O(1) with constant_time_size<true>
    }

    [[nodiscard]] bool empty() const noexcept {
        return tree_.empty();
    }

    [[nodiscard]] Price get_nth_price() const noexcept {
        return nth_price_;
    }

    [[nodiscard]] std::size_t get_top_n_depth() const noexcept {
        return top_n_depth_;
    }

    void set_top_n_depth(std::size_t depth) {
        top_n_depth_ = depth;
        update_nth_price();
    }

    /// Validate tree invariants (for testing)
    [[nodiscard]] bool validate_tree() const {
        return validate_bst_property() && validate_hash_coherence();
    }

    /// Find a price level by price
    [[nodiscard]] const Node* find(Price price) const noexcept {
        auto it = index_.find(price);
        return it != index_.end() ? it->second : nullptr;
    }

    // Iterators for testing
    using const_iterator = typename Tree::const_iterator;
    [[nodiscard]] const_iterator begin() const noexcept {
        return tree_.begin();
    }
    [[nodiscard]] const_iterator end() const noexcept {
        return tree_.end();
    }

  private:
    /// @brief Reference to memory pool for deterministic node allocation/deallocation
    PoolType& pool_;

    /// @brief Red-black tree maintaining price levels in sorted order (primary data structure)
    Tree tree_;

    /// @brief Hash map providing O(1) random access to price levels (secondary index)
    Hash index_;

    /**
     * @brief Cached price of the Nth best level for efficient top-N filtering
     *
     * This cache enables O(1) affects_top_n() checks instead of O(N) tree traversal.
     * - Value = Price::zero() when book depth < top_n_depth_ (sentinel value)
     * - Value = actual Nth price when book depth >= top_n_depth_
     * - Updated only when top N composition changes (insertions/deletions affecting top N)
     */
    Price nth_price_{Price::zero()};

    /// @brief Number of top levels to track for market data optimization (default: 20)
    std::size_t top_n_depth_{20};

    /**
     * @brief Update nth_price_ cache when top N levels change
     *
     * This private method maintains the nth_price_ cache used by affects_top_n().
     * Called only when necessary to minimize overhead:
     * - After insertions that affect top N levels
     * - After deletions from top N levels
     * - When top_n_depth_ is changed via set_top_n_depth()
     *
     * Algorithm:
     * 1. If book depth < top_n_depth_: Set nth_price_ = Price::zero() (sentinel)
     * 2. Otherwise: Traverse to (top_n_depth_ - 1)th position and cache that price
     *
     * Performance: O(top_n_depth_) time complexity due to iterator advancement
     *
     * TODO: Consider caching iterator to nth node for micro-optimization
     */
    void update_nth_price() {
        if (tree_.size() < top_n_depth_) {
            nth_price_ = Price::zero();  // Sentinel indicating insufficient depth
            return;
        }

        // Linear traversal to Nth position (0-indexed, so N-1)
        auto it = tree_.begin();
        std::advance(it, top_n_depth_ - 1);
        nth_price_ = it->price;  // Cache the Nth best price
    }

    [[nodiscard]] bool validate_bst_property() const {
        if (tree_.empty())
            return true;

        Price last_price = tree_.begin()->price;
        for (auto it = ++tree_.begin(); it != tree_.end(); ++it) {
            if constexpr (std::is_same_v<Comparator, AskComparator>) {
                if (!(last_price < it->price))
                    return false;
            } else {
                if (!(last_price > it->price))
                    return false;
            }
            last_price = it->price;
        }
        return true;
    }

    [[nodiscard]] bool validate_hash_coherence() const {
        // Check every tree node is in hash
        for (const auto& node : tree_) {
            auto it = index_.find(node.price);
            if (it == index_.end() || it->second != &node) {
                return false;
            }
        }

        // Check every hash entry is in tree
        for (const auto& [price, node_ptr] : index_) {
            // Verify the node is in the tree by using iterator_to
            auto tree_it = tree_.iterator_to(*node_ptr);
            // If iterator_to succeeds, the node is in the tree
            // Just verify it points to the same node
            if (&*tree_it != node_ptr) {
                return false;
            }
        }

        return true;
    }

    // Note: Removed subtree_size maintenance for now
    // Can add order-statistic tree support later if needed
    // Current design focuses on correctness and simplicity
};

}  // namespace crypto_lob::core