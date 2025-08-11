#pragma once

#include <cstdint>
#include <variant>

#include "exchange_enum.hpp"
#include "instrument.hpp"
#include "price.hpp"
#include "timestamp.hpp"

namespace crypto_lob::core {

// Message types
enum class MessageType : uint8_t { SNAPSHOT = 0, DELTA = 1, TRADE = 2, UNKNOWN = 255 };

// Price level for order book
struct PriceLevel {
    Price price;
    uint64_t quantity;  // Scaled by 10^8 for precision

    constexpr PriceLevel() noexcept = default;
    constexpr PriceLevel(Price p, uint64_t q) noexcept : price(p), quantity(q) {}
};

// Snapshot message containing full order book state
struct SnapshotMessage {
    static constexpr size_t MAX_LEVELS = 20;

    uint64_t update_id;
    PriceLevel bids[MAX_LEVELS];
    PriceLevel asks[MAX_LEVELS];
    size_t bid_count;
    size_t ask_count;

    constexpr SnapshotMessage() noexcept : update_id(0), bids{}, asks{}, bid_count(0), ask_count(0) {}
};

// Delta message containing incremental updates
struct DeltaMessage {
    static constexpr size_t MAX_UPDATES = 100;

    uint64_t update_id;
    uint64_t first_update_id;
    uint64_t final_update_id;

    PriceLevel bid_updates[MAX_UPDATES];
    PriceLevel ask_updates[MAX_UPDATES];
    size_t bid_update_count;
    size_t ask_update_count;

    constexpr DeltaMessage() noexcept
        : update_id(0),
          first_update_id(0),
          final_update_id(0),
          bid_updates{},
          ask_updates{},
          bid_update_count(0),
          ask_update_count(0) {}
};

// Trade message
struct TradeMessage {
    uint64_t trade_id;
    Price price;
    uint64_t quantity;   // Scaled by 10^8
    uint64_t timestamp;  // TSC or epoch timestamp
    bool is_buyer_maker;

    constexpr TradeMessage() noexcept : trade_id(0), price{}, quantity(0), timestamp{}, is_buyer_maker(false) {}
};

// Normalized message - common format for all exchanges
struct alignas(64) NormalizedMessage {  // Cache-aligned for memory pool
    MessageType type;
    ExchangeId exchange_id;
    InstrumentId instrument;
    uint64_t receive_time;  // TSC timestamp at receipt

    // Use variant to hold different message types
    std::variant<SnapshotMessage, DeltaMessage, TradeMessage> data;

    constexpr NormalizedMessage() noexcept
        : type(MessageType::UNKNOWN), exchange_id(ExchangeId::UNKNOWN), instrument{}, receive_time{}, data{} {}
};

}  // namespace crypto_lob::core