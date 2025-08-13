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

// Price level for order book messages
struct MessagePriceLevel {
    Price price;
    Quantity quantity;  // Fixed-point representation with 9 decimal places

    constexpr MessagePriceLevel() noexcept = default;
    constexpr MessagePriceLevel(Price p, Quantity q) noexcept : price(p), quantity(q) {}
};

// Snapshot message containing full order book state
struct SnapshotMessage {
    static constexpr size_t MAX_LEVELS = 20;

    uint64_t last_update_id;
    MessagePriceLevel bids[MAX_LEVELS];
    MessagePriceLevel asks[MAX_LEVELS];
    size_t bid_count;
    size_t ask_count;

    constexpr SnapshotMessage() noexcept : last_update_id(0), bids{}, asks{}, bid_count(0), ask_count(0) {}
};

// Delta message containing incremental updates
struct DeltaMessage {
    static constexpr size_t MAX_UPDATES = 100;

    uint64_t first_update_id;
    uint64_t last_update_id;

    MessagePriceLevel bid_updates[MAX_UPDATES];
    MessagePriceLevel ask_updates[MAX_UPDATES];
    size_t bid_update_count;
    size_t ask_update_count;

    constexpr DeltaMessage() noexcept
        : first_update_id(0),
          last_update_id(0),
          bid_updates{},
          ask_updates{},
          bid_update_count(0),
          ask_update_count(0) {}
};

// Trade message
struct TradeMessage {
    uint64_t trade_id;
    Price price;
    Quantity quantity;   // Fixed-point representation with 9 decimal places
    uint64_t timestamp;  // TSC or epoch timestamp
    bool is_buyer_maker;

    constexpr TradeMessage() noexcept : trade_id(0), price{}, quantity{}, timestamp{}, is_buyer_maker(false) {}
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