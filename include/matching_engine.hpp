#pragma once

#include "order_book.hpp"
#include "price_collar.hpp"

#include <vector>
#include <atomic>
#include <variant>

namespace me {

// submit_order returns either trades or a rejection.
struct Rejection {
    uint64_t     order_id;
    RejectReason reason;
};

using SubmitResult = std::variant<std::vector<Trade>, Rejection>;

class MatchingEngine {
public:
    // band_percent: collar band, e.g. 10.0 for ±10%.
    explicit MatchingEngine(double collar_band_percent = 10.0);

    // Match the incoming order against the book.
    // Returns trades on success, or a Rejection if the collar rejects.
    SubmitResult submit_order(Order incoming);

    // Cancel a resting order. Returns true if found and removed.
    bool cancel_order(uint64_t order_id);

    // Last traded price (nullopt if no trade has occurred yet).
    [[nodiscard]] std::optional<double> last_price() const;

    // Access the book (for inspection / tests).
    [[nodiscard]] const OrderBook& book() const { return book_; }

private:
    OrderBook book_;
    PriceCollar collar_;
    std::optional<double> last_price_;
    uint64_t next_timestamp_ = 1;

    template <typename MapType>
    void match_against(Order& incoming, MapType& opposite,
                       std::vector<Trade>& trades);
};

} // namespace me
