#include "price_collar.hpp"

#include <cmath>

namespace me {

PriceCollar::PriceCollar(double band_percent)
    : band_fraction_(band_percent / 100.0) {}

CollarResult PriceCollar::check(double order_price,
                                std::optional<double> last_price) const {
    // No trade yet — collar inactive.
    if (!last_price.has_value())
        return {true, RejectReason::NONE};

    double ref  = last_price.value();
    double band = ref * band_fraction_;
    double lo   = ref - band;
    double hi   = ref + band;

    if (order_price > hi)
        return {false, RejectReason::PRICE_TOO_HIGH};
    if (order_price < lo)
        return {false, RejectReason::PRICE_TOO_LOW};

    return {true, RejectReason::NONE};
}

} // namespace me
