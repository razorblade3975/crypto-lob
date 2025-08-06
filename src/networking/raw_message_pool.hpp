#pragma once

#include <array>
#include <atomic>
#include <bit>  // For C++20 bit operations
#include <cstddef>
#include <cstdint>
#include <cstdlib>  // For aligned_alloc
#include <cstring>
#include <memory>

#include <sys/mman.h>  // For huge pages
#ifdef __linux__
#include <sched.h>  // For sched_getcpu()
#ifdef HAVE_NUMA
#include <numa.h>    // For NUMA node detection
#include <numaif.h>  // For NUMA support
#endif
#endif

#include "../core/cache.hpp"
#include "../core/spsc_ring.hpp"

namespace crypto_lob::networking {

using crypto_lob::core::CACHELINE_SIZE;

/**
 * RawMessagePool - Specialized WebSocket Buffer Pool for HFT
 *
 * Part of a three-component memory management system:
 * 1. SPSCRing: Lock-free message passing between threads (~20ns)
 * 2. MemoryPool: Generic object pool with CAS operations (~200ns)
 * 3. RawMessagePool: This component - thread-local WebSocket buffers (~100ns)
 *
 * Design Philosophy:
 * - Thread-local pools eliminate ALL contention
 * - Fixed 64KB buffers match WebSocket frame maximum
 * - Uses SPSCRing internally for wait-free free list
 * - Zero allocations after initialization
 * - Huge pages (2MB) for TLB efficiency
 * - NUMA-aware memory binding
 *
 * Usage in Data Flow:
 * Network Thread → RawMessagePool → SPSCRing → Parser Thread
 *
 * @see docs/architecture/memory_management.md for complete architecture
 */

// Raw message buffer for zero-copy WebSocket data
// Sized for maximum WebSocket frame (64KB) + metadata
struct alignas(CACHELINE_SIZE) RawMessage {
    // Metadata (fits in one cache line)
    uint64_t timestamp_ns;  // RDTSC timestamp at receipt
    uint64_t sequence_num;  // Global sequence number
    uint32_t exchange_id;   // Exchange identifier
    uint32_t size;          // Actual data size
    uint32_t pool_index;    // Index in pool for return
    uint32_t reserved;      // Reserved for alignment

    // Calculate padding dynamically based on actual metadata size
    static constexpr size_t METADATA_SIZE = sizeof(uint64_t) * 2 + sizeof(uint32_t) * 4;
    static constexpr size_t PADDING_SIZE = CACHELINE_SIZE - METADATA_SIZE;
    char padding[PADDING_SIZE];

    // Data buffer (aligned to cache line for SIMD operations)
    // 64KB is WebSocket standard max uncompressed frame
    // Compressed frames are handled by separate decompression layer
    static constexpr size_t MAX_FRAME_SIZE = 65536;
    alignas(CACHELINE_SIZE) char data[MAX_FRAME_SIZE];

    // Total size for validation
    static constexpr size_t TOTAL_SIZE = CACHELINE_SIZE + MAX_FRAME_SIZE;

    // Reset for reuse (clear all metadata fields)
    void reset() noexcept {
        timestamp_ns = 0;
        sequence_num = 0;
        exchange_id = 0;
        size = 0;
        reserved = 0;  // Clear reserved field too
        // Don't clear pool_index - needed for return
    }
};

// Static assertions to ensure proper sizing and alignment
static_assert(sizeof(RawMessage) == RawMessage::TOTAL_SIZE, "RawMessage size must match TOTAL_SIZE constant");
static_assert(sizeof(RawMessage) % CACHELINE_SIZE == 0, "RawMessage total size must be cache-line aligned");
static_assert(offsetof(RawMessage, data) % CACHELINE_SIZE == 0,
              "Data buffer must start on cache-line boundary for SIMD");
static_assert(RawMessage::METADATA_SIZE <= CACHELINE_SIZE, "Metadata must fit in single cache line");

// Thread-local pool of pre-allocated raw messages
// Each I/O thread has its own pool - zero contention design
//
// Thread Safety: This pool is designed for single-threaded use only.
// Each thread should have its own pool instance. Sharing a pool
// between threads will cause data corruption.
class RawMessagePool {
  private:
    static constexpr size_t DEFAULT_CAPACITY = 16384;          // 16K messages (power of 2)
    static constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;  // 2MB

    // Round up to next power of two for SPSCRing requirement
    static constexpr size_t round_up_power_of_two(size_t n) noexcept {
        if (n <= 1)
            return 1;
            // Use C++20 bit_ceil if available, otherwise manual
#ifdef __cpp_lib_int_pow2
        return std::bit_ceil(n);
#else
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        // Only shift by 32 on 64-bit systems
        if constexpr (sizeof(size_t) > 4) {
            n |= n >> 32;
        }
        return n + 1;
#endif
    }

    // Round up to huge page boundary
    static constexpr size_t round_to_huge_page(size_t size) noexcept {
        return ((size + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE) * HUGE_PAGE_SIZE;
    }

    // Custom deleter for zero overhead (no std::function)
    struct PoolDeleter {
        size_t size;
        void operator()(char* ptr) const noexcept {
            deallocate_huge_pages(ptr, size);
        }
    };

    const size_t capacity_;
    size_t actual_alloc_size_;  // Track actual allocation size (non-const for init)
    std::unique_ptr<char, PoolDeleter> memory_block_;
    RawMessage* messages_;

    // Lock-free free list using SPSC ring
    // Safe because this pool is used by single thread only
    crypto_lob::core::SPSCRing<uint32_t> free_list_;

    // Track last allocated index for prefetching
    uint32_t last_allocated_index_{0};

    // Statistics (single thread access, no atomics needed)
    uint64_t total_allocations_{0};
    uint64_t total_deallocations_{0};
    uint64_t allocation_failures_{0};
    uint64_t release_failures_{0};  // Track failed releases

    // Allocate memory with huge pages and NUMA awareness
    static std::pair<void*, size_t> allocate_huge_pages(size_t requested_size) noexcept {
        void* ptr = nullptr;
        size_t actual_size = requested_size;

#ifdef __linux__
        // Round up to huge page boundary for MAP_HUGETLB
        actual_size = round_to_huge_page(requested_size);

        // Try to allocate with huge pages
        ptr = mmap(nullptr, actual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);

        if (ptr == MAP_FAILED) {
            // Fallback to regular pages with THP hint
            ptr = mmap(nullptr, actual_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

            if (ptr == MAP_FAILED) {
                return {nullptr, 0};
            }

            // Advise kernel for transparent huge pages
            if (madvise(ptr, actual_size, MADV_HUGEPAGE) != 0) {
// Log warning in debug builds
#ifdef DEBUG
                int saved_errno = errno;
// Could log: "madvise(MADV_HUGEPAGE) failed: errno=" + saved_errno
#endif
            }
        }

// NUMA awareness: bind memory to current CPU's NUMA node
#ifdef HAVE_NUMA
        if (numa_available() >= 0) {
            int cpu = sched_getcpu();
            if (cpu >= 0) {
                int node = numa_node_of_cpu(cpu);
                if (node >= 0) {
                    unsigned long nodemask = 1UL << node;
                    mbind(ptr, actual_size, MPOL_BIND, &nodemask, sizeof(nodemask) * 8, MPOL_MF_MOVE);
                }
            }
        }
#endif

        // Lock pages in memory to prevent swapping (best effort)
        if (mlock(ptr, actual_size) != 0) {
#ifdef DEBUG
            int saved_errno = errno;
// Could log: "mlock failed (check RLIMIT_MEMLOCK): errno=" + saved_errno
#endif
        }
#else
        // Non-Linux: use aligned allocation
        // Ensure size is multiple of alignment as required by aligned_alloc
        actual_size = ((requested_size + CACHELINE_SIZE - 1) / CACHELINE_SIZE) * CACHELINE_SIZE;
        ptr = std::aligned_alloc(CACHELINE_SIZE, actual_size);
#endif

        return {ptr, actual_size};
    }

    static void deallocate_huge_pages(void* ptr, size_t size) noexcept {
        if (!ptr)
            return;

#ifdef __linux__
        munlock(ptr, size);
        munmap(ptr, size);
#else
        std::free(ptr);
#endif
    }

  public:
    explicit RawMessagePool(size_t requested_capacity = DEFAULT_CAPACITY)
        : capacity_(round_up_power_of_two(requested_capacity)),
          actual_alloc_size_(0),  // Will be set after allocation
          free_list_(capacity_) {
        // Ensure capacity is power of two for SPSCRing
        static_assert((DEFAULT_CAPACITY & (DEFAULT_CAPACITY - 1)) == 0, "DEFAULT_CAPACITY must be power of two");

        // Calculate total memory needed
        const size_t element_size = sizeof(RawMessage);
        const size_t total_size = capacity_ * element_size;

        // Allocate all messages in one contiguous block
        auto [raw_memory, alloc_size] = allocate_huge_pages(total_size);
        if (!raw_memory) {
            // In HFT, we terminate on allocation failure
            std::terminate();
        }

        actual_alloc_size_ = alloc_size;  // No const_cast needed now

        // Set up unique_ptr with custom deleter (zero overhead)
        memory_block_ = std::unique_ptr<char, PoolDeleter>(static_cast<char*>(raw_memory), PoolDeleter{alloc_size});
        messages_ = reinterpret_cast<RawMessage*>(memory_block_.get());

        // Initialize only metadata (first cache line) - avoid 1GB memset
        // Data payload doesn't need zeroing, reset() handles metadata
        for (size_t i = 0; i < capacity_; ++i) {
            // Clear only the metadata cache line (64 bytes)
            std::memset(&messages_[i], 0, CACHELINE_SIZE);
            messages_[i].pool_index = static_cast<uint32_t>(i);

            // Add to free list
            if (!free_list_.try_push(static_cast<uint32_t>(i))) {
                std::terminate();  // Should never happen at init
            }
        }
    }

    ~RawMessagePool() = default;  // POD types don't need destruction

    // Non-copyable, non-movable
    RawMessagePool(const RawMessagePool&) = delete;
    RawMessagePool& operator=(const RawMessagePool&) = delete;
    RawMessagePool(RawMessagePool&&) = delete;
    RawMessagePool& operator=(RawMessagePool&&) = delete;

    // Acquire a message from the pool (wait-free for single thread)
    [[nodiscard]] RawMessage* acquire() noexcept {
        uint32_t index;
        if (!free_list_.try_pop(index)) {
            allocation_failures_++;
            return nullptr;
        }

        RawMessage* msg = &messages_[index];
        msg->reset();

        last_allocated_index_ = index;
        total_allocations_++;
        return msg;
    }

    // Return a message to the pool (fail-fast for thread-local pool)
    // Since this is thread-local, release should NEVER fail
    [[nodiscard]] bool release(RawMessage* msg) noexcept {
        if (!msg)
            return false;

        // Critical: Verify pointer is actually from this pool
        // Use uintptr_t to avoid UB from comparing pointers from different allocations
        const auto msg_addr = reinterpret_cast<std::uintptr_t>(msg);
        const auto pool_start = reinterpret_cast<std::uintptr_t>(messages_);
        const auto pool_end = reinterpret_cast<std::uintptr_t>(messages_ + capacity_);

        if (msg_addr < pool_start || msg_addr >= pool_end) {
            // Pointer is not from this pool - would corrupt free list
            release_failures_++;
            return false;
        }

        uint32_t index = msg->pool_index;
        if (index >= capacity_) {
            // Invalid index
            release_failures_++;
            return false;
        }

        // For thread-local pool, this should ALWAYS succeed
        // If it fails, we have a critical bug (double-free or corruption)
        if (!free_list_.try_push(index)) {
            // This should NEVER happen in correct usage
            // Fail fast in HFT rather than spin or leak
            std::terminate();
        }

        total_deallocations_++;
        return true;
    }

    // Get pool statistics (cheap - no atomics needed for single thread)
    struct Stats {
        size_t capacity;
        size_t used;  // Changed from 'available' to 'used' for clarity
        uint64_t total_allocations;
        uint64_t total_deallocations;
        uint64_t allocation_failures;
        uint64_t release_failures;  // Track failed releases
    };

    Stats get_stats() const noexcept {
        size_t free_count = free_list_.size();  // O(1) operation
        return Stats{.capacity = capacity_,
                     .used = capacity_ - free_count,  // Report used count
                     .total_allocations = total_allocations_,
                     .total_deallocations = total_deallocations_,
                     .allocation_failures = allocation_failures_,
                     .release_failures = release_failures_};
    }

    // Prefetch next likely allocation based on last pattern
    void prefetch_next() const noexcept {
        // Prefetch the message after last allocated
        uint32_t next_likely = (last_allocated_index_ + 1) % capacity_;
        __builtin_prefetch(&messages_[next_likely], 0, 3);
    }

    size_t capacity() const noexcept {
        return capacity_;
    }
};

// RAII wrapper for automatic message return
class RawMessageGuard {
  private:
    RawMessage* msg_;
    RawMessagePool* pool_;

  public:
    RawMessageGuard(RawMessage* msg, RawMessagePool* pool) noexcept : msg_(msg), pool_(pool) {}

    ~RawMessageGuard() noexcept {
        if (msg_ && pool_) {
            // Note: ignoring return value in destructor
            // If release fails, message is leaked but that's better than corruption
            (void)pool_->release(msg_);
        }
    }

    // Non-copyable but movable
    RawMessageGuard(const RawMessageGuard&) = delete;
    RawMessageGuard& operator=(const RawMessageGuard&) = delete;

    RawMessageGuard(RawMessageGuard&& other) noexcept : msg_(other.msg_), pool_(other.pool_) {
        other.msg_ = nullptr;
        other.pool_ = nullptr;
    }

    RawMessageGuard& operator=(RawMessageGuard&& other) noexcept {
        if (this != &other) {
            if (msg_ && pool_) {
                // Note: ignoring return value - better to leak than corrupt
                (void)pool_->release(msg_);
            }
            msg_ = other.msg_;
            pool_ = other.pool_;
            other.msg_ = nullptr;
            other.pool_ = nullptr;
        }
        return *this;
    }

    RawMessage* get() noexcept {
        return msg_;
    }
    const RawMessage* get() const noexcept {
        return msg_;
    }
    RawMessage* operator->() noexcept {
        return msg_;
    }
    const RawMessage* operator->() const noexcept {
        return msg_;
    }
    RawMessage& operator*() noexcept {
        return *msg_;
    }
    const RawMessage& operator*() const noexcept {
        return *msg_;
    }

    RawMessage* release() noexcept {
        RawMessage* tmp = msg_;
        msg_ = nullptr;
        pool_ = nullptr;  // Prevent double-free
        return tmp;
    }
};

// Thread-local pool management with configurable capacity
struct ThreadPoolConfig {
    size_t capacity = 16384;  // Default 16K messages
};

// Thread-local pool instance (one per I/O thread)
inline thread_local std::unique_ptr<RawMessagePool> tls_raw_pool;
inline thread_local ThreadPoolConfig tls_pool_config;

// Configure thread-local pool capacity (must be called before first use)
inline void set_thread_pool_capacity(size_t capacity) {
    // Assert if pool already created with different capacity
    if (tls_raw_pool && tls_raw_pool->capacity() != capacity) {
        // In HFT, configuration errors should fail fast
        std::terminate();  // Pool already created with different capacity
    }
    tls_pool_config.capacity = capacity;
}

// Helper to get thread-local pool (lazy init with configured capacity)
inline RawMessagePool& get_thread_pool() {
    if (!tls_raw_pool) {
        tls_raw_pool = std::make_unique<RawMessagePool>(tls_pool_config.capacity);
    }
    return *tls_raw_pool;
}

}  // namespace crypto_lob::networking