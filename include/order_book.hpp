#pragma once

#include "order.hpp"

#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace me {

class OrderBook {
public:
    // Add an order to the resting book (back of the deque at its price level = FIFO).
    void add_resting_order(Order o);

    // Cancel an order by id.  Returns true if found and removed.
    bool cancel_order(uint64_t order_id);

    // Best prices.  nullopt when that side is empty.
    [[nodiscard]] std::optional<double> best_bid() const;
    [[nodiscard]] std::optional<double> best_ask() const;

    [[nodiscard]] bool empty_side(Side s) const;

    // Human-readable dump for development / debugging.
    [[nodiscard]] std::string dump() const;

    // ----- internals exposed for the matching engine (Phase 2) -----

    // Price-level maps.  begin() is always the best price on each side.
    using BidMap = std::map<double, std::deque<Order>, std::greater<double>>;
    using AskMap = std::map<double, std::deque<Order>>;  // std::less (default)

    BidMap& bids() { return bids_; }
    AskMap& asks() { return asks_; }

    const BidMap& bids() const { return bids_; }
    const AskMap& asks() const { return asks_; }

    // Remove the cancel-index entry for an order (called after a fill).
    void remove_from_index(uint64_t order_id);

private:
    BidMap bids_;
    AskMap asks_;

    // Cancel index: order_id -> { side, price } so cancel is O(1) lookup + O(n) deque scan.
    struct CancelInfo {
        Side   side;
        double price;
    };
    std::unordered_map<uint64_t, CancelInfo> cancel_index_;
};

} // namespace me
