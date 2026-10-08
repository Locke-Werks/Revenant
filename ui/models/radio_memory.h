// What each radio was left at: its sample rate, its centre and its gain, and
// the rules for putting them back the next time that radio is opened.
//
// WHY PER RADIO AND NOT PER URI. The owner asked on 2026-10-07 for the sample
// rate, the start frequency and the gain to be kept "for each receiver". Rate
// and gain belong to the front end, so the receiver here is the radio. A
// source URI names a dongle by its index, which moves when a second one is
// plugged in, and engine/lastSource holds the centre the radio was OPENED at,
// not the one it was left at after an evening of tuning. The radio id is the
// one models/detector_scope.h keys the detector under, the engine's
// calibration key made safe for a settings path, so every per-radio value
// lives under one group:
//
//   source/radios/<radio>/sampleRate      samples per second
//   source/radios/<radio>/centreHz        absolute hertz
//   source/radios/<radio>/gain            "auto", or decibels as a decimal
//   source/radios/<radio>/directSampling  source_choice.h, unchanged
//
// A source with no serial, a recording or a synthetic scene, has no radio id
// and nothing is kept for it: two files share no front end.
//
// SAVED ON SETTLE AND NOT ON EVERY STEP. The centre moves on every wheel
// notch, and a QSettings write on Windows is a registry write. RadioSaveQueue
// below holds the latest state and hands it over once it has stopped moving
// for kRadioSaveSettleMs, or at once when the radio changes or the window
// closes.
//
// Qt-free on the rule the rest of ui/models follows, so ui/tests can hold it.

#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "models/source_choice.h"

namespace revenant::ui {

inline constexpr std::string_view kRadioRateLeaf = "sampleRate";
inline constexpr std::string_view kRadioCentreLeaf = "centreHz";
inline constexpr std::string_view kRadioGainLeaf = "gain";

// Which radio engine/lastSource was, so the reopen at launch can find that
// radio's values. The URI cannot say: rtlsdr://0 is an index, not a serial.
inline constexpr std::string_view kLastRadioKey = "source/lastRadio";

// How long a value has to stop moving before it is written.
inline constexpr std::int64_t kRadioSaveSettleMs = 1000;

// source/radios/<radio>/<leaf>, the shape direct_sampling_key already writes.
[[nodiscard]] inline std::string radio_setting_key(std::string_view radio_id,
                                                   std::string_view leaf)
{
    std::string key(kSourceGroup);
    key += "/radios/";
    key += radio_id;
    key += '/';
    key += leaf;
    return key;
}

// One radio's remembered state. Nothing in a field means nothing was kept for
// it, and the restore then leaves that key alone rather than inventing one:
// source_choice.h has what a zero standing in for "unset" cost.
struct RadioMemory {
    std::optional<std::int64_t> centre_hz;
    std::optional<std::int64_t> rate;
    std::optional<double> gain_db;
    bool gain_auto = false;

    [[nodiscard]] bool has_gain() const { return gain_auto || gain_db.has_value(); }

    friend bool operator==(const RadioMemory&, const RadioMemory&) = default;
};

// The gain as stored: "auto" or a decimal with the device's tenth of a dB.
[[nodiscard]] inline std::string gain_setting_text(const RadioMemory& memory)
{
    if (memory.gain_auto) {
        return "auto";
    }
    if (memory.gain_db.has_value()) {
        return detail::decimal(*memory.gain_db);
    }
    return {};
}

// Back from the stored text into `memory`. Junk, empty and non-finite read as
// nothing kept, because a settings file is editable and a NaN reaching a URI
// is a refusal that names the parser instead of the setting.
inline void read_gain_setting(std::string_view text, RadioMemory& memory)
{
    memory.gain_auto = false;
    memory.gain_db.reset();
    if (text == "auto") {
        memory.gain_auto = true;
        return;
    }
    if (text.empty()) {
        return;
    }
    double value = 0.0;
    const char* first = text.data();
    const char* last = first + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec == std::errc{} && result.ptr == last && std::isfinite(value)) {
        memory.gain_db = value;
    }
}

// A stored centre or rate. Zero and below read as nothing kept: zero is the
// "leave the key off" value everywhere in source_choice.h, and a negative
// rate or centre describes no radio.
[[nodiscard]] inline std::optional<std::int64_t> read_positive_setting(std::int64_t stored)
{
    if (stored <= 0) {
        return std::nullopt;
    }
    return stored;
}

// The picker's choice for a radio that has a memory.
//
// THE STORED VALUES ARE THE SOURCE OF TRUTH FOR A PICK, so they replace what
// the boxes held. The direct sampling mode goes in FIRST and on purpose: the
// centre below is clamped against the envelope that mode gives, and a 7 MHz
// centre restored before an HF radio's Q branch was known would be clamped to
// the tuner's 24 MHz floor. compose_source_uri computes the envelope from
// choice.direct_sampling, so having it set before compose is the whole rule.
[[nodiscard]] inline SourceChoice restore_radio_choice(const rpc::SourceDescriptor& source,
                                                       const RadioMemory& memory,
                                                       std::optional<std::string> direct)
{
    SourceChoice choice;
    if (source.direct_sampling_available) {
        choice.direct_sampling = std::move(direct);
    }
    choice.center_hz = memory.centre_hz;
    choice.rate = memory.rate;
    if (!source.gain_stages.empty() && memory.has_gain()) {
        GainChoice gain;
        gain.stage = source.gain_stages.front().name;
        gain.automatic = memory.gain_auto;
        gain.db = memory.gain_db;
        choice.gains.push_back(std::move(gain));
    }
    return choice;
}

// The remembered URI with the radio's own values written over it, for the
// reopen at launch.
//
// The last URI carries the centre, rate and gain the radio was OPENED with,
// and the memory carries what it was left at, so each key the memory has
// replaces the URI's. A key the memory lacks keeps the URI's value. direct=
// is written before freq= for the same reason restore_radio_choice sets it
// first: the engine opens the mode before it tunes, and a reader of the URI
// sees the mode that makes the centre legal ahead of the centre.
//
// freq= is written only over a URI that already has one. Only a tunable
// backend takes it, every other one refuses it by name, and the last URI was
// composed against the device's descriptor, so a freq= there is the proof
// the device has a tuner that this function cannot otherwise see.
[[nodiscard]] inline std::string restore_radio_uri(std::string_view uri,
                                                   const RadioMemory& memory,
                                                   std::optional<std::string_view> direct)
{
    std::string out(uri);
    if (out.empty()) {
        return out;
    }
    const bool tunable = out.find("?freq=") != std::string::npos ||
                         out.find("&freq=") != std::string::npos;

    if (direct.has_value()) {
        out = with_direct_sampling(out, *direct);
    }
    if (tunable && memory.centre_hz.has_value()) {
        out = with_query_value(out, "freq", std::to_string(*memory.centre_hz));
    }
    if (memory.rate.has_value()) {
        out = with_query_value(out, "rate", std::to_string(*memory.rate));
    }
    if (memory.has_gain()) {
        out = with_query_value(out, "gain", gain_setting_text(memory));
    }
    return out;
}

// What the window holds about the open radio, sampled on every change.
struct RadioSnapshot {
    std::string radio_id;  // empty: nothing to keep
    RadioMemory memory;

    friend bool operator==(const RadioSnapshot&, const RadioSnapshot&) = default;
};

// The debounce. note() on every change, due() from the timer, flush() at
// close. Each answers the snapshot to write, or nothing.
//
// A CHANGE OF RADIO WRITES THE OLD ONE AT ONCE. Holding it for the settle
// would let the new radio's first note replace it, and the last second of
// tuning on the radio being put down would be lost.
class RadioSaveQueue {
public:
    // Returns a snapshot to write now, which is only ever the previous radio's
    // pending state when the radio changed under it.
    std::optional<RadioSnapshot> note(const RadioSnapshot& now, std::int64_t now_ms)
    {
        std::optional<RadioSnapshot> write_now;
        if (pending_.has_value() && pending_->radio_id != now.radio_id) {
            write_now = std::move(pending_);
            pending_.reset();
        }
        if (now.radio_id.empty()) {
            return write_now;
        }
        const bool same_as_written = written_.has_value() && *written_ == now;
        if (same_as_written) {
            // Tuned away and back inside the settle: nothing is owed.
            pending_.reset();
            return write_now;
        }
        if (!pending_.has_value() || *pending_ != now) {
            pending_ = now;
            deadline_ms_ = now_ms + kRadioSaveSettleMs;
        }
        if (write_now.has_value()) {
            written_ = write_now;
        }
        return write_now;
    }

    // The pending state once it has held still for the settle, or nothing.
    std::optional<RadioSnapshot> due(std::int64_t now_ms)
    {
        if (!pending_.has_value() || now_ms < deadline_ms_) {
            return std::nullopt;
        }
        return take();
    }

    // Whatever is pending, regardless of the clock, for the window closing.
    std::optional<RadioSnapshot> flush() { return take(); }

    [[nodiscard]] bool pending() const { return pending_.has_value(); }

    // How long the timer should wait from `now_ms`, for a QTimer restarted on
    // each note.
    [[nodiscard]] std::int64_t wait_ms(std::int64_t now_ms) const
    {
        return deadline_ms_ > now_ms ? deadline_ms_ - now_ms : 0;
    }

private:
    std::optional<RadioSnapshot> take()
    {
        std::optional<RadioSnapshot> out = std::move(pending_);
        pending_.reset();
        if (out.has_value()) {
            written_ = out;
        }
        return out;
    }

    std::optional<RadioSnapshot> pending_;
    std::optional<RadioSnapshot> written_;
    std::int64_t deadline_ms_ = 0;
};

}  // namespace revenant::ui
