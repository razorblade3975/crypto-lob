#pragma once

#include <cstdint>
#include <format>
#include <string>

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
    OKX_SPOT = 7,
};

// Market side
enum class Side : uint8_t { BUY = 0, SELL = 1 };

// Order book update type
enum class UpdateType : uint8_t { SNAPSHOT = 0, DELTA = 1 };

// Market event type
enum class EventType : uint8_t { TRADE = 0, TOP_OF_BOOK = 1, LEVEL_UPDATE = 2 };

// Instrument identifier
struct InstrumentId {
    ExchangeId exchange;
    std::string symbol;  // e.g., "BTCUSDT"

    constexpr auto operator<=>(const InstrumentId& other) const noexcept = default;
};

// Hash function for InstrumentId to use in unordered containers
struct InstrumentIdHash {
    std::size_t operator()(const InstrumentId& id) const noexcept {
        return std::hash<std::string>{}(id.symbol) ^ (static_cast<std::size_t>(id.exchange) << 1);
    }
};

}  // namespace crypto_lob::core

// Formatter for Side enum
template <>
struct std::formatter<crypto_lob::core::Side> {
    constexpr auto parse(std::format_parse_context& ctx) {
        return ctx.begin();
    }

    auto format(const crypto_lob::core::Side& side, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", side == crypto_lob::core::Side::BUY ? "BUY" : "SELL");
    }
};