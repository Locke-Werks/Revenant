// The audio section's trim readout and its two counter chips.
//
// Each case names the wrong implementation it rejects.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>

#include "audio/audio_mix.h"
#include "audio/drift_trim.h"
#include "models/audio_counters.h"

using revenant::ui::AudioMix;
using revenant::ui::DriftTrim;
using revenant::ui::format_trim_ppm;
using revenant::ui::kTrimWidest;
using revenant::ui::late_skipped_detail;
using revenant::ui::realigned_detail;
using revenant::ui::show_late_skipped;
using revenant::ui::show_realigned;
using revenant::ui::skipped_millis;
using revenant::ui::SkippedFrames;

TEST_CASE("the trim reads signed to a tenth of a ppm", "[audio][counters]")
{
    // REJECTS: printing the sign only when negative, which moves every
    // character of a right-aligned readout each time the trim crosses zero,
    // and printing "-0.0" for a trim that rounds to nothing.
    CHECK(format_trim_ppm(0.0) == "+0.0 ppm");
    CHECK(format_trim_ppm(-0.04) == "+0.0 ppm");
    CHECK(format_trim_ppm(12.34) == "+12.3 ppm");
    CHECK(format_trim_ppm(-99.96) == "-100.0 ppm");
    CHECK(format_trim_ppm(-DriftTrim::kLimit * 1e6) == std::string(kTrimWidest));
    CHECK(format_trim_ppm(DriftTrim::kLimit * 1e6).size() == kTrimWidest.size());
}

TEST_CASE("late audio is summed in time, each stream at its own rate", "[audio][counters]")
{
    // REJECTS: summing frames across receivers, which counts a P25
    // receiver's 8000 S/s frame as a sixth of the time it is.
    const std::array<SkippedFrames, 2> streams{SkippedFrames{480, 48'000},
                                               SkippedFrames{80, 8'000}};
    CHECK(skipped_millis(streams) == 20.0);
    CHECK(skipped_millis(std::array<SkippedFrames, 1>{SkippedFrames{5, 0}}) == 0.0);
}

TEST_CASE("the counter chips exist only while there is something to say", "[audio][counters]")
{
    CHECK_FALSE(show_late_skipped(0.0));
    CHECK(show_late_skipped(0.1));
    CHECK_FALSE(show_realigned(0));
    CHECK(show_realigned(1));

    CHECK(late_skipped_detail(40.0).rfind("40 ms of audio", 0) == 0);
    CHECK(realigned_detail(1).find("1 time since") != std::string::npos);
    CHECK(realigned_detail(3).find("3 times since") != std::string::npos);

    // The sentence names the mix's own tolerance.
    CHECK(AudioMix::kAlignTolerance == 0.005);
    CHECK(realigned_detail(2).find("more than 5 ms off") != std::string::npos);
}
