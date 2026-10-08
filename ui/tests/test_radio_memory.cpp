// models/radio_memory.h: each radio's sample rate, centre and gain, kept under
// its own id and put back when it is opened again.
//
// The owner's request of 2026-10-07 was "save the sample rate, start freq, and
// gain for each receiver". The wrong implementations these reject are one that
// keys by URI, so a second dongle at index 0 inherits the first one's
// numbers; one that restores a 7 MHz centre before the radio's direct sampling
// mode, so it is clamped to the tuner's 24 MHz floor; and one that writes on
// every wheel notch, or drops the last second of tuning on a radio change.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "core/rpc/types.h"
#include "models/detector_scope.h"
#include "models/radio_memory.h"
#include "models/source_choice.h"

using revenant::rpc::GainStage;
using revenant::rpc::SourceDescriptor;
using revenant::rpc::TuneRange;
using revenant::ui::compose_source_uri;
using revenant::ui::descriptor_calibration_key;
using revenant::ui::direct_sampling_key;
using revenant::ui::gain_setting_text;
using revenant::ui::kRadioCentreLeaf;
using revenant::ui::kRadioGainLeaf;
using revenant::ui::kRadioRateLeaf;
using revenant::ui::kRadioSaveSettleMs;
using revenant::ui::radio_setting_key;
using revenant::ui::radio_settings_id;
using revenant::ui::RadioMemory;
using revenant::ui::RadioSaveQueue;
using revenant::ui::RadioSnapshot;
using revenant::ui::read_gain_setting;
using revenant::ui::read_positive_setting;
using revenant::ui::restore_radio_choice;
using revenant::ui::restore_radio_uri;

namespace {

[[nodiscard]] SourceDescriptor hf_dongle()
{
    SourceDescriptor out;
    out.uri = "rtlsdr://0";
    out.backend = "rtlsdr";
    out.serial = "00000001";
    out.tune_ranges.push_back(TuneRange{.low_hz = 24'000'000, .high_hz = 1'766'000'000});
    out.min_rate = 225'001;
    out.max_rate = 3'200'000;
    out.direct_sampling_available = true;

    GainStage tuner;
    tuner.name = "tuner";
    tuner.min_db = 0.0;
    tuner.max_db = 49.6;
    tuner.steps_db = {0.0, 19.7, 20.7, 49.6};
    tuner.has_auto = true;
    out.gain_stages.push_back(tuner);
    return out;
}

[[nodiscard]] RadioSnapshot snap(std::string radio, std::int64_t centre)
{
    RadioSnapshot out;
    out.radio_id = std::move(radio);
    out.memory.centre_hz = centre;
    out.memory.rate = 2'400'000;
    out.memory.gain_db = 20.7;
    return out;
}

}  // namespace

TEST_CASE("each value sits beside directSampling under the radio's id", "[radio_memory]")
{
    // Keyed by the calibration key and not the URI: rtlsdr://0 is whichever
    // dongle enumerated first today.
    const std::string id = radio_settings_id(descriptor_calibration_key(hf_dongle()));
    CHECK(id == "rtlsdr:00000001");
    CHECK(radio_setting_key(id, kRadioRateLeaf) == "source/radios/rtlsdr:00000001/sampleRate");
    CHECK(radio_setting_key(id, kRadioCentreLeaf) == "source/radios/rtlsdr:00000001/centreHz");
    CHECK(radio_setting_key(id, kRadioGainLeaf) == "source/radios/rtlsdr:00000001/gain");

    // Same group the direct sampling mode has always been written under.
    CHECK(direct_sampling_key(id) == radio_setting_key(id, "directSampling"));

    // A serial with a slash in it stays one path segment.
    CHECK(radio_setting_key(radio_settings_id("rtlsdr:a/b"), kRadioGainLeaf) ==
          "source/radios/rtlsdr:a_b/gain");

    // No serial, no id: a file or a synthetic scene keeps nothing.
    SourceDescriptor file = hf_dongle();
    file.serial.clear();
    CHECK(descriptor_calibration_key(file).empty());
}

TEST_CASE("gain round trips as auto or decibels, and junk is nothing kept", "[radio_memory]")
{
    RadioMemory memory;
    memory.gain_db = 20.7;
    CHECK(gain_setting_text(memory) == "20.7");
    memory.gain_auto = true;
    CHECK(gain_setting_text(memory) == "auto");

    RadioMemory back;
    read_gain_setting("auto", back);
    CHECK(back.gain_auto);
    CHECK_FALSE(back.gain_db.has_value());

    read_gain_setting("33.8", back);
    CHECK_FALSE(back.gain_auto);
    CHECK(back.gain_db == 33.8);

    for (const char* junk : {"", "nan", "20dB", "loud"}) {
        read_gain_setting(junk, back);
        CHECK_FALSE(back.has_gain());
    }

    // Zero is "leave the key off" everywhere in source_choice.h.
    CHECK_FALSE(read_positive_setting(0).has_value());
    CHECK_FALSE(read_positive_setting(-5).has_value());
    CHECK(read_positive_setting(2'048'000) == 2'048'000);
}

TEST_CASE("a picked radio's HF centre survives because the mode is restored first",
          "[radio_memory][order]")
{
    RadioMemory memory;
    memory.centre_hz = 7'100'000;
    memory.rate = 1'024'000;
    memory.gain_db = 20.7;

    // With the Q branch restored, 7.1 MHz is inside the envelope.
    const auto choice = restore_radio_choice(hf_dongle(), memory, std::string("q"));
    REQUIRE(choice.direct_sampling == "q");
    CHECK(compose_source_uri(hf_dongle(), choice) ==
          "rtlsdr://0?rate=1024000&freq=7100000&gain=20.7&direct=q");

    // The wrong order: no mode known when the centre is placed clamps it to
    // the tuner's floor. Asserted so the test fails if the mode stops riding
    // with the restore.
    const auto without = restore_radio_choice(hf_dongle(), memory, std::nullopt);
    CHECK(compose_source_uri(hf_dongle(), without) ==
          "rtlsdr://0?rate=1024000&freq=24000000&gain=20.7");
}

TEST_CASE("a radio with no gain stage or no direct sampling gets neither", "[radio_memory]")
{
    SourceDescriptor plain = hf_dongle();
    plain.gain_stages.clear();
    plain.direct_sampling_available = false;

    RadioMemory memory;
    memory.centre_hz = 98'100'000;
    memory.gain_auto = true;
    const auto choice = restore_radio_choice(plain, memory, std::string("q"));
    CHECK_FALSE(choice.direct_sampling.has_value());
    CHECK(choice.gains.empty());
    CHECK(compose_source_uri(plain, choice) == "rtlsdr://0?freq=98100000");
}

TEST_CASE("the reopen URI carries what the radio was left at", "[radio_memory][reopen]")
{
    RadioMemory memory;
    memory.centre_hz = 7'100'000;
    memory.rate = 1'024'000;
    memory.gain_auto = true;

    // Opened at 98.1 MHz on the tuner, left on the Q branch at 7.1 MHz.
    const std::string last = "rtlsdr://0?rate=2400000&freq=98100000&gain=20.7";
    CHECK(restore_radio_uri(last, memory, std::string_view("q")) ==
          "rtlsdr://0?direct=q&freq=7100000&rate=1024000&gain=auto");

    // Mode "off" takes direct= away rather than writing it.
    CHECK(restore_radio_uri("rtlsdr://0?freq=7100000&direct=q", RadioMemory{},
                            std::string_view("off")) == "rtlsdr://0?freq=7100000");

    // Nothing remembered leaves the URI as it was.
    CHECK(restore_radio_uri(last, RadioMemory{}, std::nullopt) == last);

    // A URI with no freq= never gains one: that backend has no tuner.
    RadioMemory centre_only;
    centre_only.centre_hz = 7'100'000;
    CHECK(restore_radio_uri("synthetic://two-tones?rate=48000", centre_only, std::nullopt) ==
          "synthetic://two-tones?rate=48000");

    CHECK(restore_radio_uri("", memory, std::nullopt).empty());
}

TEST_CASE("tuning is written once it settles, not on every step", "[radio_memory][debounce]")
{
    RadioSaveQueue queue;

    // A sweep of wheel notches 200 ms apart writes nothing while it moves.
    for (int step = 0; step < 10; ++step) {
        CHECK_FALSE(queue.note(snap("a", 100'000'000 + step * 25'000), step * 200).has_value());
        CHECK_FALSE(queue.due(step * 200).has_value());
    }
    CHECK(queue.pending());
    const std::int64_t last_note = 9 * 200;
    CHECK(queue.wait_ms(last_note) == kRadioSaveSettleMs);
    CHECK_FALSE(queue.due(last_note + kRadioSaveSettleMs - 1).has_value());

    const auto written = queue.due(last_note + kRadioSaveSettleMs);
    REQUIRE(written.has_value());
    CHECK(written->memory.centre_hz == 100'000'000 + 9 * 25'000);
    CHECK_FALSE(queue.pending());

    // The same state again is not owed a second write.
    CHECK_FALSE(queue.note(*written, 5000).has_value());
    CHECK_FALSE(queue.pending());

    // Tuned away and back inside the settle: nothing owed either.
    queue.note(snap("a", 1), 6000);
    CHECK(queue.pending());
    queue.note(*written, 6100);
    CHECK_FALSE(queue.pending());
}

TEST_CASE("a change of radio writes the old one at once", "[radio_memory][debounce]")
{
    RadioSaveQueue queue;
    queue.note(snap("a", 7'100'000), 0);

    // The new radio's identity arrives 300 ms later, well inside the settle.
    const auto old = queue.note(snap("b", 98'100'000), 300);
    REQUIRE(old.has_value());
    CHECK(old->radio_id == "a");
    CHECK(old->memory.centre_hz == 7'100'000);

    // And the new one waits for its own settle.
    CHECK(queue.pending());
    CHECK_FALSE(queue.due(300 + kRadioSaveSettleMs - 1).has_value());
    const auto next = queue.due(300 + kRadioSaveSettleMs);
    REQUIRE(next.has_value());
    CHECK(next->radio_id == "b");

    // A moment with no identity, between a close and the next descriptor,
    // also writes what was pending and keeps nothing for the empty id.
    queue.note(snap("b", 99'000'000), 5000);
    const auto flushed = queue.note(RadioSnapshot{}, 5100);
    REQUIRE(flushed.has_value());
    CHECK(flushed->memory.centre_hz == 99'000'000);
    CHECK_FALSE(queue.pending());
}

TEST_CASE("closing the window writes what has not settled", "[radio_memory][debounce]")
{
    RadioSaveQueue queue;
    CHECK_FALSE(queue.flush().has_value());

    queue.note(snap("a", 7'000'000), 0);
    const auto at_close = queue.flush();
    REQUIRE(at_close.has_value());
    CHECK(at_close->memory.centre_hz == 7'000'000);
    CHECK_FALSE(queue.flush().has_value());

    // And the flushed state counts as written.
    CHECK_FALSE(queue.note(*at_close, 10).has_value());
    CHECK_FALSE(queue.pending());
}
