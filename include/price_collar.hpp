#pragma once

#include <cstdint>
#include <optional>

namespace me {

enum class RejectReason : uint8_t {
    NONE,
    PRICE_TOO_HIGH,
    PRICE_TOO_LOW,
};

struct CollarResult {
    bool         accepted = true;
    RejectReason reason   = RejectReason::NONE;
};

class PriceCollar {
public:
    // band_percent: e.g. 10.0 for ±10%
    explicit PriceCollar(double band_percent);

    // Check whether order_price is within the band of last_price.
    // If last_price is nullopt (no trade yet), collar is inactive — accept all.
    // MARKET orders (price ignored) should not be passed to this; caller bypasses.
    [[nodiscard]] CollarResult check(double order_price,
                                     std::optional<double> last_price) const;

private:
    double band_fraction_;  // band_percent / 100.0
};

} // namespace me
