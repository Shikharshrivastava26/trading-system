#pragma once

#include <cstdint>
#include <string>

namespace me {

enum class Side : uint8_t { BUY, SELL };
enum class OrderType : uint8_t { LIMIT, MARKET };

struct Order {
    uint64_t  order_id           = 0;
    uint64_t  client_id          = 0;
    Side      side               = Side::BUY;
    OrderType type               = OrderType::LIMIT;
    double    price              = 0.0;   // ignored for MARKET orders
    uint64_t  quantity           = 0;
    uint64_t  remaining_quantity = 0;
    uint64_t  timestamp          = 0;     // monotonic sequence number, not wall clock
};

struct Trade {
    uint64_t buy_order_id  = 0;
    uint64_t sell_order_id = 0;
    double   price         = 0.0;   // always the resting order's price
    uint64_t quantity      = 0;
    uint64_t timestamp     = 0;
};

// Helpers for display
[[nodiscard]] inline const char* to_string(Side s) {
    return s == Side::BUY ? "BUY" : "SELL";
}

[[nodiscard]] inline const char* to_string(OrderType t) {
    return t == OrderType::LIMIT ? "LIMIT" : "MARKET";
}

} // namespace me
