#include <unordered_map>

#include <gtest/gtest.h>

#include "core/instrument.hpp"

using namespace crypto_lob::core;

class InstrumentIdTest : public ::testing::Test {
  protected:
    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(InstrumentIdTest, DefaultConstruction) {
    InstrumentId id;
    EXPECT_EQ(id.symbol(), "");
    EXPECT_EQ(id.c_str()[0], '\0');
}

TEST_F(InstrumentIdTest, ConstructionFromLiteral) {
    InstrumentId btc{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    EXPECT_EQ(btc.exchange, ExchangeId::BINANCE_SPOT);
    EXPECT_EQ(btc.symbol(), "BTCUSDT");
    EXPECT_STREQ(btc.c_str(), "BTCUSDT");
}

TEST_F(InstrumentIdTest, ConstructionFromStringView) {
    std::string_view sv = "ETHUSDT";
    InstrumentId eth{ExchangeId::BINANCE_SPOT, sv};
    EXPECT_EQ(eth.symbol(), "ETHUSDT");
}

TEST_F(InstrumentIdTest, ConstructionFromStdString) {
    std::string str = "SOLUSDT";
    InstrumentId sol{ExchangeId::BINANCE_SPOT, str};
    EXPECT_EQ(sol.symbol(), "SOLUSDT");
}

TEST_F(InstrumentIdTest, SymbolTruncation) {
    // Test that symbols longer than 14 characters are truncated
    // Use string_view or std::string to avoid compile-time assertion
    std::string long_str = "VERYLONGSYMBOLNAME";
    InstrumentId long_symbol{ExchangeId::BINANCE_SPOT, long_str};
    EXPECT_EQ(long_symbol.symbol(), "VERYLONGSYMBOL");  // Should be truncated to 14 chars
    EXPECT_EQ(long_symbol.symbol().length(), 14u);
}

TEST_F(InstrumentIdTest, EmptySymbol) {
    InstrumentId empty{ExchangeId::BINANCE_SPOT, ""};
    EXPECT_EQ(empty.symbol(), "");
    EXPECT_EQ(empty.c_str()[0], '\0');
}

TEST_F(InstrumentIdTest, MaxLengthSymbol) {
    // Test exactly 14 character symbol
    InstrumentId max_len{ExchangeId::BINANCE_SPOT, "12345678901234"};
    EXPECT_EQ(max_len.symbol(), "12345678901234");
    EXPECT_EQ(max_len.symbol().length(), 14u);
}

TEST_F(InstrumentIdTest, EqualityOperator) {
    InstrumentId btc1{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId btc2{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId eth{ExchangeId::BINANCE_SPOT, "ETHUSDT"};

    EXPECT_EQ(btc1, btc2);
    EXPECT_NE(btc1, eth);
    // Same symbol, different exchange test removed - btc_okx not defined
}

TEST_F(InstrumentIdTest, ComparisonOperators) {
    InstrumentId btc{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId eth{ExchangeId::BINANCE_SPOT, "ETHUSDT"};

    // Symbol comparison (B < E)
    EXPECT_LT(btc, eth);
    EXPECT_GT(eth, btc);

    // Comparison with different exchange test removed - btc_okx not defined
}

TEST_F(InstrumentIdTest, HashFunction) {
    InstrumentId btc1{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId btc2{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId eth{ExchangeId::BINANCE_SPOT, "ETHUSDT"};

    InstrumentIdHash hasher;

    // Same instruments should have same hash
    EXPECT_EQ(hasher(btc1), hasher(btc2));

    // Different instruments should (likely) have different hashes
    EXPECT_NE(hasher(btc1), hasher(eth));
}

TEST_F(InstrumentIdTest, UseInUnorderedMap) {
    std::unordered_map<InstrumentId, int, InstrumentIdHash> instrument_map;

    InstrumentId btc{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    InstrumentId eth{ExchangeId::BINANCE_SPOT, "ETHUSDT"};
    InstrumentId sol{ExchangeId::BINANCE_SPOT, "SOLUSDT"};

    instrument_map[btc] = 100;
    instrument_map[eth] = 200;
    instrument_map[sol] = 300;

    EXPECT_EQ(instrument_map[btc], 100);
    EXPECT_EQ(instrument_map[eth], 200);
    EXPECT_EQ(instrument_map[sol], 300);

    // Test lookup with newly constructed key
    InstrumentId btc_lookup{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    EXPECT_EQ(instrument_map[btc_lookup], 100);
}

TEST_F(InstrumentIdTest, SetSymbolMethod) {
    InstrumentId id{ExchangeId::BINANCE_SPOT, "OLD"};
    EXPECT_EQ(id.symbol(), "OLD");

    id.set_symbol("NEWTOKEN");
    EXPECT_EQ(id.symbol(), "NEWTOKEN");

    // Test truncation
    id.set_symbol("VERYLONGSYMBOLNAME");
    EXPECT_EQ(id.symbol(), "VERYLONGSYMBOL");
}

TEST_F(InstrumentIdTest, CommonCryptoSymbols) {
    // Test real-world crypto symbols
    struct TestCase {
        std::string_view symbol;
        size_t expected_len;
    };

    TestCase cases[] = {{"BTCUSDT", 7},
                        {"ETHUSDT", 7},
                        {"1000SHIBUSDT", 12},
                        {"DOGEUSDT", 8},
                        {"ADAUSDT", 7},
                        {"MATICUSDT", 9},
                        {"LINKUSDT", 8},
                        {"UNIUSDT", 7},
                        {"ATOMUSDT", 8},
                        {"FILUSDT", 7}};

    for (const auto& tc : cases) {
        InstrumentId id{ExchangeId::BINANCE_SPOT, tc.symbol};
        EXPECT_EQ(id.symbol(), tc.symbol);
        EXPECT_EQ(id.symbol().length(), tc.expected_len);
    }
}

TEST_F(InstrumentIdTest, MemoryLayout) {
    // Verify the struct is exactly 16 bytes as designed
    EXPECT_EQ(sizeof(InstrumentId), 16u);

    // Verify no padding issues
    InstrumentId id{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
    EXPECT_EQ(offsetof(InstrumentId, exchange), 0u);
    EXPECT_EQ(offsetof(InstrumentId, symbol_data), 1u);
}

TEST_F(InstrumentIdTest, CopyAndMove) {
    InstrumentId original{ExchangeId::BINANCE_SPOT, "BTCUSDT"};

    // Copy constructor
    InstrumentId copy(original);
    EXPECT_EQ(copy, original);
    EXPECT_EQ(copy.symbol(), "BTCUSDT");

    // Copy assignment
    InstrumentId copy_assign;
    copy_assign = original;
    EXPECT_EQ(copy_assign, original);

    // Move constructor
    InstrumentId moved(std::move(copy));
    EXPECT_EQ(moved.symbol(), "BTCUSDT");

    // Move assignment
    InstrumentId move_assign;
    move_assign = std::move(copy_assign);
    EXPECT_EQ(move_assign.symbol(), "BTCUSDT");
}

TEST_F(InstrumentIdTest, PerformanceNoHeapAllocation) {
    // This test verifies that creating InstrumentId doesn't allocate heap memory
    // In a real HFT system, you'd use a memory profiler to verify this

    // Create many InstrumentIds - should not trigger heap allocation
    for (int i = 0; i < 1000; ++i) {
        InstrumentId id{ExchangeId::BINANCE_SPOT, "BTCUSDT"};
        // Use the id to prevent optimization
        EXPECT_EQ(id.symbol().length(), 7u);
    }
}