#include "matching_engine.hpp"
#include "sharding.hpp"
#include "shard_router_queue.hpp"

#include <cstdio>
#include <gtest/gtest.h>
#include <unordered_map>
#include <variant>

using namespace me;

namespace {

struct TempShards {
    std::string base;
    uint32_t    num_shards;
    TempShards(const std::string& name, uint32_t n) : base("/tmp/me_shard_" + name), num_shards(n) {
        cleanup();
    }
    ~TempShards() { cleanup(); }
    void cleanup() const {
        for (uint32_t i = 0; i < num_shards; ++i) {
            std::remove((base + "_shard" + std::to_string(i) + ".log").c_str());
            std::remove((base + "_shard" + std::to_string(i) + ".offset").c_str());
        }
    }
};

Order make_order(uint64_t id, const std::string& symbol, Side side, double price, uint64_t qty) {
    Order o;
    o.order_id = id;
    o.side     = side;
    o.type     = OrderType::LIMIT;
    o.price    = price;
    o.quantity = o.remaining_quantity = qty;
    o.set_symbol(symbol);
    return o;
}

} // namespace

TEST(Sharding, SameSymbolAlwaysMapsToSameShard) {
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(shard_for_symbol("TATA", 8), shard_for_symbol("TATA", 8));
    }
}

TEST(Sharding, DeterministicAcrossFreshProcessesConceptually) {
    // fnv1a is a pure function of the bytes — no process/run-specific seed —
    // so two independent calls (standing in for gateway process vs engine
    // process) must agree without any coordination.
    uint32_t a = shard_for_symbol("XYZ", 10);
    uint32_t b = shard_for_symbol("XYZ", 10);
    EXPECT_EQ(a, b);
    EXPECT_LT(a, 10u);
}

// Exit criterion for Phase 3: a buy and a sell for the same symbol, entering
// via what stand in for two different gateways, must land on the same
// shard's log — and orders for a different symbol must land elsewhere.
TEST(ShardRouterQueue, RoutesSameSymbolToSameShardAndIsolatesOthers) {
    TempShards t("route", 4);
    ShardRouterQueue router(t.base, 4);

    router.push(make_order(1, "XYZ", Side::BUY,  100.0, 10));
    router.push(make_order(2, "ABC", Side::BUY,  50.0,  20));
    router.push(make_order(3, "XYZ", Side::SELL, 100.0, 5));
    router.push(make_order(4, "ABC", Side::SELL, 49.0,  8));

    uint32_t xyz_shard = shard_for_symbol("XYZ", 4);
    uint32_t abc_shard = shard_for_symbol("ABC", 4);

    LogBackedOrderQueue xyz_log(t.base + "_shard" + std::to_string(xyz_shard));
    LogBackedOrderQueue abc_log(t.base + "_shard" + std::to_string(abc_shard));

    if (xyz_shard == abc_shard) {
        // Rare but valid hash collision for this symbol pair/shard count —
        // both orders share one log, still in arrival order.
        Order o;
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 1u);
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 2u);
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 3u);
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 4u);
    } else {
        Order o;
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 1u); EXPECT_EQ(o.symbol_str(), "XYZ");
        ASSERT_TRUE(xyz_log.pop(o)); EXPECT_EQ(o.order_id, 3u); EXPECT_EQ(o.symbol_str(), "XYZ");

        ASSERT_TRUE(abc_log.pop(o)); EXPECT_EQ(o.order_id, 2u); EXPECT_EQ(o.symbol_str(), "ABC");
        ASSERT_TRUE(abc_log.pop(o)); EXPECT_EQ(o.order_id, 4u); EXPECT_EQ(o.symbol_str(), "ABC");
    }
}

// End-to-end: route through ShardRouterQueue, then run each shard's log
// through its own MatchingEngine-per-symbol map (what engine_main.cpp does),
// and confirm each symbol matches independently with the right trades.
TEST(ShardRouterQueue, EndToEndMatchesIndependentlyPerSymbol) {
    TempShards t("e2e", 4);
    {
        ShardRouterQueue router(t.base, 4);
        router.push(make_order(1, "XYZ", Side::BUY,  100.0, 10));
        router.push(make_order(2, "ABC", Side::BUY,  50.0,  20));
        router.push(make_order(3, "XYZ", Side::SELL, 100.0, 5));
        router.push(make_order(4, "ABC", Side::SELL, 49.0,  8));
    }

    std::unordered_map<std::string, std::vector<Trade>> trades_by_symbol;

    for (uint32_t shard = 0; shard < 4; ++shard) {
        LogBackedOrderQueue queue(t.base + "_shard" + std::to_string(shard));
        std::unordered_map<std::string, MatchingEngine> engines;
        Order order;
        queue.shutdown();  // no live producer left; drain whatever is already there
        while (queue.pop(order)) {
            std::string symbol = order.symbol_str();
            auto result = engines.try_emplace(symbol).first->second.submit_order(order);
            if (auto* trades = std::get_if<std::vector<Trade>>(&result)) {
                for (auto& tr : *trades) trades_by_symbol[symbol].push_back(tr);
            }
        }
    }

    ASSERT_EQ(trades_by_symbol["XYZ"].size(), 1u);
    EXPECT_EQ(trades_by_symbol["XYZ"][0].price, 100.0);
    EXPECT_EQ(trades_by_symbol["XYZ"][0].quantity, 5u);

    ASSERT_EQ(trades_by_symbol["ABC"].size(), 1u);
    EXPECT_EQ(trades_by_symbol["ABC"][0].price, 50.0);
    EXPECT_EQ(trades_by_symbol["ABC"][0].quantity, 8u);
}
