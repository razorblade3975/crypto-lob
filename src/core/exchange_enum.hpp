#pragma once

#include <cstdint>

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

}  // namespace crypto_lob::core