// What a client sees, in plain structs that depend on nothing.
//
// WHY THIS IS NOT JUST core/engine/engine.h
//
// The obvious client API hands back engine::EngineInfo and
// engine::SpectrumFrame, and it cannot: returning them means linking
// revenant_core, revenant_core is /MT, and the Qt process is /MD. That is
// the constraint the whole two-process split exists to satisfy, and a client
// header that reaches into the engine puts it straight back.
//
// So these are deliberate near-duplicates of the engine structs. The
// duplication is the cost of the process boundary and is bounded: the schema
// is the contract, these mirror the schema, and core/rpc/convert.cpp is the
// only place the engine side is touched. A field added to the schema and not
// here is a field the UI cannot see, which is a compile-time gap in the
// client rather than a silent zero.
//
// Nothing here includes capnp either, so a consumer can hold these without
// the generated header in its translation unit.
//
// Frequencies stay rational for the reason docs/conventions.md gives. A
// display converts once, where it draws the label.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revenant::rpc {

struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    [[nodiscard]] constexpr double hertz() const {
        return denominator == 0 ? 0.0
                                : static_cast<double>(numerator) /
                                      static_cast<double>(denominator);
    }
};

// Mirrors revenant::engine::Demod ordinal for ordinal. The static_asserts
// that hold that true live in core/rpc/convert.h, which is the only file
// that sees both this and the engine.
enum class Demod : std::uint8_t { Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw };

struct DeviceInfo {
    std::uint32_t index = 0;
    std::string name;
    std::string vendor;
    bool discrete = false;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
};

struct GridParams {
    std::uint32_t channels = 0;
    std::uint32_t taps_per_branch = 0;
    std::uint32_t decimation = 0;
};

struct SpectrumGeometry {
    std::uint32_t transform = 0;
    std::uint32_t bins_per_channel = 0;
    std::uint32_t channels = 0;
    std::uint32_t bins = 0;
    Rational bin_width;
    Rational bin_zero;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }
};

struct EngineInfo {
    DeviceInfo device;
    GridParams grid;
    std::uint32_t source_rate = 0;
    std::uint32_t channel_rate = 0;
    std::int64_t channel_spacing = 0;
    SpectrumGeometry spectrum;
    std::int64_t source_center = 0;
    std::uint64_t ring_samples = 0;
    double ring_seconds = 0.0;

    // The engine did not build what was asked for, and why. See the schema:
    // this is also how a clamped channel count or block size is reported,
    // because it is the only field in EngineInfo that can carry a sentence.
    bool ring_clamped = false;
    std::string ring_clamp_reason;
};

struct SourceDescriptor {
    std::string uri;
    std::string backend;
    std::string display_name;
    std::string unavailable;

    [[nodiscard]] bool available() const { return unavailable.empty(); }
};

struct SourceStats {
    std::uint64_t blocks_delivered = 0;
    std::uint64_t samples_delivered = 0;
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_lost = 0;
    std::uint64_t last_loss_index = 0;
    std::uint64_t write_index = 0;
};

struct VrxParams {
    // Hertz from the SOURCE'S BASEBAND DC, bounded by plus and minus half
    // the source rate. Not an absolute radio frequency: the engine's grid
    // has no other frame, engine::place reads this as an offset, and nothing
    // between here and there rebases it. A caller holding an absolute
    // frequency writes `center = absolute - EngineInfo::source_center`.
    //
    // Spelled out on a mirror struct that otherwise carries no comments
    // because this is the field the schema found people getting wrong, and
    // because the schema's own note on it is a retraction: it used to say
    // the offset reading had to be stated "because the engine's own header
    // says the opposite", and core/engine/vrx.h was the document that was
    // wrong. See core/rpc/revenant.capnp, which carries the full record.
    std::int64_t center = 0;

    // A SHORTHAND for the passband below, not the request itself. Expanded
    // through the mode's rule when passband_low and passband_high are both
    // zero, and ignored when either is set. Echoed back exactly as sent, so
    // it is not the granted width; granted_high minus granted_low is.
    //
    // It used to be the whole request, read as a half-width either side of
    // center, and that reading was already untrue of USB and LSB. See
    // core/rpc/revenant.capnp, which carries the full record.
    std::int64_t bandwidth = 12'000;

    // Signed hertz from center, low strictly below high, both zero for "not
    // stated". The filter passes [center + low, center + high] on every
    // mode; CW's pitch moves what is mixed to DC and is not an edge.
    std::int64_t passband_low = 0;
    std::int64_t passband_high = 0;

    Demod demod = Demod::Nfm;
    std::uint32_t audio_rate = 0;
    double squelch_dbfs = -200.0;
    double agc_attack_ms = 10.0;
    double agc_decay_ms = 500.0;
    bool agc_enabled = true;
    std::int64_t cw_pitch = 700;
};

struct VrxPlacement {
    std::uint32_t channel = 0;
    Rational channel_centre;
    Rational residual;
    std::uint32_t channel_rate = 0;

    // Either edge was pulled in to fit the channel, and the pair that came
    // back. The fit is per edge, so a clamped receiver can be off-centre as
    // well as narrow and the bool alone no longer says what happened.
    bool bandwidth_clamped = false;
    std::int64_t granted_low = 0;
    std::int64_t granted_high = 0;
};

struct VrxStatus {
    std::uint64_t id = 0;
    VrxParams params;
    VrxPlacement placement;

    // The rate the fine stage resampled to. A change that moves it is a
    // remove and an add rather than a push constant.
    std::uint32_t demod_rate = 0;

    double level_dbfs = -200.0;
    bool squelch_open = false;
    std::uint64_t audio_samples = 0;
    std::uint64_t audio_dropped = 0;
};

// Mirrors revenant::detect::TrackState ordinal for ordinal, and the schema's
// TrackState with it. core/rpc/convert.h holds the schema-to-engine asserts
// and core/rpc/client.cpp holds the schema-to-here ones, which is the same
// arrangement Demod has above.
//
// Pending is here so the ordinals line up and never arrives: the detector
// publishes Live, Held and Merged only.
enum class TrackState : std::uint8_t { Pending, Live, Held, Merged };

[[nodiscard]] constexpr const char* track_state_name(TrackState state) {
    switch (state) {
        case TrackState::Pending: return "pending";
        case TrackState::Live: return "live";
        case TrackState::Held: return "held";
        case TrackState::Merged: return "merged";
    }
    return "unknown";
}

// One thing the wideband detector is tracking. See the long note on the
// schema's Detection for what is deliberately absent, which is the tracker's
// working state and a logical centre nothing can compute yet.
struct Detection {
    std::uint64_t id = 0;

    // Absolute radio frequency and occupied bandwidth, integer hertz. Not
    // Rational, and that is not this layer rounding: the detector rounds once
    // at measurement out of the frame's exact rational axis, so a ratio here
    // would dress an estimate up as a grid frequency.
    //
    // VrxParams::center is a BASEBAND offset, so tuning to this detection is
    // `params.center = center_hz - info.source_center`. On a source with no
    // declared centre the two are equal and getting it wrong costs nothing,
    // which is exactly why it has to be written down.
    //
    // This sentence used to end "despite what the engine header calls it",
    // which was true of core/engine/vrx.h until 2026-09-19 and is not now.
    // The header called the field absolute, the code never treated it that
    // way, and the header was corrected rather than the code. Retracted here
    // rather than edited out, because the same claim stood in the schema and
    // in docs/detection.md and a reader may have taken it from any of them.
    std::int64_t center_hz = 0;
    std::int64_t bandwidth_hz = 0;

    double snr_2500_db = 0.0;
    double confidence = 0.0;
    TrackState state = TrackState::Pending;

    // Absolute source sample indices. Seconds are a difference over
    // EngineInfo::source_rate, which is the only clock the detector has.
    std::uint64_t first_seen = 0;
    std::uint64_t last_seen = 0;
    std::uint64_t last_detected = 0;

    // Meaningless unless channel_valid.
    std::uint32_t channel = 0;
    bool channel_valid = false;

    // Non-zero only while state is Merged.
    std::uint64_t merged_into = 0;

    [[nodiscard]] constexpr std::uint64_t age_samples() const {
        return last_seen - first_seen;
    }

    // Zero while Live.
    [[nodiscard]] constexpr std::uint64_t silent_samples() const {
        return last_seen - last_detected;
    }
};

struct DetectionList {
    // Ascending in frequency, and already filtered to the bar the call asked
    // for.
    std::vector<Detection> detections;

    // Zero decisions means the detector has not decided yet, which is what
    // the call that built it answers with. It is not an empty band.
    std::uint64_t decisions = 0;
    std::uint64_t last_decision = 0;

    // What the detector holds before the confidence bar, so a short list and
    // a filtered one are distinguishable.
    std::uint32_t total = 0;

    double detection_threshold_db = 0.0;

    // Seconds of SOURCE time the detector keeps a track it is no longer
    // detecting. DetectorConfig::bootstrap_hold_seconds, read back rather
    // than assumed, and the engine's statement about when it will stop
    // publishing a track rather than anything about how one is drawn. The
    // schema's note on detectorHoldSeconds has the distinction.
    //
    // Zero means the engine did not state it, which is either an engine
    // older than the field or a list nobody has fetched yet, since a
    // default-constructed one starts here. Not a track dropped the instant
    // it goes quiet.
    double detector_hold_seconds = 0.0;
};

// Unlike engine::SpectrumFrame, this one owns its bins.
//
// The engine's version hands out a span valid only for the duration of the
// sink call, which is right there and wrong here: by the time a client sees
// a frame it has already been copied onto the wire, and handing a UI a span
// into a capnp message it does not own would be a lifetime bug waiting for
// the first consumer that keeps a row.
struct SpectrumFrame {
    std::vector<float> power_db;
    SpectrumGeometry geometry;
    std::uint64_t start = 0;
    std::uint64_t count = 0;

    // Frames the ENGINE produced before this one, which is not the number
    // this subscription received. A client that asked for decimation can
    // tell its own skipping from the engine's by watching this jump by more
    // than it asked for.
    std::uint64_t sequence = 0;

    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

// The frequency axis of one receiver's passband frame.
//
// Per frame rather than in EngineInfo, because only the transform size is
// engine-wide: the width is the receiver's demodulation rate and moves
// whenever its filter does.
struct PassbandGeometry {
    std::uint32_t transform = 0;
    std::uint32_t bins = 0;
    std::uint32_t rate = 0;
    Rational bin_width;

    // Half the demodulation rate below whatever the fine stage mixed to DC,
    // which is NOT always the receiver's centre: on CW it sits a pitch
    // below. Carried rather than derived so a display drawing filter edges
    // against it needs no per-mode arithmetic of its own.
    Rational bin_zero;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }
};

struct PassbandFrame {
    std::uint64_t vrx = 0;
    std::vector<float> power_db;
    PassbandGeometry geometry;
    std::uint64_t start = 0;
    std::uint64_t count = 0;

    // Frames the engine delivered to THIS RECEIVER before this one.
    std::uint64_t sequence = 0;

    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

// Which of the two mutually incompatible readings of the same bitstream the
// RDS decoder is configured for.
//
// Ordinal for ordinal with schema::RdsRegion and with
// revenant::decode::Region, on the terms the schema states: the ordering is
// matched here so that the conversion can be a cast once the branch that
// serves rdsStation writes one, and it is not enforced yet because nothing
// converts it yet.
//
// A SETTING AND NEVER AN INFERENCE. core/decode/rds_groups.h has the argument
// and core/rpc/revenant.capnp repeats it: no field names the region, the PI
// code cannot decide it because the US call sign range collides with European
// country codes, and getting it wrong is silent because PTY 26 renders as
// National Music in one region and Hip-Hop in the other. A client may seed it
// from the tuned frequency as long as it shows it as something the operator
// can override.
//
// WHY THERE IS NO RdsStation STRUCT HERE, AND NO AudioChunk
//
// Neither surface is served. A mirror of either one would be a hundred lines
// of conversion no test could exercise and nothing could populate, sitting in
// the one header whose whole job is to be the contract a UI compiles against.
// This enum is here because Client::set_rds_region needs an argument to take;
// the payload halves arrive with the branches that fill them, and
// Client::rds_station returning Status rather than a struct is where the
// compiler will point that branch.
enum class RdsRegion : std::uint8_t { Rds, Rbds };

}  // namespace revenant::rpc
