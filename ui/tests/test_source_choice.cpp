// compose_source_uri and the three settling rules under it, which are the
// whole of what a device picker decides before a radio is opened.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_frequency_entry.cpp gives: a test that only
// asserts what the code already does certifies one reachable shape and reads
// as though it certified the behaviour.
//
// The wrong implementations that matter on this path are a picker that emits
// every key to every backend, which opens nothing and blames the device; one
// that treats a stepped gain stage as continuous, which shows the operator a
// number the device never took; one that clamps a rate request of zero up to
// the device's minimum, which pins a rate nobody chose; and one that rewrites
// the descriptor's own URI instead of appending to it.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "core/rpc/types.h"
#include "models/source_choice.h"

using revenant::rpc::GainStage;
using revenant::rpc::SourceDescriptor;
using revenant::rpc::TuneRange;
using revenant::ui::compose_source_uri;
using revenant::ui::describe_length;
using revenant::ui::GainChoice;
using revenant::ui::settle_gain;
using revenant::ui::settle_rate;
using revenant::ui::SourceChoice;
using revenant::ui::tune_envelope;

namespace {

// An R820T as the backend describes one: tunable, bounded rather than listed,
// and with a single stepped tuner gain.
[[nodiscard]] SourceDescriptor dongle()
{
    SourceDescriptor out;
    out.uri = "rtlsdr://0";
    out.backend = "rtlsdr";
    out.display_name = "Generic RTL2832U OEM (R820T/R820T2 tuner) at index 0";

    TuneRange range;
    range.low_hz = 24'000'000;
    range.high_hz = 1'766'000'000;
    out.tune_ranges.push_back(range);

    // EMPTY ON PURPOSE. The RTL-SDR's rate is a 28.8 MHz clock over an
    // integer, so it is discrete and far too dense to list; the backend
    // reports bounds and the device rounds.
    out.min_rate = 225'001;
    out.max_rate = 3'200'000;

    GainStage tuner;
    tuner.name = "tuner";
    tuner.min_db = 0.0;
    tuner.max_db = 49.6;
    tuner.steps_db = {0.0, 0.9, 1.4, 2.7, 3.7, 7.7, 8.7, 12.5, 14.4, 15.7, 16.6, 19.7, 20.7,
                      22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4, 37.2, 38.6, 40.2, 42.1, 43.4,
                      43.9, 44.5, 48.0, 49.6};
    tuner.has_auto = true;
    out.gain_stages.push_back(tuner);

    return out;
}

// A synthetic scene: no tuner, no gain stage, and a rate it will generate at
// whatever it is asked for.
[[nodiscard]] SourceDescriptor scene()
{
    SourceDescriptor out;
    out.uri = "synthetic:wideband";
    out.backend = "synthetic";
    out.display_name = "Synthetic wideband scene";
    return out;
}

}  // namespace

TEST_CASE("a key is emitted only when the device says it has the capability", "[ui][source]")
{
    // The wrong implementation this rejects emits freq= and gain= to every
    // backend. Query::reject_unknown then refuses the open by name, so the
    // operator gets "synthetic does not take freq" about a scene they only
    // wanted to play, and the picker looks broken rather than the request.
    SourceChoice choice;
    choice.rate = 2'400'000;
    choice.center_hz = 98'100'000;
    choice.gains.push_back(GainChoice{.stage = "tuner", .automatic = false, .db = 20.7});

    const std::string live = compose_source_uri(dongle(), choice);
    INFO(live);
    CHECK(live.find("rate=2400000") != std::string::npos);
    CHECK(live.find("freq=98100000") != std::string::npos);
    CHECK(live.find("gain=20.7") != std::string::npos);

    // The same choice against a backend with neither capability carries the
    // one key every backend does take, and nothing else.
    const std::string synthetic = compose_source_uri(scene(), choice);
    INFO(synthetic);
    CHECK(synthetic == "synthetic:wideband?rate=2400000");
    CHECK(synthetic.find("freq") == std::string::npos);
    CHECK(synthetic.find("gain") == std::string::npos);
}

TEST_CASE("the descriptor's own URI is appended to and never rewritten", "[ui][source]")
{
    // The wrong implementation rebuilds the URI from the backend name and an
    // index it parsed back out. That loses everything the backend put in the
    // string to identify the device, which for a dongle opened by serial is
    // the serial itself, and the picker then opens index 0 instead.
    SourceDescriptor by_serial = dongle();
    by_serial.uri = "rtlsdr://00000001";

    SourceChoice choice;
    choice.rate = 2'400'000;

    const std::string composed = compose_source_uri(by_serial, choice);
    INFO(composed);
    CHECK(composed.starts_with("rtlsdr://00000001?"));

    // A base that already carries a query gets '&' and not a second '?', which
    // would make the whole tail one value.
    //
    // THIS ASSERTION USED TO EXPECT "&freq=24000000" and that was the defect
    // rather than the behaviour: `choice` above names no centre, and the old
    // code clamped the resulting zero into the tune envelope, whose low edge on
    // an R820T is 24 MHz. A test that expects the bug is worse than no test,
    // because it stops anybody changing it.
    SourceDescriptor with_query = dongle();
    with_query.uri = "rtlsdr://0?ppm=12";
    const std::string joined = compose_source_uri(with_query, choice);
    INFO(joined);
    CHECK(joined == "rtlsdr://0?ppm=12&rate=2400000");
    CHECK(joined.find("?ppm=12?") == std::string::npos);

    // An empty descriptor composes nothing rather than a string starting with
    // '?', which the registry would refuse with a message about the scheme.
    SourceDescriptor nameless;
    CHECK(compose_source_uri(nameless, choice).empty());
}

TEST_CASE("a stepped gain stage lands on a step the device has", "[ui][source]")
{
    // The wrong implementation treats every stage as continuous and clamps.
    // 21.0 dB is not one of the R820T's 29 steps, so the device rounds to 20.7
    // and reports that back, and the picker goes on showing 21.0: the operator
    // sees a setting the radio never took and cannot tell it from one it did.
    const SourceDescriptor source = dongle();
    const GainStage& tuner = source.gain_stages.front();

    CHECK(settle_gain(tuner, 21.0) == 20.7);
    CHECK(settle_gain(tuner, 20.7) == 20.7);

    // AN EXACT TIE ROUNDS DOWN, which is the opposite of settle_rate's tie and
    // deliberately so. Too much rate is a counter climbing; too much gain
    // drives the front end past its linear range and puts intermodulation
    // products in the detector's track list at confidence 1.00, which reads as
    // real signal rather than as a fault.
    //
    // ON A STAGE BUILT FOR IT, AND NOT ON THE R820T's TABLE. 21.8 looks
    // exactly between 20.7 and 22.9 and is not: in doubles the distances are
    // 1.1000000000000014 down and 1.0999999999999996 up, so the upper step
    // wins on representation error rather than on any rule. That is the real
    // behaviour of a decimal step table and it is why this asserts the rule on
    // values that are exact, rather than asserting a tie the hardware's own
    // numbers can never produce. The tie-break still earns its place: without
    // it the answer would depend on the order the backend happened to list the
    // steps in.
    GainStage even;
    even.name = "even";
    even.steps_db = {20.0, 22.0};
    CHECK(settle_gain(even, 21.0) == 20.0);

    // Outside the table in both directions lands on an end rather than on the
    // clamp of a range the steps do not fill evenly.
    CHECK(settle_gain(tuner, -10.0) == 0.0);
    CHECK(settle_gain(tuner, 100.0) == 49.6);

    // Not finite lands on the minimum rather than reaching the URI, where it
    // would be refused as a parse error that says nothing about the slider.
    CHECK(settle_gain(tuner, std::numeric_limits<double>::quiet_NaN()) == tuner.min_db);

    // A continuous stage is the other shape and clamps.
    GainStage continuous;
    continuous.name = "lna";
    continuous.min_db = -3.0;
    continuous.max_db = 30.0;
    CHECK(settle_gain(continuous, 21.0) == 21.0);
    CHECK(settle_gain(continuous, 90.0) == 30.0);
    CHECK(settle_gain(continuous, -90.0) == -3.0);
}

TEST_CASE("the tuner's own AGC is gain=auto and not agc=1", "[ui][source]")
{
    // The wrong implementation reads "automatic" and writes agc=1, which on
    // this device is the RTL2832U's DIGITAL AGC and a different stage
    // entirely. The tuner would stay pinned at the backend's default gain
    // while a separate loop ran downstream of it, and the on-air result would
    // look like neither setting.
    SourceChoice choice;
    choice.rate = 2'400'000;
    choice.center_hz = 98'100'000;
    choice.gains.push_back(GainChoice{.stage = "tuner", .automatic = true, .db = 20.7});

    const std::string composed = compose_source_uri(dongle(), choice);
    INFO(composed);
    CHECK(composed.find("gain=auto") != std::string::npos);
    CHECK(composed.find("agc=") == std::string::npos);

    // And the dB value is not emitted beside it: it is the setting the auto
    // mode is being asked to override.
    CHECK(composed.find("20.7") == std::string::npos);
}

TEST_CASE("a gain for a stage the device does not have is dropped", "[ui][source]")
{
    // The wrong implementation walks the CHOICE and emits whatever it holds.
    // A choice left over from a three-stage device then writes that device's
    // "mixer" value against this one's only stage, and the operator's LNA
    // setting silently becomes the tuner's.
    SourceChoice choice;
    choice.rate = 2'400'000;
    choice.gains.push_back(GainChoice{.stage = "mixer", .automatic = false, .db = 12.0});

    const std::string composed = compose_source_uri(dongle(), choice);
    INFO(composed);
    CHECK(composed.find("gain=") == std::string::npos);
    CHECK(composed.find("12") == std::string::npos);
}

TEST_CASE("a rate request of zero leaves the key off", "[ui][source]")
{
    // The wrong implementation clamps zero up to min_rate, which pins 225001
    // samples per second on a dongle the operator never gave a rate for, and
    // the backend's own default never runs.
    SourceChoice choice;
    choice.center_hz = 98'100'000;

    const std::string composed = compose_source_uri(dongle(), choice);
    INFO(composed);
    CHECK(composed.find("rate=") == std::string::npos);
    CHECK(settle_rate(dongle(), 0) == 0);
    CHECK(settle_rate(dongle(), -1) == 0);
}

TEST_CASE("a listed rate is the nearest one and an unlisted one is clamped", "[ui][source]")
{
    // The wrong implementation clamps against min and max whatever the list
    // says. A device that takes 250k, 1M and 2M then gets 1.5M, rounds to
    // whichever it prefers, and the picker's label disagrees with the engine.
    SourceDescriptor listed = dongle();
    listed.sample_rates = {250'000, 1'000'000, 2'000'000};
    CHECK(settle_rate(listed, 1'400'000) == 1'000'000);
    CHECK(settle_rate(listed, 1'600'000) == 2'000'000);
    CHECK(settle_rate(listed, 99) == 250'000);
    CHECK(settle_rate(listed, 9'000'000) == 2'000'000);

    // AN EXACT TIE ROUNDS UP, AND NOT BECAUSE OF THE LOOP'S ORDER. 1.5 M is
    // exactly between 1 M and 2 M. The wrong implementation takes whichever it
    // reached first, which is the lower one here and would be the higher one if
    // somebody sorted the list the other way: the operator gets half the span
    // they asked for and finds out at a band edge where they expected signal.
    // Too much rate announces itself in the overrun counters instead.
    CHECK(settle_rate(listed, 1'500'000) == 2'000'000);

    // Bounds with no list is the RTL-SDR's real shape and clamps.
    CHECK(settle_rate(dongle(), 2'400'000) == 2'400'000);
    CHECK(settle_rate(dongle(), 100) == 225'001);
    CHECK(settle_rate(dongle(), 9'000'000) == 3'200'000);

    // Neither is a backend that did not say, and the request is untouched.
    CHECK(settle_rate(scene(), 20'000'000) == 20'000'000);
}

TEST_CASE("a centre outside the envelope is clamped rather than sent", "[ui][source]")
{
    // The wrong implementation sends whatever was typed and lets the device
    // refuse. That is the right answer for a frequency inside a tuner's GAP,
    // which this cannot know about, and the wrong one for a frequency outside
    // the envelope entirely: the open fails, and an operator who mistyped a
    // digit gets a failed open instead of a radio at the nearest edge.
    SourceChoice choice;
    choice.center_hz = 5'000'000;  // below 24 MHz

    const std::string low = compose_source_uri(dongle(), choice);
    INFO(low);
    CHECK(low.find("freq=24000000") != std::string::npos);

    choice.center_hz = 3'000'000'000;
    const std::string high = compose_source_uri(dongle(), choice);
    INFO(high);
    CHECK(high.find("freq=1766000000") != std::string::npos);
}

TEST_CASE("an envelope is the outer bounds of every usable range", "[ui][source]")
{
    // The wrong implementation reports the FIRST range and calls it the
    // envelope. An E4000 reaches 52 to 2200 MHz as two ranges with a gap, and
    // a picker greying out everything above the first range's top refuses a
    // control the device has.
    SourceDescriptor split = dongle();
    split.tune_ranges.clear();
    split.tune_ranges.push_back(TuneRange{.low_hz = 52'000'000, .high_hz = 1'100'000'000});
    split.tune_ranges.push_back(TuneRange{.low_hz = 1'250'000'000, .high_hz = 2'200'000'000});

    const auto envelope = tune_envelope(split);
    CHECK(envelope.tunable);
    CHECK(envelope.low_hz == 52'000'000);
    CHECK(envelope.high_hz == 2'200'000'000);

    // An inverted range describes nothing and is skipped rather than widening
    // the envelope to include it.
    SourceDescriptor broken = dongle();
    broken.tune_ranges.push_back(TuneRange{.low_hz = 900, .high_hz = 100});
    const auto ignored = tune_envelope(broken);
    CHECK(ignored.low_hz == 24'000'000);
    CHECK(ignored.high_hz == 1'766'000'000);

    // No ranges at all is a source that cannot be pointed anywhere, which is
    // every file and every synthetic scene, and a picker greys the control out
    // rather than offering one that always refuses.
    CHECK_FALSE(tune_envelope(scene()).tunable);
}

TEST_CASE("a live device has no length and a recording's is a duration", "[ui][source]")
{
    // The wrong implementation prints length_samples of zero as "0 s", so
    // every dongle in the list reads as an empty recording. Zero means
    // unbounded.
    CHECK(describe_length(dongle()).empty());

    SourceDescriptor recording;
    recording.uri = "file:///C:/captures/hf.cf32";
    recording.max_rate = 2'400'000;
    recording.seekable = true;

    recording.length_samples = 2'400'000ULL * 45;
    CHECK(describe_length(recording) == "45 s");

    recording.length_samples = 2'400'000ULL * 252;
    CHECK(describe_length(recording) == "4 m 12 s");

    recording.length_samples = 2'400'000ULL * 3 * 3600;
    CHECK(describe_length(recording) == "3 h 0 m");

    // Samples with no rate is a count and not a duration, and printing the
    // count as seconds would be off by whatever the rate turned out to be.
    recording.max_rate = 0;
    CHECK(describe_length(recording).empty());
}

TEST_CASE("a box the operator left empty leaves its key off", "[ui][source]")
{
    // THE DEFECT THIS FILE SHIPPED WITH, and the one the window showed on
    // 2026-09-21: an operator who filled in nothing and pressed open got
    // "rtlsdr://0?freq=24000000&gain=0". Zero was clamped into the tune
    // envelope, whose low edge on an R820T is 24 MHz exactly, and zero snapped
    // to the lowest step in the tuner's gain table, so the radio opened at the
    // very bottom of its range with no gain.
    //
    // The wrong implementation is a plain number defaulting to zero, which
    // cannot tell "nothing was typed" from "zero was asked for". Omitting the
    // key hands the decision to the backend, which has documented defaults and
    // a measurement behind the gain: 20 dB, chosen over the tuner's own AGC.
    const SourceDescriptor source = dongle();

    SourceChoice nothing;
    nothing.gains.push_back(GainChoice{.stage = "tuner"});
    const std::string bare = compose_source_uri(source, nothing);
    INFO(bare);
    CHECK(bare == "rtlsdr://0");

    // One at a time, so a field that started working cannot hide one that did
    // not.
    SourceChoice centre_only;
    centre_only.center_hz = 98'100'000;
    CHECK(compose_source_uri(source, centre_only) == "rtlsdr://0?freq=98100000");

    SourceChoice rate_only;
    rate_only.rate = 2'400'000;
    CHECK(compose_source_uri(source, rate_only) == "rtlsdr://0?rate=2400000");

    SourceChoice gain_only;
    gain_only.gains.push_back(GainChoice{.stage = "tuner", .db = 20.7});
    CHECK(compose_source_uri(source, gain_only) == "rtlsdr://0?gain=20.7");

    // AND ZERO IS STILL A REQUEST WHEN IT IS MADE. 0 dB is a real step in the
    // R820T's table and DC is a real thing to ask a direct-sampling dongle for,
    // so the fix must not turn "zero" into "nothing": that would be the same
    // conflation with the sign reversed.
    SourceChoice explicit_zero;
    explicit_zero.gains.push_back(GainChoice{.stage = "tuner", .db = 0.0});
    CHECK(compose_source_uri(source, explicit_zero) == "rtlsdr://0?gain=0");

    // auto with no dB is a request, not an absence.
    SourceChoice automatic;
    automatic.gains.push_back(GainChoice{.stage = "tuner", .automatic = true});
    CHECK(compose_source_uri(source, automatic) == "rtlsdr://0?gain=auto");
}
