#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace me {

enum class Side : uint8_t { BUY, SELL };
enum class OrderType : uint8_t { LIMIT, MARKET };

// Fixed-size symbol so Order stays POD (needed for raw fwrite in
// LogBackedOrderQueue and for the fixed-size wire format). 7 chars + NUL is
// enough for any real ticker (e.g. "GOOGL", "RELIANCE" needs 8 — bump if a
// longer symbol shows up).
constexpr size_t kSymbolLen = 8;

struct Order {
    uint64_t  order_id           = 0;
    uint64_t  client_id          = 0;
    Side      side               = Side::BUY;
    OrderType type               = OrderType::LIMIT;
    char      symbol[kSymbolLen] = {};    // NUL-padded, not necessarily NUL-terminated if 8 chars exactly
    double    price              = 0.0;   // ignored for MARKET orders
    uint64_t  quantity           = 0;
    uint64_t  remaining_quantity = 0;
    uint64_t  timestamp          = 0;     // monotonic sequence number, not wall clock

    void set_symbol(const std::string& s) {
        std::memset(symbol, 0, kSymbolLen);
        std::memcpy(symbol, s.data(), std::min(s.size(), kSymbolLen));
    }

    [[nodiscard]] std::string symbol_str() const {
        return std::string(symbol, ::strnlen(symbol, kSymbolLen));
    }
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
