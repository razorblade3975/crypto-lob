#pragma once

/**
 * @file price.hpp
 * @brief High-performance fixed-point price representation for HFT
 *
 * This file implements an ultra-low latency price representation optimized for
 * high-frequency trading systems. The design uses fixed-point arithmetic with a
 * constant scale of 10^9, providing deterministic performance without floating-point
 * precision issues or dynamic memory allocation.
 *
 * DESIGN RATIONALE:
 * - Fixed-point arithmetic eliminates floating-point precision errors that can
 *   accumulate in trading calculations
 * - Single 64-bit integer storage fits in one CPU register for maximum performance
 * - Constant scale factor (10^9) eliminates normalization overhead
 * - All operations are branch-free where possible for predictable execution time
 *
 * PRECISION & RANGE:
 * - Provides 9 decimal places of precision (nanodollar accuracy)
 * - Range: $0.000000001 to $9,223,372,036.854775807 (2^63-1 / 10^9)
 * - Suitable for all cryptocurrency prices including micro-cap tokens
 *
 * THREAD SAFETY:
 * - All operations on const Price objects are inherently thread-safe
 * - No mutable state or global variables
 * - Immutable design supports lock-free algorithms
 *
 * PERFORMANCE CHARACTERISTICS:
 * - Single 64-bit register operations throughout
 * - Zero heap allocations in arithmetic operations
 * - Cache-friendly 8-byte memory footprint
 * - Constexpr operations enable compile-time computation
 *
 * USAGE EXAMPLES:
 * @code
 *   // Construction from string with arbitrary precision
 *   auto btc_price = Price::from_string("67432.123456789");
 *
 *   // Fast arithmetic operations
 *   auto total_cost = btc_price * quantity;  // Single multiply instruction
 *
 *   // Comparison operations
 *   if (ask_price < bid_price) {  // Single comparison instruction
 *       // Handle crossed book
 *   }
 *
 *   // Raw value access for ultra-fast operations
 *   int64_t raw = price.raw_value();  // Direct access to internal value
 * @endcode
 */

#include <compare>
#include <cstddef>  // for std::size_t
#include <cstdint>
#include <format>
#include <functional>  // for std::hash
#include <limits>
#include <string>
#include <string_view>

// No longer need __int128 - using native 64-bit integers

namespace crypto_lob::core {

/**
 * @class Price
 * @brief High-performance fixed-point price representation for HFT systems
 *
 * DESIGN PHILOSOPHY:
 * The Price class prioritizes performance over flexibility. It uses a fixed-point
 * representation with a constant scale factor of 10^9, trading some range for
 * maximum speed and precision. This design choice eliminates the need for:
 * - Floating-point operations (which can be non-deterministic)
 * - Dynamic scale adjustment (which adds computational overhead)
 * - Heap allocations (which introduce latency and fragmentation)
 *
 * PRECISION GUARANTEES:
 * - Exact representation of 9 decimal places
 * - No precision loss in addition/subtraction operations
 * - Multiplication/division may introduce rounding for large values
 * - String parsing supports arbitrary precision (truncated to 9 places)
 *
 * OVERFLOW/UNDERFLOW BEHAVIOR:
 * - Arithmetic operations do NOT check for overflow (for maximum speed)
 * - Caller responsible for ensuring operations stay within int64_t range
 * - Overflow results in undefined behavior (wrap-around in practice)
 * - Use Price::max() and Price::min() to check bounds before operations
 *
 * MEMORY LAYOUT:
 * - Total size: 8 bytes (single int64_t)
 * - Alignment: 8 bytes (optimal for 64-bit architectures)
 * - No virtual functions (zero vtable overhead)
 * - POD-like semantics for maximum optimization
 */
class Price {
  private:
    // Fixed scale for all prices - eliminates normalization overhead completely
    static constexpr int64_t SCALE = 1'000'000'000LL;  // 10^9 for all operations

    // 64-bit storage - fits in single CPU register, 8-byte cache footprint
    int64_t value_;

    // Helper function to convert int64_t to string
    static std::string int64_to_string(int64_t value) {
        if (value == 0)
            return "0";

        const bool negative = value < 0;
        if (negative)
            value = -value;

        std::string result;
        while (value > 0) {
            result = char('0' + (value % 10)) + result;
            value /= 10;
        }

        if (negative)
            result = "-" + result;
        return result;
    }

  public:
    /**
     * @brief Default constructor - creates zero price
     * @details Initializes price to exactly zero (0.000000000)
     * @note Constexpr enables compile-time construction
     */
    constexpr Price() noexcept : value_(0) {}

    /**
     * @brief Direct construction from raw scaled value
     * @param raw_value The pre-scaled integer value (value * 10^9)
     * @details Use this constructor when you have a raw value that's already
     *          scaled by 10^9. This is the fastest construction method.
     * @warning No validation - caller must ensure raw_value is properly scaled
     * @note Constexpr enables compile-time construction
     */
    constexpr explicit Price(int64_t raw_value) noexcept : value_(raw_value) {}

    /**
     * @brief Construction from double precision floating-point
     * @param price The price as a double value
     * @details Converts double to fixed-point by multiplying by 10^9 and truncating
     * @warning May introduce precision errors due to floating-point representation
     * @warning Use from_string() for exact decimal input
     * @note Constexpr enables compile-time construction for constant expressions
     */
    constexpr explicit Price(double price) noexcept : value_(static_cast<int64_t>(price * SCALE)) {}

    /**
     * @brief Parse string representation with high precision
     * @param str String representation of price (e.g., "123.456789012")
     * @return Price object representing the parsed value
     *
     * PARSING ALGORITHM:
     * - Supports negative values (prefix with '-')
     * - Handles integer-only strings ("123")
     * - Handles decimal strings ("123.456")
     * - Truncates precision beyond 9 decimal places
     * - Pads short decimal representations with zeros
     *
     * SUPPORTED FORMATS:
     * - "0" -> Price(0)
     * - "123" -> Price(123 * 10^9)
     * - "123.456" -> Price(123456000000)
     * - "0.000000001" -> Price(1) [smallest unit]
     * - "-123.456" -> Price(-123456000000)
     *
     * EDGE CASES:
     * - Empty string returns zero
     * - Invalid characters are ignored (parsed as zero for that position)
     * - Overflow/underflow wraps around (no bounds checking for performance)
     * - Leading/trailing whitespace is NOT supported
     *
     * @warning No input validation for maximum performance
     * @warning Overflow/underflow results in undefined behavior
     */
    static Price from_string(std::string_view str) {
        if (str.empty())
            return Price();

        // Handle negative numbers
        bool negative = false;
        if (str[0] == '-') {
            negative = true;
            str = str.substr(1);
        }

        // Find decimal point
        auto decimal_pos = str.find('.');
        bool has_decimal = (decimal_pos != std::string_view::npos);

        // Parse integer part
        int64_t integer_part = 0;
        std::string_view integer_str = has_decimal ? str.substr(0, decimal_pos) : str;

        for (char c : integer_str) {
            if (c >= '0' && c <= '9') {
                integer_part = integer_part * 10 + (c - '0');
            }
        }

        // Parse fractional part
        int64_t fractional_part = 0;
        int decimal_places = 0;

        if (has_decimal && decimal_pos + 1 < str.length()) {
            std::string_view fractional_str = str.substr(decimal_pos + 1);

            for (char ch : fractional_str) {
                if (ch >= '0' && ch <= '9' && decimal_places < 9) {  // Limit to 9 decimal places
                    fractional_part = fractional_part * 10 + (ch - '0');
                    decimal_places++;
                }
            }
        }

        // Calculate final value with fixed scale
        int64_t final_value = integer_part * SCALE;

        if (decimal_places > 0) {
            // Pad fractional part to match our 9-decimal scale
            for (int i = decimal_places; i < 9; ++i) {
                fractional_part *= 10;
            }

            final_value += fractional_part;
        }

        if (negative) {
            final_value = -final_value;
        }

        return Price(final_value);
    }

    /**
     * @brief Access the raw scaled integer value
     * @return The internal 64-bit integer (price * 10^9)
     *
     * WHEN TO USE:
     * - Ultra-high-performance operations where you work directly with scaled values
     * - Serialization/deserialization to binary formats
     * - Network protocol implementations
     * - Comparison operations where you need maximum speed
     *
     * @warning The returned value is scaled by 10^9 - divide by SCALE to get actual price
     * @warning Direct manipulation of raw values bypasses all safety checks
     * @note Single CPU instruction - no computation overhead
     */
    [[nodiscard]] constexpr int64_t raw_value() const noexcept {
        return value_;
    }

    /**
     * @brief Get the scale factor used by this Price representation
     * @return The constant scale factor (10^9)
     * @details Useful for generic algorithms that work with different fixed-point types
     * @note Always returns 1,000,000,000 - provided for API consistency
     */
    [[nodiscard]] constexpr int64_t scale() const noexcept {
        return SCALE;
    }

    /**
     * @brief Convert to double precision floating-point
     * @return The price as a double value
     *
     * PRECISION LOSS WARNING:
     * - Double precision can only exactly represent ~15-16 decimal digits
     * - For large prices (>$1B), precision beyond 6-7 decimal places may be lost
     * - Results may not round-trip exactly: Price::from_string(price.to_double()) != price
     * - Use to_string() for exact representation
     *
     * WHEN TO USE:
     * - Display purposes where small precision loss is acceptable
     * - Mathematical operations that require floating-point (logs, trig functions)
     * - Integration with legacy floating-point APIs
     *
     * @warning Not suitable for exact financial calculations
     * @note Single division operation - relatively fast but not as fast as raw_value()
     */
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / SCALE;
    }

    /**
     * @brief Convert to string representation with full precision
     * @return String representation showing up to 9 decimal places
     *
     * OUTPUT FORMAT:
     * - Integer prices: "123" (no decimal point for whole numbers)
     * - Decimal prices: "123.456789" (trailing zeros removed)
     * - Zero: "0"
     * - Negative: "-123.456"
     * - Maximum precision: 9 decimal places
     * - Minimum unit: "0.000000001"
     *
     * PERFORMANCE CHARACTERISTICS:
     * - Requires memory allocation for string construction
     * - Optimized integer-to-string conversion without division loops
     * - Efficient trailing zero removal
     * - Not suitable for ultra-high-frequency code paths
     *
     * @warning Allocates memory - avoid in performance-critical loops
     * @note Exact representation - safe for serialization and display
     */
    [[nodiscard]] std::string to_string() const {
        if (value_ == 0)
            return "0";

        const bool negative = value_ < 0;
        const int64_t abs_value = negative ? -value_ : value_;

        const int64_t integer_part = abs_value / SCALE;
        const int64_t fractional_part = abs_value % SCALE;

        std::string result;
        if (negative)
            result += "-";

        result += int64_to_string(integer_part);

        if (fractional_part > 0) {
            result += ".";

            // Convert fractional part to string with exactly 9 digits
            std::string frac_str = int64_to_string(fractional_part);

            // Pad with leading zeros to get exactly 9 digits
            while (frac_str.length() < 9) {
                frac_str = "0" + frac_str;
            }

            // Remove trailing zeros
            while (!frac_str.empty() && frac_str.back() == '0') {
                frac_str.pop_back();
            }

            result += frac_str;
        }

        return result;
    }

    /**
     * @brief Addition operator
     * @param other The price to add
     * @return Sum of this price and other
     *
     * OVERFLOW BEHAVIOR:
     * - No overflow checking for maximum performance
     * - Overflow wraps around following two's complement arithmetic
     * - Caller responsible for ensuring result fits in int64_t range
     * - Use Price::max() - price to check available headroom
     *
     * @note Single CPU add instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr Price operator+(const Price& other) const noexcept {
        return Price(value_ + other.value_);
    }

    /**
     * @brief Subtraction operator
     * @param other The price to subtract
     * @return Difference of this price minus other
     *
     * OVERFLOW BEHAVIOR:
     * - No underflow checking for maximum performance
     * - Underflow wraps around following two's complement arithmetic
     * - Can produce negative prices (allowed in this implementation)
     * - Caller responsible for ensuring result fits in int64_t range
     *
     * @note Single CPU subtract instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr Price operator-(const Price& other) const noexcept {
        return Price(value_ - other.value_);
    }

    /**
     * @brief Multiplication by integer quantity
     * @param multiplier The integer to multiply by (typically quantity)
     * @return Product of this price and the multiplier
     *
     * OVERFLOW BEHAVIOR:
     * - No overflow checking for maximum performance
     * - Large multipliers can cause overflow
     * - Check: if (price.raw_value() > INT64_MAX / multiplier) before calling
     * - Overflow results in undefined behavior (wrap-around)
     *
     * PRECISION:
     * - Maintains full precision since both operands are integers
     * - No rounding errors introduced
     * - Result represents total value (price * quantity)
     *
     * @note Single CPU multiply instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr Price operator*(int64_t multiplier) const noexcept {
        return Price(value_ * multiplier);
    }

    /**
     * @brief Division by integer
     * @param divisor The integer to divide by
     * @return Quotient of this price divided by divisor
     *
     * OVERFLOW BEHAVIOR:
     * - No division-by-zero checking for maximum performance
     * - Division by zero results in undefined behavior
     * - Caller responsible for ensuring divisor != 0
     *
     * PRECISION:
     * - Integer division truncates toward zero
     * - May lose precision for non-exact divisions
     * - Example: Price("1.000000001") / 3 = Price("0.333333333") (loses remainder)
     * - For exact division, ensure dividend is multiple of divisor
     *
     * @warning Division by zero causes undefined behavior
     * @note Single CPU divide instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr Price operator/(int64_t divisor) const noexcept {
        return Price(value_ / divisor);
    }

    /**
     * @brief Three-way comparison operator (spaceship operator)
     * @param other The price to compare with
     * @return std::strong_ordering indicating relative order
     *
     * PERFORMANCE CHARACTERISTICS:
     * - Single 64-bit integer comparison
     * - Branch-free comparison for predictable latency
     * - Enables all comparison operators (<, <=, >, >=, ==, !=)
     * - Optimal for sorting and binary search operations
     *
     * COMPARISON SEMANTICS:
     * - Exact comparison of scaled values
     * - No floating-point precision issues
     * - Price("1.000000001") > Price("1.000000000") is reliable
     * - Total ordering: any two prices can be compared
     *
     * @note Single CPU comparison instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr std::strong_ordering operator<=>(const Price& other) const noexcept {
        return value_ <=> other.value_;
    }

    /**
     * @brief Equality comparison operator
     * @param other The price to compare with
     * @return True if prices are exactly equal
     *
     * PERFORMANCE CHARACTERISTICS:
     * - Single 64-bit integer comparison
     * - No floating-point precision issues
     * - Faster than three-way comparison for equality testing
     * - Branch-free operation for predictable timing
     *
     * PRECISION:
     * - Exact equality comparison
     * - Price("1.000000001") != Price("1.000000000")
     * - No tolerance or epsilon comparison
     * - Suitable for exact financial calculations
     *
     * @note Single CPU comparison instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    constexpr bool operator==(const Price& other) const noexcept {
        return value_ == other.value_;
    }

    /**
     * @brief Addition assignment operator
     * @param other The price to add to this price
     * @return Reference to this price after addition
     * @note Same overflow behavior as operator+
     * @note Constexpr enables compile-time computation
     */
    constexpr Price& operator+=(const Price& other) noexcept {
        *this = *this + other;
        return *this;
    }

    /**
     * @brief Subtraction assignment operator
     * @param other The price to subtract from this price
     * @return Reference to this price after subtraction
     * @note Same underflow behavior as operator-
     * @note Constexpr enables compile-time computation
     */
    constexpr Price& operator-=(const Price& other) noexcept {
        *this = *this - other;
        return *this;
    }

    /**
     * @brief Multiplication assignment operator
     * @param multiplier The integer to multiply this price by
     * @return Reference to this price after multiplication
     * @note Same overflow behavior as operator*
     * @note Constexpr enables compile-time computation
     */
    constexpr Price& operator*=(int64_t multiplier) noexcept {
        value_ *= multiplier;
        return *this;
    }

    /**
     * @brief Division assignment operator
     * @param divisor The integer to divide this price by
     * @return Reference to this price after division
     * @note Same precision loss behavior as operator/
     * @warning Division by zero causes undefined behavior
     * @note Constexpr enables compile-time computation
     */
    constexpr Price& operator/=(int64_t divisor) noexcept {
        value_ /= divisor;
        return *this;
    }

    /**
     * @brief Check if price is exactly zero
     * @return True if price equals 0.000000000
     * @note Single comparison instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return value_ == 0;
    }

    /**
     * @brief Check if price is positive
     * @return True if price > 0.000000000
     * @note Single comparison instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    [[nodiscard]] constexpr bool is_positive() const noexcept {
        return value_ > 0;
    }

    /**
     * @brief Check if price is negative
     * @return True if price < 0.000000000
     * @note Single comparison instruction - optimal performance
     * @note Constexpr enables compile-time computation
     */
    [[nodiscard]] constexpr bool is_negative() const noexcept {
        return value_ < 0;
    }

    /**
     * @brief Create zero price constant
     * @return Price representing exactly zero (0.000000000)
     * @note Equivalent to Price() but more explicit in intent
     * @note Constexpr enables compile-time computation
     */
    static constexpr Price zero() noexcept {
        return Price{};  // Use default constructor
    }

    /**
     * @brief Get maximum representable price
     * @return Maximum Price value ($9,223,372,036.854775807)
     * @details Useful for bounds checking before arithmetic operations
     * @note Constexpr enables compile-time computation
     */
    static constexpr Price max() noexcept {
        return Price(std::numeric_limits<int64_t>::max());
    }

    /**
     * @brief Get minimum representable price
     * @return Minimum Price value (-$9,223,372,036.854775808)
     * @details Useful for bounds checking before arithmetic operations
     * @note Constexpr enables compile-time computation
     */
    static constexpr Price min() noexcept {
        return Price(std::numeric_limits<int64_t>::min());
    }
};

/**
 * @typedef Quantity
 * @brief Type alias for quantities in order book levels
 * 
 * DESIGN RATIONALE:
 * - Uses the same fixed-point representation as Price for consistency
 * - Provides 9 decimal places for precise quantity representation
 * - Enables exact calculations without floating-point errors
 * - Same performance characteristics as Price (single 64-bit integer)
 * 
 * USAGE:
 * - Represents order sizes, trade volumes, and position quantities
 * - Supports same arithmetic operations as Price
 * - Range: 0.000000001 to 9,223,372,036.854775807 units
 * - Suitable for both large institutional orders and micro-trading
 * 
 * EXAMPLES:
 * @code
 *   Quantity btc_amount = Quantity::from_string("0.12345678");  // 8 decimals typical for BTC
 *   Quantity total_volume = btc_amount * 1000;  // Aggregate across orders
 *   if (available_balance >= required_quantity) { /* execute */ }
*@endcode* / using Quantity = Price;

/**
 * @concept PriceType
 * @brief Concept to constrain template parameters to Price types
 *
 * USAGE:
 * - Enables type-safe generic algorithms for price operations
 * - Prevents accidental mixing of Price with other numeric types
 * - Improves error messages for template instantiation failures
 *
 * @code
 * template<PriceType P>
 * P calculate_vwap(const std::vector<std::pair<P, Quantity>>& orders);
 * @endcode
 */
template <typename T>
concept PriceType = std::same_as<T, Price>;

/**
 * @concept QuantityType
 * @brief Concept to constrain template parameters to Quantity types
 *
 * USAGE:
 * - Enables type-safe generic algorithms for quantity operations
 * - Provides semantic distinction between prices and quantities
 * - Improves template error diagnostics
 *
 * @code
 * template<QuantityType Q>
 * Q aggregate_volume(const std::vector<Q>& order_sizes);
 * @endcode
 */
template <typename T>
concept QuantityType = std::same_as<T, Quantity>;

/**
 * @brief Hash function for Price type compatible with Boost containers
 * @param p The Price object to hash
 * @return Hash value suitable for hash tables and unordered containers
 *
 * IMPLEMENTATION:
 * - Directly hashes the raw 64-bit integer value
 * - No scale normalization needed (constant scale factor)
 * - Compatible with Boost.Container hash requirements
 * - Provides uniform distribution for typical price ranges
 *
 * PERFORMANCE:
 * - Single hash operation on 64-bit integer
 * - No additional computation or memory access
 * - Optimal for hash-based lookups in trading algorithms
 *
 * USAGE:
 * @code
 * boost::unordered_map<Price, OrderInfo> price_levels;
 * std::unordered_set<Price> unique_prices;
 * @endcode
 */
inline std::size_t hash_value(const Price& p) {
    // Simple hash of 64-bit value - no scale mixing needed
    return std::hash<int64_t>{}(p.raw_value());
}

}  // namespace crypto_lob::core

/**
 * @brief std::format specialization for Price type
 *
 * FUNCTIONALITY:
 * - Enables Price objects to be used directly with std::format
 * - Uses Price::to_string() for consistent formatting
 * - Supports all standard formatting contexts
 *
 * USAGE:
 * @code
 * Price btc_price = Price::from_string("67432.123456789");
 * std::string msg = std::format("BTC price: {}", btc_price);
 * // Output: "BTC price: 67432.123456789"
 *
 * std::println("Current ask: {} bid: {}", ask_price, bid_price);
 * @endcode
 *
 * PERFORMANCE:
 * - Delegates to Price::to_string() which allocates memory
 * - Not suitable for ultra-high-frequency logging
 * - Consider using raw_value() for performance-critical formatting
 */
template <>
struct std::formatter<crypto_lob::core::Price> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const crypto_lob::core::Price& price, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", price.to_string());
    }
};