#pragma once

#include <cstdlib>
#include <cstring>
#include <simdjson.h>
#include <string>

#include <spdlog/spdlog.h>

#include "../../core/instrument.hpp"
#include "../../core/price.hpp"
#include "../parser.hpp"

namespace crypto_lob::exchanges::binance {

using namespace crypto_lob::core;

/**
 * @brief Binance Spot market data parser
 *
 * Architecture alignment (Section 4):
 * "BinanceSpotJsonParser: The concrete Parser that understands Binance
 * Spot message format (JSON fields) and performs JSON parsing using simdjson."
 *
 * Parses Binance WebSocket messages:
 * - Depth updates (order book snapshots)
 * - Diff depth (incremental updates)
 * - Trade streams
 *
 * Performance: Uses simdjson for ~5μs parsing latency
 */
class BinanceSpotJsonParser : public Parser {
  public:
    using Parser::Parser;  // Inherit constructor

  protected:
    /**
     * @brief Parse Binance-specific JSON message
     *
     * NOTE: Combined stream unwrapping is now handled by BinanceSpotConnector.
     * Parser only receives clean market data messages in direct format.
     *
     * Message types from Binance WebSocket:
     * 1. Partial depth: {"lastUpdateId": 123, "bids": [...], "asks": [...]}
     * 2. Diff depth: {"e": "depthUpdate", "E": 123456789, "s": "BTCUSDT",
     *                 "U": 100, "u": 120, "b": [...], "a": [...]}
     * 3. Trade: {"e": "trade", "E": 123456789, "s": "BTCUSDT",
     *            "t": 12345, "p": "45000.00", "q": "0.001", ...}
     */
    bool parse_message(std::string_view json, NormalizedMessage& msg) override {
        // Create padded string for simdjson (requires 64 bytes of padding)
        simdjson::padded_string padded_json(json);

        // Parse JSON using simdjson (no try-catch per architecture guide)
        auto doc_result = parser_.iterate(padded_json);
        if (doc_result.error() != simdjson::SUCCESS) {
            // JSON parsing failed - log the error for debugging
            static int error_count = 0;
            if (error_count < 3) {
                spdlog::error("BinanceSpotJsonParser: JSON iteration failed: {} - Sample: {}",
                              simdjson::error_message(doc_result.error()),
                              json.substr(0, std::min(json.size(), size_t(100))));
                error_count++;
            }
            return false;
        }

        auto obj_result = doc_result.get_object();
        if (obj_result.error() != simdjson::SUCCESS) {
            static int error_count = 0;
            if (error_count < 3) {
                spdlog::error("BinanceSpotJsonParser: Object extraction failed: {} - Sample: {}",
                              simdjson::error_message(obj_result.error()),
                              json.substr(0, std::min(json.size(), size_t(100))));
                error_count++;
            }
            return false;
        }
        simdjson::ondemand::object obj = obj_result.value();

        // Check if it's an event message (has "e" field)
        std::string_view event_type;
        if (obj["e"].get_string().get(event_type) == simdjson::SUCCESS) {
            // Event message (diff depth or trade)
            if (event_type == "depthUpdate") {
                return parse_depth_update_obj(obj, msg);
            } else if (event_type == "trade") {
                return parse_trade_obj(obj, msg);
            }
        } else {
            // No "e" field - check if it's a snapshot
            if (obj["lastUpdateId"].error() == simdjson::SUCCESS) {
                return parse_snapshot_obj(obj, msg);
            }
        }
        return false;  // Unknown message type
    }

    /**
     * @brief Parse order book snapshot
     *
     * Format: {"lastUpdateId": 123, "bids": [["45000.00", "1.5"]],
     *          "asks": [["45001.00", "2.0"]]}
     *
     * NOTE: Symbol information for snapshots will need to be handled
     * differently since preprocessing is now in connector.
     */
    bool parse_snapshot_obj(simdjson::ondemand::object& obj, NormalizedMessage& msg) {
        auto snapshot = SnapshotMessage{};

        // Get update ID
        uint64_t update_id;
        if (obj["lastUpdateId"].get_uint64().get(update_id) != simdjson::SUCCESS) {
            return false;
        }
        snapshot.last_update_id = update_id;

        // TODO: Symbol extraction for snapshots needs to be handled
        // since stream unwrapping is now done in connector
        // For now, use a default instrument for testing
        msg.instrument = InstrumentId(config_.exchange_id, "UNKNOWN");

        // Parse bids
        simdjson::ondemand::array bids;
        if (obj["bids"].get_array().get(bids) == simdjson::SUCCESS) {
            for (auto bid : bids) {
                if (snapshot.bid_count >= SnapshotMessage::MAX_LEVELS)
                    break;

                simdjson::ondemand::array level = bid;
                auto iter = level.begin();

                // Price
                std::string_view price_str;
                (*iter).get_string().get(price_str);

                // Quantity
                ++iter;
                std::string_view qty_str;
                (*iter).get_string().get(qty_str);
                Quantity qty = Quantity::from_string(qty_str);

                snapshot.bids[snapshot.bid_count++] = MessagePriceLevel(Price::from_string(price_str), qty);
            }
        }

        // Parse asks
        simdjson::ondemand::array asks;
        if (obj["asks"].get_array().get(asks) == simdjson::SUCCESS) {
            for (auto ask : asks) {
                if (snapshot.ask_count >= SnapshotMessage::MAX_LEVELS)
                    break;

                simdjson::ondemand::array level = ask;
                auto iter = level.begin();

                // Price
                std::string_view price_str;
                (*iter).get_string().get(price_str);

                // Quantity
                ++iter;
                std::string_view qty_str;
                (*iter).get_string().get(qty_str);
                Quantity qty = Quantity::from_string(qty_str);

                snapshot.asks[snapshot.ask_count++] = MessagePriceLevel(Price::from_string(price_str), qty);
            }
        }

        msg.type = MessageType::SNAPSHOT;
        msg.data = std::move(snapshot);
        return true;
    }

    /**
     * @brief Parse incremental depth update
     *
     * Format: {"e": "depthUpdate", "E": 123456789, "s": "BTCUSDT",
     *          "U": 100, "u": 120, "b": [...], "a": [...]}
     */
    bool parse_depth_update_obj(simdjson::ondemand::object& obj, NormalizedMessage& msg) {
        auto delta = DeltaMessage{};

        // Get update IDs
        obj["U"].get_uint64().get(delta.first_update_id);
        obj["u"].get_uint64().get(delta.last_update_id);
        // Event time not stored in base DeltaMessage

        // Get symbol for instrument ID
        std::string_view symbol;
        obj["s"].get_string().get(symbol);
        msg.instrument = InstrumentId(config_.exchange_id, symbol);

        // Parse bid updates
        simdjson::ondemand::array bids;
        if (obj["b"].get_array().get(bids) == simdjson::SUCCESS) {
            for (auto bid : bids) {
                if (delta.bid_update_count >= DeltaMessage::MAX_UPDATES)
                    break;

                simdjson::ondemand::array level = bid;
                auto iter = level.begin();

                // Price
                std::string_view price_str;
                (*iter).get_string().get(price_str);

                // Quantity
                ++iter;
                std::string_view qty_str;
                (*iter).get_string().get(qty_str);
                Quantity qty = Quantity::from_string(qty_str);

                delta.bid_updates[delta.bid_update_count++] = MessagePriceLevel(Price::from_string(price_str), qty);
            }
        }

        // Parse ask updates
        simdjson::ondemand::array asks;
        if (obj["a"].get_array().get(asks) == simdjson::SUCCESS) {
            for (auto ask : asks) {
                if (delta.ask_update_count >= DeltaMessage::MAX_UPDATES)
                    break;

                simdjson::ondemand::array level = ask;
                auto iter = level.begin();

                // Price
                std::string_view price_str;
                (*iter).get_string().get(price_str);

                // Quantity
                ++iter;
                std::string_view qty_str;
                (*iter).get_string().get(qty_str);
                Quantity qty = Quantity::from_string(qty_str);

                delta.ask_updates[delta.ask_update_count++] = MessagePriceLevel(Price::from_string(price_str), qty);
            }
        }

        msg.type = MessageType::DELTA;
        msg.data = std::move(delta);
        return true;
    }

    /**
     * @brief Parse trade message
     *
     * Format: {"e": "trade", "E": 123456789, "s": "BTCUSDT",
     *          "t": 12345, "p": "45000.00", "q": "0.001",
     *          "T": 123456789, "m": true}
     */
    bool parse_trade_obj(simdjson::ondemand::object& obj, NormalizedMessage& msg) {
        auto trade = TradeMessage{};

        // Trade fields
        obj["t"].get_uint64().get(trade.trade_id);
        // Event time and trade time - store trade time as raw TSC value
        uint64_t event_time_ms, trade_time_ms;
        obj["E"].get_uint64().get(event_time_ms);
        obj["T"].get_uint64().get(trade_time_ms);
        // For now, just use the millisecond timestamp as-is
        trade.timestamp = trade_time_ms;
        obj["m"].get_bool().get(trade.is_buyer_maker);

        // Price
        std::string_view price_str;
        obj["p"].get_string().get(price_str);
        trade.price = Price::from_string(price_str);

        // Quantity
        std::string_view qty_str;
        obj["q"].get_string().get(qty_str);
        trade.quantity = Quantity::from_string(qty_str);

        // Symbol
        std::string_view symbol;
        obj["s"].get_string().get(symbol);
        msg.instrument = InstrumentId(config_.exchange_id, symbol);

        msg.type = MessageType::TRADE;
        msg.data = std::move(trade);
        return true;
    }

    // simdjson parser instance (reusable for performance)
    simdjson::ondemand::parser parser_;
};

}  // namespace crypto_lob::exchanges::binance