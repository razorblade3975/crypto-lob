#pragma once

#include "platform.hpp"  // Platform requirements and detection
#include "cache.hpp"     // For CACHELINE_SIZE

#include <cstddef>
#include <memory>
#include <new>
#include <concepts>
#include <atomic>
#include <cassert>
#include <bit>
#include <type_traits>
#include <cstdlib>
#include <stdexcept>
#include <array>
#include <thread>
#include <chrono>
#include <vector>

// Linux-specific headers for huge pages
#include <sys/mman.h>
#include <unistd.h>

namespace crypto_lob::core {

// Fixed concept - removed trivially destructible requirement
template<typename T>
concept Allocatable = std::is_default_constructible_v<T> &&
                      sizeof(T) <= 1024;  // Reasonable size limit

// Pool depletion policy
enum class PoolDepletionPolicy {
    THROW_EXCEPTION,    // Throw std::bad_alloc
    TERMINATE_PROCESS,  // std::terminate (fail-fast for HFT)
    DYNAMIC_RESIZE     // Allocate new blocks (adds complexity)
};

// Configuration for thread-local caching
struct CacheConfig {
    size_t cache_size = 64;      // Size of thread-local cache
    size_t batch_size = 32;      // Number of objects to transfer in batch operations
    size_t shrink_threshold = 16; // Shrink cache when below this utilization
    std::chrono::milliseconds shrink_interval{1000}; // How often to check for shrinking
    bool use_huge_pages = true;  // Enable huge pages for better TLB performance
    bool prefault_pages = true;  // Pre-fault huge pages to avoid runtime page faults
    size_t max_prefault_mb = 256; // Maximum memory to prefault (MB)
    
    static constexpr size_t MAX_CACHE_SIZE = 512;
    static constexpr size_t MAX_BATCH_SIZE = 256;
    
    CacheConfig() = default;
    
    CacheConfig(size_t cache_sz, size_t batch_sz, bool huge_pages = true, bool prefault = true) 
        : cache_size(std::min(cache_sz, MAX_CACHE_SIZE))
        , batch_size(std::min(batch_sz, MAX_BATCH_SIZE))
        , use_huge_pages(huge_pages)
        , prefault_pages(prefault) {
        
        // Ensure batch_size <= cache_size
        if (batch_size > cache_size) {
            batch_size = cache_size / 2;
        }
    }
};

// Utility function to round up to next power of two
constexpr size_t round_up_to_power_of_two(size_t value) noexcept {
    if (value == 0) return 1;
    return size_t{1} << (64 - std::countl_zero(value - 1));
}

// Round up to nearest multiple of alignment with safety checks
constexpr std::size_t round_up_to_alignment(std::size_t size, std::size_t alignment) noexcept {
    // Fix #4: Turn runtime assert into hard check that works in release builds
    if (!alignment || !std::has_single_bit(alignment)) {
        std::terminate(); // Hard failure for invalid alignment
    }
    return ((size + alignment - 1) / alignment) * alignment;
}

// Allocation result with metadata
struct AllocationResult {
    void* ptr;
    bool used_huge_pages;
    size_t actual_size;
    
    AllocationResult(void* p = nullptr, bool huge = false, size_t sz = 0) 
        : ptr(p), used_huge_pages(huge), actual_size(sz) {}
};

// Huge page allocator with pre-faulting support
class HugePageAllocator {
private:
    static constexpr size_t HUGE_PAGE_SIZE_2MB = 2 * 1024 * 1024;  // 2MB
    static constexpr size_t HUGE_PAGE_SIZE_1GB = 1024 * 1024 * 1024; // 1GB
    
    // Round up size to huge page boundary
    static size_t round_up_to_huge_page(size_t size) noexcept {
        // Use 2MB huge pages for allocations
        return ((size + HUGE_PAGE_SIZE_2MB - 1) / HUGE_PAGE_SIZE_2MB) * HUGE_PAGE_SIZE_2MB;
    }
    
    // Pre-fault huge pages to avoid runtime page faults
    static void prefault_memory(void* ptr, size_t size, size_t max_prefault_bytes) noexcept {
        if (!ptr || size == 0) return;
        
        // Fix #6: Cap pre-fault size to prevent startup stalls
        size_t prefault_size = std::min(size, max_prefault_bytes);
        
        // Touch each page to force allocation
        const size_t page_size = HUGE_PAGE_SIZE_2MB;
        auto* byte_ptr = static_cast<volatile char*>(ptr);
        
        for (size_t offset = 0; offset < prefault_size; offset += page_size) {
            // Read and write to force page allocation
            volatile char temp = byte_ptr[offset];
            byte_ptr[offset] = temp;
        }
        
        // Alternative: use madvise for kernel hint
        madvise(ptr, prefault_size, MADV_WILLNEED);
    }

public:
    // Allocate memory using huge pages with fallback to regular pages
    // Fix #1: Return allocation metadata to track actual allocation method
    static AllocationResult allocate(size_t size, size_t alignment, bool use_huge_pages, 
                                   bool prefault = false, size_t max_prefault_mb = 256) {
        // Fix #4: Ensure size is multiple of alignment before aligned_alloc fallback
        size = round_up_to_alignment(size, alignment);
        
        void* ptr = nullptr;
        bool used_huge_pages = false;
        size_t actual_size = size;
        
        if (use_huge_pages && size >= HUGE_PAGE_SIZE_2MB) [[likely]] {
            size_t huge_page_size = round_up_to_huge_page(size);
            actual_size = huge_page_size;
            
            // Try to allocate using huge pages on Linux
            ptr = mmap(nullptr, huge_page_size, 
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                      -1, 0);
            
            if (ptr != MAP_FAILED) [[likely]] {
                used_huge_pages = true;
                if (prefault) {
                    size_t max_prefault_bytes = max_prefault_mb * 1024 * 1024;
                    prefault_memory(ptr, huge_page_size, max_prefault_bytes);
                }
                return AllocationResult(ptr, used_huge_pages, actual_size);
            }
            
            // If huge pages failed, fall back to transparent huge pages
            ptr = mmap(nullptr, huge_page_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS,
                      -1, 0);
            
            if (ptr != MAP_FAILED) [[likely]] {
                // Advise kernel to use huge pages if available
                madvise(ptr, huge_page_size, MADV_HUGEPAGE);
                used_huge_pages = true;
                
                if (prefault) {
                    size_t max_prefault_bytes = max_prefault_mb * 1024 * 1024;
                    prefault_memory(ptr, huge_page_size, max_prefault_bytes);
                }
                return AllocationResult(ptr, used_huge_pages, actual_size);
            }
        }
        
        // Fallback to regular aligned allocation
        // Size is already rounded to alignment multiple
        ptr = std::aligned_alloc(alignment, size);
        return AllocationResult(ptr, false, size); // used_huge_pages = false for fallback
    }
    
    // Deallocate memory (handles both huge pages and regular allocation)
    static void deallocate(void* ptr, size_t size, bool was_huge_pages) noexcept {
        if (ptr == nullptr) [[unlikely]] {
            return;
        }
        
        if (was_huge_pages) [[likely]] {
            if (size >= HUGE_PAGE_SIZE_2MB) {
                size_t huge_page_size = round_up_to_huge_page(size);
                munmap(ptr, huge_page_size);
            } else {
                // Should not happen, but handle gracefully
                std::free(ptr);
            }
        } else {
            std::free(ptr);
        }
    }
};

// 128-bit tagged pointer with lock-free guarantee
template<typename T>
struct TaggedPointer128 {
    struct TaggedPtr {
        T* ptr;
        uint64_t tag;
        
        TaggedPtr() : ptr(nullptr), tag(0) {}
        TaggedPtr(T* p, uint64_t t) : ptr(p), tag(t) {}
        
        bool operator==(const TaggedPtr& other) const noexcept {
            return ptr == other.ptr && tag == other.tag;
        }
    };
    
    std::atomic<TaggedPtr> value_;
    
    TaggedPointer128() : value_(TaggedPtr{}) {
        // Fix #5: Use runtime check for lock-free guarantee to improve portability
        if (!value_.is_lock_free()) {
            std::terminate(); // Hard failure if not lock-free
        }
    }
    
    TaggedPointer128(T* ptr, uint64_t tag = 0) : value_(TaggedPtr{ptr, tag}) {
        if (!value_.is_lock_free()) {
            std::terminate();
        }
    }
    
    void set(T* ptr, uint64_t tag) noexcept {
        value_.store(TaggedPtr{ptr, tag}, std::memory_order_relaxed);
    }
    
    TaggedPtr load(std::memory_order order = std::memory_order_acquire) const noexcept {
        return value_.load(order);
    }
    
    bool compare_exchange_weak(TaggedPtr& expected, TaggedPtr new_val,
                              std::memory_order success = std::memory_order_release,
                              std::memory_order failure = std::memory_order_acquire) noexcept {
        return value_.compare_exchange_weak(expected, new_val, success, failure);
    }
};

// Forward declaration
template<Allocatable T>
class MemoryPool;

// Base class for intrusive linked list of thread-local caches
class ThreadLocalCacheBase {
public:
    ThreadLocalCacheBase* next_ = nullptr;
    
    virtual ~ThreadLocalCacheBase() = default;
    virtual void flush_to_pool() = 0;
    virtual bool belongs_to_pool(void* pool) const = 0;
};

// Thread-local head of the intrusive list
inline thread_local ThreadLocalCacheBase* tls_cache_list_head = nullptr;

// Thread-local cache for ultra-fast allocation/deallocation
template<Allocatable T>
class ThreadLocalCache : public ThreadLocalCacheBase {
private:
    using Pool = MemoryPool<T>;
    
    std::unique_ptr<void*[]> cache_;  // Store as void* (raw memory)
    size_t cache_size_;
    size_t batch_size_;
    size_t current_size_;
    size_t shrink_threshold_;
    
    Pool* global_pool_;
    
    // Statistics for adaptive behavior
    size_t allocation_count_;
    size_t deallocation_count_;
    size_t cache_hits_;
    size_t cache_misses_;
    
    std::chrono::steady_clock::time_point last_shrink_check_;
    std::chrono::milliseconds shrink_interval_;
    
    // Refill cache from global pool (batch operation)
    void refill_from_global() {
        if (!global_pool_) [[unlikely]] return;
        
        size_t objects_to_get = std::min(batch_size_, cache_size_ - current_size_);
        size_t objects_obtained = 0;
        
        // Try to get a batch of objects from global pool
        for (size_t i = 0; i < objects_to_get; ++i) {
            void* obj = global_pool_->allocate_raw();
            if (obj) [[likely]] {
                cache_[current_size_ + objects_obtained] = obj;
                ++objects_obtained;
            } else [[unlikely]] {
                break; // Global pool exhausted
            }
        }
        
        current_size_ += objects_obtained;
        cache_misses_++;
    }
    
    // Flush objects to global pool (batch operation)
    void flush_to_global() {
        if (!global_pool_ || current_size_ == 0) [[unlikely]] return;
        
        if (global_pool_->is_shutting_down()) [[unlikely]] {
            current_size_ = 0;  // Abandon objects during shutdown
            return;
        }
        
        size_t objects_to_return = std::min(batch_size_, current_size_);
        
        // Return batch of objects to global pool
        for (size_t i = 0; i < objects_to_return; ++i) {
            global_pool_->deallocate_raw(cache_[current_size_ - 1 - i]);
        }
        
        current_size_ -= objects_to_return;
    }
    
    // Adaptive cache management
    void maybe_shrink_cache() {
        auto now = std::chrono::steady_clock::now();
        if (now - last_shrink_check_ < shrink_interval_) [[likely]] {
            return;
        }
        
        last_shrink_check_ = now;
        
        // If cache utilization is low, return some objects to global pool
        if (current_size_ < shrink_threshold_ && current_size_ > batch_size_) [[unlikely]] {
            size_t objects_to_return = std::min(batch_size_ / 2, current_size_ - shrink_threshold_);
            
            if (!global_pool_->is_shutting_down()) [[likely]] {
                for (size_t i = 0; i < objects_to_return; ++i) {
                    global_pool_->deallocate_raw(cache_[current_size_ - 1 - i]);
                }
                current_size_ -= objects_to_return;
            }
        }
    }

public:
    explicit ThreadLocalCache(Pool* pool, const CacheConfig& config = CacheConfig{})
        : ThreadLocalCacheBase{}
        , cache_(std::make_unique<void*[]>(config.cache_size))
        , cache_size_(config.cache_size)
        , batch_size_(config.batch_size)
        , current_size_(0)
        , shrink_threshold_(config.shrink_threshold)
        , global_pool_(pool)
        , allocation_count_(0)
        , deallocation_count_(0)
        , cache_hits_(0)
        , cache_misses_(0)
        , last_shrink_check_(std::chrono::steady_clock::now())
        , shrink_interval_(config.shrink_interval) {
        // Add to thread-local intrusive list
        next_ = tls_cache_list_head;
        tls_cache_list_head = this;
    }
    
    ~ThreadLocalCache() {
        if (global_pool_ && !global_pool_->is_shutting_down()) [[likely]] {
            flush_all_to_global();
        }
        // If pool is shutting down, abandon cached objects
        
        // Remove from thread-local intrusive list
        ThreadLocalCacheBase** current = &tls_cache_list_head;
        while (*current) {
            if (*current == this) {
                *current = next_;
                break;
            }
            current = &((*current)->next_);
        }
    }
    
    // Non-copyable, non-movable
    ThreadLocalCache(const ThreadLocalCache&) = delete;
    ThreadLocalCache& operator=(const ThreadLocalCache&) = delete;
    ThreadLocalCache(ThreadLocalCache&&) = delete;
    ThreadLocalCache& operator=(ThreadLocalCache&&) = delete;
    
    // Raw allocation (no construction) - returns uninitialized memory
    [[clang::always_inline]] [[nodiscard]] void* allocate_raw() {
        allocation_count_++;
        
        // Hot path: Check local cache first - this is the common case
        if (current_size_ > 0) [[likely]] {
            cache_hits_++;
            return cache_[--current_size_];
        }
        
        // Cold path: Refill from global pool - this is rare
        if (current_size_ == 0) [[unlikely]] {
            refill_from_global();
            
            if (current_size_ > 0) [[likely]] {
                return cache_[--current_size_];
            }
        }
        
        // Route to depletion handler
        return global_pool_->handle_depletion_and_return();
    }
    
    // Raw deallocation (no destruction) - memory must be uninitialized
    [[clang::always_inline]] void deallocate_raw(void* ptr) {
        if (!ptr) [[unlikely]] return;
        
        deallocation_count_++;
        
        // Hot path: Store in local cache if space available - common case
        if (current_size_ < cache_size_) [[likely]] {
            cache_[current_size_++] = ptr;
            
            // Periodically check if we should shrink - uncommon
            if ((deallocation_count_ & 0xFF) == 0) [[unlikely]] {
                maybe_shrink_cache();
            }
            return;
        }
        
        // Cold path: Cache is full, flush batch to global pool - rare case
        if (current_size_ >= cache_size_) [[unlikely]] {
            flush_to_global();
            
            // Now there should be space
            if (current_size_ < cache_size_) [[likely]] {
                cache_[current_size_++] = ptr;
            } else [[unlikely]] {
                // Fallback: Direct return to global pool
                global_pool_->deallocate_raw(ptr);
            }
        }
    }
    
    // High-level allocation with construction
    [[clang::always_inline]] [[nodiscard]] T* allocate() {
        void* raw_ptr = allocate_raw();
        if (raw_ptr) [[likely]] {
            return static_cast<T*>(raw_ptr);
        }
        return nullptr;
    }
    
    // Fix #2: High-level deallocation with destruction
    // This is the ONLY function that calls destructors
    [[clang::always_inline]] void deallocate(T* ptr) {
        if (!ptr) [[unlikely]] return;
        
        // Call destructor exactly once - here
        ptr->~T();
        
        // Convert to raw pointer and deallocate
        deallocate_raw(static_cast<void*>(ptr));
    }
    
    // Construct object in allocated memory
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        T* ptr = allocate();
        if (ptr) [[likely]] {
            new (ptr) T(std::forward<Args>(args)...);
        }
        return ptr;
    }
    
    // Destroy and deallocate object
    void destroy(T* ptr) {
        if (ptr) [[likely]] {
            deallocate(ptr);  // This calls destructor and deallocates
        }
    }
    
    // Force flush all objects back to global pool
    void flush_all_to_global() {
        if (!global_pool_->is_shutting_down()) [[likely]] {
            while (current_size_ > 0) {
                global_pool_->deallocate_raw(cache_[--current_size_]);
            }
        } else [[unlikely]] {
            current_size_ = 0;  // Abandon objects during shutdown
        }
    }
    
    // Override from base class
    void flush_to_pool() override {
        flush_all_to_global();
    }
    
    // Override from base class
    bool belongs_to_pool(void* pool) const override {
        return pool == global_pool_;
    }
    
    // Cache statistics
    [[nodiscard]] size_t size() const noexcept { return current_size_; }
    [[nodiscard]] size_t capacity() const noexcept { return cache_size_; }
    [[nodiscard]] double hit_rate() const noexcept {
        return allocation_count_ > 0 ? 
               static_cast<double>(cache_hits_) / allocation_count_ : 0.0;
    }
    [[nodiscard]] size_t total_allocations() const noexcept { return allocation_count_; }
    [[nodiscard]] size_t total_deallocations() const noexcept { return deallocation_count_; }
};

// Helper to get or create thread-local cache for a specific pool
template<Allocatable T>
static ThreadLocalCache<T>& get_thread_cache(MemoryPool<T>* pool, const CacheConfig& config) {
    // Walk the intrusive list to find existing cache
    for (ThreadLocalCacheBase* node = tls_cache_list_head; node; node = node->next_) {
        if (node->belongs_to_pool(pool)) {
            return *static_cast<ThreadLocalCache<T>*>(node);
        }
    }
    
    // Not found - create new cache with proper cleanup
    // Use a thread_local vector to ensure cleanup at thread exit
    thread_local std::vector<std::unique_ptr<ThreadLocalCache<T>>> cache_storage;
    
    // Reserve space on first use to avoid reallocations
    if (cache_storage.empty()) {
        cache_storage.reserve(8);  // Most threads use few pools
    }
    
    cache_storage.emplace_back(std::make_unique<ThreadLocalCache<T>>(pool, config));
    return *cache_storage.back();
}

// Lock-free global memory pool
//
// Thread Safety & Shutdown Requirements:
// - All methods are thread-safe for concurrent access
// - The pool MUST outlive all threads that use it
// - Before destroying the pool, ensure all worker threads are joined
// - Failing to join threads before destruction causes use-after-free
//
// Example usage:
//   MemoryPool<Order> pool(10000);
//   std::vector<std::thread> workers;
//   // ... create and start worker threads ...
//   
//   // Shutdown sequence:
//   stop_flag.store(true);           // Signal workers to stop
//   for (auto& t : workers) t.join(); // Wait for all threads
//   // Pool destructor can now safely run
//
template<Allocatable T>
class MemoryPool {
private:
    struct alignas(std::hardware_destructive_interference_size) FreeNode {
        TaggedPointer128<FreeNode> next;
        
        FreeNode() = default;
        ~FreeNode() = default;
    };
    
    static_assert(sizeof(FreeNode) <= sizeof(T), 
                  "FreeNode must fit within allocated object size");
    
    // Fix #3: Ensure alignment compatibility
    static_assert(alignof(FreeNode) <= alignof(T), 
                  "FreeNode alignment must not exceed T alignment");
    
    // Pool configuration
    static constexpr std::size_t BLOCK_SIZE = std::max(sizeof(T), sizeof(FreeNode));
    
    // Ensure alignment is power of two
    static constexpr std::size_t ALIGNMENT = round_up_to_power_of_two(
        std::max(alignof(T), std::hardware_destructive_interference_size)
    );
    
    // Memory storage - properly aligned
    std::byte* memory_;
    std::size_t capacity_;
    std::size_t block_count_;
    PoolDepletionPolicy depletion_policy_;
    CacheConfig cache_config_;
    bool uses_huge_pages_;
    
    // Shutdown flag
    std::atomic<bool> shutting_down_;
    
    // Free list head with ABA protection
    alignas(std::hardware_destructive_interference_size) 
    TaggedPointer128<FreeNode> free_head_;
    
    // Statistics (for monitoring)
    alignas(std::hardware_destructive_interference_size)
    std::atomic<std::size_t> allocated_count_;
    
    // Fix #1: Custom deleter that uses correct deallocation method
    struct MemoryDeleter {
        size_t size_;
        bool huge_pages_;
        
        MemoryDeleter(size_t size, bool huge_pages) 
            : size_(size), huge_pages_(huge_pages) {}
        
        void operator()(std::byte* ptr) const {
            HugePageAllocator::deallocate(ptr, size_, huge_pages_);
        }
    };
    
    std::unique_ptr<std::byte, MemoryDeleter> memory_holder_;
    
    // Initialize the free list
    void initialize_free_list() noexcept {
        std::byte* current = memory_;
        FreeNode* prev = nullptr;
        
        for (std::size_t i = 0; i < block_count_; ++i) {
            auto* node = new (current) FreeNode();
            
            if (prev) [[likely]] {
                prev->next.set(node, 0);
            }
            
            prev = node;
            current += BLOCK_SIZE;
        }
        
        free_head_.set(reinterpret_cast<FreeNode*>(memory_), 0);
    }
    
    // Handle pool depletion based on policy
    [[noreturn]] void handle_depletion() const {
        switch (depletion_policy_) {
            case PoolDepletionPolicy::THROW_EXCEPTION:
                throw std::bad_alloc();
            
            case PoolDepletionPolicy::TERMINATE_PROCESS:
                std::terminate();
            
            case PoolDepletionPolicy::DYNAMIC_RESIZE:
                std::terminate(); // Not implemented yet
        }
        
        std::terminate();
    }

protected:
    // Virtual function for NUMA override
    // Micro-performance: Inline virtual allocator hooks
    [[clang::always_inline]] virtual AllocationResult allocate_memory(size_t size, size_t alignment) {
        return HugePageAllocator::allocate(size, alignment, cache_config_.use_huge_pages, 
                                         cache_config_.prefault_pages, cache_config_.max_prefault_mb);
    }

public:
    explicit MemoryPool(std::size_t initial_capacity = 1024, 
                       PoolDepletionPolicy policy = PoolDepletionPolicy::TERMINATE_PROCESS,
                       const CacheConfig& config = CacheConfig{}) 
        : block_count_(initial_capacity)
        , depletion_policy_(policy)
        , cache_config_(config)
        , uses_huge_pages_(false) // Will be set correctly after allocation
        , shutting_down_(false)
        , allocated_count_(0)
        , memory_holder_(nullptr, MemoryDeleter{0, false}) {
        
        capacity_ = round_up_to_alignment(initial_capacity * BLOCK_SIZE, ALIGNMENT);
        
        // Fix #1: Use allocation result to get correct huge page flag
        AllocationResult result = allocate_memory(capacity_, ALIGNMENT);
        if (!result.ptr) [[unlikely]] {
            throw std::bad_alloc();
        }
        
        memory_ = static_cast<std::byte*>(result.ptr);
        uses_huge_pages_ = result.used_huge_pages; // Set actual value
        
        memory_holder_ = std::unique_ptr<std::byte, MemoryDeleter>(
            memory_, MemoryDeleter{result.actual_size, uses_huge_pages_}
        );
        
        initialize_free_list();
    }
    
    virtual ~MemoryPool() {
        // Signal shutdown to prevent cache flush
        shutting_down_.store(true, std::memory_order_release);
        
        // Memory fence to ensure all threads see the shutdown flag
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        // CRITICAL: Application must ensure all worker threads are joined before
        // destroying the pool. Thread-local caches in other threads may still
        // hold pointers into this pool's memory. Destroying the pool while
        // worker threads are active will cause use-after-free errors.
        //
        // Correct shutdown sequence:
        // 1. Signal worker threads to stop
        // 2. Join all worker threads
        // 3. Destroy the memory pool
        //
        // In debug builds, we could add a check:
        // assert(/* all threads using this pool are joined */);
    }
    
    // Non-copyable, non-movable for thread safety
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;
    MemoryPool(MemoryPool&&) = delete;
    MemoryPool& operator=(MemoryPool&&) = delete;
    
    // Check shutdown status
    [[nodiscard]] bool is_shutting_down() const noexcept {
        return shutting_down_.load(std::memory_order_acquire);
    }
    
    // Check if pointer belongs to this pool
    [[nodiscard]] bool owns(const void* ptr) const noexcept {
        const auto* byte_ptr = static_cast<const std::byte*>(ptr);
        return byte_ptr >= memory_ && byte_ptr < memory_ + capacity_;
    }
    
    // Get memory range for fast lookup
    [[nodiscard]] std::pair<const void*, const void*> memory_range() const noexcept {
        return {memory_, memory_ + capacity_};
    }
    
    // Allocate raw memory from global pool (used by thread-local caches)
    [[clang::always_inline]] [[nodiscard]] void* allocate_raw() {
        if (is_shutting_down()) [[unlikely]] {
            handle_depletion();
        }
        
        auto current = free_head_.load();
        
        while (current.ptr != nullptr) [[likely]] {
            auto next = current.ptr->next.load();
            
            // Software prefetch: Load next.ptr into cache to reduce latency on miss path
            __builtin_prefetch(next.ptr, 0, 3);
            
            auto new_head = typename TaggedPointer128<FreeNode>::TaggedPtr{
                next.ptr, current.tag + 1
            };
            
            if (free_head_.compare_exchange_weak(current, new_head)) [[likely]] {
                allocated_count_.fetch_add(1, std::memory_order_relaxed);
                return static_cast<void*>(current.ptr);
            }
            // CAS failed, retry with updated current value
        }
        
        return nullptr; // Pool exhausted
    }
    
    // Deallocate raw memory to global pool (used by thread-local caches)
    [[clang::always_inline]] void deallocate_raw(void* ptr) noexcept {
        if (!ptr || !owns(ptr) || is_shutting_down()) [[unlikely]] {
            return;
        }
        
        // No destructor call here - memory is already raw
        auto* node = new (ptr) FreeNode();
        
        auto current = free_head_.load();
        using NodePtr = typename TaggedPointer128<FreeNode>::TaggedPtr;
        
        NodePtr new_head;
        do {
            node->next.set(current.ptr, current.tag);
            new_head = NodePtr{node, current.tag + 1};
        } while (!free_head_.compare_exchange_weak(current, new_head));
        
        allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    }
    
    // Centralized depletion handling
    [[nodiscard]] void* handle_depletion_and_return() {
        handle_depletion();
        return nullptr; // Never reached
    }
    
    // High-level allocation interface (uses thread-local caching)
    [[clang::always_inline]] [[nodiscard]] T* allocate() {
        auto& cache = get_thread_cache(this, cache_config_);
        return cache.allocate();
    }
    
    // High-level deallocation interface (uses thread-local caching)
    [[clang::always_inline]] void deallocate(T* ptr) {
        auto& cache = get_thread_cache(this, cache_config_);
        cache.deallocate(ptr);
    }
    
    // Construct object in allocated memory
    template<typename... Args>
    [[nodiscard]] T* construct(Args&&... args) {
        auto& cache = get_thread_cache(this, cache_config_);
        return cache.construct(std::forward<Args>(args)...);
    }
    
    // Destroy and deallocate object
    void destroy(T* ptr) {
        auto& cache = get_thread_cache(this, cache_config_);
        cache.destroy(ptr);
    }
    
    // Pool statistics
    [[nodiscard]] std::size_t object_capacity() const noexcept { return block_count_; }
    [[nodiscard]] std::size_t byte_capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t allocated_objects() const noexcept { 
        return allocated_count_.load(std::memory_order_relaxed); 
    }
    [[nodiscard]] std::size_t available_objects() const noexcept { 
        return object_capacity() - allocated_objects(); 
    }
    [[nodiscard]] double utilization() const noexcept {
        return static_cast<double>(allocated_objects()) / object_capacity();
    }
    [[nodiscard]] bool uses_huge_pages() const noexcept { return uses_huge_pages_; }
    
    [[nodiscard]] bool empty() const noexcept { return allocated_objects() == 0; }
    [[nodiscard]] bool full() const noexcept { return allocated_objects() == object_capacity(); }
    
    // Force flush of current thread's cache - useful for testing
    void flush_thread_cache() {
        // Walk the intrusive list and flush all caches belonging to this pool
        for (ThreadLocalCacheBase* node = tls_cache_list_head; node; node = node->next_) {
            if (node->belongs_to_pool(this)) {
                node->flush_to_pool();
            }
        }
    }
    
    // Get cache configuration
    [[nodiscard]] const CacheConfig& cache_config() const noexcept { return cache_config_; }
    
    // Deprecated - use object_capacity() instead
    [[deprecated("Use object_capacity() for clarity")]]
    [[nodiscard]] std::size_t capacity() const noexcept { return object_capacity(); }
};

// RAII wrapper for pool-allocated objects
template<Allocatable T>
class PoolPtr {
private:
    T* ptr_;
    MemoryPool<T>* pool_;

public:
    explicit PoolPtr(MemoryPool<T>& pool) 
        : ptr_(pool.allocate()), pool_(&pool) {
        if (ptr_) [[likely]] {
            new (ptr_) T();
        }
    }
    
    template<typename... Args>
    PoolPtr(MemoryPool<T>& pool, Args&&... args) 
        : ptr_(pool.construct(std::forward<Args>(args)...)), pool_(&pool) {}
    
    ~PoolPtr() {
        if (ptr_ && pool_) [[likely]] {
            pool_->destroy(ptr_);
        }
    }
    
    // Move-only semantics
    PoolPtr(const PoolPtr&) = delete;
    PoolPtr& operator=(const PoolPtr&) = delete;
    
    PoolPtr(PoolPtr&& other) noexcept 
        : ptr_(other.ptr_), pool_(other.pool_) {
        other.ptr_ = nullptr;
        other.pool_ = nullptr;
    }
    
    PoolPtr& operator=(PoolPtr&& other) noexcept {
        if (this != &other) [[likely]] {
            if (ptr_ && pool_) [[likely]] {
                pool_->destroy(ptr_);
            }
            
            ptr_ = other.ptr_;
            pool_ = other.pool_;
            other.ptr_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }
    
    // Access operators
    T* operator->() noexcept { return ptr_; }
    const T* operator->() const noexcept { return ptr_; }
    
    T& operator*() noexcept { return *ptr_; }
    const T& operator*() const noexcept { return *ptr_; }
    
    // Utility functions
    [[nodiscard]] T* get() noexcept { return ptr_; }
    [[nodiscard]] const T* get() const noexcept { return ptr_; }
    
    [[nodiscard]] bool valid() const noexcept { return ptr_ != nullptr; }
    [[nodiscard]] explicit operator bool() const noexcept { return valid(); }
    
    T* release() noexcept {
        T* temp = ptr_;
        ptr_ = nullptr;
        return temp;
    }
    
    template<typename... Args>
    void reset(Args&&... args) {
        if (ptr_ && pool_) [[likely]] {
            pool_->destroy(ptr_);
        }
        ptr_ = pool_->construct(std::forward<Args>(args)...);
    }
    
    void reset() noexcept {
        if (ptr_ && pool_) [[likely]] {
            pool_->destroy(ptr_);
        }
        ptr_ = nullptr;
    }
};

// Helper function to create PoolPtr with dependency injection
template<Allocatable T, typename... Args>
[[nodiscard]] PoolPtr<T> make_pooled(MemoryPool<T>& pool, Args&&... args) {
    return PoolPtr<T>(pool, std::forward<Args>(args)...);
}

}  // namespace crypto_lob::core