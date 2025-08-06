#pragma once

/**
 * @file message_types.hpp
 * @brief Core message type definitions for multi-exchange market data processing
 *
 * This file defines the fundamental message structures for high-frequency trading
 * market data processing across multiple cryptocurrency exchanges. The design follows
 * an exchange-agnostic architecture where common message structures are defined here
 * and exchange-specific adapters transform native exchange formats into these
 * standardized types.
 *
 * Architecture Overview:
 * - Exchange-agnostic core types that work across all supported exchanges
 * - Exchange-specific adapters convert native formats to these common structures
 * - Fixed-size structures with cache-line alignment for optimal performance
 * - Zero-copy design patterns throughout for minimal latency
 * - Support for both snapshot and delta synchronization patterns
 *
 * Memory Layout Optimization:
 * - All message structures are cache-line aligned (64-byte boundaries)
 * - Hot data (frequently accessed fields) packed in first cache line
 * - Cold data (rarely accessed fields) placed in subsequent cache lines
 * - Fixed-size arrays to avoid heap allocation and fragmentation
 * - Careful padding to prevent false sharing between threads
 *
 * Thread Safety Model:
 * - Message structures are designed for single-writer, multiple-reader patterns
 * - No internal locks or atomic operations for maximum performance
 * - Immutable once constructed and published to consumers
 * - Memory pools handle allocation/deallocation thread safety externally
 *
 * Integration with Order Book:
 * - PriceLevel format matches orderbook/price_level.hpp for zero-copy updates
 * - Sequence tracking enables gap detection and recovery across exchanges
 * - Supports both full snapshots and incremental delta updates
 * - Built-in continuity checking for maintaining order book integrity
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <span>

#include "core/cache.hpp"
#include "core/enums.hpp"
#include "core/price.hpp"

namespace crypto_lob::exchanges::base {

// Import core types into exchange namespace for convenience
using crypto_lob::core::ExchangeId;  ///< Exchange identifier enum
using crypto_lob::core::Price;       ///< Fixed-point price representation
using crypto_lob::core::Side;        ///< Order/trade side (BUY/SELL)

/**
 * @brief Common timestamp type - nanoseconds since epoch
 *
 * All timestamps in the system use nanosecond precision to support high-frequency
 * trading requirements. Exchange timestamps are converted to this format during
 * message parsing to ensure consistent time handling across all exchanges.
 */
using Timestamp = std::chrono::nanoseconds;

/**
 * @brief Convert milliseconds to nanoseconds with overflow protection
 * @param millis Millisecond timestamp from exchange (typically Unix epoch)
 * @return Nanosecond timestamp suitable for internal use
 *
 * This function safely converts exchange timestamps (usually in milliseconds)
 * to our internal nanosecond representation. Uses std::chrono for overflow
 * protection and type safety. Most exchanges provide millisecond precision
 * timestamps which are converted to nanoseconds for consistency with
 * high-resolution local timestamps.
 */
constexpr Timestamp timestamp_from_millis(int64_t millis) noexcept {
    // Use chrono for overflow-safe conversion
    return std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds{millis});
}

/**
 * @brief Price level representation for order book data
 *
 * Fixed-size price level structure that matches the format used in
 * orderbook/price_level.hpp for zero-copy compatibility. This struct
 * is designed for optimal memory layout and cache efficiency.
 *
 * Memory Layout:
 * - 16 bytes total (8 bytes price + 8 bytes quantity)
 * - Natural alignment on 8-byte boundaries
 * - No virtual functions or vtable overhead
 * - Trivially copyable for efficient memcpy operations
 *
 * Scaling Considerations:
 * - Price uses fixed-point representation for deterministic calculations
 * - Quantity stored as raw uint64_t to avoid scaling overhead during updates
 * - Supports up to 18 decimal places of precision for cryptocurrencies
 * - Compatible with exchange-specific quantity formats through scaling
 */
struct PriceLevel {
    Price price;        ///< Fixed-point price representation
    uint64_t quantity;  ///< Raw quantity, not Price type to avoid extra conversions

    /// Default constructor - zero price and quantity
    constexpr PriceLevel() noexcept : price(Price::zero()), quantity(0) {}

    /// Constructor with explicit price and quantity
    constexpr PriceLevel(Price p, uint64_t q) noexcept : price(p), quantity(q) {}

    /// Equality comparison for testing and deduplication
    [[nodiscard]] constexpr bool operator==(const PriceLevel& other) const noexcept {
        return price == other.price && quantity == other.quantity;
    }
};

/**
 * @brief Message type enumeration for fast dispatch and type safety
 *
 * Single-byte enum for efficient storage and fast switching. Values are
 * explicitly assigned to ensure ABI stability across compilations.
 * The order is optimized for common message frequency (snapshots and deltas first).
 */
enum class MessageType : uint8_t {
    SNAPSHOT = 0,           ///< Full order book snapshot
    DELTA = 1,              ///< Incremental order book update
    TRADE = 2,              ///< Executed trade information
    TICKER = 3,             ///< 24h ticker/summary statistics
    HEARTBEAT = 4,          ///< Connection keepalive message
    CONNECTION_STATUS = 5,  ///< Connection state changes
    SUBSCRIPTION_ACK = 6    ///< Subscription confirmation/error
};

/**
 * @brief Maximum price levels supported in a single message
 *
 * This constant defines the upper bound for price levels in snapshot and delta
 * messages. The value 512 covers all known exchange WebSocket message formats:
 * - Binance: up to 1000 levels (we limit to 512 for memory efficiency)
 * - OKX: up to 400 levels
 * - KuCoin: up to 200 levels
 * - Bybit: up to 500 levels
 *
 * Using a fixed size eliminates heap allocations and provides predictable
 * memory usage patterns essential for low-latency trading applications.
 */
static constexpr size_t MAX_LEVELS_PER_MESSAGE = 512;

/**
 * @brief Common message header for all message types
 *
 * Cache line aligned header structure that contains metadata common to all
 * message types. The layout is carefully optimized for cache efficiency with
 * frequently accessed fields (hot data) packed in the first 32 bytes.
 *
 * Cache Line Alignment Strategy:
 * - 64-byte alignment ensures header fits in exactly one cache line
 * - Hot data (frequently accessed fields) in first 32 bytes
 * - Cold data (rarely accessed fields) in remaining 32 bytes
 * - Prevents false sharing when multiple threads access different messages
 *
 * Sequence Handling Across Exchanges:
 * - Internal sequence_number: monotonic counter for our message ordering
 * - Exchange sequence_number: native exchange sequence for gap detection
 * - Dual sequence tracking enables both internal ordering and exchange sync
 * - Critical for maintaining order book integrity during reconnections
 */
struct alignas(64) MessageHeader {
    // Hot data - frequently accessed fields packed in first 32 bytes
    MessageType type;              ///< Message type for dispatch (1 byte)
    ExchangeId exchange_id;        ///< Source exchange identifier (1 byte)
    uint16_t instrument_index;     ///< Index into instrument lookup table (2 bytes)
    uint32_t sequence_number;      ///< Internal monotonic sequence (4 bytes)
    Timestamp exchange_timestamp;  ///< Exchange-provided timestamp (8 bytes)
    Timestamp local_timestamp;     ///< Local receipt timestamp (8 bytes)
    uint64_t exchange_sequence;    ///< Exchange's native sequence/update_id (8 bytes)

    // Cold data - rarely accessed fields in second half of cache line
    uint32_t checksum;       ///< CRC32 checksum for some exchanges (4 bytes)
    uint16_t bid_count;      ///< Number of valid bid levels in message (2 bytes)
    uint16_t ask_count;      ///< Number of valid ask levels in message (2 bytes)
    uint8_t flags;           ///< Message flags (see MessageFlags enum) (1 byte)
    uint8_t reserved[23]{};  ///< Padding to 64 bytes - zero-initialized

    /// Default constructor with sensible defaults
    constexpr MessageHeader() noexcept
        : type(MessageType::SNAPSHOT),
          exchange_id(ExchangeId::BINANCE_SPOT),
          instrument_index(0),
          sequence_number(0),
          exchange_timestamp(Timestamp{0}),
          local_timestamp(Timestamp{0}),
          exchange_sequence(0),
          checksum(0),
          bid_count(0),
          ask_count(0),
          flags(0),
          reserved{} {}
};

/// Ensure MessageHeader is exactly one cache line for optimal performance
static_assert(sizeof(MessageHeader) == 64, "MessageHeader must be cache-line sized");
/// Ensure MessageHeader is aligned to cache line boundaries to prevent false sharing
static_assert(alignof(MessageHeader) == 64, "MessageHeader must be cache-aligned");

/**
 * @brief Flags for MessageHeader.flags field
 *
 * Bit flags that provide additional context about the message content.
 * Uses explicit bit shifting for clarity and to prevent accidental overlap.
 * Single byte storage allows for up to 8 different flags.
 */
enum MessageFlags : uint8_t {
    FLAG_NONE = 0,                 ///< No special flags set
    FLAG_TICK_BY_TICK = 1 << 0,    ///< OKX tick-by-tick granular update
    FLAG_HAS_CHECKSUM = 1 << 1,    ///< Checksum field contains valid data
    FLAG_SEQUENCE_RESET = 1 << 2,  ///< Exchange sequence number was reset
    FLAG_SNAPSHOT_SYNC = 1 << 3,   ///< Part of initial snapshot synchronization
    FLAG_COMPRESSED = 1 << 4,      ///< Message data is compressed (future use)
};

/**
 * @brief Exchange-specific sequence tracking information
 *
 * Different exchanges use varying sequence number schemes for gap detection
 * and message ordering. This struct accommodates all known patterns while
 * maintaining cache-line alignment for performance.
 *
 * Exchange-Specific Sequence Requirements:
 * - Binance/Gate.io: Use first_update_id (U) and last_update_id (u) range
 * - KuCoin: Use sequence_start and sequence_end for incremental ranges
 * - OKX: Use previous_update_id and current sequence for linked updates
 * - Bybit/Bitget: Use single monotonic sequence_number
 *
 * Gap Detection Strategy:
 * - Each exchange defines continuity rules in is_continuous_with()
 * - Supports both range-based (U/u) and increment-based sequences
 * - Handles sequence resets and synchronization scenarios
 * - Critical for maintaining order book integrity during reconnections
 */
struct alignas(64) SequenceInfo {
    // Binance/Gate.io style: first_update_id (U) and last_update_id (u)
    uint64_t first_update_id;  ///< Starting sequence ID for range-based updates
    uint64_t last_update_id;   ///< Ending sequence ID for range-based updates

    // KuCoin style: sequence_start and sequence_end
    uint64_t sequence_start;  ///< KuCoin sequence range start
    uint64_t sequence_end;    ///< KuCoin sequence range end

    // OKX style: previous_update_id for linked updates
    uint64_t previous_update_id;  ///< Previous update ID for chain validation

    // Single sequence number (Bitget, Bybit)
    uint64_t sequence_number;  ///< Monotonic sequence number

    // Reserved for future exchange sequence schemes - zero-initialized
    uint64_t reserved[2]{};

    /// Default constructor - all sequences start at zero
    constexpr SequenceInfo() noexcept
        : first_update_id(0),
          last_update_id(0),
          sequence_start(0),
          sequence_end(0),
          previous_update_id(0),
          sequence_number(0),
          reserved{} {}

    /**
     * @brief Set sequence values based on exchange-specific requirements
     * @param exchange Exchange identifier to determine sequence format
     * @param val1 Primary sequence value (meaning varies by exchange)
     * @param val2 Secondary sequence value (optional, defaults to 0)
     *
     * Exchange-Specific Configuration Requirements:
     * - Binance: val1=first_update_id (U), val2=last_update_id (u)
     * - KuCoin: val1=sequence_start, val2=sequence_end
     * - OKX: val1=previous_update_id (prevSeqId), val2=current sequence_number
     * - Bybit/Bitget: val1=sequence_number, val2 ignored
     *
     * This method clears all fields first to prevent cross-contamination
     * between different exchange sequence formats. Uses std::terminate()
     * for unknown exchanges to fail fast rather than silently corrupt data.
     */
    void set_for_exchange(ExchangeId exchange, uint64_t val1, uint64_t val2 = 0) noexcept {
        // Clear all fields first to prevent stale data
        *this = SequenceInfo{};

        switch (exchange) {
            case ExchangeId::BINANCE_SPOT:
            case ExchangeId::BINANCE_FUT:
                first_update_id = val1;
                last_update_id = val2 ? val2 : val1;
                break;
            case ExchangeId::KUCOIN_SPOT:
            case ExchangeId::KUCOIN_FUT:
                sequence_start = val1;
                sequence_end = val2 ? val2 : val1;
                break;
            case ExchangeId::OKX_SPOT:
            case ExchangeId::OKX_FUT:
                previous_update_id = val1;
                sequence_number = val2;
                break;
            case ExchangeId::BYBIT_SPOT:
            case ExchangeId::BYBIT_FUT:
                sequence_number = val1;
                break;
            default:
                // Unknown exchange - fail fast in debug builds
                std::terminate();
        }
    }
};

/// Ensure SequenceInfo is exactly one cache line for optimal performance
static_assert(sizeof(SequenceInfo) == 64, "SequenceInfo must be cache-line sized");
/// Ensure SequenceInfo is aligned to cache line boundaries
static_assert(alignof(SequenceInfo) == 64, "SequenceInfo must be cache-aligned");

/**
 * @brief Primary market data message containing order book snapshots and deltas
 *
 * This is the core message type for order book data processing. It accommodates
 * both full snapshots and incremental delta updates using a unified format.
 * The design prioritizes zero-copy access patterns and predictable memory usage.
 *
 * Design Rationale for Fixed Arrays:
 * - Eliminates heap allocations completely (critical for low latency)
 * - Provides predictable memory usage patterns for pool allocation
 * - Enables zero-copy access to price level data
 * - Supports worst-case message sizes from all exchanges
 * - Memory overhead is acceptable given performance benefits
 *
 * Performance Implications:
 * - Large structure size (~16KB) requires careful memory pool management
 * - Cache-friendly layout with header and sequence info in first cache lines
 * - Fixed arrays enable SIMD operations on price level processing
 * - Move-only semantics prevent expensive accidental copies
 * - Alignment ensures no false sharing between message instances
 *
 * Usage Patterns:
 * - Snapshots: full order book state with all price levels
 * - Deltas: incremental updates with only changed levels
 * - Zero quantities indicate price level deletions in deltas
 * - Thread-safe for single-writer, multiple-reader scenarios
 */
struct alignas(64) MarketDataMessage {
    MessageHeader header;   ///< Common header with timestamps and metadata
    SequenceInfo sequence;  ///< Exchange-specific sequence tracking

    // Fixed arrays for price levels - only header.bid_count/ask_count entries are valid
    /// Bid price levels - sorted highest to lowest price
    std::array<PriceLevel, MAX_LEVELS_PER_MESSAGE> bids;
    /// Ask price levels - sorted lowest to highest price
    std::array<PriceLevel, MAX_LEVELS_PER_MESSAGE> asks;

    // Prevent accidental copies of this large structure (~16KB)
    MarketDataMessage(const MarketDataMessage&) = delete;
    MarketDataMessage& operator=(const MarketDataMessage&) = delete;

    /// Default constructor is allowed for pool allocation
    MarketDataMessage() = default;

    /// Move operations are allowed for efficient pool management
    MarketDataMessage(MarketDataMessage&&) = default;
    MarketDataMessage& operator=(MarketDataMessage&&) = default;

    /// Check if this message contains a full order book snapshot
    [[nodiscard]] bool is_snapshot() const noexcept {
        return header.type == MessageType::SNAPSHOT;
    }

    /// Check if this message contains incremental order book updates
    [[nodiscard]] bool is_delta() const noexcept {
        return header.type == MessageType::DELTA;
    }

    /**
     * @brief Get valid bid levels with bounds checking
     * @return Span of valid bid price levels (highest to lowest price)
     *
     * Memory Layout and Zero-Copy Access Patterns:
     * - Returns std::span for zero-copy access to internal array
     * - Bounds checking prevents buffer overruns from malformed messages
     * - Span size limited by min(header.bid_count, array capacity)
     * - Direct pointer access to cache-aligned memory for optimal performance
     * - Suitable for range-based loops and SIMD operations
     */
    [[nodiscard]] std::span<const PriceLevel> get_bids() const noexcept {
        const size_t count = std::min(static_cast<size_t>(header.bid_count), bids.size());
        return std::span<const PriceLevel>(bids.data(), count);
    }

    /**
     * @brief Get valid ask levels with bounds checking
     * @return Span of valid ask price levels (lowest to highest price)
     *
     * Memory Layout and Zero-Copy Access Patterns:
     * - Returns std::span for zero-copy access to internal array
     * - Bounds checking prevents buffer overruns from malformed messages
     * - Span size limited by min(header.ask_count, array capacity)
     * - Direct pointer access to cache-aligned memory for optimal performance
     * - Suitable for range-based loops and SIMD operations
     */
    [[nodiscard]] std::span<const PriceLevel> get_asks() const noexcept {
        const size_t count = std::min(static_cast<size_t>(header.ask_count), asks.size());
        return std::span<const PriceLevel>(asks.data(), count);
    }

    /**
     * @brief Check message sequence continuity with previous message
     * @param prev Previous message to check continuity against
     * @return true if this message is sequential with prev, false otherwise
     *
     * Sequence Continuity Algorithm for Each Exchange:
     *
     * Binance (Range-based):
     * - Current first_update_id must equal prev.last_update_id + 1
     * - Ensures no gaps in the range-based update sequence
     * - Critical for maintaining order book integrity
     *
     * KuCoin (Strict Increment):
     * - Current sequence_start must equal prev.sequence_end + 1
     * - No overlapping ranges allowed (strict increment requirement)
     * - Detects both gaps and duplicate/overlapping updates
     *
     * OKX (Linked Updates):
     * - Current previous_update_id must equal prev.sequence_number
     * - Creates a linked chain of updates for gap detection
     * - Supports tick-by-tick granular update tracking
     *
     * Bybit/Bitget (Monotonic):
     * - Current sequence_number must equal prev.sequence_number + 1
     * - Simple monotonic increment check
     * - Most straightforward continuity validation
     *
     * This method is critical for order book synchronization and recovery.
     * Gaps trigger snapshot requests to resync with exchange state.
     */
    [[nodiscard]] bool is_continuous_with(const MarketDataMessage& prev) const noexcept {
        // Messages must be from same exchange to be continuous
        if (header.exchange_id != prev.header.exchange_id)
            return false;

        switch (header.exchange_id) {
            case ExchangeId::BINANCE_SPOT:
            case ExchangeId::BINANCE_FUT:
                return sequence.first_update_id == prev.sequence.last_update_id + 1;

            case ExchangeId::KUCOIN_SPOT:
            case ExchangeId::KUCOIN_FUT:
                // KuCoin requires strict increment (no overlaps)
                return sequence.sequence_start == prev.sequence.sequence_end + 1;

            case ExchangeId::OKX_SPOT:
            case ExchangeId::OKX_FUT:
                return sequence.previous_update_id == prev.sequence.sequence_number;

            case ExchangeId::BYBIT_SPOT:
            case ExchangeId::BYBIT_FUT:
                return sequence.sequence_number == prev.sequence.sequence_number + 1;

            default:
                // Unknown exchange - assume not continuous
                return false;
        }
    }
};

/**
 * @brief Real-time trade data representation
 *
 * Optimized structure for high-frequency trade message processing. Contains
 * executed trade information with cache-optimized field layout for minimal
 * latency access to critical trading data.
 *
 * Real-time Trade Data Features:
 * - Price and quantity in first cache line for immediate access
 * - Side information (BUY/SELL) for market analysis
 * - Maker/taker flag for liquidity analysis and fee calculations
 * - Exchange timestamps for precise trade timing
 * - Trade ID for deduplication and reference
 *
 * Memory Layout Optimization:
 * - Hot fields (price, quantity, side) in first 64 bytes with header
 * - Cold fields (trade_id) in second cache line
 * - Explicit padding prevents false sharing
 * - 128-byte total size (2 cache lines) for predictable access patterns
 * - Alignment ensures optimal memory bus utilization
 */
struct alignas(64) TradeMessage {
    MessageHeader header;  ///< Common header (header.type = MessageType::TRADE)

    // Hot data - frequently accessed fields for trade analysis (24 bytes)
    Price price;             ///< Execution price (8 bytes)
    uint64_t quantity;       ///< Trade quantity (8 bytes)
    Side side;               ///< Aggressor side - BUY or SELL (1 byte)
    bool is_maker;           ///< true if maker order, false if taker (1 byte)
    uint8_t reserved1[6]{};  ///< Padding to align next field - zero-initialized

    // Cold data - rarely accessed during real-time processing
    uint64_t trade_id;        ///< Exchange-specific trade identifier (8 bytes)
    uint8_t reserved2[32]{};  ///< Padding to 128 bytes (2 cache lines) - zero-initialized

    /// Default constructor with sensible trade defaults
    constexpr TradeMessage() noexcept
        : header(),
          price(Price::zero()),
          quantity(0),
          side(Side::BUY),
          is_maker(false),
          reserved1{},
          trade_id(0),
          reserved2{} {
        header.type = MessageType::TRADE;
    }
};

/// Ensure TradeMessage uses exactly 2 cache lines for optimal memory access
static_assert(sizeof(TradeMessage) == 128, "TradeMessage should be 2 cache lines");

/**
 * @brief 24-hour ticker/summary statistics message
 *
 * Contains aggregate market statistics typically updated every few seconds
 * by exchanges. Used for market overview displays and basic price discovery.
 * Less time-critical than trade or order book messages.
 */
struct alignas(64) TickerMessage {
    MessageHeader header;  ///< Common header (header.type = MessageType::TICKER)

    // Current market state
    Price best_bid;         ///< Current best bid price
    Price best_ask;         ///< Current best ask price
    uint64_t best_bid_qty;  ///< Quantity at best bid
    uint64_t best_ask_qty;  ///< Quantity at best ask

    // 24-hour statistics
    Price last_price;  ///< Last trade price
    Price volume_24h;  ///< 24-hour trading volume
    Price high_24h;    ///< 24-hour high price
    Price low_24h;     ///< 24-hour low price

    /// Default constructor with zero prices and volumes
    constexpr TickerMessage() noexcept
        : header(),
          best_bid(Price::zero()),
          best_ask(Price::zero()),
          best_bid_qty(0),
          best_ask_qty(0),
          last_price(Price::zero()),
          volume_24h(Price::zero()),
          high_24h(Price::zero()),
          low_24h(Price::zero()) {
        header.type = MessageType::TICKER;
    }
};

/// Ensure TickerMessage uses exactly 2 cache lines
static_assert(sizeof(TickerMessage) == 128, "TickerMessage should be 2 cache lines");

/**
 * @brief Connection status states for WebSocket connections
 *
 * Tracks the lifecycle of exchange connections from initial connection
 * through authentication and subscription setup. Used for connection
 * management and error handling in the feed infrastructure.
 */
enum class ConnectionStatus : uint8_t {
    CONNECTED = 0,      ///< Successfully connected to exchange
    DISCONNECTED = 1,   ///< Connection lost or not established
    RECONNECTING = 2,   ///< Attempting to reconnect
    AUTHENTICATED = 3,  ///< Connected and authenticated (private feeds)
    SUBSCRIBED = 4,     ///< Subscribed to market data channels
    ERROR = 5           ///< Connection error occurred
};

/**
 * @brief Connection status and error reporting message
 *
 * Used for reporting WebSocket connection state changes and error conditions.
 * Includes space for diagnostic information to aid in debugging connection issues.
 */
struct alignas(64) ConnectionMessage {
    MessageHeader header;     ///< Common header (header.type = MessageType::CONNECTION_STATUS)
    ConnectionStatus status;  ///< Current connection state
    uint8_t error_code;       ///< Exchange-specific error code (if applicable)
    uint16_t retry_count;     ///< Number of reconnection attempts
    uint32_t reserved1{};     ///< Padding for alignment
    char details[56];         ///< Short error/status message (padded to 128 bytes total)

    /// Default constructor - starts in disconnected state
    constexpr ConnectionMessage() noexcept
        : header(), status(ConnectionStatus::DISCONNECTED), error_code(0), retry_count(0), reserved1{}, details{} {
        header.type = MessageType::CONNECTION_STATUS;
        // details{} already zero-initializes in constexpr context
    }
};

/// Ensure ConnectionMessage uses exactly 2 cache lines
static_assert(sizeof(ConnectionMessage) == 128, "ConnectionMessage should be 2 cache lines");

/**
 * @brief Unified message union for zero-copy type-safe access
 *
 * This union allows efficient message dispatch without runtime polymorphism
 * or heap allocations. The common MessageHeader enables type identification
 * while maintaining optimal memory layout for each message type.
 *
 * Zero-Copy Design Benefits:
 * - No virtual function table overhead
 * - Type dispatch via header.type field
 * - Direct memory access to specialized message fields
 * - Compatible with memory pool allocation patterns
 * - Eliminates need for message downcasting
 */
union alignas(64) Message {
    MessageHeader header;           ///< Common header for type dispatch
    MarketDataMessage market_data;  ///< Order book snapshot/delta messages
    TradeMessage trade;             ///< Executed trade messages
    TickerMessage ticker;           ///< 24h summary statistics
    ConnectionMessage connection;   ///< Connection status updates

    /// Default constructor creates minimal header (lowest overhead)
    constexpr Message() noexcept : header() {}

    /// Get message type for dispatch logic
    [[nodiscard]] MessageType type() const noexcept {
        return header.type;
    }

    /// Check if message contains order book data (snapshot or delta)
    [[nodiscard]] bool is_market_data() const noexcept {
        return header.type == MessageType::SNAPSHOT || header.type == MessageType::DELTA;
    }
};

/**
 * @brief Configuration hint for message pool pre-allocation
 *
 * Provides capacity hints for different message types to optimize
 * memory pool allocation. Values are tuned based on typical exchange
 * message rates and should be adjusted for specific trading scenarios.
 */
struct MessagePoolConfig {
    size_t market_data_capacity = 10000;  ///< Number of market data messages (~160MB)
    size_t trade_capacity = 5000;         ///< Number of trade messages (~640KB)
    size_t ticker_capacity = 1000;        ///< Number of ticker messages (~128KB)
    size_t other_capacity = 1000;         ///< Other message types (~128KB)
};

}  // namespace crypto_lob::exchanges::base