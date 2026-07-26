#include "matching_engine.hpp"

#include <algorithm>

namespace me {

MatchingEngine::MatchingEngine(double collar_band_percent)
    : collar_(collar_band_percent) {}

template <typename MapType>
void MatchingEngine::match_against(Order& incoming, MapType& opposite,
                                   std::vector<Trade>& trades) {
    while (incoming.remaining_quantity > 0 && !opposite.empty()) {
        auto best_it = opposite.begin();
        double resting_price = best_it->first;

        if (incoming.type == OrderType::LIMIT) {
            if (incoming.side == Side::BUY && incoming.price < resting_price)
                break;
            if (incoming.side == Side::SELL && incoming.price > resting_price)
                break;
        }

        auto& dq = best_it->second;
        Order& front = dq.front();

        uint64_t fill = std::min(incoming.remaining_quantity,
                                 front.remaining_quantity);

        Trade t;
        t.price    = front.price;
        t.quantity = fill;
        t.timestamp = next_timestamp_++;

        if (incoming.side == Side::BUY) {
            t.buy_order_id  = incoming.order_id;
            t.sell_order_id = front.order_id;
        } else {
            t.buy_order_id  = front.order_id;
            t.sell_order_id = incoming.order_id;
        }

        trades.push_back(t);

        incoming.remaining_quantity -= fill;
        front.remaining_quantity    -= fill;
        last_price_ = front.price;

        if (front.remaining_quantity == 0) {
            book_.remove_from_index(front.order_id);
            dq.pop_front();
            if (dq.empty()) {
                opposite.erase(best_it);
            }
        }
    }
}

SubmitResult MatchingEngine::submit_order(Order incoming) {
    incoming.timestamp          = next_timestamp_++;
    incoming.remaining_quantity = incoming.quantity;

    // MARKET orders bypass the collar (no price to check).
    if (incoming.type == OrderType::LIMIT) {
        auto result = collar_.check(incoming.price, last_price_);
        if (!result.accepted) {
            return Rejection{incoming.order_id, result.reason};
        }
    }

    std::vector<Trade> trades;

    if (incoming.side == Side::BUY) {
        match_against(incoming, book_.asks(), trades);
    } else {
        match_against(incoming, book_.bids(), trades);
    }

    if (incoming.remaining_quantity > 0 && incoming.type == OrderType::LIMIT) {
        book_.add_resting_order(std::move(incoming));
    }

    return trades;
}

bool MatchingEngine::cancel_order(uint64_t order_id) {
    return book_.cancel_order(order_id);
}

std::optional<double> MatchingEngine::last_price() const {
    return last_price_;
}

template void MatchingEngine::match_against(Order&, OrderBook::AskMap&,
                                            std::vector<Trade>&);
template void MatchingEngine::match_against(Order&, OrderBook::BidMap&,
                                            std::vector<Trade>&);

} // namespace me
