#pragma once

#include <boost/unordered/unordered_flat_map.hpp>
#include <cassert>
#include <cstdint>
#include <type_traits>

#include "core/memory_pool.hpp"

namespace crypto_lob::core {

/// Simple hash map for order ID lookups
/// WARNING: Not thread-safe - caller must provide external synchronization
template <typename T>
class OrderMap {
    static_assert(std::is_pointer_v<T>, "OrderMap only supports pointer values");
    
public:
    using OrderID = uint64_t;
    using ValueType = T;
    using MapType = boost::unordered_flat_map<OrderID, ValueType>;
    using const_iterator = typename MapType::const_iterator;
    using iterator = typename MapType::iterator;
    
    explicit OrderMap(size_t initial_capacity = 65536)
        : map_() {
        map_.max_load_factor(0.75f);
        map_.reserve(initial_capacity);
    }
    
    [[nodiscard]] bool insert(OrderID order_id, ValueType value) noexcept {
        auto [it, inserted] = map_.try_emplace(order_id, value);
        return inserted;
    }
    
    [[nodiscard]] ValueType find(OrderID order_id) const noexcept {
        auto it = map_.find(order_id);
        return (it != map_.end()) ? it->second : nullptr;
    }
    
    [[nodiscard]] bool update(OrderID order_id, ValueType new_value) noexcept {
        auto it = map_.find(order_id);
        if (it != map_.end()) {
            it->second = new_value;
            return true;
        }
        return false;
    }
    
    bool remove(OrderID order_id) noexcept {
        return map_.erase(order_id) > 0;
    }
    
    void clear() noexcept {
        map_.clear();
    }
    
    [[nodiscard]] size_t size() const noexcept {
        return map_.size();
    }
    
    [[nodiscard]] bool empty() const noexcept {
        return map_.empty();
    }
    
    [[nodiscard]] size_t capacity() const noexcept {
        return map_.capacity();
    }
    
    void reserve(size_t new_capacity) {
        map_.reserve(new_capacity);
    }
    
    [[nodiscard]] iterator begin() noexcept { return map_.begin(); }
    [[nodiscard]] iterator end() noexcept { return map_.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return map_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return map_.end(); }
    
private:
    MapType map_;
};

/// Hash map that integrates with MemoryPool for Order objects
/// WARNING: Not thread-safe - caller must provide external synchronization
template <typename Order>
class PooledOrderMap {
public:
    using OrderID = uint64_t;
    using OrderPtr = Order*;
    using PoolType = MemoryPool<Order>;
    using MapType = boost::unordered_flat_map<OrderID, OrderPtr>;
    
    PooledOrderMap(PoolType& order_pool, size_t initial_capacity = 65536)
        : pool_(&order_pool),
          map_() {
        map_.max_load_factor(0.75f);
        map_.reserve(initial_capacity);
    }
    
    ~PooledOrderMap() {
        clear_and_destroy_all();
    }
    
    PooledOrderMap(const PooledOrderMap&) = delete;
    PooledOrderMap& operator=(const PooledOrderMap&) = delete;
    
    PooledOrderMap(PooledOrderMap&& other) noexcept
        : pool_(other.pool_),
          map_(std::move(other.map_)) {
        assert(other.pool_ == pool_ && "cannot move between different pools");
        other.pool_ = nullptr;
    }
    
    PooledOrderMap& operator=(PooledOrderMap&& other) noexcept {
        if (this != &other) {
            assert(other.pool_ == pool_ && "cannot move between different pools");
            clear_and_destroy_all();
            pool_ = other.pool_;
            map_ = std::move(other.map_);
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    /// Construct Order and insert into map
    /// If try_emplace throws, constructed Order will leak (acceptable for HFT)
    template <typename... Args>
    std::pair<OrderPtr, bool> emplace(OrderID order_id, Args&&... args) {
        // Check if key exists first (fast path)
        auto it = map_.find(order_id);
        if (it != map_.end()) {
            return {it->second, false};
        }
        
        // Construct order, then insert
        auto* order = pool_->construct(std::forward<Args>(args)...);
        auto [insert_it, inserted] = map_.try_emplace(order_id, order);
        
        // If try_emplace throws, we leak one order - acceptable for HFT
        return {order, true};
    }
    
    [[nodiscard]] OrderPtr find(OrderID order_id) const noexcept {
        auto it = map_.find(order_id);
        return (it != map_.end()) ? it->second : nullptr;
    }
    
    bool remove_and_destroy(OrderID order_id) noexcept {
        auto it = map_.find(order_id);
        if (it != map_.end()) {
            OrderPtr order = it->second;
            map_.erase(it);
            pool_->destroy(order);
            return true;
        }
        return false;
    }
    
    void clear_and_destroy_all() noexcept {
        if (pool_) {
            for (auto& [id, order] : map_) {
                pool_->destroy(order);
            }
            map_.clear();
        }
    }
    
    [[nodiscard]] size_t size() const noexcept { return map_.size(); }
    [[nodiscard]] bool empty() const noexcept { return map_.empty(); }
    [[nodiscard]] size_t capacity() const noexcept { return map_.capacity(); }
    
    void reserve(size_t new_capacity) {
        map_.reserve(new_capacity);
    }
    
    auto begin() noexcept { return map_.begin(); }
    auto end() noexcept { return map_.end(); }
    auto begin() const noexcept { return map_.begin(); }
    auto end() const noexcept { return map_.end(); }
    
private:
    PoolType* pool_;
    MapType map_;
};

}  // namespace crypto_lob::core