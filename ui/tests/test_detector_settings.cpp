// What the operator set on the detector, across a restart of this window and
// a restart of the engine. models/detector_settings.h.
//
// The tune the owner reported on 2026-09-23 is the engine's half and is held
// in tests/rpc/test_rpc_detect.cpp, "the detection threshold outlives the
// detector a retune throws away". These are this window's half: what it reads
// back at startup and what it sends when it finds an engine.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <optional>

#include "models/detector_settings.h"

using revenant::ui::DetectorMemory;
using revenant::ui::restore_detection_bar;
using revenant::ui::restore_detection_threshold;
using revenant::ui::restore_detector_memory;
using revenant::ui::threshold_for_connection;
using revenant::ui::threshold_overridden;
using revenant::ui::threshold_wanted;

namespace {

// The largest double below one, which is the engine's own top for either bar
// and what EngineLink passes.
const double kTop = std::nextafter(1.0, 0.0);

}  // namespace

// Rejects the margin bar starting at zero on every launch, which is what it
// did until 2026-09-23, and the threshold being left to whatever the engine
// had. All three come back as the operator left them.
TEST_CASE("the three settings come back after a restart", "[detector]")
{
    const DetectorMemory memory = restore_detector_memory(0.40, 0.72, 11.5, kTop);
    CHECK(memory.confidence_bar == 0.40);
    CHECK(memory.margin_bar == 0.72);
    REQUIRE(memory.threshold_db.has_value());
    CHECK(*memory.threshold_db == 11.5);
}

// Rejects a first run imposing a number on an engine: until the operator has
// set a threshold from this window there is nothing to send, and the engine's
// own value, which another client may have set, stands.
TEST_CASE("a window that never set a threshold sends none", "[detector]")
{
    const DetectorMemory memory =
        restore_detector_memory(std::nullopt, std::nullopt, std::nullopt, kTop);
    CHECK(memory.confidence_bar == 0.0);
    CHECK(memory.margin_bar == 0.0);
    CHECK_FALSE(threshold_for_connection(memory).has_value());
    CHECK(threshold_wanted(memory, 6.0) == 6.0);
    CHECK_FALSE(threshold_overridden(memory, 6.0, true));
}

// Rejects sending the remembered threshold only on the first connection. A
// restarted engine is a new connection from here and starts at 6 dB, so the
// operator's value goes again on every one.
TEST_CASE("every connection carries the operator's threshold", "[detector]")
{
    const DetectorMemory memory = restore_detector_memory(0.0, 0.0, 14.0, kTop);
    for (int connection = 0; connection < 3; ++connection) {
        const auto sent = threshold_for_connection(memory);
        REQUIRE(sent.has_value());
        CHECK(*sent == 14.0);
    }
}

// Rejects trusting a settings file. It is editable, and a bar of 2 read
// straight in would refuse every poll of the session; a threshold of 500
// clamped to 120 would be a detector finding nothing, which is a claim about
// the band nobody made, so that is dropped rather than clamped.
TEST_CASE("an edited settings file cannot refuse every poll", "[detector]")
{
    CHECK(restore_detection_bar(2.0, kTop) == kTop);
    CHECK(restore_detection_bar(-1.0, kTop) == 0.0);
    CHECK(restore_detection_bar(std::numeric_limits<double>::quiet_NaN(), kTop) == 0.0);

    CHECK_FALSE(restore_detection_threshold(500.0).has_value());
    CHECK_FALSE(restore_detection_threshold(-500.0).has_value());
    CHECK_FALSE(restore_detection_threshold(std::numeric_limits<double>::infinity()).has_value());
    CHECK_FALSE(restore_detection_threshold(std::numeric_limits<double>::quiet_NaN()).has_value());
    REQUIRE(restore_detection_threshold(-60.0).has_value());
    REQUIRE(restore_detection_threshold(120.0).has_value());
}

// Rejects the handle following the value in force once the operator has
// chosen one: another client's write is shown as another client's, beside a
// handle that stays where this operator put it.
TEST_CASE("the handle is the operator's and the difference is reported", "[detector]")
{
    const DetectorMemory memory = restore_detector_memory(0.0, 0.0, 11.5, kTop);
    CHECK(threshold_wanted(memory, 6.0) == 11.5);

    // Not before the detector has decided: the write goes out ahead of the
    // first poll, and a list from before it is not another client.
    CHECK_FALSE(threshold_overridden(memory, 6.0, false));
    CHECK(threshold_overridden(memory, 6.0, true));

    // Within the control's own step is the same value.
    CHECK_FALSE(threshold_overridden(memory, 11.5, true));
    CHECK_FALSE(threshold_overridden(memory, 12.0, true));
}
