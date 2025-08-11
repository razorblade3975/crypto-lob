/**
 * @file instrument.hpp
 * @brief Fixed-size instrument identifier for ultra-low latency trading systems
 *
 * This file provides a zero-allocation instrument identification system optimized
 * for HFT applications. The InstrumentId struct is designed to be:
 * - Stack-allocated with no heap usage
 * - Exactly 16 bytes for cache-line efficiency
 * - Trivially copyable for lock-free data structures
 * - Hashable for O(1) lookups in order book managers
 *
 * Performance characteristics:
 * - Zero heap allocations during construction or comparison
 * - Optimized for CPU cache with fixed 16-byte size
 * - Suitable for use in lock-free queues and shared memory
 * - Fast comparison operators for sorted containers
 */

#pragma once

#include <array>
#include <cstddef>  // for std::size_t
#include <cstdint>
#include <functional>  // for std::hash
#include <string>
#include <string_view>

#include "core/exchange_enum.hpp"

namespace crypto_lob::core {

/**
 * @class InstrumentId
 * @brief Fixed-size instrument identifier combining exchange and symbol
 *
 * InstrumentId provides a compact, cache-friendly representation of a trading
 * instrument (e.g., "BTCUSDT" on Binance). It combines an exchange identifier
 * with a symbol string in exactly 16 bytes, making it ideal for high-frequency
 * trading systems where every nanosecond counts.
 *
 * Memory layout (16 bytes total):
 * - 1 byte: ExchangeId enum
 * - 15 bytes: Null-terminated symbol string (max 14 chars + null)
 *
 * The fixed size ensures:
 * - Predictable memory layout in arrays and vectors
 * - No heap allocations or pointer indirection
 * - Efficient copying in lock-free data structures
 * - Cache-line alignment when used in arrays
 *
 * Example usage:
 * @code
 * InstrumentId btc{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
 * InstrumentId eth{ExchangeId::BINANCE_SPOT, "ETHUSDT"};
 *
 * // Use in unordered_map for O(1) lookups
 * std::unordered_map<InstrumentId, OrderBook, InstrumentIdHash> books;
 * books[btc] = OrderBook{...};
 * @endcode
 */
struct InstrumentId {
    ExchangeId exchange;                 // 1 byte - Exchange identifier
    std::array<char, 15> symbol_data{};  // 15 bytes - Null-terminated symbol string

    /**
     * @brief Default constructor - creates an empty instrument ID
     *
     * Initializes with ExchangeId default value and empty symbol.
     * Useful for containers that require default construction.
     */
    constexpr InstrumentId() noexcept = default;

    /**
     * @brief Construct from exchange and C-string literal
     *
     * @tparam N Size of the string literal (including null terminator)
     * @param ex Exchange identifier
     * @param sym Symbol as C-string literal (e.g., "BTCUSDT")
     *
     * This constructor is optimized for compile-time string literals.
     * The template parameter ensures string length validation at compile time.
     *
     * @note Maximum symbol length is 14 characters (15th byte is null terminator)
     * @note Compile-time assertion fails if symbol exceeds 15 characters
     */
    template <std::size_t N>
    constexpr InstrumentId(ExchangeId ex, const char (&sym)[N]) noexcept : exchange(ex), symbol_data{} {
        static_assert(N <= 16, "Symbol too long (max 15 chars)");
        for (std::size_t i = 0; i < N - 1 && i < 14; ++i) {
            symbol_data[i] = sym[i];
        }
    }

    /**
     * @brief Construct from exchange and string_view
     *
     * @param ex Exchange identifier
     * @param sym Symbol as string_view
     *
     * Runtime constructor for dynamic symbol creation.
     * Automatically truncates symbols longer than 14 characters.
     */
    constexpr InstrumentId(ExchangeId ex, std::string_view sym) noexcept : exchange(ex), symbol_data{} {
        set_symbol(sym);
    }

    /**
     * @brief Construct from exchange and std::string
     *
     * @param ex Exchange identifier
     * @param sym Symbol as std::string
     *
     * Compatibility constructor for std::string inputs.
     * Internally converts to string_view for processing.
     */
    InstrumentId(ExchangeId ex, const std::string& sym) noexcept : exchange(ex), symbol_data{} {
        set_symbol(sym);
    }

    /**
     * @brief Update the symbol portion of the instrument ID
     *
     * @param sv New symbol as string_view
     *
     * Replaces the current symbol with a new one.
     * Automatically handles:
     * - Truncation if longer than 14 characters
     * - Null termination
     * - Safe bounds checking
     *
     * @note Does not modify the exchange field
     */
    constexpr void set_symbol(std::string_view sv) noexcept {
        std::size_t len = sv.length() < 14 ? sv.length() : 14;
        for (std::size_t i = 0; i < len; ++i) {
            symbol_data[i] = sv[i];
        }
        symbol_data[len] = '\0';
    }

    /**
     * @brief Get symbol as string_view
     *
     * @return string_view of the symbol (without null terminator)
     *
     * Returns a view into the internal symbol data.
     * No memory allocation or copying occurs.
     * The returned view is valid as long as the InstrumentId exists.
     */
    [[nodiscard]] constexpr std::string_view symbol() const noexcept {
        // Find null terminator for correct string length
        std::size_t len = 0;
        while (len < 15 && symbol_data[len] != '\0') {
            ++len;
        }
        return std::string_view(symbol_data.data(), len);
    }

    /**
     * @brief Get symbol as null-terminated C-string
     *
     * @return Pointer to internal null-terminated string
     *
     * Useful for C API compatibility or printf-style formatting.
     * The returned pointer is valid as long as the InstrumentId exists.
     */
    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return symbol_data.data();
    }

    /**
     * @brief Equality comparison operator
     *
     * @param other InstrumentId to compare with
     * @return true if both exchange and symbol match
     *
     * Performs efficient comparison:
     * 1. First compares exchange (1 byte)
     * 2. Then compares symbol strings up to null terminator
     *
     * Early termination on first difference for performance.
     */
    [[nodiscard]] constexpr bool operator==(const InstrumentId& other) const noexcept {
        if (exchange != other.exchange)
            return false;
        // Compare symbol data up to first null terminator
        for (std::size_t i = 0; i < 15; ++i) {
            if (symbol_data[i] != other.symbol_data[i])
                return false;
            if (symbol_data[i] == '\0')
                break;  // Both strings end here
        }
        return true;
    }

    /**
     * @brief Three-way comparison operator (spaceship operator)
     *
     * @param other InstrumentId to compare with
     * @return Ordering result (less, equal, or greater)
     *
     * Enables use in sorted containers (std::map, std::set).
     * Comparison order:
     * 1. First by exchange ID (numerical order)
     * 2. Then by symbol (lexicographical order)
     *
     * This ordering groups instruments by exchange, then alphabetically.
     */
    [[nodiscard]] constexpr auto operator<=>(const InstrumentId& other) const noexcept {
        if (auto cmp = exchange <=> other.exchange; cmp != 0)
            return cmp;
        // Compare symbol strings lexicographically
        for (std::size_t i = 0; i < 15; ++i) {
            if (symbol_data[i] < other.symbol_data[i])
                return std::strong_ordering::less;
            if (symbol_data[i] > other.symbol_data[i])
                return std::strong_ordering::greater;
            if (symbol_data[i] == '\0')
                return std::strong_ordering::equal;
        }
        return std::strong_ordering::equal;
    }
};

/**
 * @class InstrumentIdHash
 * @brief Hash functor for InstrumentId to enable unordered container usage
 *
 * Provides a hash function suitable for std::unordered_map and std::unordered_set.
 * The hash combines:
 * - Symbol hash using std::hash<string_view> (optimized implementation)
 * - Exchange ID mixed in with XOR and bit shift
 *
 * This ensures good distribution even when instruments share common prefixes
 * (e.g., many symbols ending in "USDT").
 *
 * Example usage:
 * @code
 * std::unordered_map<InstrumentId, OrderBook, InstrumentIdHash> order_books;
 * std::unordered_set<InstrumentId, InstrumentIdHash> active_instruments;
 * @endcode
 */
struct InstrumentIdHash {
    /**
     * @brief Compute hash value for an InstrumentId
     *
     * @param inst_id Instrument identifier to hash
     * @return Hash value suitable for unordered containers
     *
     * The hash function is designed to:
     * - Avoid collisions between different exchanges with same symbol
     * - Leverage optimized std::hash<string_view> implementation
     * - Maintain good distribution for typical crypto symbols
     */
    std::size_t operator()(const InstrumentId& inst_id) const noexcept {
        // Combine symbol hash with exchange ID using XOR and bit shift
        // The shift ensures exchange differences affect the hash significantly
        return std::hash<std::string_view>{}(inst_id.symbol()) ^ (static_cast<std::size_t>(inst_id.exchange) << 1);
    }
};

}  // namespace crypto_lob::core