// What an operator chose in the picker, turned into a source URI.
//
// WHY THIS IS ITS OWN HEADER AND HOLDS NO Qt
//
// ui/tests links it, for the reason ui/models/frequency_entry.h gives about
// the frequency box: the part of a device picker that can be WRONG is none of
// the widget. It is which query keys a backend accepts, where a rate lands
// when the device only takes some of them, which gain value a stepped stage
// rounds to, and whether a control is offered at all for a device that cannot
// do it. Every one of those is arithmetic and string building over a
// description, and every one of them stays wrong quietly.
//
// So it lives here, ui/tests asserts it, and the QML does the panel.
//
// THE QUERY KEYS ARE BACKEND KNOWLEDGE AND THIS IS WHERE THEY LIVE, WHICH IS
// NOT WHERE THEY BELONG
//
// core/rpc/revenant.capnp says a client should not parse a source URI, because
// each backend's grammar is its own, and that is right. Composing one needs
// the same knowledge: `rate=` is taken by all three backends, `freq=` is the
// RTL-SDR's and the file and synthetic backends call their equivalent
// `center=`, and `gain=` exists only on a live radio.
//
// SourceDescriptor does not carry parameter names, so this is where the
// mapping sits. Two things make that safe rather than merely small:
//
//   A key is emitted only when the descriptor says the device has the
//   capability. `freq=` goes out only for a source with a tune range and
//   `gain=` only for one with a gain stage, so the file and synthetic
//   backends, which have neither, never see either key.
//
//   A wrong key is LOUD. Every backend calls Query::reject_unknown with its
//   own name, so a key it does not take is refused by name at open rather
//   than ignored. A picker that guessed wrong gets a sentence naming the
//   backend and the key, which it shows; it does not get a source opened at
//   the wrong frequency.
//
// What would remove the guess: parameter names on SourceDescriptor, or an
// openSource that takes a structured request and lets the backend compose its
// own URI. Neither is worth building for one tunable backend, and the second
// one duplicates the query mechanism that already exists. A NEW tunable
// backend that does not call its centre `freq` needs an entry here, and will
// announce itself by refusing.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

// One gain stage's setting. A LIST OF THESE AND NOT ONE NUMBER, because a
// device's stages are its own: an R820T has one tuner gain, an Airspy has
// three, a file has none.
struct GainChoice {
    // The stage's own name, as SourceDescriptor::gain_stages reported it.
    std::string stage;

    // True asks the device to set this stage itself, and `db` is then unused.
    //
    // OFFERED AND NOT DEFAULTED. The RTL-SDR's own AGC maximises the level at
    // its output, so it is set by the loudest thing anywhere in the span:
    // measured on air on 2026-09-20 at 95.1 MHz it put three intermodulation
    // products in the detector's track list at confidence 1.00, and gain=20
    // improved the measured SNR of the station by 5.7 dB and removed all
    // three. README.md carries that measurement.
    bool automatic = false;

    double db = 0.0;
};

struct SourceChoice {
    // Absolute hertz for the front end. Ignored for a source with no tune
    // range, which is every file and every synthetic scene.
    std::int64_t center_hz = 0;

    // Samples per second. Zero leaves the key off entirely, which is how an
    // operator asks for whatever the backend defaults to.
    std::int64_t rate = 0;

    std::vector<GainChoice> gains;
};

// Whether the front end can be pointed anywhere, and the outer bounds of where.
//
// AN ENVELOPE AND NOT A PROMISE, on the terms SourceDescriptor states: an E4000
// reaches 52 to 2200 MHz with a gap in the middle and reports the outer pair, so
// a frequency inside the gap is still refused by the source in its own words.
// This exists so a control that could never work is not offered, which is the
// one case where a clean refusal is not good enough because the operator has to
// discover it by trying.
struct TuneEnvelope {
    bool tunable = false;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
};

[[nodiscard]] inline TuneEnvelope tune_envelope(const rpc::SourceDescriptor& source)
{
    TuneEnvelope out;
    for (const rpc::TuneRange& range : source.tune_ranges) {
        // The server already drops an inverted range, so this is belt and
        // braces against a descriptor built some other way rather than a second
        // filter. Skipping is the same answer either way: a range whose high is
        // below its low describes nothing.
        if (range.high_hz < range.low_hz) {
            continue;
        }
        if (!out.tunable) {
            out.tunable = true;
            out.low_hz = range.low_hz;
            out.high_hz = range.high_hz;
            continue;
        }
        out.low_hz = std::min(out.low_hz, range.low_hz);
        out.high_hz = std::max(out.high_hz, range.high_hz);
    }
    return out;
}

// Where a rate an operator asked for actually lands.
//
// THREE SHAPES AND THEY ARE NOT INTERCHANGEABLE, which is why this is a
// function and not a clamp at the call site:
//
//   A populated sample_rates list means the device takes those values and
//   nothing else, so the answer is the nearest one. An RTL-SDR does NOT have
//   this shape, despite being the obvious candidate: its rate is a 28.8 MHz
//   clock over an integer, discrete but far too dense to list.
//
//   An empty list with bounds means continuous between them, and the answer is
//   the request clamped. The device may still round, which is why
//   configure() reads back what it landed on and EngineInfo reports that
//   rather than what was asked.
//
//   No bounds at all means the backend did not say, and the answer is the
//   request untouched. A synthetic scene is this: it generates whatever rate
//   it is given.
//
// A ZERO REQUEST COMES BACK AS ZERO in every shape, because zero means "leave
// the key off and take the backend's default" rather than "the slowest rate
// you have". Rounding it up to min_rate would silently pin a rate the operator
// did not choose.
[[nodiscard]] inline std::int64_t settle_rate(const rpc::SourceDescriptor& source,
                                             std::int64_t asked)
{
    if (asked <= 0) {
        return 0;
    }

    if (!source.sample_rates.empty()) {
        // A TIE ROUNDS UP, AND THE DIRECTION IS A DECISION RATHER THAN
        // WHICHEVER THE LOOP REACHED FIRST. 1.5 MS/s against a device offering
        // 1 M and 2 M is exactly between them. Rounding down silently gives
        // half the span that was asked for, which shows up later as a band edge
        // where the operator expected signal; rounding up covers everything the
        // request would have and costs bus bandwidth, which is visible at once
        // in the overrun counters. Prefer the failure that announces itself.
        //
        // settle_gain below breaks its tie the other way for the same kind of
        // reason, which is why neither of them leans on the loop's order.
        std::int64_t best = source.sample_rates.front();
        std::int64_t best_distance = std::numeric_limits<std::int64_t>::max();
        for (const std::uint32_t rate : source.sample_rates) {
            const auto candidate = static_cast<std::int64_t>(rate);
            const std::int64_t distance =
                candidate > asked ? candidate - asked : asked - candidate;
            if (distance < best_distance || (distance == best_distance && candidate > best)) {
                best_distance = distance;
                best = candidate;
            }
        }
        return best;
    }

    if (source.min_rate != 0 && asked < static_cast<std::int64_t>(source.min_rate)) {
        return static_cast<std::int64_t>(source.min_rate);
    }
    if (source.max_rate != 0 && asked > static_cast<std::int64_t>(source.max_rate)) {
        return static_cast<std::int64_t>(source.max_rate);
    }
    return asked;
}

// Where a gain an operator asked for actually lands on one stage.
//
// A populated steps_db means the stage takes those values and nothing else, so
// a continuous slider over one of those shows the operator a number the device
// never took. An R820T's tuner gain has 29 of them. Empty means continuous
// between min_db and max_db and the answer is the request clamped.
//
// Not finite comes back as the minimum rather than propagating: a NaN reaching
// a URI is a string the backend refuses with a parse error, which tells the
// operator nothing about the slider that produced it.
[[nodiscard]] inline double settle_gain(const rpc::GainStage& stage, double asked)
{
    if (!std::isfinite(asked)) {
        return stage.min_db;
    }

    if (!stage.steps_db.empty()) {
        // A TIE ROUNDS DOWN, WHICH IS THE OPPOSITE OF settle_rate ABOVE AND
        // FOR THE SAME KIND OF REASON. Too much rate is a counter climbing;
        // too much gain is a front end driven past its linear range, which
        // puts intermodulation products in the detector's track list at full
        // confidence and reads as real signal. README.md has that measured on
        // air. The safe direction here is less, so a request exactly between
        // two steps takes the lower one.
        double best = stage.steps_db.front();
        double best_distance = std::numeric_limits<double>::max();
        for (const double step : stage.steps_db) {
            const double distance = std::abs(step - asked);
            if (distance < best_distance || (distance == best_distance && step < best)) {
                best_distance = distance;
                best = step;
            }
        }
        return best;
    }

    return std::clamp(asked, stage.min_db, stage.max_db);
}

namespace detail {

// A decimal with the trailing zeros off, because a gain of "20" and a gain of
// "20.000000" are the same request and only one of them reads as a setting.
//
// std::format's {} on a double is the shortest round-tripping form, which is
// exactly this, so there is nothing to trim. Written out rather than formatted
// because this header is included by a test that links no <format>-heavy
// translation unit, and because one decimal place is all a gain stage resolves:
// librtlsdr's steps are tenths of a dB.
[[nodiscard]] inline std::string decimal(double value)
{
    const auto tenths = static_cast<std::int64_t>(std::llround(value * 10.0));
    const std::int64_t whole = tenths / 10;
    const std::int64_t fraction = tenths < 0 ? -(tenths % 10) : tenths % 10;
    std::string out = std::to_string(whole);
    if (whole == 0 && tenths < 0) {
        out.insert(out.begin(), '-');
    }
    if (fraction != 0) {
        out.push_back('.');
        out.push_back(static_cast<char>('0' + fraction));
    }
    return out;
}

}  // namespace detail

// The URI to hand to Session::openSource.
//
// THE DESCRIPTOR'S OWN uri IS THE BASE AND IS NEVER REWRITTEN. It is what
// listSources handed back, the backend composed it, and it already carries
// whatever identifies the device: an index for a dongle, a scene name for the
// synthetic backend, a path for a file. Settings are appended. Nothing here
// parses it, which is what core/rpc/revenant.capnp asks for; what it does have
// to know is whether the base already carries a query, because a second '?'
// makes the whole tail one value.
//
// EMPTY IN, EMPTY OUT. A descriptor with no URI cannot be opened and composing
// a query onto nothing would produce a string starting with '?', which the
// registry refuses with a message about the scheme rather than about the
// descriptor that was empty.
[[nodiscard]] inline std::string compose_source_uri(const rpc::SourceDescriptor& source,
                                                    const SourceChoice& choice)
{
    if (source.uri.empty()) {
        return {};
    }

    std::string out = source.uri;
    bool has_query = out.find('?') != std::string::npos;

    const auto append = [&out, &has_query](std::string_view key, const std::string& value) {
        out.push_back(has_query ? '&' : '?');
        has_query = true;
        out.append(key);
        out.push_back('=');
        out.append(value);
    };

    // Every backend takes this one, which is what makes it the only key here
    // emitted without asking the descriptor's permission first.
    const std::int64_t rate = settle_rate(source, choice.rate);
    if (rate > 0) {
        append("rate", std::to_string(rate));
    }

    // Only for a device that can be pointed somewhere, so the file and
    // synthetic backends never see this key. See the header note: that gate is
    // half of what makes composing a URI here safe, and Query::reject_unknown
    // is the other half.
    const TuneEnvelope envelope = tune_envelope(source);
    if (envelope.tunable) {
        const std::int64_t centre =
            std::clamp(choice.center_hz, envelope.low_hz, envelope.high_hz);
        append("freq", std::to_string(centre));
    }

    // ONE KEY FOR THE FIRST STAGE AND THE REST DROPPED, because `gain=` is
    // singular in every grammar this can reach. The RTL-SDR has exactly one
    // stage, so nothing is lost today; a device with three needs per-stage keys
    // and the backend that adds them adds the mapping.
    //
    // Walked in the descriptor's order rather than the choice's, so a choice
    // carrying a stage name the device does not have contributes nothing
    // instead of being emitted against the wrong stage.
    for (const rpc::GainStage& stage : source.gain_stages) {
        const auto chosen = std::ranges::find_if(
            choice.gains, [&stage](const GainChoice& one) { return one.stage == stage.name; });
        if (chosen == choice.gains.end()) {
            continue;
        }
        if (chosen->automatic) {
            // `gain=auto` AND NOT `agc=1`, WHICH ARE TWO DIFFERENT CONTROLS ON
            // THIS DEVICE. gain=auto is the TUNER's own AGC, which is what
            // GainStage::has_auto describes and what the measurement in
            // README.md is about. agc= is the digital AGC inside the RTL2832U,
            // a separate stage this picker does not offer. Emitting the second
            // for a stage that advertised the first would leave the tuner
            // pinned at the backend's default gain while a different loop ran
            // downstream of it, and the on-air result would look like neither
            // setting.
            append("gain", "auto");
        } else {
            append("gain", detail::decimal(settle_gain(stage, chosen->db)));
        }
        break;
    }

    return out;
}

// A recording's length in a form a person reads, or empty for a live device.
//
// ZERO LENGTH MEANS UNBOUNDED AND IS NOT A RECORDING OF NO LENGTH, which is
// what SourceDescriptor::length_samples says, so this answers empty rather than
// "0 s" for every dongle. A rate of zero answers empty too: samples with no
// rate is a count and not a duration, and printing the count as seconds would
// be off by whatever the rate turned out to be.
[[nodiscard]] inline std::string describe_length(const rpc::SourceDescriptor& source)
{
    if (source.length_samples == 0 || source.max_rate == 0) {
        return {};
    }

    const auto seconds = static_cast<std::int64_t>(source.length_samples /
                                                   static_cast<std::uint64_t>(source.max_rate));
    if (seconds < 60) {
        return std::to_string(seconds) + " s";
    }
    const std::int64_t minutes = seconds / 60;
    const std::int64_t rest = seconds % 60;
    if (minutes < 60) {
        return std::to_string(minutes) + " m " + std::to_string(rest) + " s";
    }
    return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " m";
}

}  // namespace revenant::ui
