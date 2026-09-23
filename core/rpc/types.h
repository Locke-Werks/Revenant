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
#include <string_view>
#include <variant>
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
enum class Demod : std::uint8_t {
    Raw,
    Am,
    Nfm,
    Wfm,
    Usb,
    Lsb,
    Dsb,
    Cw,
    P25p1,
    Dstar,
    Tetra,
};

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

    // Baseband DC in real radio frequency. IT MOVES: set_source_center
    // retunes the front end and this follows, so a client that cached it at
    // connect draws every absolute frequency a retune's worth of hertz out.
    // Read it again after a retune rather than adding the delta, because
    // the delta asked for is not always the one the device took.
    std::int64_t source_center = 0;

    std::uint64_t ring_samples = 0;
    double ring_seconds = 0.0;

    // The engine did not build what was asked for, and why. See the schema:
    // this is also how a clamped channel count or block size is reported,
    // because it is the only field in EngineInfo that can carry a sentence.
    bool ring_clamped = false;
    std::string ring_clamp_reason;

    // Capture seconds delivered per wall second, over the last
    // realtime_window_seconds. 1.0 is realtime, above is a replay running
    // faster than the recording was made, below is the source falling
    // behind. ZERO IS NOT MEASURED, which is what info() answers before the
    // run starts, and is not a stalled source. A pause the engine made to
    // retune or change a gain is not charged to the source; see
    // core/engine/pacing_window.h.
    //
    // Read it with source_paced_by: a factor of 0.5 is a fault when the
    // pace is zero and is the setting when the pace is 0.5. A client saying
    // "the audio is starving" from an empty queue is describing the queue
    // and naming the wrong component; this is what names the right one.
    double realtime_factor = 0.0;

    // The engine's --pace, as a multiple of realtime. Zero is unthrottled,
    // and means nothing at all on a live radio, which runs on its own clock.
    double source_paced_by = 0.0;

    // Which stream the sample indices on this connection belong to. Zero
    // before any source has been opened, one for the first, one higher for
    // every openSource after it.
    //
    // THE SECOND FIELD HERE THAT MOVES WHILE A CONNECTION STAYS UP, and
    // source_center is the first. That is the difference between a retune and
    // a source change: a retune keeps the stream and moves the constant
    // relating baseband to real frequency, and a source change starts a new
    // stream numbered from zero.
    //
    // Compare it for equality against the epoch a correlation was made at and
    // re-derive when it differs. There is no offset between two streams to
    // adjust by. core/rpc/revenant.capnp's note on sourceEpoch has the whole
    // argument, including what a client that ignores this gets wrong: audio
    // from one radio lined up against a spectrum frame from another, with the
    // arithmetic consistent throughout.
    std::uint64_t source_epoch = 0;

    // The wall seconds realtime_factor covers. Zero from an engine built
    // before the factor was windowed, whose factor is a mean over the whole
    // run and cannot say whether the source is behind now.
    double realtime_window_seconds = 0.0;
};

// Whether the front end can be pointed somewhere else, and where.
//
// The pair is an ENVELOPE rather than a promise: a device with a gap in its
// coverage reports the outer bounds and still refuses a frequency inside the
// gap, in its own words. It exists so a client can grey out a control it
// could never use, rather than making the operator discover that by trying.
struct SourceTuning {
    // False for every file and every synthetic scene, whose centre is a
    // property of samples already written rather than a setting.
    bool can_retune = false;

    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;
};

// Why a retune removed a receiver: the schema's RetuneCause, ordinal for
// ordinal, which client.cpp static_asserts. Unknown is what a server built
// before the field sends, and an ordinal this client has no name for reads as
// Unknown too rather than being cast into one of the others.
enum class RetuneCause : std::uint8_t { Unknown, OutsideSpan, Unplaceable, ShapeChanged };

// One receiver a front-end retune removed, with the absolute frequency its
// centre was on before the tune, why, and the engine's sentence saying why.
// reason is empty from a server built before the field.
struct RetuneRemoval {
    std::uint64_t id = 0;
    std::int64_t frequency_hz = 0;
    RetuneCause cause = RetuneCause::Unknown;
    std::string reason;
};

// What Session.setSourceCenter answers: the centre the device took, and every
// receiver the retune removed, whether its centre fell outside the span or its
// new place in a channel needed a different filter. The server has already
// ended each removed receiver's audio and decoder subscriptions with the
// engine's reason by the time this arrives.
struct SourceRetune {
    std::int64_t granted_hz = 0;
    std::vector<RetuneRemoval> removed;
};

// What a source can be pointed at. An ENVELOPE and not a promise: a device with
// a gap in its coverage reports the ranges it knows about and still refuses a
// frequency inside a gap, in its own words.
struct TuneRange {
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    // Zero is continuous. A non-zero step is what set_source_center rounds to,
    // which is why it answers with the centre the device took.
    std::int64_t step_hz = 0;
};

// One gain control, named the way the device names it. A LIST AND NOT A NUMBER:
// an R820T has one tuner gain with 29 discrete steps, an Airspy has three
// stages, a file has none, and flattening those into a percentage is how a
// client comes to offer a control the device does not have.
struct GainStage {
    std::string name;
    double min_db = 0.0;
    double max_db = 0.0;

    // Empty is continuous. Populated means the stage only takes these values
    // and a request lands on the nearest, so a continuous slider over a stepped
    // stage shows the operator a number the device never took.
    std::vector<double> steps_db;

    // Whether the device will set this stage itself. A choice to offer, not the
    // sensible setting: see the schema's note and README.md's measurement of
    // what the RTL-SDR's own AGC did to the detector's track list.
    bool has_auto = false;
};

// Who sets the pace, which decides how realtime_factor is read.
enum class FlowControl : std::uint8_t { Paced, Demand };

// What the device puts on the bus. Every value before Unknown mirrors
// revenant::source::SampleFormat and the schema's SampleFormat, ordinal for
// ordinal, and client.cpp static_asserts each one. Cs24 went in ahead of
// Unknown rather than after it, because Unknown has no wire ordinal and the
// known values have to keep matching the schema's.
//
// Unknown IS THIS SIDE'S ONLY AND HAS NO ORDINAL ON THE WIRE. A Cap'n Proto
// enum field may legally hold a value the reader's schema has never heard of,
// which is how a client reaches an engine built against a newer one, and a
// descriptor read cannot refuse the way read_demod does: describing a list of
// sources is not all or nothing, and one dongle reporting a format this build
// does not know must not hide the backends that cannot fail. So an ordinal past
// Cs24 lands here rather than being clamped onto one of the known values,
// because the label is what a picker prints and "cf32" about an unknown format
// is a lie a reader would act on.
enum class SampleFormat : std::uint8_t { Cu8, Cs8, Cs16, Cf32, Cs24, Unknown };

[[nodiscard]] constexpr const char* sample_format_name(SampleFormat format) {
    switch (format) {
        case SampleFormat::Cu8:
            return "cu8";
        case SampleFormat::Cs8:
            return "cs8";
        case SampleFormat::Cs16:
            return "cs16";
        case SampleFormat::Cf32:
            return "cf32";
        case SampleFormat::Cs24:
            return "cs24";
        case SampleFormat::Unknown:
            break;
    }
    return "unknown";
}

struct SourceDescriptor {
    std::string uri;
    std::string backend;
    std::string display_name;
    std::string unavailable;

    [[nodiscard]] bool available() const { return unavailable.empty(); }

    // Conditions worth an operator's attention that did not stop the source
    // opening. Show them; they are not folded into unavailable, which is about
    // a source that cannot be used at all.
    std::vector<std::string> notes;

    // Empty for a source that cannot be tuned, and a client greys the frequency
    // control out rather than making the operator discover the refusal.
    std::vector<TuneRange> tune_ranges;

    // EMPTY MEANS CONTINUOUS between min_rate and max_rate. The RTL-SDR is
    // neither shape: its rate is a 28.8 MHz clock over an integer, discrete but
    // far too dense to list, so it reports the bounds and rounds.
    std::vector<std::uint32_t> sample_rates;
    std::uint32_t min_rate = 0;
    std::uint32_t max_rate = 0;

    SampleFormat native_format = SampleFormat::Cf32;

    // Bits per I or Q component, which is not eight times the byte width
    // everywhere: an Airspy in packed mode puts 12 bits in 16.
    std::uint8_t bits_per_component = 0;

    std::vector<GainStage> gain_stages;

    FlowControl flow = FlowControl::Demand;

    // True for a recording and false for every live device. Nothing seeks on
    // this wire; it is here so a picker can show a recording as a recording.
    bool seekable = false;

    // ZERO IS UNBOUNDED, which is every live device, and not a recording of no
    // length.
    std::uint64_t length_samples = 0;
};

// Mirrors revenant::detect::FrontEndVerdict ordinal for ordinal, and the
// schema's FrontEndState with it, on the same arrangement TrackState below
// has: convert.h holds the schema-to-engine asserts and client.cpp holds the
// schema-to-here ones.
//
// core/detect/front_end.h is the authority on what each of these means and,
// more to the point, on what it does not mean. Read its note on what the
// measurement cannot tell apart before turning FloorFollowsSignal into the
// word "overload" in front of an operator.
enum class FrontEndState : std::uint8_t { Unmeasured, Steady, SpanScales, FloorFollowsSignal };

[[nodiscard]] constexpr const char* front_end_state_name(FrontEndState state) {
    switch (state) {
        case FrontEndState::Unmeasured: return "unmeasured";
        case FrontEndState::Steady: return "steady";
        case FrontEndState::SpanScales: return "span-scales";
        case FrontEndState::FloorFollowsSignal: return "floor-follows-signal";
    }
    return "unknown";
}

struct SourceStats {
    std::uint64_t blocks_delivered = 0;
    std::uint64_t samples_delivered = 0;
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_lost = 0;
    std::uint64_t last_loss_index = 0;
    std::uint64_t write_index = 0;

    // What the full-span spectrum says about the front end. Computed from
    // the detector's own averaged spectrum and noise floor, so it stays
    // Unmeasured until something asks the engine for detections.
    FrontEndState front_end = FrontEndState::Unmeasured;

    // Decibels of floor movement per decibel the strongest signal on the
    // span moved, from the least-following part of the span. One is a gain
    // change; above one the floor is outrunning the signal driving it.
    double front_end_slope = 0.0;

    // How far the span's mean floor sits above the quietest this engine's
    // monitor has seen. Descriptive, a session low-water mark, and no
    // verdict rests on it.
    double front_end_floor_lift_db = 0.0;

    // Retunes a stage refused, which are the ones set_vrx_params reported as
    // successes: the call answers when the op is QUEUED and the graph applies
    // it a block later.
    //
    // REQUIRED TO STAY AT ZERO. A placement the demodulator cannot carry is
    // refused before the op is queued, so a non-zero value here is not an
    // operator asking for the impossible; it is placement and the stage
    // disagreeing. Read it as a defect rather than as a condition to handle.
    std::uint64_t vrx_retune_refusals = 0;

    // Times the recording thread waited for a frame slot. EXPECTED ON A DEMAND
    // SOURCE AND A WARNING ON A PACED ONE, so read it beside
    // SourceDescriptor::flow: a client that draws this without the flow control
    // reports the healthy case as a fault on every file and every synthetic
    // scene.
    std::uint64_t frame_stalls = 0;
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

    // WHAT A CALLER WITH NO EVIDENCE GETS, AND NOT A DECISION. Nfm is right
    // for a land mobile channel and wrong for everything wider, and a
    // client that leaves it alone while tuning from a measurement is
    // choosing a demodulator by omission. That is what produced a 16 kHz
    // receiver on a 145 kHz broadcast station: nothing overrode this, so
    // nothing had to be wrong for the audio to be mush.
    //
    // A caller holding a measurement asks engine::demod_for_signal, which
    // is the one rule and carries its own record of what it gets wrong.
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

    // Empty when nothing was clamped. Otherwise the sentence to put in front
    // of the operator: what was asked for, what one grid channel could
    // carry, whether that makes the demodulator wrong rather than merely
    // narrow, and that the fix is the engine's channel count.
    //
    // Prose, never parsed. bandwidth_clamped and the granted pair are the
    // machine-readable half.
    std::string clamp_reason;
};

struct VrxStatus {
    std::uint64_t id = 0;
    VrxParams params;
    VrxPlacement placement;

    // The rate the fine stage resampled to. A change that moves it is a
    // remove and an add rather than a push constant.
    std::uint32_t demod_rate = 0;

    // The audio rate this receiver ACTUALLY runs at. params.audio_rate is an
    // echo and reads zero whenever the request named no rate, so every
    // question that depends on the audio rate depends on this one: the
    // de-emphasis curve, stereo, and whether the receiver can carry an RDS
    // composite at all. Zero means nobody filled it in, from an engine built
    // before the field existed or from a default-constructed status.
    std::uint32_t resolved_audio_rate = 0;

    double level_dbfs = -200.0;
    bool squelch_open = false;
    std::uint64_t audio_samples = 0;
    std::uint64_t audio_dropped = 0;

    // Times the engine restarted this receiver because its input samples were
    // overwritten before it could filter them, and the audio frames those
    // restarts skipped past. The dropout counters: audio_dropped is an engine
    // fault that ends the run and these are gaps a listener hears.
    //
    // Commented on a mirror struct that mostly carries none because the pair
    // means nothing on its own. reanchors climbing while
    // SourceStats::samples_lost stands still is this machine failing to keep
    // up with its own source; both climbing together is a device retune
    // declaring the gap it opened. core/rpc/revenant.capnp carries the full
    // note, and core/engine/vrx.h says where the numbers come from.
    std::uint64_t reanchors = 0;
    std::uint64_t reanchor_frames_skipped = 0;

    // Who this receiver belongs to. creator_session is the session that
    // added it, zero when it was added by the process hosting the engine
    // rather than over the wire. A receiver goes when its creator's session
    // ends unless `kept` is set, which addVrx grants on request for a
    // headless recorder. owned_by_caller is creator_session compared against
    // the session asking, so a client can tell its own receivers from
    // another operator's without holding a session id of its own.
    std::uint64_t creator_session = 0;
    bool kept = false;
    bool owned_by_caller = false;
};

// What addVrx is asked to do with a receiver when the session that created
// it ends. Session is the default and is the owner decision recorded on
// 2026-09-22: a crashed window takes its receivers with it. Kept is for a
// headless recorder, whose receivers have to survive a client restarting.
enum class VrxLifetime : std::uint8_t { Session, Kept };

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

    // Consecutive detections, not signal quality. See the schema.
    double confidence = 0.0;

    // How far above the threshold in force, zero to one. The calibrated one.
    double margin_confidence = 0.0;

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

    // The band's excess in its strongest three adjacent bins over its excess
    // in total, which is the unit characterise::spectral_concentration answers
    // in. See the schema for what it is for; the short version is that
    // confidence and margin_confidence both say nothing about the band being
    // SHAPED like a transmission and this does.
    //
    // READ IT WITH bandwidth_hz, and check shape_measured before believing a
    // zero: unmeasured arrives as 0.0 and 0.0 is the most noise-like reading
    // there is.
    double concentration = 0.0;
    bool shape_measured = false;

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
// engine-wide: the width is half the receiver's display rate, which steps
// only when the passband's reach crosses a rung. revenant.capnp has the rule.
// (This used to say the width "is the receiver's demodulation rate and moves
// whenever its filter does", while the frame was the fine stream.)
struct PassbandGeometry {
    // bins is transform / 2, the transform's central half.
    std::uint32_t transform = 0;
    std::uint32_t bins = 0;

    // The display stream's rate, twice the width of the frame.
    std::uint32_t rate = 0;
    Rational bin_width;

    // A quarter of the display rate below whatever the fine stage mixed to
    // DC, which is NOT always the receiver's centre: on CW it sits a pitch
    // below. Carried rather than derived so a display drawing filter edges
    // against it needs no per-mode arithmetic of its own. (Half the
    // demodulation rate below it, this used to say.)
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

// One receiver's audio, as it reaches a client.
//
// Owns its samples, for the reason SpectrumFrame owns its bins: by the time
// a client sees one it has already been copied onto the wire, and handing a
// UI a span into a capnp message it does not own is a lifetime bug waiting
// for the first consumer that keeps a buffer.
struct AudioChunk {
    // Interleaved when channel_count is above one. Real audio, never complex
    // baseband: a raw tap cannot be subscribed to.
    std::vector<float> samples;

    // Both on every chunk rather than cached from VrxStatus. A pane that
    // outlives the receiver behind it, which ui/models/receiver_link.cpp
    // makes the ordinary case by turning a refused retune into a remove and
    // an add, would otherwise play the next receiver's stream at the last
    // one's rate.
    std::uint32_t sample_rate = 0;
    std::uint16_t channel_count = 1;

    // Absolute index of the first FRAME, from the start of this receiver's
    // audio stream. Contiguous when it equals the previous chunk's index
    // plus its frame count.
    std::uint64_t sample_index = 0;

    // Frames this subscription lost between the previous chunk and this one
    // because its queue was full. A sample_index gap LARGER than this lost
    // the remainder upstream, which is a different fault with a different
    // fix, so the two are carried separately rather than inferred from each
    // other.
    std::uint64_t frames_dropped_before = 0;

    // False means the samples above are zeros the engine wrote because the
    // gate was shut, not a quiet band and not a drop. The chunk still
    // crosses at the full rate and the timeline stays whole.
    bool squelch_open = false;

    [[nodiscard]] std::uint64_t frames() const {
        return channel_count == 0 ? 0 : samples.size() / channel_count;
    }
};

// One audio subscription's running totals, read back from the engine.
//
// Per subscription and not per server: a slow client's drops must never show
// up on a fast client's status line.
struct AudioStats {
    std::uint64_t frames_sent = 0;
    std::uint64_t frames_dropped = 0;

    // Times the queue went from not evicting to evicting. One two-second
    // stall and four hundred scattered hitches lose the same frames and
    // sound nothing alike.
    std::uint64_t drop_events = 0;

    std::uint64_t backlog_frames = 0;

    // The depth being enforced, in frames. Zero until the first chunk has
    // arrived: the server cannot turn the granted milliseconds into frames
    // before it knows the receiver's audio rate and a chunk's length, and it
    // learns both from the first chunk. See the schema's note on
    // subscribeAudio.
    std::uint64_t buffer_frames = 0;
};

// Which of the two mutually incompatible readings of the same bitstream the
// RDS decoder is configured for.
//
// Ordinal for ordinal with schema::RdsRegion and with
// revenant::decode::Region, on the terms the schema states.
//
// THE SERVER CONVERTS IT IN TWO DIRECTIONS AND THEY ARE NOT THE SAME CODE,
// both in core/rpc/convert.cpp, declared in core/rpc/convert.h, which is the
// file allowed to see the decoder. Outbound, decode::Region to
// schema::RdsRegion, is an exhaustive switch with no default, so a third
// region added to the decoder alone stops compiling rather than reaching the
// wire as an ordinal no reader has a name for. Inbound, schema::RdsRegion to
// decode::Region, is a range check that REFUSES an ordinal above kRbds and
// then a cast, because a Cap'n Proto enum field can legally carry a value
// the reader's schema has never heard of and a newer client is how it gets
// there. The static_asserts in core/rpc/convert.h are what make the inbound
// cast safe once the range holds; they catch a renumbering and they cannot
// catch either of the two cases above.
//
// WHAT THIS PARAGRAPH USED TO SAY. Until 2026-09-20 it read "the conversion
// on the server's side is a cast with a static_assert over it, in
// core/rpc/convert.h". Two things wrong with it: neither conversion is in
// that header, and describing the inbound one as a bare cast lost the
// refusal, which is the whole of what stops an unknown ordinal being decoded
// as whichever region happens to sit at it. core/rpc/revenant.capnp carried
// the mirror image of the same mistake, describing both directions as the
// switch, and is corrected too.
//
// A SETTING AND NEVER AN INFERENCE. core/decode/rds_groups.h has the argument
// and core/rpc/revenant.capnp repeats it: no field names the region, the PI
// code cannot decide it because the US call sign range collides with European
// country codes, and getting it wrong is silent because PTY 26 renders as
// National Music in one region and Hip-Hop in the other. A client may seed it
// from the tuned frequency as long as it shows it as something the operator
// can override.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it carried a section headed "WHY THERE IS NO RdsStation
// STRUCT HERE", which argued that a mirror of the schema struct would be a
// hundred lines of conversion no test could exercise and nothing could
// populate, and that Client::rds_station returning Status rather than a
// struct was where the compiler would point the branch that served it. It
// pointed exactly there. The struct is below, core/rpc/client.cpp populates
// it and tests/rpc/test_rpc_rds.cpp drives it through a real engine, so the
// paragraph is retracted rather than deleted: a reader arriving from the
// schema's own note is owed the reason it is no longer true.
enum class RdsRegion : std::uint8_t { Rds, Rbds };

// The physical layer's state. Ordinal for ordinal with schema::RdsLock and
// with revenant::decode::RdsLock, asserted in core/rpc/client.cpp against
// the schema and in core/rpc/convert.h against the decoder.
//
// NO BITS ARE EMITTED IN EITHER OF THE FIRST TWO. An acquiring decoder is
// measuring biphase consistency and has not reached its threshold; it is not
// one that is half working, so a display drawing partial text from one would
// be drawing text nothing produced.
enum class RdsLock : std::uint8_t { Unlocked, Acquiring, Locked };

// The BLOCK layer's state, which is a different thing from the bit layer's
// lock above: a decoder can be locked to the subcarrier and still hunting
// for the offset words, which is what the first second after a tune looks
// like.
enum class RdsSync : std::uint8_t { Hunting, PreSync, Synced };

// EN 50067 clause 3.1.5.6 clock time, as the transmitter stated it.
//
// THE ENGINE READS NO CLOCK AND NOTHING HERE IS VERIFIED AGAINST ONE. A
// client that wants to know whether the station's clock is right compares it
// against its own.
struct RdsClockTime {
    std::int32_t mjd = 0;

    // The Gregorian date the MJD converts to, UTC. Carried as well as the
    // MJD because the conversion is Annex G arithmetic and a client that
    // re-derived it would be writing that arithmetic a second time.
    std::int32_t year = 0;
    std::int32_t month = 0;
    std::int32_t day = 0;

    std::int32_t hour = 0;
    std::int32_t minute = 0;

    // Local time offset in signed half hours.
    std::int32_t offset_half_hours = 0;

    // Nothing above means anything unless this is set. A group 4A arrives
    // about once a minute, so this is false for the first minute of every
    // tune on a station that sends clock time at all, and forever on one
    // that does not.
    bool valid = false;
};

// What the physical and block layers are doing, which is what tells a stale
// display from a dead signal.
//
// EVERY COUNTER IS CUMULATIVE FROM THE MOMENT THE DECODER WAS BUILT and
// there is deliberately no rate here. A rate over that whole window is not
// the rate now: a station that faded five minutes ago and has been clean
// since reads badly forever. A client differences two polls and divides by
// the gap, which RdsStation::last_group_sample gives it.
struct RdsHealth {
    RdsLock lock = RdsLock::Unlocked;
    RdsSync sync = RdsSync::Hunting;

    // Biphase sign consistency mapped onto [0, 1]. Zero is indistinguishable
    // from noise and one is a clean eye. This is the quality figure to draw:
    // clause 1.7 makes every symbol an odd impulse pair, so the two halves
    // of a bit always have opposite signs whatever the payload is, which
    // makes it mean the same thing at every SNR.
    double quality = 0.0;
    double biphase_consistency = 0.5;

    // |E[z^2]| / E[|z|^2] on the derotated baseband, which equals
    // SNR/(1 + SNR) in the post-filter bandwidth.
    double carrier_coherence = 0.0;

    // Measurements and not tuning requests, which is why these two are
    // doubles where every frequency in this header is a rational: clause 1.1
    // allows the subcarrier plus or minus 6 Hz and rounding a measurement to
    // whole hertz throws away the evidence a drifting transmitter leaves.
    double carrier_offset_hz = 0.0;
    double bit_rate_hz = 0.0;

    // Clause 1.1 permits a mono transmission to carry RDS with no pilot at
    // all, so an absent pilot is not a fault.
    bool pilot_locked = false;
    double pilot_level = 0.0;

    std::uint64_t samples_consumed = 0;

    // THE BIT STREAM HAS GAPS AND NOTHING MARKS THEM: the decoder stops
    // emitting when lock falls away and starts again wherever it relocks, so
    // bits_emitted is not elapsed time. Sample reacquisitions beside it.
    std::uint64_t bits_emitted = 0;
    std::uint64_t reacquisitions = 0;

    std::uint64_t bits_fed = 0;
    std::uint64_t groups_decoded = 0;
    std::uint64_t blocks_good = 0;

    // Blocks where a burst was repaired rather than received clean, so they
    // are trusted less than blocks_good rather than equally.
    std::uint64_t blocks_corrected = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t sync_acquisitions = 0;
    std::uint64_t sync_losses = 0;
};

// An Open Data Application announcement from a type 3A group.
struct RdsOda {
    std::uint8_t group_type = 0;
    bool version_b = false;
    std::uint16_t message = 0;

    // 0x0000 means the group is used for its normal feature rather than for
    // an application.
    std::uint16_t aid = 0;
};

// A count of the groups put to one use and the most recent of them, raw. The
// standard defines nothing past the block 2 header for these payloads, so a
// client shows the count and at most the bits, never decoded fields.
struct RdsRawGroup {
    // Zero means none has arrived and every field below is meaningless.
    std::uint64_t groups = 0;

    std::uint8_t group_type = 0;
    bool version_b = false;
    std::uint8_t block2_low = 0;  // b4..b0 of block 2

    // Not payload in a version B group, which repeats the PI there.
    std::uint16_t block3 = 0;
    bool block3_valid = false;
    std::uint16_t block4 = 0;
    bool block4_valid = false;

    // A block this payload depends on, block 2 included, was corrected.
    bool corrected = false;
};

// One distinct 37-bit type 8A payload and how often it arrived.
struct RdsTmcMessage {
    std::uint8_t x = 0;   // block 2 b4..b0
    std::uint16_t y = 0;  // block 3
    std::uint16_t z = 0;  // block 4
    std::uint32_t receptions = 0;
    std::uint32_t corrected_receptions = 0;

    // ISO 14819-1 Introduction 0.3: a group's data is used once a second
    // identical group has verified it.
    [[nodiscard]] bool confirmed() const { return receptions >= 2; }
};

// RDS-TMC, attributed and counted, NOT DECODED. ALERT-C's field positions
// were not in hand, so there is no event or location code here and a client
// must not derive one by guessing which bits are which. See the schema.
struct RdsTmc {
    bool announced = false;
    std::uint16_t aid = 0;
    std::uint8_t group_type = 0;
    bool version_b = false;
    std::uint16_t oda_message = 0;

    std::uint16_t identification = 0;  // type 1A variant 1, raw
    bool identification_valid = false;

    std::uint64_t groups = 0;
    std::uint64_t oda_groups = 0;
    std::uint64_t incomplete = 0;
    std::uint64_t evicted = 0;

    // User messages, tuning information and encryption administration, not
    // told apart. Show the confirmed ones.
    std::vector<RdsTmcMessage> messages;
};

// One Enhanced Other Networks entry: what this station says about another.
struct RdsEonEntry {
    std::uint16_t pi = 0;

    // Annex E code points, not UTF-8. See the note on RdsStation::ps.
    std::vector<std::uint8_t> ps;
    std::uint8_t ps_received = 0;

    bool tp = false;
    bool ta = false;
    bool ta_valid = false;

    std::uint8_t pty = 0;
    bool pty_valid = false;

    std::uint16_t linkage = 0;
    bool linkage_valid = false;

    std::vector<std::int64_t> af;
};

// One station's accumulated RDS state, as a display needs it.
//
// EVERY FIELD HAS A VALIDITY COMPANION AND A READER MUST CHECK IT. There is
// no sentinel meaning "not received" for most of these: PTY 0 is a real
// programme type, TA false is a real state, and a PI of zero is what an
// uninitialised struct holds. The flags are the only honest answer, and a
// client that draws the value without them shows a station that has sent
// nothing as a station announcing no traffic.
struct RdsStation {
    // Echoed so a poll's answer names the receiver it came from, which
    // matters to a client polling several.
    std::uint64_t vrx = 0;

    // Read back rather than assumed: it is engine-side state and a second
    // client may have set it.
    RdsRegion region = RdsRegion::Rds;

    // The raw 16 bits, not split into country code, coverage area and
    // reference, because those three are a pure function of this.
    std::uint16_t pi = 0;
    bool pi_valid = false;

    // Derived from the PI under the region's own rules. Empty means NO CALL
    // SIGN IS DERIVABLE from this PI in this region, which is a different
    // thing from "PI not yet received": pi_valid answers that one. A string
    // and not bytes, unlike the four fields below, because the derivation
    // produces ASCII letters by construction.
    std::string call_sign;

    std::uint8_t pty = 0;
    bool pty_valid = false;

    // The PTY's display name under the configured region, at the widths
    // clause 3.2.1.1 specifies. Carried rather than left to the client
    // because half the table differs between RDS and RBDS: a client with one
    // hardcoded table shows the wrong genre on the other continent and
    // nothing faults.
    std::string pty_short_name;  // 8 characters
    std::string pty_long_name;   // 16 characters

    bool tp = false;
    bool tp_valid = false;
    bool ta = false;
    bool ta_valid = false;

    // Composite sample index at which ta last changed, or zero if it never
    // has. THIS IS WHY THIS SURFACE CAN STAY A POLL: everything else here
    // persists once received, and a traffic announcement is an event that
    // can begin and end between two polls. Differencing this against the
    // previous poll's value says one happened.
    std::uint64_t ta_changed_at = 0;

    bool music = false;
    bool music_valid = false;

    // Decoder Identification. di_received has bit n set once d(n) has
    // arrived, because the four flags come one per group over four groups
    // and a client showing "mono" before the bit arrived is showing this
    // struct's default.
    bool di_stereo = false;
    bool di_artificial_head = false;
    bool di_compressed = false;
    bool di_dynamic_pty = false;
    std::uint8_t di_received = 0;

    // Programme Service name, eight bytes, and RadioText, up to 64.
    //
    // BYTES AND NOT A STRING, WHICH IS A CORRECTNESS DECISION AND NOT A
    // STYLE ONE. These are EN 50067 Annex E code points, an 8-bit repertoire
    // that is not ASCII above 0x7F and is not UTF-8 anywhere. The decoder
    // stores what was received and transcodes nothing, and transcoding
    // belongs where a font is being chosen, which is the client.
    //
    // The masks are not optional extras. PS arrives as four two-character
    // segments and RadioText as sixteen, so a partially received one holds
    // real characters beside placeholders and the placeholders are not
    // distinguishable from transmitted spaces. ps_received has one bit per
    // segment 0..3, rt_received one bit per segment 0..15.
    std::vector<std::uint8_t> ps;
    std::uint8_t ps_received = 0;

    std::vector<std::uint8_t> rt;
    std::uint32_t rt_received = 0;

    // Position of the 0x0D terminator once one has arrived, or the highest
    // character index received plus one until then. Carried because the
    // terminator is inside the payload and a client scanning for it cannot
    // tell an unreceived byte from a transmitted one.
    //
    // rt[0, rt_length) IS THE SPAN TO RENDER AND THAT IS ALL IT IS. It says
    // nothing about the byte at rt_length, which is the 0x0D only once one
    // has arrived and is an unreceived NUL or one past the end of the field
    // until then. And it is NOT MONOTONIC: the decoder rescans the buffer on
    // every RadioText group, so it falls when a terminator lands in front of
    // the highest index received and rises again if the segment carrying
    // that terminator is overwritten with ordinary characters under the same
    // A/B flag. Redraw the span each poll rather than latching the longest
    // one seen.
    //
    // The schema said the opposite until 2026-09-20, as a stated invariant
    // of the wire, and core/rpc/revenant.capnp carries the retraction. A
    // client written against the old text latches at the first terminator
    // and renders the tail of a long message forever once the station sends
    // a shorter one without toggling the flag.
    std::uint32_t rt_length = 0;

    // A TOGGLE OF rt_ab IS THE ONLY SIGNAL THAT THE MESSAGE CHANGED, and a
    // client that does not watch it renders one message overwritten
    // character by character by the next.
    bool rt_ab = false;
    bool rt_ab_valid = false;
    bool rt_version_b = false;

    std::vector<std::uint8_t> ptyn;
    std::uint8_t ptyn_received = 0;
    bool ptyn_ab = false;
    bool ptyn_ab_valid = false;

    RdsClockTime clock;

    // Programme Item Number, clause 3.1.5.6.
    std::int32_t pin_day = 0;
    std::int32_t pin_hour = 0;
    std::int32_t pin_minute = 0;
    bool pin_valid = false;

    std::uint8_t ecc = 0;
    bool ecc_valid = false;

    // Raised when the ECC says ITU region 2 and the decoder is configured
    // for RDS. A DIAGNOSTIC FOR AN OPERATOR AND NEVER A SWITCH: the decoder
    // does not change region on its own and neither should a client, because
    // the converse is deliberately not raised.
    bool ecc_contradicts_region = false;

    std::uint8_t language = 0;
    bool language_valid = false;

    bool linkage_actuator = false;
    bool linkage_actuator_valid = false;

    // Alternative frequencies, VHF and LF/MF, deduplicated, in hertz.
    // Integers and not rationals: these are channel-plan frequencies the
    // standard states as whole numbers, so nothing exact is being rounded.
    std::vector<std::int64_t> af;

    // The transmitter's own statement of how many alternatives exist, from a
    // count code 225..249. Zero if none was seen. Comparing it against
    // af.size() is how a client knows the list is still filling.
    std::uint8_t af_announced = 0;

    // AN INDICATOR, NOT A DETERMINATION. A nonzero count is consistent with
    // both AF methods; only a count that stays at zero says anything, and
    // what it says is that the list has not wrapped yet.
    std::uint32_t af_repeats = 0;

    std::vector<RdsOda> oda;
    std::vector<RdsEonEntry> eon;

    RdsHealth health;

    // The composite rate this station is being decoded at, which is the
    // receiver's audio rate. Carried so the two sample indices below have a
    // stated unit.
    std::uint32_t composite_rate = 0;

    // Composite sample index at which the most recently completed group
    // finished, or zero if none has. This is the equivalent of
    // DetectionList::last_decision: it lets a client tell a repeated answer
    // from a fresh one without diffing the whole struct, and it is the
    // denominator for differencing the cumulative counters in health between
    // two polls.
    //
    // NOT a source sample index. The decoder counts in the samples it is fed
    // and has no access to the source timeline. Divide by composite_rate for
    // seconds.
    std::uint64_t last_group_sample = 0;

    // Empty while the decoder is running. Non-empty is why it stopped, and
    // every field above is frozen at the last chunk it accepted.
    //
    // CHECK IT BEFORE DRAWING ANYTHING ELSE. A decoder can stop without its
    // receiver going, and the two states are not otherwise distinguishable:
    // a station that stopped transmitting and a decoder that stopped reading
    // both look like a struct whose counters no longer move. TERMINAL for
    // the life of the receiver, because the only two faults are shape and
    // set_vrx_params refuses a change of shape. See the schema for what this
    // call did before the field existed.
    std::string fault;

    // The retune fence. True means the decoder was cleared for a retune and
    // the sample path has not yet delivered a chunk from the new tuning, so
    // it is being fed nothing on purpose and every field above is at its
    // default rather than at what the band holds.
    //
    // CHECK IT ALONGSIDE fault, FOR THE SAME REASON. A discarding decoder
    // and a receiver on a dead channel produce the identical struct: empty
    // fault, zero counters, nothing valid. Draw "retuning" rather than "no
    // RDS" while this is set. It clears on its own within about a block
    // period.
    bool discarding = false;

    // Chunks the fence threw away, cumulative for the life of the decoder.
    // Climbing while discarding stays true is a fence that is not clearing,
    // which is a server defect rather than a quiet band.
    std::uint64_t discarded_chunks = 0;

    // One bit per PTYN segment, set when a block the segment depends on was
    // corrected. A set bit means those four characters may not be the
    // station's.
    std::uint8_t ptyn_corrected = 0;

    RdsTmc tmc;

    // Emergency Warning System, type 9A. ews.groups above zero is the fact
    // to surface: this station has sent an emergency warning group, or a
    // test of one. The payload is each country's own format.
    RdsRawGroup ews;

    // Type 1A variant 7, raw.
    std::uint16_t ews_channel_identification = 0;
    bool ews_channel_identification_valid = false;

    [[nodiscard]] bool decoding() const { return fault.empty(); }
};

// ---------------------------------------------------------------------------
// Decoded messages
// ---------------------------------------------------------------------------
//
// ONE SHAPE FOR EVERY DECODER THAT PRODUCES EVENTS, which is the seam and the
// reason these exist. RDS has its own struct and keeps it, because a station
// is a STATE that accumulates and is polled. A P25 frame, a D-STAR header, a
// TETRA synchronisation burst, and the RTTY, APRS, POCSAG, PSK31, CW and M17
// output that follows them are EVENTS, and an event is a name, a place in the
// stream and a handful of typed values. Growing the schema by one struct per
// mode would mean a schema change, a conversion and a client change for every
// decoder, and a client that could not show a mode it was built before.
// core/rpc/decoders.h is the engine side of the same seam.

// A field's value. One of five kinds and never a string that has to be
// parsed back into a number: a talkgroup is an integer on the wire, so a
// client sorting or filtering by it does not guess at a format.
//
// BYTES ARE NOT TEXT. A field holding what a transmitter sent, a D-STAR
// callsign or a P25 message indicator, is text only where the standard says
// it is ASCII; anything else crosses as bytes and a client decides how to
// show it, on the argument RdsStation::ps makes.
using DecodedValue =
    std::variant<std::int64_t, double, bool, std::string, std::vector<std::uint8_t>>;

struct DecodedField {
    // Lower snake case, stable per decoder, and documented beside the decoder
    // that emits it in core/rpc/decoders.h. A client keys a column on this.
    std::string key;
    DecodedValue value;

    [[nodiscard]] const std::int64_t* integer() const {
        return std::get_if<std::int64_t>(&value);
    }
    [[nodiscard]] const double* real() const { return std::get_if<double>(&value); }
    [[nodiscard]] const bool* flag() const { return std::get_if<bool>(&value); }
    [[nodiscard]] const std::string* text() const { return std::get_if<std::string>(&value); }
    [[nodiscard]] const std::vector<std::uint8_t>* bytes() const {
        return std::get_if<std::vector<std::uint8_t>>(&value);
    }
};

// One thing a decoder recovered from one receiver's stream.
struct DecodedMessage {
    std::uint64_t vrx = 0;

    // The registry name of the decoder that produced it, "p25p1" and so on,
    // and the kind of message within that decoder: "hdu", "ldu1", "header",
    // "sync". Both lower case, both stable.
    std::string decoder;
    std::string kind;

    // WHERE IN THE RECEIVER'S OWN STREAM, as [start_sample, end_sample) at
    // sample_rate, in the same frame AudioChunk::sample_index counts in.
    //
    // THE SPAN OF THE DELIVERY THAT COMPLETED THE MESSAGE, which bounds when it
    // ENDED to within one chunk and does not say where it began. A P25 header
    // is 82.5 ms on the air and a chunk is about 7 ms at the test fixture's
    // block size, so the span is the last few symbols of it and not the whole.
    // Sample indices rather than wall clock, per docs/conventions.md: a
    // message from a replayed capture carries the index it had live.
    std::uint64_t start_sample = 0;
    std::uint64_t end_sample = 0;
    std::uint32_t sample_rate = 0;

    std::vector<DecodedField> fields;

    // One line a person can read, or empty. Never parsed: the fields are the
    // machine-readable half and say everything this does.
    std::string text;

    // Messages this decoder produced before this one, so a gap between two
    // consecutive sequences is messages this subscription did not see.
    std::uint64_t sequence = 0;

    // Messages this subscription lost from its queue between the previous one
    // it was sent and this one, because it could not keep up. A sequence gap
    // larger than this was lost somewhere else, which no path does today.
    std::uint64_t dropped_before = 0;

    // The first field named `key`, or null. Linear, because a message carries
    // about ten.
    [[nodiscard]] const DecodedField* field(std::string_view key) const {
        for (const DecodedField& entry : fields) {
            if (entry.key == key) {
                return &entry;
            }
        }
        return nullptr;
    }
};

// What a decoder reads. A complex decoder is fed a receiver's raw tap, two
// floats per sample, and an audio decoder a receiver's real audio.
enum class DecoderInput : std::uint8_t { ComplexBaseband, RealAudio };

struct DecoderInfo {
    std::string name;
    DecoderInput input = DecoderInput::ComplexBaseband;

    // One sentence naming the standard it implements and what it recovers.
    std::string description;

    // The receiver modes it reads, as demod names: "usb", "lsb", "nfm", "raw".
    // The whole list, never empty for "any". Empty only from an engine older
    // than the field, where it means the engine did not say.
    std::vector<std::string> modes;
};

// One decoded-message subscription's running totals.
struct DecodedStats {
    std::uint64_t messages_sent = 0;
    std::uint64_t messages_dropped = 0;
    std::uint64_t backlog = 0;

    // Chunks the decoder behind this subscription threw away because a retune
    // was still in flight. Shared by every subscriber to the same decoder.
    std::uint64_t chunks_discarded = 0;

    // The rate the decoder was built for, zero until the first chunk.
    std::uint32_t sample_rate = 0;
};

}  // namespace revenant::rpc
