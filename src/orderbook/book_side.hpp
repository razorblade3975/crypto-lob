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

/// One side of an order book (bid or ask)
/// ⚠️ Never access RB-tree internals directly—use Boost iterator API only
template <typename Comparator>
class BookSide {
  public:
    using Node = PriceLevelNode;

    // Key extractor for heterogeneous lookup without temporary nodes
    struct PriceKeyOfValue {
        using type = Price;
        const type& operator()(const Node& node) const noexcept {
            return node.price;
        }
    };

    using Tree = boost::intrusive::set<Node,
                                       boost::intrusive::compare<Comparator>,
                                       boost::intrusive::key_of_value<PriceKeyOfValue>,
                                       boost::intrusive::constant_time_size<true>,  // O(1) size()
                                       boost::intrusive::optimize_size<false>       // Better for 64-bit platforms
                                       >;
    using Hash = boost::unordered_flat_map<Price, Node*>;
    using PoolType = MemoryPool<Node>;

    struct UpdateResult {
        std::int64_t delta_qty = 0;
        bool affects_top_n = false;
        bool is_new_level = false;
        bool is_deletion = false;
    };

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

    /// Apply an update to a price level
    UpdateResult apply_update(Price px, std::uint64_t qty, std::uint32_t seq) {
        UpdateResult result{0, false, false, false};  // Explicit initialization

        auto it = index_.find(px);
        if (it == index_.end()) {
            // New price level
            if (qty == 0) {
                return result;  // Nothing to do
            }

            auto* node = pool_.construct(px, qty, seq);
            auto insert_result = tree_.insert(*node);
            if (unlikely(!insert_result.second)) {
                // Critical error - new price insertion must succeed
                // Fail fast to prevent data corruption
                std::abort();
            }
            index_[px] = node;

            result.delta_qty = static_cast<std::int64_t>(qty);
            result.is_new_level = true;
            result.affects_top_n = affects_top_n(px);

            // Update nth price if needed
            if (tree_.size() == top_n_depth_ || (tree_.size() > top_n_depth_ && result.affects_top_n)) {
                update_nth_price();
            }

        } else {
            // Existing price level
            Node* node = it->second;
            std::uint64_t old_qty = node->quantity;

            if (qty == 0) {
                // Remove level
                bool was_in_top_n = affects_top_n(px);

                tree_.erase(tree_.iterator_to(*node));
                index_.erase(it);  // Use iterator erase to avoid extra lookup
                pool_.destroy(node);

                result.delta_qty = -static_cast<std::int64_t>(old_qty);
                result.is_deletion = true;
                result.affects_top_n = was_in_top_n;

                // Update nth price if needed
                if (tree_.size() == top_n_depth_ - 1 || was_in_top_n) {
                    update_nth_price();
                }

            } else {
                // Update quantity only
                node->quantity = qty;
                node->update_seq = seq;  // Wraps at 4B messages

                result.delta_qty = static_cast<std::int64_t>(qty) - static_cast<std::int64_t>(old_qty);
                result.affects_top_n = affects_top_n(px);
            }
        }

        return result;
    }

    /// Get the best price level
    [[nodiscard]] const Node* best() const noexcept {
        return tree_.empty() ? nullptr : &*tree_.begin();
    }

    /// Get price at position k (0-indexed) - O(k) without subtree sizes
    [[nodiscard]] const Node* kth_level(std::size_t k) const noexcept {
        if (k >= tree_.size())
            return nullptr;

        auto it = tree_.begin();
        std::advance(it, k);
        return &*it;
    }

    /// Fill top N levels into output array
    template <typename OutputIterator>
    std::size_t get_top_n(std::size_t n, OutputIterator out) const {
        std::size_t count = 0;
        auto it = tree_.begin();
        const auto end = tree_.end();

        while (it != end && count < n) {
            *out++ = PriceLevel{it->price, it->quantity};
            ++it;
            ++count;
        }

        return count;
    }

    /// Check if a price would be in the top N levels
    [[nodiscard]] bool affects_top_n(Price px) const noexcept {
        if (tree_.size() < top_n_depth_)
            return true;

        // For asks: price <= nth_price
        // For bids: price >= nth_price
        if constexpr (std::is_same_v<Comparator, AskComparator>) {
            return px <= nth_price_;
        } else {
            return px >= nth_price_;
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
        nth_price_ = Price{0};  // Sentinel: means "book narrower than N"; never used in affects_top_n()
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
    [[nodiscard]] const Node* find(Price px) const noexcept {
        auto it = index_.find(px);
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
    PoolType& pool_;
    Tree tree_;
    Hash index_;
    Price nth_price_{0};  // nth_price_ == 0 means "book narrower than N"; never used in affects_top_n()
    std::size_t top_n_depth_{20};

    // TODO: Consider caching iterator to nth node for micro-optimization
    void update_nth_price() {
        if (tree_.size() < top_n_depth_) {
            nth_price_ = Price{0};
            return;
        }

        auto it = tree_.begin();
        std::advance(it, top_n_depth_ - 1);
        nth_price_ = it->price;
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

using AskSide = BookSide<AskComparator>;
using BidSide = BookSide<BidComparator>;

}  // namespace crypto_lob::core