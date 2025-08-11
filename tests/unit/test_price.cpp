#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

#include <gtest/gtest.h>

#include "core/price.hpp"

using namespace crypto_lob::core;

class PriceTest : public ::testing::Test {
  protected:
    // Helper to check double equality with tolerance
    bool near_equal(double a, double b, double tolerance = 1e-9) {
        return std::abs(a - b) < tolerance;
    }
};

// Construction tests
TEST_F(PriceTest, DefaultConstruction) {
    Price p;
    EXPECT_EQ(p.raw_value(), 0);
    EXPECT_TRUE(p.is_zero());
    EXPECT_FALSE(p.is_positive());
    EXPECT_FALSE(p.is_negative());
    EXPECT_EQ(p.to_double(), 0.0);
    EXPECT_EQ(p.to_string(), "0");
}

TEST_F(PriceTest, RawValueConstruction) {
    Price p1(int64_t(1'000'000'000));  // $1.00
    EXPECT_EQ(p1.raw_value(), 1'000'000'000);
    EXPECT_EQ(p1.to_double(), 1.0);
    EXPECT_EQ(p1.to_string(), "1");

    Price p2(int64_t(1'500'000'000));  // $1.50
    EXPECT_EQ(p2.raw_value(), 1'500'000'000);
    EXPECT_EQ(p2.to_double(), 1.5);
    EXPECT_EQ(p2.to_string(), "1.5");

    Price p3(int64_t(123'456'789));  // $0.123456789
    EXPECT_EQ(p3.raw_value(), 123'456'789);
    EXPECT_DOUBLE_EQ(p3.to_double(), 0.123456789);
    EXPECT_EQ(p3.to_string(), "0.123456789");
}

TEST_F(PriceTest, DoubleConstruction) {
    // Note: Price constructor takes raw int64_t, not double
    // For doubles, use Price::from_string() or provide raw value
    Price p1 = Price::from_string("1.0");
    EXPECT_EQ(p1.raw_value(), 1'000'000'000);
    EXPECT_DOUBLE_EQ(p1.to_double(), 1.0);

    Price p2 = Price::from_string("0.123456789");
    EXPECT_EQ(p2.raw_value(), 123'456'789);
    EXPECT_TRUE(near_equal(p2.to_double(), 0.123456789));

    Price p3 = Price::from_string("1234.567890123");  // More than 9 decimal places
    EXPECT_TRUE(near_equal(p3.to_double(), 1234.567890123, 1e-8));
}

TEST_F(PriceTest, NegativePrices) {
    Price p1 = Price::from_string("-1.0");
    EXPECT_EQ(p1.raw_value(), -1'000'000'000);
    EXPECT_TRUE(p1.is_negative());
    EXPECT_FALSE(p1.is_positive());
    EXPECT_FALSE(p1.is_zero());
    EXPECT_EQ(p1.to_string(), "-1");

    Price p2 = Price::from_string("-0.000000001");  // -1 satoshi
    EXPECT_EQ(p2.raw_value(), -1);
    EXPECT_EQ(p2.to_string(), "-0.000000001");
}

// String parsing tests
TEST_F(PriceTest, StringParsingWholeNumbers) {
    EXPECT_EQ(Price::from_string("0").raw_value(), 0);
    EXPECT_EQ(Price::from_string("1").raw_value(), 1'000'000'000);
    EXPECT_EQ(Price::from_string("42").raw_value(), 42'000'000'000);
    EXPECT_EQ(Price::from_string("1000000").raw_value(), 1'000'000'000'000'000);
}

TEST_F(PriceTest, StringParsingDecimals) {
    EXPECT_EQ(Price::from_string("0.1").raw_value(), 100'000'000);
    EXPECT_EQ(Price::from_string("0.123456789").raw_value(), 123'456'789);
    EXPECT_EQ(Price::from_string("1.5").raw_value(), 1'500'000'000);
    EXPECT_EQ(Price::from_string("42.123").raw_value(), 42'123'000'000);
}

TEST_F(PriceTest, StringParsingHighPrecision) {
    // Exactly 9 decimal places
    EXPECT_EQ(Price::from_string("0.123456789").raw_value(), 123'456'789);
    EXPECT_EQ(Price::from_string("1.987654321").raw_value(), 1'987'654'321);

    // More than 9 decimal places (should truncate)
    EXPECT_EQ(Price::from_string("0.1234567891234").raw_value(), 123'456'789);
    EXPECT_EQ(Price::from_string("0.9999999999999").raw_value(), 999'999'999);
}

TEST_F(PriceTest, StringParsingNegativeNumbers) {
    EXPECT_EQ(Price::from_string("-1").raw_value(), -1'000'000'000);
    EXPECT_EQ(Price::from_string("-0.1").raw_value(), -100'000'000);
    EXPECT_EQ(Price::from_string("-42.123456789").raw_value(), -42'123'456'789);
}

TEST_F(PriceTest, StringParsingEdgeCases) {
    // Empty string
    EXPECT_EQ(Price::from_string("").raw_value(), 0);

    // Just decimal point
    EXPECT_EQ(Price::from_string(".").raw_value(), 0);
    EXPECT_EQ(Price::from_string("-.").raw_value(), 0);

    // Leading decimal
    EXPECT_EQ(Price::from_string(".5").raw_value(), 500'000'000);
    EXPECT_EQ(Price::from_string("-.5").raw_value(), -500'000'000);

    // Trailing decimal
    EXPECT_EQ(Price::from_string("5.").raw_value(), 5'000'000'000);
    EXPECT_EQ(Price::from_string("-5.").raw_value(), -5'000'000'000);

    // Multiple decimal points (continues parsing digits, ignoring extra dots)
    // Parses as "1.23" because it skips non-digit chars in fractional part
    EXPECT_EQ(Price::from_string("1.2.3").raw_value(), 1'230'000'000);

    // Leading zeros
    EXPECT_EQ(Price::from_string("00001").raw_value(), 1'000'000'000);
    EXPECT_EQ(Price::from_string("0.00001").raw_value(), 10'000);
}

TEST_F(PriceTest, StringParsingCryptoPrices) {
    // Bitcoin-like prices
    EXPECT_EQ(Price::from_string("50000").raw_value(), 50'000'000'000'000);
    EXPECT_EQ(Price::from_string("50000.50").raw_value(), 50'000'500'000'000);

    // Micro-cap prices
    EXPECT_EQ(Price::from_string("0.000000001").raw_value(), 1);  // 1 nano-unit
    EXPECT_EQ(Price::from_string("0.000001234").raw_value(), 1'234);

    // High precision stablecoin prices
    EXPECT_EQ(Price::from_string("0.999999999").raw_value(), 999'999'999);
    EXPECT_EQ(Price::from_string("1.000000001").raw_value(), 1'000'000'001);
}

// String output tests
TEST_F(PriceTest, StringOutputWholeNumbers) {
    EXPECT_EQ(Price(int64_t(0)).to_string(), "0");
    EXPECT_EQ(Price(int64_t(1'000'000'000)).to_string(), "1");
    EXPECT_EQ(Price(int64_t(42'000'000'000)).to_string(), "42");
    EXPECT_EQ(Price(int64_t(-5'000'000'000)).to_string(), "-5");
}

TEST_F(PriceTest, StringOutputDecimals) {
    EXPECT_EQ(Price(int64_t(100'000'000)).to_string(), "0.1");
    EXPECT_EQ(Price(int64_t(123'456'789)).to_string(), "0.123456789");
    EXPECT_EQ(Price(int64_t(1'500'000'000)).to_string(), "1.5");
    EXPECT_EQ(Price(int64_t(42'123'000'000)).to_string(), "42.123");
    EXPECT_EQ(Price(int64_t(-100'000'000)).to_string(), "-0.1");
}

TEST_F(PriceTest, StringOutputNoTrailingZeros) {
    EXPECT_EQ(Price(int64_t(1'000'000'000)).to_string(), "1");     // Not "1.000000000"
    EXPECT_EQ(Price(int64_t(1'100'000'000)).to_string(), "1.1");   // Not "1.100000000"
    EXPECT_EQ(Price(int64_t(1'230'000'000)).to_string(), "1.23");  // Not "1.230000000"
}

TEST_F(PriceTest, StringRoundTrip) {
    // Test that parsing and then converting back to string is consistent
    std::vector<std::string> test_values = {
        "0", "1", "0.1", "0.000000001", "42.123456789", "-1", "-0.5", "-999.999999999", "1000000", "0.000001"};

    for (const auto& val : test_values) {
        Price p = Price::from_string(val);
        Price p2 = Price::from_string(p.to_string());
        EXPECT_EQ(p.raw_value(), p2.raw_value()) << "Failed round trip for: " << val;
    }
}

// Arithmetic operations tests
TEST_F(PriceTest, Addition) {
    Price p1(int64_t(1'000'000'000));  // $1
    Price p2(int64_t(500'000'000));    // $0.50

    Price sum = p1 + p2;
    EXPECT_EQ(sum.raw_value(), 1'500'000'000);
    EXPECT_EQ(sum.to_string(), "1.5");

    // Negative addition
    Price p3(int64_t(-300'000'000));  // -$0.30
    Price sum2 = p2 + p3;
    EXPECT_EQ(sum2.raw_value(), 200'000'000);
    EXPECT_EQ(sum2.to_string(), "0.2");
}

TEST_F(PriceTest, Subtraction) {
    Price p1(int64_t(1'000'000'000));  // $1
    Price p2(int64_t(300'000'000));    // $0.30

    Price diff = p1 - p2;
    EXPECT_EQ(diff.raw_value(), 700'000'000);
    EXPECT_EQ(diff.to_string(), "0.7");

    // Result becomes negative
    Price diff2 = p2 - p1;
    EXPECT_EQ(diff2.raw_value(), -700'000'000);
    EXPECT_EQ(diff2.to_string(), "-0.7");
}

TEST_F(PriceTest, Multiplication) {
    Price p1(int64_t(1'500'000'000));  // $1.50

    Price doubled = p1 * 2;
    EXPECT_EQ(doubled.raw_value(), 3'000'000'000);
    EXPECT_EQ(doubled.to_string(), "3");

    Price halved = p1 * 0;  // Note: multiplying by 0
    EXPECT_EQ(halved.raw_value(), 0);

    Price negative = p1 * -3;
    EXPECT_EQ(negative.raw_value(), -4'500'000'000);
    EXPECT_EQ(negative.to_string(), "-4.5");
}

TEST_F(PriceTest, Division) {
    Price p1(int64_t(3'000'000'000));  // $3

    Price halved = p1 / 2;
    EXPECT_EQ(halved.raw_value(), 1'500'000'000);
    EXPECT_EQ(halved.to_string(), "1.5");

    Price third = p1 / 3;
    EXPECT_EQ(third.raw_value(), 1'000'000'000);
    EXPECT_EQ(third.to_string(), "1");

    // Integer division truncation
    Price p2(int64_t(1'000'000'000));  // $1
    Price result = p2 / 3;
    EXPECT_EQ(result.raw_value(), 333'333'333);  // Truncated
}

TEST_F(PriceTest, CompoundAssignmentOperators) {
    Price p(int64_t(1'000'000'000));  // $1

    p += Price(int64_t(500'000'000));
    EXPECT_EQ(p.raw_value(), 1'500'000'000);

    p -= Price(int64_t(200'000'000));
    EXPECT_EQ(p.raw_value(), 1'300'000'000);

    p *= 2;
    EXPECT_EQ(p.raw_value(), 2'600'000'000);

    p /= 4;
    EXPECT_EQ(p.raw_value(), 650'000'000);
}

// Comparison tests
TEST_F(PriceTest, EqualityComparison) {
    Price p1(int64_t(1'000'000'000));
    Price p2(int64_t(1'000'000'000));
    Price p3(int64_t(1'000'000'001));

    EXPECT_TRUE(p1 == p2);
    EXPECT_FALSE(p1 == p3);
    EXPECT_FALSE(p1 != p2);
    EXPECT_TRUE(p1 != p3);
}

TEST_F(PriceTest, OrderingComparison) {
    Price p1(int64_t(100));
    Price p2(int64_t(200));
    Price p3(int64_t(200));

    EXPECT_TRUE(p1 < p2);
    EXPECT_FALSE(p2 < p1);
    EXPECT_FALSE(p2 < p3);

    EXPECT_TRUE(p1 <= p2);
    EXPECT_TRUE(p2 <= p3);
    EXPECT_FALSE(p2 <= p1);

    EXPECT_TRUE(p2 > p1);
    EXPECT_FALSE(p1 > p2);
    EXPECT_FALSE(p2 > p3);

    EXPECT_TRUE(p2 >= p1);
    EXPECT_TRUE(p2 >= p3);
    EXPECT_FALSE(p1 >= p2);
}

TEST_F(PriceTest, ThreeWayComparison) {
    Price p1(int64_t(100));
    Price p2(int64_t(200));
    Price p3(int64_t(200));

    EXPECT_TRUE((p1 <=> p2) < 0);
    EXPECT_TRUE((p2 <=> p1) > 0);
    EXPECT_TRUE((p2 <=> p3) == 0);
}

// Utility method tests
TEST_F(PriceTest, UtilityMethods) {
    Price zero;
    Price positive(int64_t(100));
    Price negative(int64_t(-100));

    EXPECT_TRUE(zero.is_zero());
    EXPECT_FALSE(zero.is_positive());
    EXPECT_FALSE(zero.is_negative());

    EXPECT_FALSE(positive.is_zero());
    EXPECT_TRUE(positive.is_positive());
    EXPECT_FALSE(positive.is_negative());

    EXPECT_FALSE(negative.is_zero());
    EXPECT_FALSE(negative.is_positive());
    EXPECT_TRUE(negative.is_negative());
}

TEST_F(PriceTest, StaticFactoryMethods) {
    Price zero = Price::zero();
    EXPECT_TRUE(zero.is_zero());
    EXPECT_EQ(zero.raw_value(), 0);

    Price max_price = Price::max();
    EXPECT_EQ(max_price.raw_value(), std::numeric_limits<int64_t>::max());
    EXPECT_TRUE(max_price.is_positive());

    Price min_price = Price::min();
    EXPECT_EQ(min_price.raw_value(), std::numeric_limits<int64_t>::min());
    EXPECT_TRUE(min_price.is_negative());
}

// Hash function test
TEST_F(PriceTest, HashFunction) {
    Price p1(int64_t(1'000'000'000));
    Price p2(int64_t(1'000'000'000));
    Price p3(int64_t(1'000'000'001));

    // Same prices should have same hash
    EXPECT_EQ(hash_value(p1), hash_value(p2));

    // Different prices should (usually) have different hashes
    EXPECT_NE(hash_value(p1), hash_value(p3));

    // Test that hash function is consistent
    size_t h1 = hash_value(p1);
    size_t h2 = hash_value(p1);
    EXPECT_EQ(h1, h2);  // Same object hashed twice should give same result

    // Test hash distribution (simple check)
    Price p4(int64_t(2'000'000'000));
    Price p5(int64_t(3'000'000'000));
    EXPECT_NE(hash_value(p4), hash_value(p5));
}

// Format tests - commented out due to -fno-rtti incompatibility
// TEST_F(PriceTest, StdFormat) {
//     Price p1(int64_t(1'234'567'890));  // $1.23456789
//     Price p2(int64_t(-500'000'000));   // -$0.50
//
//     // Test the formatter works correctly
//     std::string formatted1 = std::format("{}", p1);
//     std::string formatted2 = std::format("{}", p2);
//     std::string formatted3 = std::format("Price: {}", Price::zero());
//
//     EXPECT_EQ(formatted1, "1.23456789");
//     EXPECT_EQ(formatted2, "-0.5");
//     EXPECT_EQ(formatted3, "Price: 0");
// }

// Alternative test using to_string directly
TEST_F(PriceTest, ToStringFormatting) {
    Price p1(int64_t(1'234'567'890));  // $1.23456789
    Price p2(int64_t(-500'000'000));   // -$0.50
    Price p3 = Price::zero();

    // Test direct to_string conversion
    EXPECT_EQ(p1.to_string(), "1.23456789");
    EXPECT_EQ(p2.to_string(), "-0.5");
    EXPECT_EQ(p3.to_string(), "0");

    // Test in string concatenation context
    EXPECT_EQ("Price: " + p3.to_string(), "Price: 0");
}

// Precision and overflow tests
TEST_F(PriceTest, PrecisionLimits) {
    // Smallest positive price
    Price smallest(int64_t(1));
    EXPECT_EQ(smallest.to_string(), "0.000000001");
    EXPECT_TRUE(smallest.is_positive());

    // Test precision near boundaries
    Price p1 = Price::from_string("0.999999999");
    Price p2 = Price::from_string("0.000000001");
    Price sum = p1 + p2;
    EXPECT_EQ(sum.raw_value(), 1'000'000'000);
    EXPECT_EQ(sum.to_string(), "1");
}

TEST_F(PriceTest, LargePriceHandling) {
    // Test large cryptocurrency prices (e.g., Bitcoin at $1M)
    Price btc_price = Price::from_string("1000000");
    EXPECT_EQ(btc_price.raw_value(), 1'000'000'000'000'000);
    EXPECT_EQ(btc_price.to_string(), "1000000");

    // Test arithmetic with large prices
    Price double_btc = btc_price * 2;
    EXPECT_EQ(double_btc.to_string(), "2000000");
}

// Quantity type alias test
TEST_F(PriceTest, QuantityTypeAlias) {
    Quantity qty1(int64_t(1'000'000'000));  // 1 unit
    Quantity qty2(int64_t(500'000'000));    // 0.5 units

    Quantity total = qty1 + qty2;
    EXPECT_EQ(total.raw_value(), 1'500'000'000);
    EXPECT_EQ(total.to_string(), "1.5");

    // Quantity behaves exactly like Price
    static_assert(std::is_same_v<Quantity, Price>);
}

// Concept tests
TEST_F(PriceTest, TypeConcepts) {
    static_assert(PriceType<Price>);
    static_assert(!PriceType<int>);
    static_assert(!PriceType<double>);

    static_assert(QuantityType<Quantity>);
    static_assert(QuantityType<Price>);  // Since Quantity is an alias
    static_assert(!QuantityType<int>);
}