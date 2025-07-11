#pragma once

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

// High-performance fixed-point price representation for cryptocurrency prices
// Uses 64-bit integer with fixed 10^9 scaling for optimal HFT performance
// Covers crypto range from $0.000000001 to $9,223,372,036 with 9 decimal places
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
    constexpr Price() noexcept : value_(0) {}

    constexpr explicit Price(int64_t raw_value) noexcept : value_(raw_value) {}

    constexpr explicit Price(double price) noexcept : value_(static_cast<int64_t>(price * SCALE)) {}

    // Parse string representation with arbitrary precision
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

    [[nodiscard]] constexpr int64_t raw_value() const noexcept {
        return value_;
    }
    [[nodiscard]] constexpr int64_t scale() const noexcept {
        return SCALE;
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / SCALE;
    }

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

    // Arithmetic operations - ultra-fast 64-bit integer math
    constexpr Price operator+(const Price& other) const noexcept {
        return Price(value_ + other.value_);
    }

    constexpr Price operator-(const Price& other) const noexcept {
        return Price(value_ - other.value_);
    }

    constexpr Price operator*(int64_t multiplier) const noexcept {
        return Price(value_ * multiplier);
    }

    constexpr Price operator/(int64_t divisor) const noexcept {
        return Price(value_ / divisor);
    }

    // Comparison operators - simple 64-bit integer comparisons
    constexpr std::strong_ordering operator<=>(const Price& other) const noexcept {
        return value_ <=> other.value_;
    }

    constexpr bool operator==(const Price& other) const noexcept {
        return value_ == other.value_;
    }

    // Assignment operators
    constexpr Price& operator+=(const Price& other) noexcept {
        *this = *this + other;
        return *this;
    }

    constexpr Price& operator-=(const Price& other) noexcept {
        *this = *this - other;
        return *this;
    }

    constexpr Price& operator*=(int64_t multiplier) noexcept {
        value_ *= multiplier;
        return *this;
    }

    constexpr Price& operator/=(int64_t divisor) noexcept {
        value_ /= divisor;
        return *this;
    }

    // Utility methods
    [[nodiscard]] constexpr bool is_zero() const noexcept {
        return value_ == 0;
    }
    [[nodiscard]] constexpr bool is_positive() const noexcept {
        return value_ > 0;
    }
    [[nodiscard]] constexpr bool is_negative() const noexcept {
        return value_ < 0;
    }

    static constexpr Price zero() noexcept {
        return Price{};  // Use default constructor
    }
    static constexpr Price max() noexcept {
        return Price(std::numeric_limits<int64_t>::max());
    }
    static constexpr Price min() noexcept {
        return Price(std::numeric_limits<int64_t>::min());
    }
};

// Quantity type for price level quantities (same precision requirements as Price)
using Quantity = Price;

// Concepts for type safety
template <typename T>
concept PriceType = std::same_as<T, Price>;

template <typename T>
concept QuantityType = std::same_as<T, Quantity>;

// Hash function for Price type to support Boost containers
inline std::size_t hash_value(const Price& p) {
    // Simple hash of 64-bit value - no scale mixing needed
    return std::hash<int64_t>{}(p.raw_value());
}

}  // namespace crypto_lob::core

// Formatter for Price type
template <>
struct std::formatter<crypto_lob::core::Price> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const crypto_lob::core::Price& price, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", price.to_string());
    }
};