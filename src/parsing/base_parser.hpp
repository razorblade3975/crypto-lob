#pragma once

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <simdjson.h>
#include <string_view>
#include <type_traits>

#include "../core/memory_pool.hpp"
#include "../exchange/message_types.hpp"

namespace crypto_lob::parsing {

enum class ParseError : uint8_t {
    INVALID_JSON,
    MISSING_REQUIRED_FIELD,
    INVALID_FIELD_TYPE,
    INVALID_PRICE_FORMAT,
    INVALID_QUANTITY_FORMAT,
    INVALID_SEQUENCE_NUMBER,
    BUFFER_TOO_SMALL,
    UNSUPPORTED_MESSAGE_TYPE,
    EXCHANGE_SPECIFIC_ERROR,
    NUMERIC_OVERFLOW,
    NULL_FIELD
};

enum class ChecksumResult : uint8_t { VALID, INVALID, NOT_SUPPORTED };

// Minimal error info for HFT - only used on failure path
struct ParseErrorInfo {
    ParseError error = ParseError::INVALID_JSON;

    // Don't store details to minimize size - use error enum for specificity
    constexpr ParseErrorInfo() noexcept = default;
    constexpr ParseErrorInfo(ParseError err) noexcept : error(err) {}
};

// HFT-optimized: Use raw pointers with thread-local pool for automatic RAII
// Memory cleanup handled by pool's thread-local cache design

// Forward declaration for CRTP
template <typename Derived>
class ParserBase;

// Parser traits for compile-time configuration - MUST be specialized
template <typename T>
struct ParserTraits {
    // Force explicit specialization - no default exchange
    static constexpr exchange::ExchangeId exchange_id = exchange::ExchangeId::UNKNOWN;
    static constexpr bool supports_checksum = false;
};

// Helper for static_assert in templates
template <typename T>
inline constexpr bool always_false_v = false;

// CRTP base class for static polymorphism - HFT optimized
template <typename Derived>
class ParserBase {
  public:
    using MessagePtr = exchange::MarketDataMessage*;
    using Timestamp = std::chrono::nanoseconds;
    using Pool = core::MemoryPool<exchange::MarketDataMessage>;

    explicit ParserBase(Pool& message_pool) noexcept : message_pool_(message_pool) {}

    ~ParserBase() = default;

    // Enable move operations for container storage
    ParserBase(ParserBase&&) = default;
    ParserBase& operator=(ParserBase&&) = default;

    // Disable copy operations
    ParserBase(const ParserBase&) = delete;
    ParserBase& operator=(const ParserBase&) = delete;

    // Zero-overhead success path: return raw pointer, nullptr on error
    [[nodiscard]] MessagePtr parse_snapshot(std::string_view json_data,
                                            const std::string& symbol,
                                            Timestamp receive_time,
                                            ParseErrorInfo* error_out = nullptr) noexcept {
        return static_cast<Derived*>(this)->parse_snapshot_impl(json_data, symbol, receive_time, error_out);
    }

    [[nodiscard]] MessagePtr parse_update(std::string_view json_data,
                                          const std::string& symbol,
                                          Timestamp receive_time,
                                          ParseErrorInfo* error_out = nullptr) noexcept {
        return static_cast<Derived*>(this)->parse_update_impl(json_data, symbol, receive_time, error_out);
    }

    [[nodiscard]] static constexpr exchange::ExchangeId get_exchange_id() noexcept {
        static_assert(ParserTraits<Derived>::exchange_id != exchange::ExchangeId::UNKNOWN,
                      "ParserTraits must be explicitly specialized for each parser type");
        return ParserTraits<Derived>::exchange_id;
    }

    [[nodiscard]] static constexpr bool supports_checksum() noexcept {
        return ParserTraits<Derived>::supports_checksum;
    }

    // Return bool for success/failure, error details in out-parameter
    [[nodiscard]] bool verify_checksum(const exchange::MarketDataMessage& msg,
                                       std::string_view checksum_str,
                                       ChecksumResult& result_out,
                                       ParseErrorInfo* error_out = nullptr) const noexcept {
        if constexpr (ParserTraits<Derived>::supports_checksum) {
            return static_cast<const Derived*>(this)->verify_checksum_impl(msg, checksum_str, result_out, error_out);
        } else {
            result_out = ChecksumResult::NOT_SUPPORTED;
            return false;
        }
    }

  protected:
    // HFT-optimized: Lazy thread-local parser to avoid cold-start penalty after CPU migration
    static simdjson::ondemand::parser& get_parser() noexcept {
        thread_local static simdjson::ondemand::parser parser;
        return parser;
    }

    Pool& message_pool_;

    // RAII wrapper for safe message allocation
    class MessageGuard {
      public:
        MessageGuard(Pool& pool) noexcept : pool_(pool), ptr_(pool.construct()) {}
        ~MessageGuard() {
            if (ptr_)
                pool_.destroy(ptr_);
        }

        MessageGuard(const MessageGuard&) = delete;
        MessageGuard& operator=(const MessageGuard&) = delete;
        MessageGuard(MessageGuard&& other) noexcept : pool_(other.pool_), ptr_(other.ptr_) {
            other.ptr_ = nullptr;
        }
        MessageGuard& operator=(MessageGuard&& other) noexcept {
            if (this != &other) {
                if (ptr_)
                    pool_.destroy(ptr_);
                ptr_ = other.ptr_;
                other.ptr_ = nullptr;
            }
            return *this;
        }

        MessagePtr get() noexcept {
            return ptr_;
        }
        MessagePtr release() noexcept {
            auto* tmp = ptr_;
            ptr_ = nullptr;
            return tmp;
        }
        explicit operator bool() const noexcept {
            return ptr_ != nullptr;
        }

      private:
        Pool& pool_;
        MessagePtr ptr_;
    };

    // Leak-safe allocation - returns RAII guard
    [[nodiscard]] MessageGuard allocate_message() noexcept {
        return MessageGuard(message_pool_);
    }

    // HFT-optimized error reporting - populate out-parameter only
    static void set_error(ParseErrorInfo* error_out, ParseError error) noexcept {
        if (error_out) {
            error_out->error = error;
        }
    }

    // Fast numeric parsing using simdjson - HFT optimized
    template <typename T>
    [[nodiscard]] bool parse_number(simdjson::ondemand::value& val,
                                    T& result_out,
                                    ParseErrorInfo* error_out = nullptr) const noexcept {
        if constexpr (std::is_same_v<T, uint64_t>) {
            auto result = val.get_uint64();
            if (result.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            result_out = result.value();
            return true;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            auto result = val.get_int64();
            if (result.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            result_out = result.value();
            return true;
        } else if constexpr (std::is_same_v<T, double>) {
            auto result = val.get_double();
            if (result.error()) {
                set_error(error_out, ParseError::INVALID_FIELD_TYPE);
                return false;
            }
            result_out = result.value();
            return true;
        } else {
            static_assert(always_false_v<T>, "Unsupported parse_number type");
            return false;  // Unreachable, but satisfies compiler
        }
    }

    // Parse price from string directly (preserves precision)
    [[nodiscard]] bool parse_price_string(std::string_view price_str,
                                          core::Price& result,
                                          ParseErrorInfo* error_out = nullptr) const noexcept {
        // Validate input before parsing
        if (price_str.empty()) {
            set_error(error_out, ParseError::INVALID_PRICE_FORMAT);
            return false;
        }

        // Reject negative prices - prices cannot be negative in market data
        if (price_str.front() == '-') {
            set_error(error_out, ParseError::INVALID_PRICE_FORMAT);
            return false;
        }

        // Check for invalid characters beyond digits and decimal point
        for (char c : price_str) {
            if (c != '.' && (c < '0' || c > '9')) {
                set_error(error_out, ParseError::INVALID_PRICE_FORMAT);
                return false;
            }
        }

        result = core::Price::from_string(price_str);

        // Reject zero prices as they indicate parsing failure or invalid data
        // Zero prices in order books are dangerous and usually indicate bad data
        if (result == core::Price::zero()) {
            // Only allow explicit "0" or "0.0" strings
            if (price_str != "0" && price_str != "0.0" && price_str != "0.00") {
                set_error(error_out, ParseError::INVALID_PRICE_FORMAT);
                return false;
            }
        }

        return true;
    }

    // Parse price from simdjson value - HFT precision-safe (string or uint64 only)
    [[nodiscard]] bool parse_price(simdjson::ondemand::value& val,
                                   core::Price& result_out,
                                   ParseErrorInfo* error_out = nullptr) const noexcept {
        // Try string first (most common in crypto exchanges)
        auto str_result = val.get_string();
        if (!str_result.error()) {
            return parse_price_string(str_result.value(), result_out, error_out);
        }

        // Try uint64 for pre-scaled integer prices
        auto uint_result = val.get_uint64();
        if (!uint_result.error()) {
            // Create Price from raw value (assumes proper scaling)
            result_out = core::Price(static_cast<int64_t>(uint_result.value()));
            return true;
        }

        // REJECT floating-point tokens - precision loss unacceptable in HFT
        // This prevents rounding arbitrage and ensures exact pricing
        set_error(error_out, ParseError::INVALID_PRICE_FORMAT);
        return false;
    }

    // Parse quantity as uint64_t - HFT precision-safe (string or uint64 only)
    [[nodiscard]] bool parse_quantity_raw(simdjson::ondemand::value& val,
                                          uint64_t& result_out,
                                          ParseErrorInfo* error_out = nullptr) const noexcept {
        // Try string first (decimal string parsing by derived class)
        auto str_result = val.get_string();
        if (!str_result.error()) {
            // Derived class implements string to scaled uint64_t conversion
            return static_cast<const Derived*>(this)->parse_quantity_string(str_result.value(), result_out, error_out);
        }

        // Try direct uint64 (already exact)
        auto uint_result = val.get_uint64();
        if (!uint_result.error()) {
            result_out = uint_result.value();
            return true;
        }

        // REJECT int64 and floating-point to avoid casting/precision issues
        // Large quantities (>2^63) or FP values could cause UB or precision loss
        set_error(error_out, ParseError::INVALID_QUANTITY_FORMAT);
        return false;
    }
};

// Interface conformance checks - evaluated after derived class is complete
template <typename Derived>
struct ParserInterfaceCheck {
    static_assert(std::is_same_v<typename ParserBase<Derived>::MessagePtr,
                                 decltype(std::declval<Derived>().parse_snapshot_impl(
                                     std::declval<std::string_view>(),
                                     std::declval<const std::string&>(),
                                     std::declval<typename ParserBase<Derived>::Timestamp>(),
                                     std::declval<ParseErrorInfo*>()))>,
                  "Derived class must implement parse_snapshot_impl with correct signature");

    static_assert(std::is_same_v<typename ParserBase<Derived>::MessagePtr,
                                 decltype(std::declval<Derived>().parse_update_impl(
                                     std::declval<std::string_view>(),
                                     std::declval<const std::string&>(),
                                     std::declval<typename ParserBase<Derived>::Timestamp>(),
                                     std::declval<ParseErrorInfo*>()))>,
                  "Derived class must implement parse_update_impl with correct signature");

    static_assert(
        std::is_same_v<bool,
                       decltype(std::declval<const Derived>().parse_quantity_string(std::declval<std::string_view>(),
                                                                                    std::declval<uint64_t&>(),
                                                                                    std::declval<ParseErrorInfo*>()))>,
        "Derived class must implement parse_quantity_string with correct signature");
};

// Parser is now accessed via get_parser() function for HFT optimization

}  // namespace crypto_lob::parsing