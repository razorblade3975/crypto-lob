#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "cache.hpp"
#include "exchange_enum.hpp"
#include "timestamp.hpp"

namespace crypto_lob::core {

/**
 * @brief Raw WebSocket message data
 *
 * POD struct for passing WebSocket data from ExchangeConnector to Parser.
 * Passed by value through SPSC queue as specified in architecture guide.
 *
 * Architecture alignment (Section 3 - Hot Path):
 * "The connector... enqueues the raw message into an SPSC ring buffer"
 *
 * Design decisions:
 * - Pass-by-value eliminates memory management complexity
 * - 64KB copy (~500ns) is negligible vs <50μs target
 * - Fixed-size buffer matches WebSocket frame maximum
 * - Cache-aligned for efficient copying
 */
struct alignas(CACHELINE_SIZE) RawMessage {
    // WebSocket frame data (max 64KB per WebSocket standard)
    static constexpr size_t MAX_FRAME_SIZE = 65536;
    std::array<char, MAX_FRAME_SIZE> data;

    // Metadata
    uint32_t size = 0;  // Actual data size in buffer
    ExchangeId exchange_id = ExchangeId::UNKNOWN;
    uint64_t sequence_number = 0;  // For ordering/gap detection
    uint64_t receive_timestamp;    // RDTSC timestamp at receipt

    // Default constructor
    RawMessage() = default;

    // Constructor from raw data
    RawMessage(const char* raw_data, uint32_t data_size, ExchangeId exchange) : size(data_size), exchange_id(exchange) {
        if (data_size > MAX_FRAME_SIZE) {
            size = MAX_FRAME_SIZE;  // Truncate if too large
        }
        std::memcpy(data.data(), raw_data, size);
    }

    // Get data as string_view for JSON parsing
    [[nodiscard]] std::string_view get_json() const noexcept {
        return std::string_view(data.data(), size);
    }

    // Check if message is valid
    [[nodiscard]] bool is_valid() const noexcept {
        return size > 0 && size <= MAX_FRAME_SIZE && exchange_id != ExchangeId::UNKNOWN;
    }
};

// Ensure reasonable size for passing by value
static_assert(sizeof(RawMessage) <= 65536 + 128, "RawMessage too large for efficient copying");

}  // namespace crypto_lob::core