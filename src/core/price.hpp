#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <string_view>

#include "platform.hpp"  // Ensures __int128 support

namespace crypto_lob::core {

// Adaptive fixed-point price representation for cryptocurrency prices
// Uses 128-bit integer with dynamic scaling to handle extreme price ranges
// From $0.000000001 (1e-9) to $100,000,000 (1e8) with high precision
class Price {
  private:
    // Use 128-bit integer for extended range
    using value_type = __int128;

    // Dynamic scale based on price magnitude
    // For prices >= $1: scale = 1e8 (8 decimal places)
    // For prices < $1: scale = 1e18 (18 decimal places)
    static constexpr int64_t HIGH_SCALE = 100'000'000LL;               // 10^8 for prices >= $1
    static constexpr int64_t LOW_SCALE = 1'000'000'000'000'000'000LL;  // 10^18 for prices < $1
    static constexpr double HIGH_SCALE_THRESHOLD = 1.0;

    value_type value_;
    int64_t scale_;

    // Determine appropriate scale for a given double value
    static constexpr int64_t determine_scale(double price) noexcept {
        return (std::abs(price) >= HIGH_SCALE_THRESHOLD) ? HIGH_SCALE : LOW_SCALE;
    }

    // Normalize two prices to the same scale for operations
    static constexpr std::pair<value_type, value_type> normalize_for_operation(const Price& a,
                                                                               const Price& b) noexcept {
        if (a.scale_ == b.scale_) {
            return {a.value_, b.value_};
        }

        // Convert to the higher precision scale
        int64_t target_scale = std::max(a.scale_, b.scale_);

        value_type a_normalized = a.value_;
        value_type b_normalized = b.value_;

        if (a.scale_ < target_scale) {
            a_normalized = a.value_ * (target_scale / a.scale_);
        }
        if (b.scale_ < target_scale) {
            b_normalized = b.value_ * (target_scale / b.scale_);
        }

        return {a_normalized, b_normalized};
    }

    // Helper function to convert __int128 to string
    static std::string int128_to_string(value_type value) {
        if (value == 0)
            return "0";

        bool negative = value < 0;
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
    constexpr Price() noexcept : value_(0), scale_(HIGH_SCALE) {}

    constexpr explicit Price(value_type raw_value, int64_t scale) noexcept : value_(raw_value), scale_(scale) {}

    constexpr Price(double price) noexcept {
        scale_ = determine_scale(price);
        value_ = static_cast<value_type>(price * scale_);
    }

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
        value_type integer_part = 0;
        std::string_view integer_str = has_decimal ? str.substr(0, decimal_pos) : str;

        for (char c : integer_str) {
            if (c >= '0' && c <= '9') {
                integer_part = integer_part * 10 + (c - '0');
            }
        }

        // Parse fractional part
        value_type fractional_part = 0;
        int decimal_places = 0;

        if (has_decimal && decimal_pos + 1 < str.length()) {
            std::string_view fractional_str = str.substr(decimal_pos + 1);

            for (char c : fractional_str) {
                if (c >= '0' && c <= '9') {
                    fractional_part = fractional_part * 10 + (c - '0');
                    decimal_places++;
                }
            }
        }

        // Determine scale based on the string input directly
        // If integer part is 0 and we have decimal places, use LOW_SCALE
        // Otherwise use HIGH_SCALE
        bool is_less_than_one = (integer_part == 0 && has_decimal);
        int64_t scale = is_less_than_one ? LOW_SCALE : HIGH_SCALE;

        // Calculate final value
        value_type final_value = integer_part * scale;

        if (decimal_places > 0) {
            // Scale fractional part appropriately
            int64_t fractional_scale = 1;
            for (int i = 0; i < decimal_places; ++i) {
                fractional_scale *= 10;
            }

            final_value += (fractional_part * scale) / fractional_scale;
        }

        if (negative) {
            final_value = -final_value;
        }

        return Price(final_value, scale);
    }

    [[nodiscard]] constexpr value_type raw_value() const noexcept {
        return value_;
    }
    [[nodiscard]] constexpr int64_t scale() const noexcept {
        return scale_;
    }

    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(value_) / scale_;
    }

    [[nodiscard]] std::string to_string() const {
        if (value_ == 0)
            return "0";

        bool negative = value_ < 0;
        value_type abs_value = negative ? -value_ : value_;

        value_type integer_part = abs_value / scale_;
        value_type fractional_part = abs_value % scale_;

        std::string result;
        if (negative)
            result += "-";

        // Convert __int128 to string manually
        result += int128_to_string(integer_part);

        if (fractional_part > 0) {
            result += ".";

            // Determine number of decimal places to show
            std::size_t decimal_places = (scale_ == HIGH_SCALE) ? 8 : 18;

            // Convert fractional part to string with leading zeros
            std::string frac_str = int128_to_string(fractional_part);

            // Pad with leading zeros if necessary
            while (frac_str.length() < decimal_places) {
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

    // Arithmetic operations
    constexpr Price operator+(const Price& other) const noexcept {
        auto [a_norm, b_norm] = normalize_for_operation(*this, other);
        int64_t result_scale = std::max(scale_, other.scale_);
        return Price(a_norm + b_norm, result_scale);
    }

    constexpr Price operator-(const Price& other) const noexcept {
        auto [a_norm, b_norm] = normalize_for_operation(*this, other);
        int64_t result_scale = std::max(scale_, other.scale_);
        return Price(a_norm - b_norm, result_scale);
    }

    constexpr Price operator*(int64_t multiplier) const noexcept {
        return Price(value_ * multiplier, scale_);
    }

    constexpr Price operator/(int64_t divisor) const noexcept {
        return Price(value_ / divisor, scale_);
    }

    // Comparison operators
    constexpr std::strong_ordering operator<=>(const Price& other) const noexcept {
        auto [a_norm, b_norm] = normalize_for_operation(*this, other);
        return a_norm <=> b_norm;
    }

    constexpr bool operator==(const Price& other) const noexcept {
        auto [a_norm, b_norm] = normalize_for_operation(*this, other);
        return a_norm == b_norm;
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
        return Price(0, HIGH_SCALE);
    }
    static constexpr Price max() noexcept {
        return Price(std::numeric_limits<value_type>::max(), HIGH_SCALE);
    }
    static constexpr Price min() noexcept {
        return Price(std::numeric_limits<value_type>::min(), HIGH_SCALE);
    }

};

// Quantity type for price level quantities (same precision requirements as Price)
using Quantity = Price;

// Concepts for type safety
template <typename T>
concept PriceType = std::same_as<T, Price>;

template <typename T>
concept QuantityType = std::same_as<T, Quantity>;

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