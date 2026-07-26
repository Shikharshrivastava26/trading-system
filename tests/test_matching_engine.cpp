#include "matching_engine.hpp"
#include <gtest/gtest.h>

using namespace me;

static Order make(uint64_t id, Side side, OrderType type,
                  double price, uint64_t qty) {
    Order o;
    o.order_id = id;
    o.side     = side;
    o.type     = type;
    o.price    = price;
    o.quantity = qty;
    return o;
}

// Helper: submit and expect trades (not rejection).
static std::vector<Trade> submit_ok(MatchingEngine& eng, Order o) {
    auto result = eng.submit_order(std::move(o));
    EXPECT_TRUE(std::holds_alternative<std::vector<Trade>>(result));
    return std::get<std::vector<Trade>>(result);
}

// ---- Test 1: Exact price match ----
TEST(Matching, ExactPriceMatch) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_EQ(trades[0].buy_order_id, 2u);
    EXPECT_EQ(trades[0].sell_order_id, 1u);
    EXPECT_TRUE(eng.book().empty_side(Side::BUY));
    EXPECT_TRUE(eng.book().empty_side(Side::SELL));
}

// ---- Test 2: Price priority ----
TEST(Matching, PricePriority) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 101.0, 10));
    submit_ok(eng, make(2, Side::SELL, OrderType::LIMIT, 100.0, 10));

    auto trades = submit_ok(eng, make(3, Side::BUY, OrderType::LIMIT, 101.0, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_order_id, 2u);
    EXPECT_EQ(trades[0].price, 100.0);
}

// ---- Test 3: Time priority ----
TEST(Matching, TimePriority) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    submit_ok(eng, make(2, Side::SELL, OrderType::LIMIT, 100.0, 10));

    auto trades = submit_ok(eng, make(3, Side::BUY, OrderType::LIMIT, 100.0, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].sell_order_id, 1u);
}

// ---- Test 4: Partial fill ----
TEST(Matching, PartialFill) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 20));
    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 10u);
    EXPECT_FALSE(eng.book().empty_side(Side::SELL));
    EXPECT_EQ(eng.book().asks().begin()->second.front().remaining_quantity, 10u);
}

// ---- Test 5: Post-partial book state (no further cross) ----
TEST(Matching, PostPartialNoCross) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 20));
    submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));

    auto bb = eng.book().best_bid();
    auto ba = eng.book().best_ask();
    EXPECT_FALSE(bb.has_value());
    EXPECT_TRUE(ba.has_value());
}

// ---- Test 6: Multi-level walk ----
TEST(Matching, MultiLevelWalk) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 5));
    submit_ok(eng, make(2, Side::SELL, OrderType::LIMIT, 101.0, 5));
    submit_ok(eng, make(3, Side::SELL, OrderType::LIMIT, 102.0, 5));

    auto trades = submit_ok(eng, make(4, Side::BUY, OrderType::LIMIT, 102.0, 15));
    ASSERT_EQ(trades.size(), 3u);
    EXPECT_EQ(trades[0].price, 100.0);
    EXPECT_EQ(trades[1].price, 101.0);
    EXPECT_EQ(trades[2].price, 102.0);
    EXPECT_TRUE(eng.book().empty_side(Side::SELL));
}

// ---- Test 7: Price improvement ----
TEST(Matching, PriceImprovement) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 105.0, 10));

    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100.0);
}

// ---- Test 8: Cancel ----
TEST(Matching, Cancel) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    EXPECT_TRUE(eng.cancel_order(1));
    EXPECT_TRUE(eng.book().empty_side(Side::SELL));

    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));
    EXPECT_TRUE(trades.empty());
}

// ---- Test 9: Collar reject ----
TEST(Collar, Reject) {
    MatchingEngine eng(10.0);  // ±10% band

    // Establish a last_price of 100.
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));
    ASSERT_EQ(eng.last_price().value(), 100.0);

    // Order at 115 is >10% above 100 — should be rejected.
    auto result = eng.submit_order(make(3, Side::BUY, OrderType::LIMIT, 115.0, 10));
    ASSERT_TRUE(std::holds_alternative<Rejection>(result));
    EXPECT_EQ(std::get<Rejection>(result).reason, RejectReason::PRICE_TOO_HIGH);

    // Order at 85 is >10% below 100 — should be rejected.
    result = eng.submit_order(make(4, Side::SELL, OrderType::LIMIT, 85.0, 10));
    ASSERT_TRUE(std::holds_alternative<Rejection>(result));
    EXPECT_EQ(std::get<Rejection>(result).reason, RejectReason::PRICE_TOO_LOW);

    // Rejected orders must NOT be in the book.
    EXPECT_TRUE(eng.book().empty_side(Side::BUY));
    EXPECT_TRUE(eng.book().empty_side(Side::SELL));
}

// ---- Test 10: Collar accept ----
TEST(Collar, Accept) {
    MatchingEngine eng(10.0);

    // Establish last_price = 100.
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));

    // 109 is within ±10% of 100 — should be accepted and rest.
    auto trades = submit_ok(eng, make(3, Side::BUY, OrderType::LIMIT, 109.0, 5));
    EXPECT_TRUE(trades.empty());  // no matching ask
    EXPECT_FALSE(eng.book().empty_side(Side::BUY));
}

// ---- Collar: inactive before first trade ----
TEST(Collar, InactiveBeforeFirstTrade) {
    MatchingEngine eng(10.0);

    // No trades yet — any price should be accepted.
    auto trades = submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 999.0, 10));
    EXPECT_TRUE(trades.empty());
    EXPECT_FALSE(eng.book().empty_side(Side::SELL));
}

// ---- Collar: MARKET orders bypass ----
TEST(Collar, MarketBypassesCollar) {
    MatchingEngine eng(10.0);

    // Establish last_price = 100.
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 100.0, 10));

    // Add a resting ask far from last_price.
    submit_ok(eng, make(3, Side::SELL, OrderType::LIMIT, 100.0, 5));

    // MARKET buy should not be collar-checked.
    auto trades = submit_ok(eng, make(4, Side::BUY, OrderType::MARKET, 0.0, 5));
    ASSERT_EQ(trades.size(), 1u);
}

// ---- Invariant: Quantity conservation ----
TEST(Invariants, QuantityConservation) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 30));
    submit_ok(eng, make(2, Side::SELL, OrderType::LIMIT, 101.0, 20));

    auto trades = submit_ok(eng, make(3, Side::BUY, OrderType::LIMIT, 101.0, 40));
    uint64_t total = 0;
    for (const auto& t : trades) total += t.quantity;
    EXPECT_EQ(total, 40u);
}

// ---- Invariant: No crossed book ----
TEST(Invariants, NoCrossedBook) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 99.0, 10));
    submit_ok(eng, make(3, Side::BUY, OrderType::LIMIT, 100.0, 5));

    auto bb = eng.book().best_bid();
    auto ba = eng.book().best_ask();
    if (bb.has_value() && ba.has_value()) {
        EXPECT_LT(bb.value(), ba.value());
    }
}

// ---- MARKET order tests ----
TEST(Matching, MarketOrderFill) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 10));
    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::MARKET, 0.0, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, 100.0);
}

TEST(Matching, MarketOrderRemainderDiscarded) {
    MatchingEngine eng;
    submit_ok(eng, make(1, Side::SELL, OrderType::LIMIT, 100.0, 5));
    auto trades = submit_ok(eng, make(2, Side::BUY, OrderType::MARKET, 0.0, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].quantity, 5u);
    EXPECT_TRUE(eng.book().empty_side(Side::BUY));
    EXPECT_TRUE(eng.book().empty_side(Side::SELL));
}

// ---- Manual test: 2 buyers, 1 seller ----
// Buyer 1: BUY 10 @ 100
// Buyer 2: BUY 15 @ 101
// Seller 1: SELL 10 @ 100
// Expected: Seller matches Buyer 2 first (best bid 101), fills 10 @ 101
TEST(Matching, TwoBuyersOneSeller) {
    MatchingEngine eng;

    // Two buyers rest on the book
    auto t1 = submit_ok(eng, make(1, Side::BUY, OrderType::LIMIT, 100.0, 10));
    EXPECT_TRUE(t1.empty());

    auto t2 = submit_ok(eng, make(2, Side::BUY, OrderType::LIMIT, 101.0, 15));
    EXPECT_TRUE(t2.empty());

    // Seller comes in at 100 — should match best bid (101) first
    auto trades = submit_ok(eng, make(3, Side::SELL, OrderType::LIMIT, 100.0, 10));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].buy_order_id, 2u);   // matched buyer 2 (best bid)
    EXPECT_EQ(trades[0].sell_order_id, 3u);
    EXPECT_EQ(trades[0].price, 101.0);        // trades at resting order price
    EXPECT_EQ(trades[0].quantity, 10u);

    // Buyer 1 (10 @ 100) still resting, Buyer 2 has 5 remaining @ 101
    EXPECT_FALSE(eng.book().empty_side(Side::BUY));
    EXPECT_EQ(eng.book().best_bid().value(), 101.0);
}
