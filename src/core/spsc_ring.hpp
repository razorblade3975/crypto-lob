#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <new>  // For std::align_val_t
#include <type_traits>

#include "cache.hpp"  // Assumes this defines CACHELINE_SIZE and CACHE_ALIGNED

namespace crypto_lob::core {

// Always inline macro for hot path functions - using Clang-specific attribute
#define ALWAYS_INLINE [[clang::always_inline]] inline

// Aligned memory deleter for use with unique_ptr
struct AlignedDeleter {
    void operator()(void* ptr) const {
        ::operator delete[](ptr, std::align_val_t(CACHELINE_SIZE));
    }
};

// Single-producer, single-consumer wait-free ring buffer.
// Optimized for ultra-low latency with cache-line aligned slots.
// Uses Disruptor-style per-slot sequence numbers for correctness.
template <typename T>
class SPSCRing {
  private:
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable for wait-free operation");

    // Maximum safe capacity to prevent sequence number issues
    static constexpr size_t MAX_CAPACITY = size_t{1} << 32;

    // Ensure capacity is a power of two for fast modulo via bit-mask.
    static constexpr size_t round_up_to_power_of_two(size_t value) noexcept {
        if (value <= 2)
            return 2;  // Minimum capacity of 2
        if (value > MAX_CAPACITY)
            return MAX_CAPACITY;  // Guard against overflow
        return size_t{1} << std::bit_width(value - 1);
    }

    // Slot structure - cache-line aligned to prevent false sharing.
    struct alignas(CACHELINE_SIZE) Slot {
        std::atomic<uint64_t> sequence{0};  // Sequence number for this slot
        T data;

        // Padding to ensure each slot occupies a full cache line.
        static constexpr size_t content_size = sizeof(std::atomic<uint64_t>) + sizeof(T);
        static_assert(content_size <= CACHELINE_SIZE, "Slot payload too large for a single cache line");

        static constexpr size_t padding_size = (content_size < CACHELINE_SIZE) ? (CACHELINE_SIZE - content_size) : 0;
        std::array<std::byte, padding_size> padding{};
    };

    // Statically enforce that the Slot struct has the expected size and alignment.
    static_assert(sizeof(Slot) == CACHELINE_SIZE, "Slot struct must be exactly CACHELINE_SIZE bytes");
    static_assert(alignof(Slot) >= CACHELINE_SIZE, "Slot struct must be cache-line aligned");

    const size_t capacity_;
    const size_t mask_;

    // Producer and consumer sequence numbers - separated by cache lines to avoid false sharing.
    CACHE_ALIGNED std::atomic<uint64_t> head_seq_{0};  // Next sequence to write
    CACHE_ALIGNED std::atomic<uint64_t> tail_seq_{0};  // Next sequence to read

    // The ring buffer storage.
    std::unique_ptr<Slot[], AlignedDeleter> slots_;

  public:
    explicit SPSCRing(size_t capacity) : capacity_(round_up_to_power_of_two(capacity)), mask_(capacity_ - 1) {
        // Check original request before rounding/capping
        if (capacity > MAX_CAPACITY) {
#ifdef __cpp_exceptions
            throw std::invalid_argument("Ring capacity exceeds maximum safe size");
#else
            // When exceptions are disabled, terminate on invalid capacity
            std::terminate();
#endif
        }

        // Allocate cache-line aligned memory for the slots array.
        void* raw_mem = ::operator new[](capacity_ * sizeof(Slot), std::align_val_t(CACHELINE_SIZE));
        slots_.reset(static_cast<Slot*>(raw_mem));

        // Initialize slots with proper sequence numbers
        for (size_t i = 0; i < capacity_; ++i) {
            new (&slots_[i]) Slot();
            // Initialize each slot's sequence to its index (Disruptor pattern)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    ~SPSCRing() {
        // Manually call destructors since we used placement-new
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].~Slot();
        }
    }

    // Non-copyable, non-movable for thread safety.
    SPSCRing(const SPSCRing&) = delete;
    SPSCRing& operator=(const SPSCRing&) = delete;
    SPSCRing(SPSCRing&&) = delete;
    SPSCRing& operator=(SPSCRing&&) = delete;

    // Producer: Try to push an item (non-blocking).
    [[nodiscard]] ALWAYS_INLINE bool try_push(const T& item) noexcept {
        const uint64_t current_head = head_seq_.load(std::memory_order_relaxed);
        Slot& slot = slots_[current_head & mask_];

        // Slot is free when sequence == head_seq
        const uint64_t slot_seq = slot.sequence.load(std::memory_order_acquire);
        if (slot_seq != current_head) [[unlikely]] {
            return false;  // Slot not yet consumed
        }

        slot.data = item;

        // Publish by setting sequence = head_seq + 1
        slot.sequence.store(current_head + 1, std::memory_order_release);
        head_seq_.store(current_head + 1, std::memory_order_relaxed);

        return true;
    }

    // Producer: Try to push an item (move semantics).
    [[nodiscard]] ALWAYS_INLINE bool try_push(T&& item) noexcept {
        const uint64_t current_head = head_seq_.load(std::memory_order_relaxed);
        Slot& slot = slots_[current_head & mask_];

        const uint64_t slot_seq = slot.sequence.load(std::memory_order_acquire);
        if (slot_seq != current_head) [[unlikely]] {
            return false;
        }

        slot.data = std::move(item);
        slot.sequence.store(current_head + 1, std::memory_order_release);
        head_seq_.store(current_head + 1, std::memory_order_relaxed);

        return true;
    }

    // Consumer: Try to pop an item (non-blocking).
    [[nodiscard]] ALWAYS_INLINE bool try_pop(T& item) noexcept {
        const uint64_t current_tail = tail_seq_.load(std::memory_order_relaxed);
        Slot& slot = slots_[current_tail & mask_];

        // Slot is ready when sequence == tail_seq + 1
        const uint64_t expected_seq = current_tail + 1;
        const uint64_t slot_seq = slot.sequence.load(std::memory_order_acquire);
        if (slot_seq != expected_seq) [[unlikely]] {
            return false;  // No data available
        }

        item = std::move(slot.data);

        // Mark slot free for next wrap: sequence = tail_seq + capacity
        slot.sequence.store(current_tail + capacity_, std::memory_order_release);
        tail_seq_.store(current_tail + 1, std::memory_order_relaxed);

        return true;
    }

    // Get the ring's capacity.
    [[nodiscard]] size_t capacity() const noexcept {
        return capacity_;
    }

    // Get the number of items currently in the ring.
    [[nodiscard]] size_t size() const noexcept {
        const uint64_t head = head_seq_.load(std::memory_order_acquire);
        const uint64_t tail = tail_seq_.load(std::memory_order_acquire);
        return static_cast<size_t>(head - tail);
    }

    // Check if the ring is empty.
    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

    // Check if the ring is full.
    [[nodiscard]] bool full() const noexcept {
        return size() >= capacity_;
    }

    // Get the fill percentage (0.0 to 1.0).
    [[nodiscard]] double fill_ratio() const noexcept {
        return static_cast<double>(size()) / capacity_;
    }
};

}  // namespace crypto_lob::core