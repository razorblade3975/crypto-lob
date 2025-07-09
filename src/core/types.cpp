#include "types.hpp"
#include <chrono>

namespace crypto_lob::core {

Timestamp current_timestamp() noexcept {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

}  // namespace crypto_lob::core