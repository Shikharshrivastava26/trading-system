#include "order_book.hpp"
#include <iostream>
#include <cassert>

// Phase 1 exit criterion: insert 6 orders across 3 price levels on both sides,
// dump() prints them in exact price-then-arrival order.

int main() {
    me::OrderBook book;
    uint64_t seq = 1;

    auto make_order = [&](uint64_t id, me::Side side, double price, uint64_t qty) {
        me::Order o;
        o.order_id           = id;
        o.side               = side;
        o.type               = me::OrderType::LIMIT;
        o.price              = price;
        o.quantity           = qty;
        o.remaining_quantity = qty;
        o.timestamp          = seq++;
        return o;
    };

    // 3 bid levels: 101, 100, 99  (best = 101)
    book.add_resting_order(make_order(1, me::Side::BUY, 100.0, 10));
    book.add_resting_order(make_order(2, me::Side::BUY, 101.0, 20));
    book.add_resting_order(make_order(3, me::Side::BUY, 99.0,  5));

    // 3 ask levels: 102, 103, 104  (best = 102)
    book.add_resting_order(make_order(4, me::Side::SELL, 103.0, 15));
    book.add_resting_order(make_order(5, me::Side::SELL, 102.0, 10));
    book.add_resting_order(make_order(6, me::Side::SELL, 104.0, 25));

    std::cout << book.dump() << std::endl;

    // Verify best prices
    auto bb = book.best_bid();
    auto ba = book.best_ask();
    assert(bb.has_value() && bb.value() == 101.0);
    assert(ba.has_value() && ba.value() == 102.0);

    // Verify empty_side
    assert(!book.empty_side(me::Side::BUY));
    assert(!book.empty_side(me::Side::SELL));

    // Cancel order 2 (best bid at 101) — best bid should become 100
    assert(book.cancel_order(2));
    assert(book.best_bid().value() == 100.0);

    // Cancel non-existent order
    assert(!book.cancel_order(999));

    // Cancel all bids, verify empty
    book.cancel_order(1);
    book.cancel_order(3);
    assert(book.empty_side(me::Side::BUY));
    assert(!book.best_bid().has_value());

    std::cout << "After cancelling all bids:\n" << book.dump() << std::endl;

    // FIFO test: two orders at same price, first should appear first in dump
    book.add_resting_order(make_order(10, me::Side::BUY, 100.0, 5));
    book.add_resting_order(make_order(11, me::Side::BUY, 100.0, 8));

    const auto& bids = book.bids();
    const auto& level = bids.at(100.0);
    assert(level.size() == 2);
    assert(level[0].order_id == 10);  // first in
    assert(level[1].order_id == 11);  // second in

    std::cout << "Final state:\n" << book.dump() << std::endl;
    std::cout << "ALL CHECKS PASSED\n";
    return 0;
}
