// The time-shift scrub bar's arithmetic, models/timeshift_scrub.h.
//
// EVERY CASE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the rest
// of ui/tests follows.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "models/timeshift_scrub.h"

using revenant::ui::timeshift_label;
using revenant::ui::timeshift_position;
using revenant::ui::timeshift_seconds_ago;
using revenant::ui::timeshift_target_ns;

// Rejects a slider with live at the left, which would run time the opposite
// way to the waterfall above it, and rejects a position past either end
// reaching further back than what is held.
TEST_CASE("the scrub bar runs from the oldest sample on the left to live on the right",
          "[timeshift]")
{
    CHECK(timeshift_seconds_ago(1.0, 120.0) == 0.0);
    CHECK(timeshift_seconds_ago(0.0, 120.0) == 120.0);
    CHECK(timeshift_seconds_ago(0.5, 120.0) == 60.0);
    CHECK(timeshift_seconds_ago(-3.0, 120.0) == 120.0);
    CHECK(timeshift_seconds_ago(7.0, 120.0) == 0.0);
}

// Rejects starting a second graph for a drag released a hair from the right
// end, and rejects any answer at all when nothing is held.
TEST_CASE("a position near live is live, and nothing held is always live", "[timeshift]")
{
    CHECK(timeshift_seconds_ago(0.998, 120.0) == 0.0);
    CHECK(timeshift_seconds_ago(0.99, 120.0) > 1.0);
    CHECK(timeshift_seconds_ago(0.2, 0.0) == 0.0);
    CHECK(timeshift_seconds_ago(std::numeric_limits<double>::quiet_NaN(), 120.0) == 0.0);
}

// Rejects a handle drawn from a different mapping than the one a drag uses,
// which would make the handle jump on release.
TEST_CASE("the handle is drawn where the drag would have put it", "[timeshift]")
{
    for (const double ago : {1.0, 30.0, 90.0, 120.0}) {
        CHECK(std::abs(timeshift_seconds_ago(timeshift_position(ago, 120.0), 120.0) - ago) <
              1e-9);
    }
    CHECK(timeshift_position(0.0, 120.0) == 1.0);
    CHECK(timeshift_position(500.0, 120.0) == 0.0);
}

// Rejects a label with the seconds unpadded ("-1:5") and one that truncates
// rather than rounds, which would read -0:59 for a replay a minute back.
TEST_CASE("the label reads live or minutes and seconds behind it", "[timeshift]")
{
    CHECK(timeshift_label(0.0) == "live");
    CHECK(timeshift_label(0.3) == "live");
    CHECK(timeshift_label(5.0) == "-0:05");
    CHECK(timeshift_label(59.7) == "-1:00");
    CHECK(timeshift_label(120.0) == "-2:00");
    CHECK(timeshift_label(65.2) == "-1:05");
}

TEST_CASE("the replay starts that many seconds before the newest sample", "[timeshift]")
{
    CHECK(timeshift_target_ns(10'000'000'000, 2.5) == 7'500'000'000);
    CHECK(timeshift_target_ns(10'000'000'000, 0.0) == 10'000'000'000);
}
