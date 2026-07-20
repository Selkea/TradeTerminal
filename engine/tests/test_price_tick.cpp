// snap_to_tick: order prices must land on a venue-legal increment or IBKR
// rejects them with error 110. Cases mirror the live log that surfaced the bug.
#include "doctest.h"

#include "engine/price_tick.h"

#include <cmath>
#include <initializer_list>

using namespace tt;

namespace {
// True when x is an exact multiple of tick to within FP noise — i.e. what the
// TWS API's bounded double encoder will serialize as a clean price string.
bool on_grid(double x, double tick) {
    const double units = x / tick;
    return std::abs(units - std::round(units)) < 1e-6;
}
} // namespace

TEST_CASE("snap_to_tick fixes the scalper's sub-penny exit prices") {
    // Longs from the log: entry * (1 + 1bp) / (1 - 1bp) produced 205.870585 etc.
    CHECK(snap_to_tick(205.85 * 1.0001) == doctest::Approx(205.87));
    CHECK(snap_to_tick(205.85 * 0.9999) == doctest::Approx(205.83));
    CHECK(snap_to_tick(384.65 * 1.0001) == doctest::Approx(384.69));
    CHECK(snap_to_tick(384.65 * 0.9999) == doctest::Approx(384.61));

    // Every result sits exactly on the penny grid (the 110 fix).
    for (double raw : {205.85 * 1.0001, 205.85 * 0.9999, 384.65 * 1.0001,
                       206.00 * 1.0001, 71.4321, 999.995}) {
        CHECK(on_grid(snap_to_tick(raw), 0.01));
    }
}

TEST_CASE("snap_to_tick: increment follows the $1 boundary and leaves MKT alone") {
    CHECK(tick_size_for(205.85) == doctest::Approx(0.01));
    CHECK(tick_size_for(0.50) == doctest::Approx(0.0001));

    // Sub-dollar snaps to 1/100 cent, not to a penny.
    CHECK(on_grid(snap_to_tick(0.123456), 0.0001));

    // Already-legal prices are unchanged; market orders carry 0.0 -> untouched.
    CHECK(snap_to_tick(205.87) == doctest::Approx(205.87));
    CHECK(snap_to_tick(0.0) == 0.0);
    CHECK(snap_to_tick(-1.0) == -1.0);
}
