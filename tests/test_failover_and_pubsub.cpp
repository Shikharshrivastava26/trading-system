#include "shard_leader_lock.hpp"
#include "trade_log.hpp"

#include <cstdio>
#include <gtest/gtest.h>
#include <memory>

using namespace me;

namespace {

struct TempPath {
    std::string path;
    explicit TempPath(const std::string& name) : path("/tmp/me_" + name) { std::remove(path.c_str()); }
    ~TempPath() { std::remove(path.c_str()); }
};

Trade make_trade(uint64_t buy, uint64_t sell, double price, uint64_t qty) {
    Trade t;
    t.buy_order_id  = buy;
    t.sell_order_id = sell;
    t.price         = price;
    t.quantity      = qty;
    return t;
}

} // namespace

// Exit criterion for Phase 4: only one replica can be active at a time, and
// when the active one dies (here: destroyed, which is what kill -9 also
// triggers at the OS level for flock), a standby's very next attempt
// succeeds — no coordinator, no manual intervention.
TEST(ShardLeaderLock, OnlyOneHolderAtATimeAndFailoverOnRelease) {
    TempPath t("leader_lock");

    auto active = std::make_unique<ShardLeaderLock>(t.path);
    ShardLeaderLock standby(t.path);

    ASSERT_TRUE(active->try_acquire());
    EXPECT_FALSE(standby.try_acquire());   // active still holds it

    // Simulate the active process dying: destroying the lock object closes
    // its fd, which releases the flock — exactly what the OS does on
    // process exit or kill -9, no cleanup code involved.
    active.reset();

    EXPECT_TRUE(standby.try_acquire());    // now promotable
}

TEST(ShardLeaderLock, RepeatedAcquireByHolderIsIdempotent) {
    TempPath t("leader_idem");
    ShardLeaderLock lock(t.path);
    EXPECT_TRUE(lock.try_acquire());
    EXPECT_TRUE(lock.try_acquire());
    EXPECT_TRUE(lock.held());
}

// Exit criterion for Phase 5: a subscriber created after some trades were
// already published still sees all of them ("historical"), and continues
// to see new ones published afterward ("live") — the same mechanism, no
// separate backfill step.
TEST(TradeLog, NewSubscriberReplaysHistoryThenSeesLiveTrades) {
    std::string base = "/tmp/me_tradelog_hist";
    std::remove((base + "_trades.log").c_str());
    std::remove((base + "_trades.sub1.offset").c_str());

    TradeLog publisher(base);
    publisher.publish(make_trade(1, 2, 100.0, 10));
    publisher.publish(make_trade(3, 4, 101.0, 5));

    // Subscriber attaches only now, after 2 trades already happened.
    TradeLog::Subscriber sub(base, "sub1");

    Trade t;
    ASSERT_TRUE(sub.try_next(t));
    EXPECT_EQ(t.buy_order_id, 1u);
    ASSERT_TRUE(sub.try_next(t));
    EXPECT_EQ(t.buy_order_id, 3u);
    EXPECT_FALSE(sub.try_next(t));  // caught up, nothing more yet

    // A new trade happens live.
    publisher.publish(make_trade(5, 6, 102.0, 7));
    ASSERT_TRUE(sub.try_next(t));
    EXPECT_EQ(t.buy_order_id, 5u);

    std::remove((base + "_trades.log").c_str());
    std::remove((base + "_trades.sub1.offset").c_str());
}

TEST(TradeLog, IndependentSubscribersDoNotAffectEachOther) {
    std::string base = "/tmp/me_tradelog_indep";
    std::remove((base + "_trades.log").c_str());
    std::remove((base + "_trades.subA.offset").c_str());
    std::remove((base + "_trades.subB.offset").c_str());

    TradeLog publisher(base);
    publisher.publish(make_trade(1, 2, 100.0, 10));

    TradeLog::Subscriber a(base, "subA");
    Trade t;
    ASSERT_TRUE(a.try_next(t));  // A reads the trade
    EXPECT_EQ(a.offset(), 1u);

    TradeLog::Subscriber b(base, "subB");
    ASSERT_TRUE(b.try_next(t));  // B independently sees the same trade
    EXPECT_EQ(b.offset(), 1u);

    std::remove((base + "_trades.log").c_str());
    std::remove((base + "_trades.subA.offset").c_str());
    std::remove((base + "_trades.subB.offset").c_str());
}
