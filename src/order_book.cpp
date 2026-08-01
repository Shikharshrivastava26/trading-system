#include "order_book.hpp"

#include <sstream>
#include <iomanip>

namespace me {

void OrderBook::add_resting_order(Order o) {
    uint64_t id = o.order_id;
    Side     s  = o.side;
    double   p  = o.price;

    if (s == Side::BUY) {
        bids_[p].push_back(std::move(o));
    } else {
        asks_[p].push_back(std::move(o));
    }

    cancel_index_[id] = {s, p};
}

bool OrderBook::cancel_order(uint64_t order_id) {
    auto it = cancel_index_.find(order_id);
    if (it == cancel_index_.end()) return false;

    Side   side  = it->second.side;
    double price = it->second.price;

    if (side == Side::BUY) {
        auto level_it = bids_.find(price);
        if (level_it != bids_.end()) {
            auto& dq = level_it->second;
            for (auto oi = dq.begin(); oi != dq.end(); ++oi) {
                if (oi->order_id == order_id) {
                    dq.erase(oi);
                    if (dq.empty()) bids_.erase(level_it);
                    break;
                }
            }
        }
    } else {
        auto level_it = asks_.find(price);
        if (level_it != asks_.end()) {
            auto& dq = level_it->second;
            for (auto oi = dq.begin(); oi != dq.end(); ++oi) {
                if (oi->order_id == order_id) {
                    dq.erase(oi);
                    if (dq.empty()) asks_.erase(level_it);
                    break;
                }
            }
        }
    }

    cancel_index_.erase(it);
    return true;
}

std::optional<double> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<double> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

bool OrderBook::empty_side(Side s) const {
    return s == Side::BUY ? bids_.empty() : asks_.empty();
}

void OrderBook::remove_from_index(uint64_t order_id) {
    cancel_index_.erase(order_id);
}

std::string OrderBook::dump() const {
    std::ostringstream os;
    os << std::fixed << std::setprecision(2);

    os << "=== ASKS (low -> high, best first) ===\n";
    for (auto it = asks_.begin(); it != asks_.end(); ++it) {
        os << "  " << it->first << ": ";
        for (const auto& o : it->second) {
            os << "[id=" << o.order_id << " qty=" << o.remaining_quantity
               << " ts=" << o.timestamp << "] ";
        }
        os << "\n";
    }

    os << "=== BIDS (high -> low, best first) ===\n";
    for (auto it = bids_.begin(); it != bids_.end(); ++it) {
        os << "  " << it->first << ": ";
        for (const auto& o : it->second) {
            os << "[id=" << o.order_id << " qty=" << o.remaining_quantity
               << " ts=" << o.timestamp << "] ";
        }
        os << "\n";
    }

    return os.str();
}

} // namespace me
