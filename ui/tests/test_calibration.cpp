// The calibration section's rules: the PPM box, and a known carrier measured
// against the detection nearest it.
//
// The measurement cases build their detections the way the engine publishes
// them for a dongle whose crystal is off by a known amount, so the correction
// they recover is compared with the error that was put in rather than with
// another copy of the arithmetic.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "core/source/frequency_correction.h"
#include "models/calibration.h"

using revenant::rpc::Detection;
using revenant::rpc::TrackState;
using revenant::ui::format_ppm;
using revenant::ui::measure_against_carrier;
using revenant::ui::parse_ppm;

namespace {

// NOAA weather radio through a dongle 20 ppm fast, tuned 50 kHz below it.
// The device sits at 162.5 MHz on its own scale, which is 162503250 Hz
// really, and the carrier is 46749.07 Hz above that in the stream's own
// sample clock.
constexpr std::int64_t kCarrierHz = 162'550'000;
constexpr std::int64_t kErrorPpb = 20'000;
constexpr std::int64_t kDeviceHz = 162'500'000;
constexpr std::int64_t kOffsetInStream = 46'749;

// Where the engine labels the carrier with `applied` in force.
Detection carrier_seen_with(std::int64_t applied) {
    Detection detection;
    detection.id = 7;
    detection.center_hz = revenant::source::device_to_true(kDeviceHz, applied) + kOffsetInStream;
    detection.bandwidth_hz = 10'000;
    detection.state = TrackState::Live;
    return detection;
}

}  // namespace

TEST_CASE("a correction typed in ppm is read exactly, to the ppb", "[calibration]") {
    CHECK(parse_ppm("12").ppb == 12'000);
    CHECK(parse_ppm("-3.25").ppb == -3'250);
    CHECK(parse_ppm("+0.5").ppb == 500);
    CHECK(parse_ppm(" 1.5 ppm ").ppb == 1'500);
    CHECK(parse_ppm("0.001PPM").ppb == 1);
    CHECK(parse_ppm(".3").ppb == 300);
    CHECK(parse_ppm("-1000").ppb == -1'000'000);

    // 0.3 is not a double, and this is not reading one.
    CHECK(parse_ppm("0.3").ppb == 300);

    CHECK(!parse_ppm("").ppb);
    CHECK(!parse_ppm("ppm").ppb);
    CHECK(!parse_ppm("1.2345").ppb);
    CHECK(parse_ppm("1.2345").reason.find("three decimals") != std::string::npos);
    CHECK(!parse_ppm("1000.001").ppb);
    CHECK(!parse_ppm("--3").ppb);
    CHECK(!parse_ppm("twelve").ppb);
    CHECK(!parse_ppm("1e3").ppb);
}

TEST_CASE("a correction is written with its sign and three decimals", "[calibration]") {
    CHECK(format_ppm(0) == "0 ppm");
    CHECK(format_ppm(20'000) == "+20.000 ppm");
    CHECK(format_ppm(-1'250) == "-1.250 ppm");
    CHECK(format_ppm(7) == "+0.007 ppm");
    for (const std::int64_t ppb : {-999'999, -1, 1, 437, 20'000, 1'000'000}) {
        std::string text = format_ppm(ppb);
        INFO(text);
        CHECK(parse_ppm(text).ppb == ppb);
    }
}

TEST_CASE("a known carrier measured against its detection gives the crystal's error back",
          "[calibration]") {
    // Uncorrected, the carrier reads 3251 Hz low and the measurement is the
    // whole error.
    {
        const std::vector<Detection> seen{carrier_seen_with(0)};
        const auto measured = measure_against_carrier(kCarrierHz, seen, kDeviceHz, 0);
        INFO(measured.text);
        REQUIRE(measured.found);
        CHECK(measured.offset_hz == -3'251);
        // The label is to the hertz: 0.07 Hz of the carrier's 46749.07 was
        // rounded off, which is under one ppb of 162.5 MHz.
        CHECK(std::llabs(measured.ppb - kErrorPpb) <= 1);
    }

    // With half the correction already applied the answer is still the whole
    // error and not the remaining half, because it replaces the correction in
    // force rather than adding to it.
    {
        const std::int64_t half = kErrorPpb / 2;
        const std::vector<Detection> seen{carrier_seen_with(half)};
        const auto measured = measure_against_carrier(
            kCarrierHz, seen, revenant::source::device_to_true(kDeviceHz, half), half);
        INFO(measured.text);
        REQUIRE(measured.found);
        CHECK(std::llabs(measured.ppb - kErrorPpb) <= 1);
    }
}

TEST_CASE("the carrier is the nearest detection within two hundred ppm and never a merged one",
          "[calibration]") {
    Detection carrier = carrier_seen_with(0);

    Detection neighbour = carrier;
    neighbour.id = 8;
    neighbour.center_hz = kCarrierHz + 25'000;  // the next channel up

    Detection merged = carrier;
    merged.id = 9;
    merged.center_hz = kCarrierHz - 100;
    merged.state = TrackState::Merged;

    const std::vector<Detection> seen{neighbour, merged, carrier};
    const auto measured = measure_against_carrier(kCarrierHz, seen, kDeviceHz, 0);
    INFO(measured.text);
    REQUIRE(measured.found);
    CHECK(measured.detection_hz == carrier.center_hz);

    // 200 ppm of 162.55 MHz is 32.5 kHz. A detection 40 kHz away is not it.
    Detection far = carrier;
    far.center_hz = kCarrierHz - 40'000;
    const std::vector<Detection> only_far{far};
    const auto missed = measure_against_carrier(kCarrierHz, only_far, kDeviceHz, 0);
    CHECK(!missed.found);
    CHECK(missed.text.find("no detection within 32.5 kHz") != std::string::npos);

    // Low in HF the window does not shrink below 5 kHz.
    CHECK(revenant::ui::carrier_search_hz(5'000'000) == 5'000);
    CHECK(revenant::ui::carrier_search_hz(162'550'000) == 32'510);
}
