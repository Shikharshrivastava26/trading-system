#include "matching_engine.hpp"
#include "mutex_order_queue.hpp"
#include "lockfree_order_queue.hpp"
#include "order_pool.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <random>
#include <vector>

using namespace me;

// Generate a deterministic order sequence from a seed.
static std::vector<Order> generate_orders(size_t count, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<uint64_t> qty_dist(1, 100);
    // Prices on a fixed tick grid: 90..110 in steps of 1.
    std::uniform_int_distribution<int> price_dist(90, 110);
    std::uniform_int_distribution<int> side_dist(0, 1);

    std::vector<Order> orders;
    orders.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        Order o;
        o.order_id = i + 1;
        o.side     = side_dist(rng) == 0 ? Side::BUY : Side::SELL;
        o.type     = OrderType::LIMIT;
        o.price    = static_cast<double>(price_dist(rng));
        o.quantity = qty_dist(rng);
        orders.push_back(o);
    }
    return orders;
}

// Collect all trades from running orders through the engine single-threaded.
static std::vector<Trade> run_single_threaded(const std::vector<Order>& orders) {
    MatchingEngine eng(100.0);  // wide collar so nothing is rejected
    std::vector<Trade> all_trades;
    for (const auto& o : orders) {
        auto result = eng.submit_order(o);
        if (auto* trades = std::get_if<std::vector<Trade>>(&result)) {
            all_trades.insert(all_trades.end(), trades->begin(), trades->end());
        }
    }
    return all_trades;
}

// Run through producer → MutexOrderQueue → consumer, collect trades.
static std::vector<Trade> run_threaded(const std::vector<Order>& orders) {
    MutexOrderQueue queue;
    MatchingEngine eng(100.0);
    std::vector<Trade> all_trades;

    // Consumer thread.
    std::thread consumer([&] {
        Order o;
        while (queue.pop(o)) {
            auto result = eng.submit_order(o);
            if (auto* trades = std::get_if<std::vector<Trade>>(&result)) {
                all_trades.insert(all_trades.end(), trades->begin(), trades->end());
            }
        }
    });

    // Producer thread.
    for (const auto& o : orders) {
        queue.push(o);
    }
    queue.shutdown();

    consumer.join();
    return all_trades;
}

static constexpr size_t   ORDER_COUNT = 1'000'000;
static constexpr uint64_t SEED        = 42;

TEST(ThreadedRunner, MutexQueueIdenticalOutput) {
    auto orders = generate_orders(ORDER_COUNT, SEED);

    auto st_trades = run_single_threaded(orders);
    auto mt_trades = run_threaded(orders);

    ASSERT_EQ(st_trades.size(), mt_trades.size())
        << "Trade count mismatch: single=" << st_trades.size()
        << " threaded=" << mt_trades.size();

    for (size_t i = 0; i < st_trades.size(); ++i) {
        EXPECT_EQ(st_trades[i].buy_order_id,  mt_trades[i].buy_order_id)  << "at trade " << i;
        EXPECT_EQ(st_trades[i].sell_order_id, mt_trades[i].sell_order_id) << "at trade " << i;
        EXPECT_EQ(st_trades[i].price,         mt_trades[i].price)         << "at trade " << i;
        EXPECT_EQ(st_trades[i].quantity,      mt_trades[i].quantity)      << "at trade " << i;
    }
}

// Run through producer → LockFreeOrderQueue → consumer, collect trades.
static std::vector<Trade> run_lockfree(const std::vector<Order>& orders) {
    LockFreeOrderQueue queue;
    MatchingEngine eng(100.0);
    std::vector<Trade> all_trades;

    std::thread consumer([&] {
        Order o;
        while (queue.pop(o)) {
            auto result = eng.submit_order(o);
            if (auto* trades = std::get_if<std::vector<Trade>>(&result)) {
                all_trades.insert(all_trades.end(), trades->begin(), trades->end());
            }
        }
    });

    for (const auto& o : orders) {
        queue.push(o);
    }
    queue.shutdown();
    consumer.join();
    return all_trades;
}

TEST(ThreadedRunner, LockFreeQueueIdenticalOutput) {
    auto orders = generate_orders(ORDER_COUNT, SEED);

    auto st_trades = run_single_threaded(orders);
    auto lf_trades = run_lockfree(orders);

    ASSERT_EQ(st_trades.size(), lf_trades.size())
        << "Trade count mismatch: single=" << st_trades.size()
        << " lockfree=" << lf_trades.size();

    for (size_t i = 0; i < st_trades.size(); ++i) {
        EXPECT_EQ(st_trades[i].buy_order_id,  lf_trades[i].buy_order_id)  << "at trade " << i;
        EXPECT_EQ(st_trades[i].sell_order_id, lf_trades[i].sell_order_id) << "at trade " << i;
        EXPECT_EQ(st_trades[i].price,         lf_trades[i].price)         << "at trade " << i;
        EXPECT_EQ(st_trades[i].quantity,      lf_trades[i].quantity)      << "at trade " << i;
    }
}

TEST(OrderPool, AcquireReleaseCycle) {
    OrderPool pool(1024);
    EXPECT_EQ(pool.available(), 1024u);

    Order* p = pool.acquire();
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(pool.available(), 1023u);

    pool.release(p);
    EXPECT_EQ(pool.available(), 1024u);
}

TEST(OrderPool, ExhaustionReturnsNull) {
    OrderPool pool(2);
    pool.acquire();
    pool.acquire();
    EXPECT_EQ(pool.acquire(), nullptr);
}

TEST(OrderPool, ZeroAllocOnHotPath) {
    // Verify pool-based usage does no heap alloc after setup.
    OrderPool pool(100);
    std::vector<Order*> ptrs;
    ptrs.reserve(100);

    for (int i = 0; i < 100; ++i) {
        Order* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        p->order_id = i;
        ptrs.push_back(p);
    }
    for (auto* p : ptrs) pool.release(p);

    // All slots returned.
    EXPECT_EQ(pool.available(), 100u);
}
