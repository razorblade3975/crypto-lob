#pragma once

#include <array>
#include <cassert>
#include <cstddef>  // for std::size_t
#include <cstdint>
#include <cstring>  // for std::memcpy, std::strlen
#include <format>
#include <functional>  // for std::hash
#include <string>
#include <string_view>

namespace crypto_lob::core {

// Exchange identifier
enum class ExchangeId : uint8_t {
    BINANCE_SPOT = 0,
    BINANCE_FUT = 1,
    BYBIT_SPOT = 2,
    BYBIT_FUT = 3,
    KUCOIN_SPOT = 4,
    KUCOIN_FUT = 5,
    OKX_SPOT = 6,
    OKX_FUT = 7,
    UNKNOWN = 255  // Sentinel value for unspecialized parsers
};

// Market side
enum class Side : uint8_t { BUY = 0, SELL = 1 };

// Order book update type
enum class UpdateType : uint8_t { SNAPSHOT = 0, DELTA = 1 };

// Market event type
enum class EventType : uint8_t { TRADE = 0, TOP_OF_BOOK = 1, LEVEL_UPDATE = 2 };

// Instrument identifier - optimized for HFT with zero heap allocation
struct InstrumentId {
    ExchangeId exchange;                 // 1 byte
    std::array<char, 15> symbol_data{};  // 15 bytes - null-terminated C-string

    // Default constructor
    constexpr InstrumentId() noexcept = default;

    // Constructor from exchange and C-string literal
    template <std::size_t N>
    constexpr InstrumentId(ExchangeId ex, const char (&sym)[N]) noexcept : exchange(ex), symbol_data{} {
        static_assert(N <= 16, "Symbol too long (max 15 chars)");
        for (std::size_t i = 0; i < N - 1 && i < 14; ++i) {
            symbol_data[i] = sym[i];
        }
    }

    // Constructor from exchange and string_view
    constexpr InstrumentId(ExchangeId ex, std::string_view sym) noexcept : exchange(ex), symbol_data{} {
        set_symbol(sym);
    }

    // Constructor from exchange and std::string (for compatibility)
    InstrumentId(ExchangeId ex, const std::string& sym) noexcept : exchange(ex), symbol_data{} {
        set_symbol(sym);
    }

    // Set symbol from string_view (safe with bounds checking)
    constexpr void set_symbol(std::string_view sv) noexcept {
        std::size_t len = sv.length() < 14 ? sv.length() : 14;
        for (std::size_t i = 0; i < len; ++i) {
            symbol_data[i] = sv[i];
        }
        symbol_data[len] = '\0';
    }

    // Get symbol as string_view
    [[nodiscard]] constexpr std::string_view symbol() const noexcept {
        // Find null terminator
        std::size_t len = 0;
        while (len < 15 && symbol_data[len] != '\0') {
            ++len;
        }
        return std::string_view(symbol_data.data(), len);
    }

    // Get symbol as C-string
    [[nodiscard]] constexpr const char* c_str() const noexcept {
        return symbol_data.data();
    }

    // Comparison operators
    [[nodiscard]] constexpr bool operator==(const InstrumentId& other) const noexcept {
        if (exchange != other.exchange)
            return false;
        // Compare symbol data up to first null terminator
        for (std::size_t i = 0; i < 15; ++i) {
            if (symbol_data[i] != other.symbol_data[i])
                return false;
            if (symbol_data[i] == '\0')
                break;
        }
        return true;
    }

    [[nodiscard]] constexpr auto operator<=>(const InstrumentId& other) const noexcept {
        if (auto cmp = exchange <=> other.exchange; cmp != 0)
            return cmp;
        // Compare symbol strings
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

// Hash function for InstrumentId to use in unordered containers
struct InstrumentIdHash {
    std::size_t operator()(const InstrumentId& inst_id) const noexcept {
        // Use std::hash<string_view> which is already optimized
        return std::hash<std::string_view>{}(inst_id.symbol()) ^ (static_cast<std::size_t>(inst_id.exchange) << 1);
    }
};

}  // namespace crypto_lob::core

// Formatter for Side enum
template <>
struct std::formatter<crypto_lob::core::Side> {
    static constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const crypto_lob::core::Side& side, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", side == crypto_lob::core::Side::BUY ? "BUY" : "SELL");
    }
};