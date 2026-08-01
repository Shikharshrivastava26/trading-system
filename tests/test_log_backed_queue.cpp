#include "log_backed_order_queue.hpp"

#include <cstdio>
#include <gtest/gtest.h>
#include <memory>
#include <string>

using namespace me;

namespace {

// Fresh base path per test, cleaned up on both sides of the run.
struct TempLog {
    std::string base;
    explicit TempLog(const std::string& name) : base("/tmp/me_wal_" + name) {
        cleanup();
    }
    ~TempLog() { cleanup(); }
    void cleanup() const {
        std::remove((base + ".log").c_str());
        std::remove((base + ".offset").c_str());
    }
};

Order make_order(uint64_t id) {
    Order o;
    o.order_id = id;
    o.quantity = o.remaining_quantity = 10;
    o.price    = 100.0;
    return o;
}

} // namespace

// Exit criterion for Phase 1: kill the consumer mid-stream, restart against
// the same files, and resume from the last committed offset with zero
// orders lost or duplicated.
TEST(LogBackedOrderQueue, ResumesAfterSimulatedCrash) {
    TempLog t("resume");

    {
        LogBackedOrderQueue queue(t.base);
        for (uint64_t i = 1; i <= 5; ++i) queue.push(make_order(i));

        Order out;
        ASSERT_TRUE(queue.pop(out));
        EXPECT_EQ(out.order_id, 1u);
        ASSERT_TRUE(queue.pop(out));
        EXPECT_EQ(out.order_id, 2u);
        // No shutdown() call: simulates the process being killed here,
        // with 2 orders consumed and 3 still unread.
    }

    // Restart against the same log + checkpoint files.
    LogBackedOrderQueue queue(t.base);
    EXPECT_EQ(queue.write_offset(), 5u);
    EXPECT_EQ(queue.consumer_offset(), 2u);

    Order out;
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.order_id, 3u);
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.order_id, 4u);
    ASSERT_TRUE(queue.pop(out));
    EXPECT_EQ(out.order_id, 5u);

    queue.shutdown();
    EXPECT_FALSE(queue.pop(out));  // drained, no phantom 6th order
}

TEST(LogBackedOrderQueue, PushThenPopInOrder) {
    TempLog t("order");
    LogBackedOrderQueue queue(t.base);

    for (uint64_t i = 1; i <= 100; ++i) queue.push(make_order(i));

    for (uint64_t i = 1; i <= 100; ++i) {
        Order out;
        ASSERT_TRUE(queue.pop(out));
        EXPECT_EQ(out.order_id, i);
    }
}

TEST(LogBackedOrderQueue, RestartWithNothingConsumedYetReplaysEverything) {
    TempLog t("fresh");

    {
        LogBackedOrderQueue queue(t.base);
        for (uint64_t i = 1; i <= 3; ++i) queue.push(make_order(i));
        // Crash before a single pop().
    }

    LogBackedOrderQueue queue(t.base);
    EXPECT_EQ(queue.consumer_offset(), 0u);
    for (uint64_t i = 1; i <= 3; ++i) {
        Order out;
        ASSERT_TRUE(queue.pop(out));
        EXPECT_EQ(out.order_id, i);
    }
}
