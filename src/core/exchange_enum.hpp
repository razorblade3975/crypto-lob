#pragma once

#include <cstdint>

namespace crypto_lob::core {

// Exchange identifier
// Following the architecture guide, starting with Binance Spot only
// Additional exchanges can be added as the system evolves
enum class ExchangeId : uint8_t {
    BINANCE_SPOT = 0,
    UNKNOWN = 255  // Sentinel value for unspecialized parsers
};

}  // namespace crypto_lob::core