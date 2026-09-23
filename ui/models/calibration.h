// The calibration section of the device picker, as rules.
//
// Pure and Qt-free so ui/tests can cover it, like models/gain_control.h. Three
// things in here can be wrong while the panel still looks right: what a
// number typed into the PPM box means, which detection a known carrier is
// measured against, and the correction that measurement produces. The QML
// binds names and nothing else.
//
// THE ARITHMETIC IS THE ENGINE'S. measured_correction_ppb and the two
// conversions come from core/source/frequency_correction.h, which is header
// only and standard-library only so this client can compute exactly what the
// engine will apply without linking any of it. Two copies of that arithmetic
// would disagree at the rounding.

#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "core/rpc/types.h"
#include "core/source/frequency_correction.h"

namespace revenant::ui {

// What the PPM box holds, read.
struct PpmEntry {
    // Set when the text is a correction; `reason` says why when it is not.
    std::optional<std::int64_t> ppb;
    std::string reason;
};

// Parts per million as an operator writes them: "12", "-3.25", "+0.5",
// "1.5 ppm". Three decimals at most, because a ppb is the unit the engine
// keeps and a fourth digit would be a precision that is silently dropped.
//
// Integer arithmetic throughout, for the reason models/frequency_entry.h
// gives: 0.3 is not a double, and the correction is the one number here whose
// last digit an operator measured on purpose.
[[nodiscard]] inline PpmEntry parse_ppm(std::string_view text) {
    PpmEntry out;
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
        --end;
    }
    std::string_view body = text.substr(begin, end - begin);

    // An optional unit, in any case.
    if (body.size() >= 3) {
        const std::string_view tail = body.substr(body.size() - 3);
        const bool is_ppm = (tail[0] == 'p' || tail[0] == 'P') && (tail[1] == 'p' || tail[1] == 'P') &&
                            (tail[2] == 'm' || tail[2] == 'M');
        if (is_ppm) {
            body.remove_suffix(3);
            while (!body.empty() && (body.back() == ' ' || body.back() == '\t')) {
                body.remove_suffix(1);
            }
        }
    }
    if (body.empty()) {
        out.reason = "type a correction in parts per million, for example 12.5 or -3";
        return out;
    }

    bool negative = false;
    if (body.front() == '+' || body.front() == '-') {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }

    const std::size_t point = body.find('.');
    const std::string_view whole = body.substr(0, point);
    const std::string_view fraction =
        point == std::string_view::npos ? std::string_view{} : body.substr(point + 1);
    if (whole.empty() && fraction.empty()) {
        out.reason = std::format("'{}' is not a number of parts per million", text);
        return out;
    }
    if (fraction.size() > 3) {
        out.reason = "three decimals at most: the engine keeps parts per billion";
        return out;
    }

    std::int64_t ppm = 0;
    if (!whole.empty()) {
        const auto parsed = std::from_chars(whole.data(), whole.data() + whole.size(), ppm);
        if (parsed.ec != std::errc{} || parsed.ptr != whole.data() + whole.size() || ppm < 0) {
            out.reason = std::format("'{}' is not a number of parts per million", text);
            return out;
        }
    }
    std::int64_t thousandths = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        thousandths *= 10;
        if (i < fraction.size()) {
            const char c = fraction[i];
            if (c < '0' || c > '9') {
                out.reason = std::format("'{}' is not a number of parts per million", text);
                return out;
            }
            thousandths += c - '0';
        }
    }
    if (ppm > source::kMaxCorrectionPpb / 1000) {
        out.reason = "a correction past a thousand ppm is not a crystal error, it is a different "
                     "radio";
        return out;
    }

    const std::int64_t magnitude = ppm * 1000 + thousandths;
    if (magnitude > source::kMaxCorrectionPpb) {
        out.reason = "a correction past a thousand ppm is not a crystal error, it is a different "
                     "radio";
        return out;
    }
    out.ppb = negative ? -magnitude : magnitude;
    return out;
}

// "+20.000 ppm", "-1.250 ppm", "0 ppm". The sign is always written, because
// which way the crystal is off is half of what the number says.
[[nodiscard]] inline std::string format_ppm(std::int64_t ppb) {
    if (ppb == 0) {
        return "0 ppm";
    }
    const std::int64_t magnitude = ppb < 0 ? -ppb : ppb;
    return std::format("{}{}.{:03} ppm", ppb < 0 ? '-' : '+', magnitude / 1000, magnitude % 1000);
}

// What the front-end stage has measured, in a line, or why there is nothing
// to say.
[[nodiscard]] inline std::string describe_front_end(const rpc::Calibration& state) {
    if (!state.open) {
        return "no source open";
    }
    if (!state.settings.dc_removal && !state.settings.iq_correction) {
        return "DC and I/Q correction off: nothing is measured while both are off";
    }
    if (!state.measured) {
        return "measuring";
    }
    std::string out = std::format("centre spike {:.1f} dBFS", state.dc_dbfs);
    out += std::format(", I/Q {:+.2f} dB {:+.2f} deg, image rejection {:.1f} dB",
                       state.gain_error_db, state.phase_error_deg, state.image_rejection_db);
    if (state.settings.iq_correction && !state.iq_plausible) {
        out += ", too large to be a mixer's error, so not applied";
    }
    return out;
}

// A known carrier, measured against the detection nearest it.
struct CarrierMeasurement {
    bool found = false;

    // The correction the carrier implies, which replaces the one in force
    // rather than adding to it.
    std::int64_t ppb = 0;

    // Where the detection was and how far it sat from the known frequency.
    std::int64_t detection_hz = 0;
    std::int64_t offset_hz = 0;

    // The line the panel shows, whether it found one or not.
    std::string text;
};

// How far from the typed frequency a detection may be and still be taken for
// it: two hundred ppm, which is ten times a generic dongle's crystal
// tolerance, and never less than five kilohertz so a carrier near the bottom
// of HF is still found.
[[nodiscard]] inline std::int64_t carrier_search_hz(std::int64_t known_hz) {
    const std::int64_t magnitude = known_hz < 0 ? -known_hz : known_hz;
    const std::int64_t by_ppm = magnitude / 5'000;
    return by_ppm > 5'000 ? by_ppm : 5'000;
}

// THE DETECTION NEAREST THE TYPED FREQUENCY, within carrier_search_hz of it.
//
// A detection and not the span's peak, because the detector has already
// decided which bumps are signals and placed each one's centre from the
// span's exact rational axis; the peak bin alone is the loudest thing, which
// on a band with a broadcast station in it is not the carrier the operator
// named. A merged track is skipped: its centre is the track it merged into.
//
// HOW GOOD IT IS. A detection's centre is placed to within a fraction of a
// bin, and a bin is 36.6 Hz on the shipped RTL-SDR geometry, so the measured
// correction is good to about 20 Hz over the carrier frequency: 0.12 ppm at
// 162.55 MHz, 0.2 ppm at 100 MHz. A carrier higher in frequency measures
// finer. A wideband FM station's centre is the middle of its occupied band,
// which is its carrier only while the programme is symmetric, so a narrowband
// carrier such as NOAA weather radio or a GSM or DVB pilot is the better
// reference.
[[nodiscard]] inline CarrierMeasurement measure_against_carrier(
    std::int64_t known_hz, std::span<const rpc::Detection> detections,
    std::int64_t source_center_hz, std::int64_t current_ppb) {
    CarrierMeasurement out;
    if (known_hz <= 0) {
        out.text = "type the carrier's true frequency";
        return out;
    }

    const std::int64_t window = carrier_search_hz(known_hz);
    const rpc::Detection* nearest = nullptr;
    std::int64_t nearest_distance = 0;
    for (const rpc::Detection& detection : detections) {
        if (detection.state == rpc::TrackState::Merged) {
            continue;
        }
        const std::int64_t distance = std::llabs(detection.center_hz - known_hz);
        if (distance > window) {
            continue;
        }
        if (nearest == nullptr || distance < nearest_distance) {
            nearest = &detection;
            nearest_distance = distance;
        }
    }
    if (nearest == nullptr) {
        out.text = std::format(
            "no detection within {:.1f} kHz of {:.6f} MHz. Tune so the carrier is in the span "
            "and the detector has found it",
            static_cast<double>(window) / 1e3, static_cast<double>(known_hz) / 1e6);
        return out;
    }

    const std::int64_t ppb = source::measured_correction_ppb(known_hz, nearest->center_hz,
                                                             source_center_hz, current_ppb);
    out.detection_hz = nearest->center_hz;
    out.offset_hz = nearest->center_hz - known_hz;
    if (!source::correction_in_range(ppb)) {
        out.text = std::format("the detection at {:.6f} MHz would mean {}, which is not a crystal "
                               "error: it is not the carrier",
                               static_cast<double>(nearest->center_hz) / 1e6, format_ppm(ppb));
        return out;
    }
    out.found = true;
    out.ppb = ppb;
    out.text = std::format("detection at {:.6f} MHz, {:+} Hz from it: {}",
                           static_cast<double>(nearest->center_hz) / 1e6, out.offset_hz,
                           format_ppm(ppb));
    return out;
}

}  // namespace revenant::ui
