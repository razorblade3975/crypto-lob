#include <cstring>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "networking/raw_message_pool.hpp"

using namespace crypto_lob::networking;

class RawMessagePoolTest : public ::testing::Test {
  protected:
    static constexpr size_t TEST_CAPACITY = 16;  // Small capacity for testing

    void SetUp() override {
        // Reset thread-local pool for each test
        tls_raw_pool.reset();
        tls_pool_config.capacity = TEST_CAPACITY;
    }

    void TearDown() override {
        // Clean up thread-local pool
        tls_raw_pool.reset();
    }
};

// Test basic pool construction and capacity
TEST_F(RawMessagePoolTest, ConstructorInitializesCorrectly) {
    RawMessagePool pool(10);

    // Verify capacity is rounded up to power of 2
    EXPECT_EQ(pool.capacity(), 16);

    auto stats = pool.get_stats();
    EXPECT_EQ(stats.capacity, 16);
    EXPECT_EQ(stats.used, 0);
    EXPECT_EQ(stats.total_allocations, 0);
    EXPECT_EQ(stats.total_deallocations, 0);
    EXPECT_EQ(stats.allocation_failures, 0);
}

// Test message acquisition and release
TEST_F(RawMessagePoolTest, AcquireAndRelease) {
    RawMessagePool pool(TEST_CAPACITY);

    // Acquire a message
    RawMessage* msg = pool.acquire();
    ASSERT_NE(msg, nullptr);

    // Verify message is reset
    EXPECT_EQ(msg->timestamp_ns, 0);
    EXPECT_EQ(msg->sequence_num, 0);
    EXPECT_EQ(msg->exchange_id, 0);
    EXPECT_EQ(msg->size, 0);

    // Verify stats
    auto stats = pool.get_stats();
    EXPECT_EQ(stats.used, 1);
    EXPECT_EQ(stats.total_allocations, 1);

    // Release the message
    EXPECT_TRUE(pool.release(msg));

    stats = pool.get_stats();
    EXPECT_EQ(stats.used, 0);
    EXPECT_EQ(stats.total_deallocations, 1);
}

// Test pool exhaustion
TEST_F(RawMessagePoolTest, PoolExhaustion) {
    RawMessagePool pool(4);  // Will round up to 4
    std::vector<RawMessage*> messages;

    // Acquire all messages
    for (size_t i = 0; i < 4; ++i) {
        RawMessage* msg = pool.acquire();
        ASSERT_NE(msg, nullptr);
        messages.push_back(msg);
    }

    // Pool should be exhausted
    RawMessage* msg = pool.acquire();
    EXPECT_EQ(msg, nullptr);

    auto stats = pool.get_stats();
    EXPECT_EQ(stats.allocation_failures, 1);

    // Release all messages
    for (auto* m : messages) {
        EXPECT_TRUE(pool.release(m));
    }

    // Should be able to acquire again
    msg = pool.acquire();
    EXPECT_NE(msg, nullptr);
    EXPECT_TRUE(pool.release(msg));
}

// Test message data integrity
TEST_F(RawMessagePoolTest, MessageDataIntegrity) {
    RawMessagePool pool(TEST_CAPACITY);

    RawMessage* msg = pool.acquire();
    ASSERT_NE(msg, nullptr);

    // Write test data
    msg->timestamp_ns = 123456789;
    msg->sequence_num = 42;
    msg->exchange_id = 1;
    msg->size = 100;
    std::memcpy(msg->data, "test data", 10);

    // Release and re-acquire
    EXPECT_TRUE(pool.release(msg));
    msg = pool.acquire();
    ASSERT_NE(msg, nullptr);

    // Message should be reset (metadata cleared)
    EXPECT_EQ(msg->timestamp_ns, 0);
    EXPECT_EQ(msg->sequence_num, 0);
    EXPECT_EQ(msg->exchange_id, 0);
    EXPECT_EQ(msg->size, 0);

    EXPECT_TRUE(pool.release(msg));
}

// Test RAII guard basic functionality
TEST_F(RawMessagePoolTest, RAIIGuardAutoRelease) {
    RawMessagePool pool(TEST_CAPACITY);

    {
        RawMessage* raw_msg = pool.acquire();
        ASSERT_NE(raw_msg, nullptr);
        RawMessageGuard guard(raw_msg, &pool);

        // Can access message through guard
        guard->timestamp_ns = 999;
        EXPECT_EQ(guard->timestamp_ns, 999);

        auto stats = pool.get_stats();
        EXPECT_EQ(stats.used, 1);

        // Guard destructor will release
    }

    // Message should be released
    auto stats = pool.get_stats();
    EXPECT_EQ(stats.used, 0);
    EXPECT_EQ(stats.total_deallocations, 1);
}

// Test RAII guard move semantics
TEST_F(RawMessagePoolTest, RAIIGuardMoveSemantics) {
    RawMessagePool pool(TEST_CAPACITY);

    RawMessage* raw_msg = pool.acquire();
    ASSERT_NE(raw_msg, nullptr);

    // Test move constructor
    {
        RawMessageGuard guard1(raw_msg, &pool);
        RawMessageGuard guard2(std::move(guard1));

        EXPECT_EQ(guard1.get(), nullptr);
        EXPECT_EQ(guard2.get(), raw_msg);

        // guard2 destructor will release
    }

    auto stats = pool.get_stats();
    EXPECT_EQ(stats.used, 0);
}

// Test thread-local pool management
TEST_F(RawMessagePoolTest, ThreadLocalPool) {
    // Configure capacity
    set_thread_pool_capacity(8);

    // Get thread-local pool
    RawMessagePool& pool = get_thread_pool();
    EXPECT_EQ(pool.capacity(), 8);

    // Acquire and release
    RawMessage* msg = pool.acquire();
    ASSERT_NE(msg, nullptr);
    EXPECT_TRUE(pool.release(msg));

    // Same pool on subsequent calls
    RawMessagePool& pool2 = get_thread_pool();
    EXPECT_EQ(&pool, &pool2);
}

// Test invalid release attempts
TEST_F(RawMessagePoolTest, InvalidRelease) {
    RawMessagePool pool(TEST_CAPACITY);

    // Null pointer - returns false but doesn't count as failure
    EXPECT_FALSE(pool.release(nullptr));

    // Pointer not from this pool - counts as failure
    RawMessage fake_msg;
    EXPECT_FALSE(pool.release(&fake_msg));

    auto stats = pool.get_stats();
    // Only the non-null invalid pointer counts as a failure
    EXPECT_EQ(stats.release_failures, 1);
}

// Test multiple acquire/release cycles
TEST_F(RawMessagePoolTest, MultipleCycles) {
    RawMessagePool pool(4);

    for (int cycle = 0; cycle < 10; ++cycle) {
        std::vector<RawMessage*> messages;

        // Acquire all
        for (size_t i = 0; i < 4; ++i) {
            RawMessage* msg = pool.acquire();
            ASSERT_NE(msg, nullptr) << "Failed on cycle " << cycle;
            messages.push_back(msg);
        }

        // Release all
        for (auto* msg : messages) {
            EXPECT_TRUE(pool.release(msg));
        }
    }

    auto stats = pool.get_stats();
    EXPECT_EQ(stats.total_allocations, 40);
    EXPECT_EQ(stats.total_deallocations, 40);
    EXPECT_EQ(stats.used, 0);
}

// Test prefetching (basic - just ensure it doesn't crash)
TEST_F(RawMessagePoolTest, PrefetchNext) {
    RawMessagePool pool(TEST_CAPACITY);

    pool.prefetch_next();  // Should not crash on empty pool

    RawMessage* msg = pool.acquire();
    ASSERT_NE(msg, nullptr);

    pool.prefetch_next();  // Should not crash with allocated message

    EXPECT_TRUE(pool.release(msg));
    pool.prefetch_next();  // Should not crash after release
}