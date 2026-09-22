// The client half of the wire, implementing core/rpc/client.h.
//
// WHAT MAY BE INCLUDED HERE, WHICH IS A SHORT LIST ON PURPOSE
//
// capnp, kj, the generated schema, core/rpc/types.h and core/error.h. Nothing
// from core/engine, core/dsp or core/source, not even for a struct
// definition. This translation unit is compiled a second time inside the Qt
// project, against the dynamic CRT, where revenant_core is not linked at all.
// An engine include here builds fine in this tree and fails in that one, so
// the restriction has to be kept by hand. core/rpc/CMakeLists.txt records
// why the split exists.
//
// It is also why the readers below share no code with core/rpc/convert.cpp.
// That file converts between the schema and the engine; this one converts
// between the schema and core/rpc/types.h. The two cannot meet, and the
// duplication is the process boundary being paid for once rather than a
// missed refactor.
//
// THE THREAD MODEL
//
// One kj event loop thread per Client, started by ClientImpl::start and
// joined by the destructor. Every kj and capnp object this file creates lives
// in LoopState on that thread's stack and is destroyed there as the loop
// unwinds. None of them may be a member of ClientImpl: a member is destroyed
// on whichever thread deletes the Client, and a capability or a
// PromiseFulfiller touched from the wrong thread corrupts the promise system
// rather than racing on a field. server.h states the same rule for the other
// end of the connection.
//
// Caller threads reach the loop only through kj::Executor::executeSync, which
// runs a functor there and blocks until the promise it returns resolves. One
// mutex around that makes concurrent callers queue instead of interleaving,
// which is what client.h promises them.
//
// The spectrum callback runs on the loop thread inside the frame() call, and
// that call is not answered until the callback returns. That is what makes
// the engine's one-frame-in-flight rule press on a slow subscriber instead of
// filling a queue on its behalf.
//
// NOTHING THROWS OUT OF HERE
//
// kj is exception-based and this API is not. Every entry point that can reach
// kj catches kj::Exception and std::exception and turns them into an Error.
// The event loop thread swallows the same at its top level, because an
// exception escaping a std::thread is a terminate().

#include "core/rpc/client.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <capnp/rpc-twoparty.h>
#include <kj/async-io.h>
#include <kj/async.h>
#include <kj/common.h>
#include <kj/exception.h>
#include <kj/memory.h>

#include "core/error.h"
#include "core/rpc/revenant.capnp.h"
#include "core/rpc/types.h"

namespace revenant::rpc {
namespace {

// The cast in read_demod and write_demod, made a compile error to break.
// core/rpc/convert.h holds the same check between the schema and the engine;
// this is the other pair, and this file is the only one that sees both.
static_assert(static_cast<std::uint16_t>(schema::Demod::RAW) ==
              static_cast<std::uint16_t>(Demod::Raw));
static_assert(static_cast<std::uint16_t>(schema::Demod::AM) ==
              static_cast<std::uint16_t>(Demod::Am));
static_assert(static_cast<std::uint16_t>(schema::Demod::NFM) ==
              static_cast<std::uint16_t>(Demod::Nfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::WFM) ==
              static_cast<std::uint16_t>(Demod::Wfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::USB) ==
              static_cast<std::uint16_t>(Demod::Usb));
static_assert(static_cast<std::uint16_t>(schema::Demod::LSB) ==
              static_cast<std::uint16_t>(Demod::Lsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::DSB) ==
              static_cast<std::uint16_t>(Demod::Dsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::CW) ==
              static_cast<std::uint16_t>(Demod::Cw));
static_assert(static_cast<std::uint16_t>(schema::Demod::P25P1) ==
              static_cast<std::uint16_t>(Demod::P25p1));
static_assert(static_cast<std::uint16_t>(schema::Demod::DSTAR) ==
              static_cast<std::uint16_t>(Demod::Dstar));
static_assert(static_cast<std::uint16_t>(schema::Demod::TETRA) ==
              static_cast<std::uint16_t>(Demod::Tetra));

// The same pair for the RDS region, used by the cast in set_rds_region.
//
// core/decode/rds_groups.h holds the third member of this chain, Region, and
// nothing asserts against it here: core/rpc/types.h may not include anything
// under core/decode any more than under core/engine. core/rpc/convert.h
// carries that assert, which is the file allowed to see both.
static_assert(static_cast<std::uint16_t>(schema::RdsRegion::RDS) ==
              static_cast<std::uint16_t>(RdsRegion::Rds));
static_assert(static_cast<std::uint16_t>(schema::RdsRegion::RBDS) ==
              static_cast<std::uint16_t>(RdsRegion::Rbds));

// And for the two decoder states. Read and never written back, so a
// renumbering would not retune anything, and it would still be silent: an
// acquiring decoder drawn as locked is a display showing text that nothing
// produced.
static_assert(static_cast<std::uint16_t>(schema::RdsLock::UNLOCKED) ==
              static_cast<std::uint16_t>(RdsLock::Unlocked));
static_assert(static_cast<std::uint16_t>(schema::RdsLock::ACQUIRING) ==
              static_cast<std::uint16_t>(RdsLock::Acquiring));
static_assert(static_cast<std::uint16_t>(schema::RdsLock::LOCKED) ==
              static_cast<std::uint16_t>(RdsLock::Locked));
static_assert(static_cast<std::uint16_t>(schema::RdsSync::HUNTING) ==
              static_cast<std::uint16_t>(RdsSync::Hunting));
static_assert(static_cast<std::uint16_t>(schema::RdsSync::PRE_SYNC) ==
              static_cast<std::uint16_t>(RdsSync::PreSync));
static_assert(static_cast<std::uint16_t>(schema::RdsSync::SYNCED) ==
              static_cast<std::uint16_t>(RdsSync::Synced));

// The same pair for the detector's track state, used by read_track_state.
static_assert(static_cast<std::uint16_t>(schema::TrackState::PENDING) ==
              static_cast<std::uint16_t>(TrackState::Pending));
static_assert(static_cast<std::uint16_t>(schema::TrackState::LIVE) ==
              static_cast<std::uint16_t>(TrackState::Live));
static_assert(static_cast<std::uint16_t>(schema::TrackState::HELD) ==
              static_cast<std::uint16_t>(TrackState::Held));
static_assert(static_cast<std::uint16_t>(schema::TrackState::MERGED) ==
              static_cast<std::uint16_t>(TrackState::Merged));

// And for the front end's verdict, used by read_source_stats.
static_assert(static_cast<std::uint16_t>(schema::FrontEndState::UNMEASURED) ==
              static_cast<std::uint16_t>(FrontEndState::Unmeasured));
static_assert(static_cast<std::uint16_t>(schema::FrontEndState::STEADY) ==
              static_cast<std::uint16_t>(FrontEndState::Steady));
static_assert(static_cast<std::uint16_t>(schema::FrontEndState::SPAN_SCALES) ==
              static_cast<std::uint16_t>(FrontEndState::SpanScales));
static_assert(static_cast<std::uint16_t>(schema::FrontEndState::FLOOR_FOLLOWS_SIGNAL) ==
              static_cast<std::uint16_t>(FrontEndState::FloorFollowsSignal));

// And for the two on SourceDescriptor, used by read_source_descriptor.
//
// PINNED RATHER THAN CAST IN THE READER, so a value added to one enumeration
// and not the other fails here instead of arriving as a number the far side
// reads as something else. convert.cpp writes both by name through a switch for
// the same reason and the reader casts, which these make safe.
static_assert(static_cast<std::uint16_t>(schema::SampleFormat::CU8) ==
              static_cast<std::uint16_t>(SampleFormat::Cu8));
static_assert(static_cast<std::uint16_t>(schema::SampleFormat::CS8) ==
              static_cast<std::uint16_t>(SampleFormat::Cs8));
static_assert(static_cast<std::uint16_t>(schema::SampleFormat::CS16) ==
              static_cast<std::uint16_t>(SampleFormat::Cs16));
static_assert(static_cast<std::uint16_t>(schema::SampleFormat::CF32) ==
              static_cast<std::uint16_t>(SampleFormat::Cf32));

static_assert(static_cast<std::uint16_t>(schema::FlowControl::PACED) ==
              static_cast<std::uint16_t>(FlowControl::Paced));
static_assert(static_cast<std::uint16_t>(schema::FlowControl::DEMAND) ==
              static_cast<std::uint16_t>(FlowControl::Demand));

// Sets a field for the length of a scope and puts it back on the way out,
// including out of an exception. Both uses are loop-thread-only fields whose
// stale value would be read by code running after the scope: a dangling
// LoopState pointer, and a re-entrancy flag.
template <typename T>
class ScopedSet {
public:
    ScopedSet(T& slot, T active, T restored) : slot_(slot), restored_(restored) {
        slot_ = active;
    }
    ~ScopedSet() { slot_ = restored_; }

    ScopedSet(const ScopedSet&) = delete;
    ScopedSet& operator=(const ScopedSet&) = delete;
    ScopedSet(ScopedSet&&) = delete;
    ScopedSet& operator=(ScopedSet&&) = delete;

private:
    T& slot_;
    T restored_;
};

template <typename T>
struct PromiseValue;

template <typename T>
struct PromiseValue<kj::Promise<T>> {
    using type = T;
};

// A kj exception, said in a sentence a person can act on.
//
// The Error carries no code. core/error.h reserves that field for the
// originating API's own number, and kj has a four-value Type rather than an
// errno, so putting the ordinal there would read in a log as though a driver
// had returned it.
//
// IT CARRIES A CATEGORY, AND THE TYPE IS THE WHOLE OF WHERE ONE COMES FROM.
// Three of the four kj types are a verdict a caller can act on and the fourth,
// FAILED, is rpc.capnp's own "this would fail again unchanged", which is
// Unclassified rather than a fifth meaning. core/rpc/server.cpp's
// to_exception_type is the other half of this mapping and error.h says what the
// round trip loses. A caller that knows more than the type does, because of
// WHERE it caught this, narrows the category afterwards; ClientImpl::run is the
// one place that does.
[[nodiscard]] Error translate(std::string_view what, const kj::Exception& failure) {
    const kj::StringPtr description = failure.getDescription();
    std::string detail(description.begin(), description.end());
    if (detail.empty()) {
        detail = "the Cap'n Proto layer gave no detail";
    }

    switch (failure.getType()) {
        case kj::Exception::Type::DISCONNECTED:
            return Error{std::format("{}: the connection to the engine is gone: {}", what, detail),
                         ErrorCategory::Disconnected};
        case kj::Exception::Type::OVERLOADED:
            return Error{std::format("{}: the engine is out of resources: {}", what, detail),
                         ErrorCategory::Overloaded};
        case kj::Exception::Type::UNIMPLEMENTED:
            return Error{
                std::format("{}: the engine does not implement this call, so it is older than "
                            "this client: {}",
                            what, detail),
                ErrorCategory::Unimplemented};
        case kj::Exception::Type::FAILED:
        default:
            return Error{std::format("{}: {}", what, detail)};
    }
}

[[nodiscard]] std::string read_text(capnp::Text::Reader in) {
    return std::string(in.begin(), in.end());
}

// Copied field for field, including a zero denominator. types.h::hertz()
// already decides what a zero denominator means, and a second policy here
// would make the same wire value mean two different things depending on which
// layer looked at it.
[[nodiscard]] Rational read_rational(schema::Rational::Reader in) {
    Rational out;
    out.numerator = in.getNumerator();
    out.denominator = in.getDenominator();
    return out;
}

// An ordinal with no enumerator is rejected rather than cast, for the reason
// core/rpc/convert.h gives in the other direction: a Cap'n Proto enum field
// may legally hold a value the reader's schema has never heard of, which is
// how a client reaches an engine built against a newer one. Mode is the
// parameter where being wrong is inaudible until a recording turns out to be
// unintelligible.
[[nodiscard]] Expected<Demod> read_demod(schema::Demod mode) {
    const auto ordinal = static_cast<std::uint16_t>(mode);
    if (ordinal > static_cast<std::uint16_t>(Demod::Tetra)) {
        return fail(std::format(
            "demodulator ordinal {} is not one this client knows; the engine was built against "
            "a newer schema",
            ordinal));
    }
    return static_cast<Demod>(ordinal);
}

[[nodiscard]] schema::Demod write_demod(Demod mode) {
    return static_cast<schema::Demod>(static_cast<std::uint16_t>(mode));
}

[[nodiscard]] DeviceInfo read_device(schema::DeviceInfo::Reader in) {
    DeviceInfo out;
    out.index = in.getIndex();
    out.name = read_text(in.getName());
    out.vendor = read_text(in.getVendor());
    out.discrete = in.getDiscrete();
    out.api_version = in.getApiVersion();
    out.driver_version = in.getDriverVersion();
    return out;
}

[[nodiscard]] GridParams read_grid(schema::GridParams::Reader in) {
    GridParams out;
    out.channels = in.getChannels();
    out.taps_per_branch = in.getTapsPerBranch();
    out.decimation = in.getDecimation();
    return out;
}

[[nodiscard]] SpectrumGeometry read_geometry(schema::SpectrumGeometry::Reader in) {
    SpectrumGeometry out;
    out.transform = in.getTransform();
    out.bins_per_channel = in.getBinsPerChannel();
    out.channels = in.getChannels();
    out.bins = in.getBins();
    out.bin_width = read_rational(in.getBinWidth());
    out.bin_zero = read_rational(in.getBinZero());
    return out;
}

[[nodiscard]] EngineInfo read_engine_info(schema::EngineInfo::Reader in) {
    EngineInfo out;
    out.device = read_device(in.getDevice());
    out.grid = read_grid(in.getGrid());
    out.source_rate = in.getSourceRate();
    out.channel_rate = in.getChannelRate();
    out.channel_spacing = in.getChannelSpacing();
    out.spectrum = read_geometry(in.getSpectrum());
    out.source_center = in.getSourceCenter();
    out.ring_samples = in.getRingSamples();
    out.ring_seconds = in.getRingSeconds();

    // Not ring trivia, whatever the field names say. core/engine/engine.cpp
    // overloads the ring's clamp reason as the one field in EngineInfo that
    // can carry a sentence, so a clamped channel count or block size rides
    // out in it too. Dropping it here would hand a UI an engine that built
    // something other than what was asked for and no way to find out.
    out.ring_clamped = in.getRingClamped();
    out.ring_clamp_reason = read_text(in.getRingClampReason());

    // Read together and shown together. A factor of 0.5 is a source that
    // cannot keep up when the pace is zero and is exactly what was asked
    // for when the pace is 0.5, so a client that draws one without the
    // other raises an alarm on every deliberate half-speed replay.
    out.realtime_factor = in.getRealtimeFactor();
    out.source_paced_by = in.getSourcePacedBy();

    // The only other field here that moves while a connection stays up, and
    // the one a client has to read on every poll rather than at connect. See
    // EngineInfo::source_epoch: it says which stream the sample indices in
    // every frame and chunk belong to, and two streams' zeros are different
    // instants.
    out.source_epoch = in.getSourceEpoch();
    return out;
}

[[nodiscard]] SourceDescriptor read_source_descriptor(schema::SourceDescriptor::Reader in) {
    SourceDescriptor out;
    out.uri = read_text(in.getUri());
    out.backend = read_text(in.getBackend());
    out.display_name = read_text(in.getDisplayName());
    out.unavailable = read_text(in.getUnavailable());

    for (const capnp::Text::Reader note : in.getNotes()) {
        out.notes.push_back(read_text(note));
    }

    out.tune_ranges.reserve(in.getTuneRanges().size());
    for (const schema::TuneRange::Reader range : in.getTuneRanges()) {
        TuneRange one;
        one.low_hz = range.getLowHz();
        one.high_hz = range.getHighHz();
        one.step_hz = range.getStepHz();
        out.tune_ranges.push_back(one);
    }

    out.sample_rates.reserve(in.getSampleRates().size());
    for (const std::uint32_t rate : in.getSampleRates()) {
        out.sample_rates.push_back(rate);
    }
    out.min_rate = in.getMinRate();
    out.max_rate = in.getMaxRate();

    // Clamped to Unknown rather than onto one of the four, and to Demand rather
    // than refused, on the precedent read_source_stats sets for FrontEndState: a
    // struct read has nowhere to put a refusal, and a descriptor read
    // specifically must not fail, because one dongle reporting an ordinal this
    // build does not know would otherwise hide the synthetic and file backends
    // that cannot fail and are what somebody reaches for when the radio is busy.
    //
    // The two clamps land in different places for different reasons.
    // SampleFormat has an Unknown of its own on this side, because the label is
    // printed and naming the wrong format is a lie a reader acts on. FlowControl
    // has only two values and lands on Demand, which is the conservative one:
    // sourcePacedBy is meaningful on a Demand source and meaningless on a Paced
    // one, so a client that guesses Demand shows a number that may mean nothing
    // and one that guessed Paced would hide a number that means everything.
    const auto format = static_cast<std::uint16_t>(in.getNativeFormat());
    out.native_format = format <= static_cast<std::uint16_t>(SampleFormat::Cf32)
                            ? static_cast<SampleFormat>(format)
                            : SampleFormat::Unknown;
    out.bits_per_component = in.getBitsPerComponent();

    out.gain_stages.reserve(in.getGainStages().size());
    for (const schema::GainStage::Reader stage : in.getGainStages()) {
        GainStage one;
        one.name = read_text(stage.getName());
        one.min_db = stage.getMinDb();
        one.max_db = stage.getMaxDb();
        one.has_auto = stage.getHasAuto();
        one.steps_db.reserve(stage.getStepsDb().size());
        for (const double step : stage.getStepsDb()) {
            one.steps_db.push_back(step);
        }
        out.gain_stages.push_back(std::move(one));
    }

    const auto flow = static_cast<std::uint16_t>(in.getFlow());
    out.flow = flow <= static_cast<std::uint16_t>(FlowControl::Demand)
                   ? static_cast<FlowControl>(flow)
                   : FlowControl::Demand;

    out.seekable = in.getSeekable();
    out.length_samples = in.getLengthSamples();
    return out;
}

[[nodiscard]] SourceStats read_source_stats(schema::SourceStats::Reader in) {
    SourceStats out;
    out.blocks_delivered = in.getBlocksDelivered();
    out.samples_delivered = in.getSamplesDelivered();
    out.overrun_events = in.getOverrunEvents();
    out.samples_lost = in.getSamplesLost();
    out.last_loss_index = in.getLastLossIndex();
    out.write_index = in.getWriteIndex();

    // An ordinal from a newer engine becomes Unmeasured rather than an
    // error. This whole call is a counter poll that a client makes several
    // times a second, and failing it over one field would take the overrun
    // count with it; Unmeasured already means "this cannot be said", which
    // is exactly true of a state the client has never heard of.
    const auto front_end = static_cast<std::uint16_t>(in.getFrontEnd());
    out.front_end = front_end <= static_cast<std::uint16_t>(FrontEndState::FloorFollowsSignal)
                        ? static_cast<FrontEndState>(front_end)
                        : FrontEndState::Unmeasured;
    out.front_end_slope = in.getFrontEndSlope();
    out.front_end_floor_lift_db = in.getFrontEndFloorLiftDb();

    // The graph's two, which the source has never heard of. See
    // engine::GraphConditions for why they ride on this message and not on a
    // call of their own.
    out.vrx_retune_refusals = in.getVrxRetuneRefusals();
    out.frame_stalls = in.getFrameStalls();
    return out;
}

[[nodiscard]] Expected<VrxParams> read_vrx_params(schema::VrxParams::Reader in) {
    auto mode = read_demod(in.getDemod());
    if (!mode) {
        return std::unexpected(mode.error());
    }

    VrxParams out;
    out.center = in.getCenter();
    out.bandwidth = in.getBandwidth();
    out.demod = *mode;
    out.audio_rate = in.getAudioRate();
    out.squelch_dbfs = in.getSquelchDbfs();
    out.agc_attack_ms = in.getAgcAttackMs();
    out.agc_decay_ms = in.getAgcDecayMs();
    out.agc_enabled = in.getAgcEnabled();
    out.cw_pitch = in.getCwPitch();
    out.passband_low = in.getPassbandLow();
    out.passband_high = in.getPassbandHigh();
    return out;
}

void write_vrx_params(schema::VrxParams::Builder out, const VrxParams& in) {
    out.setCenter(in.center);
    out.setBandwidth(in.bandwidth);
    out.setDemod(write_demod(in.demod));
    out.setAudioRate(in.audio_rate);
    out.setSquelchDbfs(in.squelch_dbfs);
    out.setAgcAttackMs(in.agc_attack_ms);
    out.setAgcDecayMs(in.agc_decay_ms);
    out.setAgcEnabled(in.agc_enabled);
    out.setCwPitch(in.cw_pitch);
    out.setPassbandLow(in.passband_low);
    out.setPassbandHigh(in.passband_high);
}

[[nodiscard]] VrxPlacement read_vrx_placement(schema::VrxPlacement::Reader in) {
    VrxPlacement out;
    out.channel = in.getChannel();
    out.channel_centre = read_rational(in.getChannelCentre());
    out.residual = read_rational(in.getResidual());
    out.channel_rate = in.getChannelRate();
    out.bandwidth_clamped = in.getBandwidthClamped();
    out.granted_low = in.getGrantedLow();
    out.granted_high = in.getGrantedHigh();

    // Empty unless something was clamped. The numbers above say that
    // something was; this says what it means and what to do about it, which
    // is the part no client worked out for itself.
    out.clamp_reason = read_text(in.getClampReason());
    return out;
}

[[nodiscard]] Expected<VrxStatus> read_vrx_status(schema::VrxStatus::Reader in) {
    auto params = read_vrx_params(in.getParams());
    if (!params) {
        return std::unexpected(params.error());
    }

    VrxStatus out;
    out.id = in.getId();
    out.params = std::move(*params);
    out.placement = read_vrx_placement(in.getPlacement());
    out.demod_rate = in.getDemodRate();
    out.level_dbfs = in.getLevelDbfs();
    out.squelch_open = in.getSquelchOpen();
    out.audio_samples = in.getAudioSamples();
    out.audio_dropped = in.getAudioDropped();
    return out;
}

// An ordinal with no enumerator is rejected rather than cast, the same as
// read_demod above. The consequence is quieter here and still wrong: an
// unknown state would land on whichever of live, held and merged happens to
// sit at that ordinal, and a display that drew a held track as live would
// stop decaying anything without ever looking broken.
[[nodiscard]] Expected<TrackState> read_track_state(schema::TrackState state) {
    const auto ordinal = static_cast<std::uint16_t>(state);
    if (ordinal > static_cast<std::uint16_t>(TrackState::Merged)) {
        return fail(std::format(
            "track state ordinal {} is not one this client knows; the engine was built against "
            "a newer schema",
            ordinal));
    }
    return static_cast<TrackState>(ordinal);
}

[[nodiscard]] Expected<Detection> read_detection(schema::Detection::Reader in) {
    auto state = read_track_state(in.getState());
    if (!state) {
        return std::unexpected(state.error());
    }

    Detection out;
    out.id = in.getId();
    out.center_hz = in.getCenterHz();
    out.bandwidth_hz = in.getBandwidthHz();
    out.snr_2500_db = in.getSnr2500Db();
    out.confidence = in.getConfidence();
    out.state = *state;
    out.first_seen = in.getFirstSeen();
    out.last_seen = in.getLastSeen();
    out.last_detected = in.getLastDetected();
    out.channel = in.getChannel();
    out.channel_valid = in.getChannelValid();
    out.merged_into = in.getMergedInto();
    return out;
}

[[nodiscard]] Expected<DetectionList> read_detection_list(schema::DetectionList::Reader in) {
    auto rows = in.getDetections();

    DetectionList out;
    out.detections.reserve(rows.size());
    for (auto row : rows) {
        auto detection = read_detection(row);
        if (!detection) {
            return std::unexpected(detection.error());
        }
        out.detections.push_back(std::move(*detection));
    }
    out.decisions = in.getDecisions();
    out.last_decision = in.getLastDecision();
    out.total = in.getTotal();
    out.detection_threshold_db = in.getDetectionThresholdDb();
    out.detector_hold_seconds = in.getDetectorHoldSeconds();
    return out;
}

// ---------------------------------------------------------------------------
// RDS
// ---------------------------------------------------------------------------

// The four text fields of an RdsStation are Data and are copied byte for
// byte, because they are EN 50067 Annex E code points rather than UTF-8. See
// the note on rpc::RdsStation::ps: transcoding belongs where a font is being
// chosen.
[[nodiscard]] std::vector<std::uint8_t> read_bytes(capnp::Data::Reader in) {
    return std::vector<std::uint8_t>(in.begin(), in.end());
}

[[nodiscard]] std::vector<std::int64_t> read_frequencies(
    capnp::List<std::int64_t>::Reader in) {
    std::vector<std::int64_t> out;
    out.reserve(in.size());
    for (const std::int64_t hz : in) {
        out.push_back(hz);
    }
    return out;
}

// Rejected rather than cast, on the same terms read_track_state is. A Cap'n
// Proto enum field can legally hold an ordinal this build has never heard
// of, which is how a newer engine reaches an older client, and a blind cast
// turns "a state added later" into whichever of these three sits at that
// number.
[[nodiscard]] Expected<RdsLock> read_rds_lock(schema::RdsLock in) {
    const auto ordinal = static_cast<std::uint16_t>(in);
    if (ordinal > static_cast<std::uint16_t>(RdsLock::Locked)) {
        return fail(std::format(
            "RDS lock ordinal {} is not one this client knows; the engine was built against a "
            "newer schema",
            ordinal));
    }
    return static_cast<RdsLock>(ordinal);
}

[[nodiscard]] Expected<RdsSync> read_rds_sync(schema::RdsSync in) {
    const auto ordinal = static_cast<std::uint16_t>(in);
    if (ordinal > static_cast<std::uint16_t>(RdsSync::Synced)) {
        return fail(std::format(
            "RDS sync ordinal {} is not one this client knows; the engine was built against a "
            "newer schema",
            ordinal));
    }
    return static_cast<RdsSync>(ordinal);
}

[[nodiscard]] Expected<RdsRegion> read_rds_region(schema::RdsRegion in) {
    const auto ordinal = static_cast<std::uint16_t>(in);
    if (ordinal > static_cast<std::uint16_t>(RdsRegion::Rbds)) {
        return fail(std::format(
            "RDS region ordinal {} is not one this client knows; the engine was built against "
            "a newer schema",
            ordinal));
    }
    return static_cast<RdsRegion>(ordinal);
}

[[nodiscard]] Expected<RdsHealth> read_rds_health(schema::RdsHealth::Reader in) {
    auto lock = read_rds_lock(in.getLock());
    if (!lock) {
        return std::unexpected(lock.error());
    }
    auto sync = read_rds_sync(in.getSync());
    if (!sync) {
        return std::unexpected(sync.error());
    }

    RdsHealth out;
    out.lock = *lock;
    out.sync = *sync;
    out.quality = in.getQuality();
    out.biphase_consistency = in.getBiphaseConsistency();
    out.carrier_coherence = in.getCarrierCoherence();
    out.carrier_offset_hz = in.getCarrierOffsetHz();
    out.bit_rate_hz = in.getBitRateHz();
    out.pilot_locked = in.getPilotLocked();
    out.pilot_level = in.getPilotLevel();
    out.samples_consumed = in.getSamplesConsumed();
    out.bits_emitted = in.getBitsEmitted();
    out.reacquisitions = in.getReacquisitions();
    out.bits_fed = in.getBitsFed();
    out.groups_decoded = in.getGroupsDecoded();
    out.blocks_good = in.getBlocksGood();
    out.blocks_corrected = in.getBlocksCorrected();
    out.blocks_dropped = in.getBlocksDropped();
    out.sync_acquisitions = in.getSyncAcquisitions();
    out.sync_losses = in.getSyncLosses();
    return out;
}

[[nodiscard]] RdsClockTime read_rds_clock(schema::RdsClockTime::Reader in) {
    RdsClockTime out;
    out.mjd = in.getMjd();
    out.year = in.getYear();
    out.month = in.getMonth();
    out.day = in.getDay();
    out.hour = in.getHour();
    out.minute = in.getMinute();
    out.offset_half_hours = in.getOffsetHalfHours();
    out.valid = in.getValid();
    return out;
}

[[nodiscard]] Expected<RdsStation> read_rds_station(schema::RdsStation::Reader in) {
    auto region = read_rds_region(in.getRegion());
    if (!region) {
        return std::unexpected(region.error());
    }
    auto health = read_rds_health(in.getHealth());
    if (!health) {
        return std::unexpected(health.error());
    }

    RdsStation out;
    out.vrx = in.getVrx();
    out.region = *region;

    out.pi = in.getPi();
    out.pi_valid = in.getPiValid();
    out.call_sign = in.getCallSign();

    out.pty = in.getPty();
    out.pty_valid = in.getPtyValid();
    out.pty_short_name = in.getPtyShortName();
    out.pty_long_name = in.getPtyLongName();

    out.tp = in.getTp();
    out.tp_valid = in.getTpValid();
    out.ta = in.getTa();
    out.ta_valid = in.getTaValid();
    out.ta_changed_at = in.getTaChangedAt();

    out.music = in.getMusic();
    out.music_valid = in.getMusicValid();

    out.di_stereo = in.getDiStereo();
    out.di_artificial_head = in.getDiArtificialHead();
    out.di_compressed = in.getDiCompressed();
    out.di_dynamic_pty = in.getDiDynamicPty();
    out.di_received = in.getDiReceived();

    out.ps = read_bytes(in.getPs());
    out.ps_received = in.getPsReceived();
    out.rt = read_bytes(in.getRt());
    out.rt_received = in.getRtReceived();
    out.rt_length = in.getRtLength();
    out.rt_ab = in.getRtAb();
    out.rt_ab_valid = in.getRtAbValid();
    out.rt_version_b = in.getRtVersionB();

    out.ptyn = read_bytes(in.getPtyn());
    out.ptyn_received = in.getPtynReceived();
    out.ptyn_ab = in.getPtynAb();
    out.ptyn_ab_valid = in.getPtynAbValid();

    out.clock = read_rds_clock(in.getClock());

    out.pin_day = in.getPinDay();
    out.pin_hour = in.getPinHour();
    out.pin_minute = in.getPinMinute();
    out.pin_valid = in.getPinValid();

    out.ecc = in.getEcc();
    out.ecc_valid = in.getEccValid();
    out.ecc_contradicts_region = in.getEccContradictsRegion();

    out.language = in.getLanguage();
    out.language_valid = in.getLanguageValid();

    out.linkage_actuator = in.getLinkageActuator();
    out.linkage_actuator_valid = in.getLinkageActuatorValid();

    out.af = read_frequencies(in.getAf());
    out.af_announced = in.getAfAnnounced();
    out.af_repeats = in.getAfRepeats();

    auto odas = in.getOda();
    out.oda.reserve(odas.size());
    for (auto row : odas) {
        RdsOda entry;
        entry.group_type = row.getGroupType();
        entry.version_b = row.getVersionB();
        entry.message = row.getMessage();
        entry.aid = row.getAid();
        out.oda.push_back(entry);
    }

    auto networks = in.getEon();
    out.eon.reserve(networks.size());
    for (auto row : networks) {
        RdsEonEntry entry;
        entry.pi = row.getPi();
        entry.ps = read_bytes(row.getPs());
        entry.ps_received = row.getPsReceived();
        entry.tp = row.getTp();
        entry.ta = row.getTa();
        entry.ta_valid = row.getTaValid();
        entry.pty = row.getPty();
        entry.pty_valid = row.getPtyValid();
        entry.linkage = row.getLinkage();
        entry.linkage_valid = row.getLinkageValid();
        entry.af = read_frequencies(row.getAf());
        out.eon.push_back(std::move(entry));
    }

    out.health = *health;
    out.composite_rate = in.getCompositeRate();
    out.last_group_sample = in.getLastGroupSample();
    out.fault = read_text(in.getFault());
    out.discarding = in.getDiscarding();
    out.discarded_chunks = in.getDiscardedChunks();
    return out;
}

// Fills a caller-owned frame rather than returning one, so the bins buffer can
// be reused across frames. The callback holds a const reference for the length
// of the call and copies whatever it keeps, which client.h already requires of
// it, and the bin count does not change from frame to frame.
void read_spectrum_frame(SpectrumFrame& out, schema::SpectrumFrame::Reader in) {
    auto bins = in.getPowerDb();
    out.power_db.resize(bins.size());
    for (unsigned i = 0; i < bins.size(); ++i) {
        out.power_db[i] = bins[i];
    }
    out.geometry = read_geometry(in.getGeometry());
    out.start = in.getStart();
    out.count = in.getCount();
    out.sequence = in.getSequence();
    out.floor_db = in.getFloorDb();
    out.ceiling_db = in.getCeilingDb();
    out.percentile_low_db = in.getPercentileLowDb();
    out.percentile_high_db = in.getPercentileHighDb();
}

[[nodiscard]] PassbandGeometry read_passband_geometry(schema::PassbandGeometry::Reader in) {
    PassbandGeometry out;
    out.transform = in.getTransform();
    out.bins = in.getBins();
    out.rate = in.getRate();
    out.bin_width = read_rational(in.getBinWidth());
    out.bin_zero = read_rational(in.getBinZero());
    return out;
}

void read_passband_frame(PassbandFrame& out, schema::PassbandFrame::Reader in) {
    auto bins = in.getPowerDb();
    out.vrx = in.getVrx();
    out.power_db.resize(bins.size());
    for (unsigned i = 0; i < bins.size(); ++i) {
        out.power_db[i] = bins[i];
    }
    out.geometry = read_passband_geometry(in.getGeometry());
    out.start = in.getStart();
    out.count = in.getCount();
    out.sequence = in.getSequence();
    out.floor_db = in.getFloorDb();
    out.ceiling_db = in.getCeilingDb();
    out.percentile_low_db = in.getPercentileLowDb();
    out.percentile_high_db = in.getPercentileHighDb();
}

void read_audio_chunk(AudioChunk& out, schema::AudioChunk::Reader in) {
    auto samples = in.getSamples();
    out.samples.resize(samples.size());
    for (unsigned i = 0; i < samples.size(); ++i) {
        out.samples[i] = samples[i];
    }
    out.sample_rate = in.getSampleRate();
    out.channel_count = in.getChannelCount();
    out.sample_index = in.getSampleIndex();
    out.frames_dropped_before = in.getFramesDroppedBefore();
    out.squelch_open = in.getSquelchOpen();
}

[[nodiscard]] AudioStats read_audio_stats(schema::AudioStats::Reader in) {
    AudioStats out;
    out.frames_sent = in.getFramesSent();
    out.frames_dropped = in.getFramesDropped();
    out.drop_events = in.getDropEvents();
    out.backlog_frames = in.getBacklogFrames();
    out.buffer_frames = in.getBufferFrames();
    return out;
}

// Everything the event loop thread owns, in one place on its own stack.
//
// See the thread model at the top of the file. These three cannot be members
// of ClientImpl, because a Client is deleted on a caller's thread and all
// three must be destroyed on the loop's.
struct LoopState {
    schema::Session::Client session;

    // Fulfilled from inside an executeSync, which is the only way to reach it:
    // kj requires a PromiseFulfiller be fulfilled on the thread that made it.
    kj::Own<kj::PromiseFulfiller<void>> shutdown;

    // Null when there is no subscription. kj::Maybe would say the same thing,
    // but the idiom for reading one differs between kj releases (this build
    // has KJ_IF_MAYBE and no kj::none) while a null kj::Own reads the same in
    // every version.
    kj::Own<schema::SpectrumSubscription::Client> subscription;

    // One per receiver being watched, keyed by the receiver's id. A map and
    // not a single slot, because a rack of receivers with a detail display
    // open on two of them is the ordinary case and a client watching two
    // should not have to open two connections.
    std::map<std::uint64_t, kj::Own<schema::PassbandSubscription::Client>> passbands;

    // The same, for audio. A separate map rather than a pair in the one
    // above, because a client watching a receiver's passband and listening
    // to it is the ordinary case and the two end independently.
    std::map<std::uint64_t, kj::Own<schema::AudioSubscription::Client>> audios;
};

class ClientImpl;

// The capability the engine calls. Its whole job is to get onto ClientImpl,
// which owns the callback and the counters.
class SpectrumReceiverImpl final : public schema::SpectrumReceiver::Server {
public:
    explicit SpectrumReceiverImpl(ClientImpl& owner) : owner_(owner) {}

    kj::Promise<void> frame(FrameContext context) override;

private:
    // No lifetime problem to solve: the Client's destructor stops the loop and
    // joins it before any member of the Client is destroyed, and the loop
    // unwinding is what drops the last reference to this object.
    ClientImpl& owner_;
};

// The same, for one receiver. It carries the receiver's id because the
// engine's frame does too but the subscription is what a client keyed its
// callback on, and trusting the frame's field to route would let a
// misaddressed frame reach the wrong display.
class PassbandReceiverImpl final : public schema::PassbandReceiver::Server {
public:
    PassbandReceiverImpl(ClientImpl& owner, std::uint64_t vrx) : owner_(owner), vrx_(vrx) {}

    kj::Promise<void> frame(FrameContext context) override;

private:
    ClientImpl& owner_;
    std::uint64_t vrx_ = 0;
};

// The same, for one receiver's audio. It carries the receiver's id for the
// reason PassbandReceiverImpl does: the subscription is what a client keyed
// its callback on, and a misaddressed chunk must not reach another pane.
class AudioReceiverImpl final : public schema::AudioReceiver::Server {
public:
    AudioReceiverImpl(ClientImpl& owner, std::uint64_t vrx) : owner_(owner), vrx_(vrx) {}

    kj::Promise<void> chunk(ChunkContext context) override;
    kj::Promise<void> ended(EndedContext context) override;

private:
    ClientImpl& owner_;
    std::uint64_t vrx_ = 0;
};

class ClientImpl final : public Client {
public:
    ClientImpl() = default;

    // kj::Executor's destructor is declared noexcept(false), which makes this
    // class's implicit destructor potentially throwing: a wider exception
    // specification than the Client destructor it overrides, and so a compile
    // error. Hence the explicit noexcept. The body catches everything it calls
    // that can fail, and the one throw left is joining the loop from the loop,
    // which is the misuse client.h already rules out.
    ~ClientImpl() noexcept override;

    [[nodiscard]] Status start(std::string address, std::uint16_t port, Token token);

    [[nodiscard]] Expected<EngineInfo> info() override;
    [[nodiscard]] Expected<bool> running() override;
    [[nodiscard]] Expected<std::vector<SourceDescriptor>> list_sources() override;
    [[nodiscard]] Expected<SourceStats> source_stats() override;

    [[nodiscard]] Expected<std::int64_t> set_source_center(std::int64_t center_hz) override;
    [[nodiscard]] Expected<double> set_source_gain(std::string_view stage, double db) override;
    [[nodiscard]] Status set_source_gain_auto(std::string_view stage, bool on) override;
    [[nodiscard]] Expected<std::optional<SourceDescriptor>> source_descriptor() override;
    [[nodiscard]] Expected<SourceTuning> source_can_retune() override;
    [[nodiscard]] Status open_source(std::string_view uri) override;
    [[nodiscard]] Status close_source() override;

    [[nodiscard]] Expected<std::uint64_t> add_vrx(const VrxParams& params) override;
    [[nodiscard]] Status remove_vrx(std::uint64_t id) override;
    [[nodiscard]] Status set_vrx_params(std::uint64_t id, const VrxParams& params) override;
    [[nodiscard]] Expected<VrxStatus> vrx_status(std::uint64_t id) override;
    [[nodiscard]] Expected<std::vector<std::uint64_t>> vrx_ids() override;

    [[nodiscard]] Expected<DetectionList> detections(double min_confidence) override;
    [[nodiscard]] Status set_detection_threshold(double threshold_db) override;

    [[nodiscard]] Status subscribe_spectrum(std::uint32_t every_nth,
                                            FrameCallback callback) override;
    void unsubscribe_spectrum() override;

    [[nodiscard]] Status subscribe_passband(std::uint64_t vrx, std::uint32_t every_nth,
                                            PassbandCallback callback) override;
    void unsubscribe_passband(std::uint64_t vrx) override;

    [[nodiscard]] Expected<std::uint32_t> subscribe_audio(
        std::uint64_t vrx, std::uint32_t buffer_millis, AudioCallback on_chunk,
        AudioEndedCallback on_ended) override;
    void unsubscribe_audio(std::uint64_t vrx) override;
    [[nodiscard]] Expected<AudioStats> audio_stats(std::uint64_t vrx) override;

    [[nodiscard]] Expected<RdsStation> rds_station(std::uint64_t vrx) override;
    [[nodiscard]] Status set_rds_region(std::uint64_t vrx, RdsRegion region) override;

    [[nodiscard]] std::uint64_t frames_received() const override;
    [[nodiscard]] std::uint64_t frames_dropped() const override;

    // Loop thread only, called by SpectrumReceiverImpl.
    void deliver(schema::SpectrumFrame::Reader in);

    // Loop thread only, called by PassbandReceiverImpl.
    void deliver_passband(std::uint64_t vrx, schema::PassbandFrame::Reader in);

    // Loop thread only, called by AudioReceiverImpl.
    void deliver_audio(std::uint64_t vrx, schema::AudioChunk::Reader in);
    void deliver_audio_ended(std::uint64_t vrx, capnp::Text::Reader reason);

private:
    void run(const std::string& address, std::uint16_t port, std::promise<Status>& ready);

    // Read on the loop thread during the login handshake and never written
    // again after start() hands it over.
    Token token_{};

    // Loop thread only.
    [[nodiscard]] kj::Promise<void> end_subscription(LoopState& state);
    [[nodiscard]] kj::Promise<void> end_passband(LoopState& state, std::uint64_t vrx);
    [[nodiscard]] kj::Promise<void> end_audio(LoopState& state, std::uint64_t vrx);

    // Everything one audio subscription owns on this side, dropped in one
    // place. Loop thread only.
    //
    // It reaches state_ rather than taking a LoopState&, which is what makes
    // it usable from both teardown paths. end_audio is called from inside an
    // on_loop body and has one in hand; deliver_audio_ended is called from a
    // capability the server invoked and has none, and that asymmetry is how
    // the two came to differ.
    void forget_audio(std::uint64_t vrx);

    // Runs body on the event loop thread and waits for the promise it returns,
    // translating whatever comes back into an Expected.
    //
    // Defined here rather than out of line because the return type is deduced
    // from the promise body hands back, and spelling that twice is worse than
    // having a template body in a class definition.
    template <typename Func>
    auto on_loop(std::string_view what, Func&& body)
        -> Expected<typename PromiseValue<std::invoke_result_t<Func&, LoopState&>>::type> {
        using Value = typename PromiseValue<std::invoke_result_t<Func&, LoopState&>>::type;

        // Checked before the mutex, not after. A callback that called back
        // into its own Client would otherwise block on a lock the thread
        // waiting for it is holding, and the hang would say nothing about
        // which rule was broken.
        if (std::this_thread::get_id() == loop_id_) {
            return fail(std::format(
                "{}: called from the spectrum callback. client.h forbids it, because the "
                "callback already runs on the event loop thread and this call waits for that "
                "thread",
                what));
        }

        if (executor_.get() == nullptr) {
            return fail(std::format("{}: this client is not connected", what));
        }

        const std::lock_guard<std::mutex> serialise(calls_);

        try {
            auto deliver_to_loop = [this, &body]() {
                LoopState* state = state_;
                if (state == nullptr) {
                    kj::throwFatalException(
                        KJ_EXCEPTION(DISCONNECTED, "the client event loop is shutting down"));
                }
                return body(*state);
            };

            if constexpr (std::is_void_v<Value>) {
                executor_->executeSync(deliver_to_loop);
                return {};
            } else {
                return executor_->executeSync(deliver_to_loop);
            }
        } catch (const kj::Exception& failure) {
            return std::unexpected(translate(what, failure));
        } catch (const std::exception& failure) {
            return std::unexpected(Error{std::format("{}: {}", what, failure.what())});
        }
    }

    std::thread loop_;

    // Written on the loop thread before the start handshake completes and not
    // again, so a caller that has seen start() return sees a settled value.
    // The reference is counted, so holding it is safe even if the loop exits
    // under us; kj destroys an unreferenced Executor with its event loop.
    kj::Own<const kj::Executor> executor_;
    std::thread::id loop_id_{};

    // Loop thread only, all four. state_ is cleared before the loop's stack
    // unwinds, so a functor delivered during teardown sees null rather than a
    // dead pointer.
    LoopState* state_ = nullptr;
    FrameCallback callback_;
    bool callback_active_ = false;
    SpectrumFrame scratch_;

    // One callback and one scratch frame per receiver being watched. The
    // scratch is per receiver rather than shared because two receivers have
    // different bin counts and a shared buffer would resize on every
    // alternating frame.
    std::map<std::uint64_t, PassbandCallback> passband_callbacks_;
    std::map<std::uint64_t, PassbandFrame> passband_scratch_;

    // The same three per receiver being listened to. The scratch chunk is
    // reused rather than reallocated per chunk, which at 146 chunks a second
    // per receiver is the difference between one allocation and a stream of
    // them; AudioChunk::samples keeps its capacity across an assign.
    std::map<std::uint64_t, AudioCallback> audio_callbacks_;
    std::map<std::uint64_t, AudioEndedCallback> audio_ended_;
    std::map<std::uint64_t, AudioChunk> audio_scratch_;

    // client.h says calls queue. This is what makes them.
    std::mutex calls_;

    // Read from any thread and ordered against nothing: they are counters for
    // a display, not a handshake.
    //
    // frames_dropped_ counts re-entrant delivery and nothing else, and
    // deliver() explains why that leaves it at zero for the life of any
    // client this file can produce.
    std::atomic<std::uint64_t> frames_received_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
};

kj::Promise<void> AudioReceiverImpl::chunk(ChunkContext context) {
    owner_.deliver_audio(vrx_, context.getParams().getChunk());

    // Returning only now, after the callback has run, is what makes the
    // engine's queue fill rather than this process's. A slow callback is a
    // chunk the engine evicts and counts, which is the whole of the
    // backpressure rule core/rpc/server.h states for audio.
    return kj::READY_NOW;
}

kj::Promise<void> AudioReceiverImpl::ended(EndedContext context) {
    owner_.deliver_audio_ended(vrx_, context.getParams().getReason());
    return kj::READY_NOW;
}

kj::Promise<void> PassbandReceiverImpl::frame(FrameContext context) {
    owner_.deliver_passband(vrx_, context.getParams().getFrame());

    // The same backpressure as the span's: the engine allows one frame in
    // flight per subscription and cannot start another until this returns.
    return kj::READY_NOW;
}

kj::Promise<void> SpectrumReceiverImpl::frame(FrameContext context) {
    owner_.deliver(context.getParams().getFrame());

    // Returning only now, after the callback has run, is the backpressure.
    // server.h allows one frame in flight per subscription, so the engine
    // cannot start another until this return reaches it, and a slow callback
    // makes the engine drop frames rather than queue them here.
    return kj::READY_NOW;
}

ClientImpl::~ClientImpl() noexcept {
    {
        const std::lock_guard<std::mutex> serialise(calls_);

        if (executor_.get() != nullptr && std::this_thread::get_id() != loop_id_) {
            try {
                executor_->executeSync([this]() {
                    if (state_ != nullptr) {
                        state_->shutdown->fulfill();
                    }
                });
            } catch (const kj::Exception&) {
                // The loop is already gone. The join below is all that is left
                // to do, and there is nobody to report this to from here.
            } catch (const std::exception&) {
            }
        }
    }

    // Outside the lock: a frame may still be in the callback, and the callback
    // runs on the thread being joined. Every member it touches is still alive,
    // because members are destroyed after this body returns.
    if (loop_.joinable()) {
        loop_.join();
    }
}

Status ClientImpl::start(std::string address, std::uint16_t port, Token token) {
    token_ = token;

    std::promise<Status> ready;
    std::future<Status> settled = ready.get_future();

    try {
        // The promise is moved into the thread rather than captured by
        // reference: start() returns as soon as the handshake lands, and its
        // frame goes with it while the loop thread runs on.
        loop_ = std::thread(
            [this, host = std::move(address), port, ready = std::move(ready)]() mutable {
                run(host, port, ready);
            });
    } catch (const std::system_error& failure) {
        return fail(std::format("could not start the client event loop thread: {}",
                                failure.what()));
    }

    Status result;
    try {
        result = settled.get();
    } catch (const std::exception& failure) {
        result = fail(std::format("the client event loop thread reported nothing: {}",
                                  failure.what()));
    }

    if (!result) {
        loop_.join();
        return result;
    }
    return {};
}

void ClientImpl::run(const std::string& address, std::uint16_t port,
                     std::promise<Status>& ready) {
    const std::string context = std::format("connecting to {}:{}", address, port);

    bool announced = false;
    const auto announce = [&](Status result) {
        if (!announced) {
            announced = true;
            ready.set_value(std::move(result));
        }
    };

    // HOW FAR THE HANDSHAKE GOT, WHICH IS WHAT SEPARATES TWO FAILURES THAT
    // ARRIVE IDENTICAL
    //
    // Both of these land in the one catch below, and until 2026-09-21 both came
    // out of it as an Error carrying a sentence and nothing else:
    //
    //   Nothing is listening on that port yet, which is the ordinary state of a
    //   client started before its engine.
    //
    //   The engine is listening and refused the token, which no amount of
    //   asking again will change.
    //
    // ui/models/engine_link.cpp retried both once a second, because the only
    // thing distinguishing them was the wording of a message. The phase is
    // recorded as the handshake walks it, so the distinction is made by WHERE
    // the throw came from rather than by reading what it said.
    //
    // The phase wins over the kj type for Connecting, and the type wins inside
    // LoggingIn. Nothing answering at an address is Unreachable whether kj
    // called it a refused connection or a disconnection, because either way
    // there is no engine there. A throw during login is the token being refused
    // when kj calls it FAILED, which is login's only documented refusal, and is
    // NOT an authentication verdict when kj calls it DISCONNECTED: that is an
    // engine that died mid-handshake and is worth asking again.
    enum class Phase : std::uint8_t { Connecting, LoggingIn, Serving };
    Phase phase = Phase::Connecting;

    try {
        kj::AsyncIoContext io = kj::setupAsyncIo();

        auto resolved = io.provider->getNetwork()
                            .parseAddress(kj::StringPtr(address.c_str()), port)
                            .wait(io.waitScope);
        auto stream = resolved->connect().wait(io.waitScope);

        capnp::TwoPartyClient rpc(*stream);
        auto shutdown = kj::newPromiseAndFulfiller<void>();

        // THE BOOTSTRAP IS AN Authenticator AND THE SESSION IS PIPELINED ON
        // THE LOGIN THAT HAS NOT COME BACK YET
        //
        // Until 2026-09-20 this line was
        // rpc.bootstrap().castAs<schema::Session>() and opening the socket
        // was the whole of the authorisation.
        //
        // getSession() before the answer arrives is not an optimisation and
        // not a shortcut. It is what Cap'n Proto is for: Session calls made
        // on it travel behind the login rather than after it, so a good token
        // costs one round trip in total. Nothing leaks by doing it. When
        // login fails, capnp breaks this capability with login's own
        // exception and every call on it fails with that exception WITHOUT
        // the engine's Session implementation being entered, which is the
        // property the whole single-check design rests on.
        //
        // The phase moves here rather than after send(), because send() itself
        // can throw on a connection the peer closed between connect and this
        // line, and that is already the token's round trip failing rather than
        // the socket's.
        phase = Phase::LoggingIn;

        auto login = rpc.bootstrap().castAs<schema::Authenticator>().loginRequest();
        login.setToken(capnp::Data::Reader(
            reinterpret_cast<const kj::byte*>(token_.data()), token_.size()));
        auto pending = login.send();

        // The empty brace is a null kj::Own: its constructor from nullptr is
        // explicit, so it cannot be spelled here.
        LoopState state{
            pending.getSession(),
            kj::mv(shutdown.fulfiller),
            {},
        };

        // Declared after `state` so it clears the pointer before `state` is
        // destroyed. The event loop outlives LoopState by the length of this
        // unwinding, and an executeSync delivered in that window would
        // otherwise dereference a dead pointer.
        const ScopedSet<LoopState*> published{state_, &state, nullptr};

        loop_id_ = std::this_thread::get_id();
        executor_ = kj::getCurrentThreadExecutor().addRef();

        // WAITING HERE IS WHAT MAKES A WRONG TOKEN A CONNECT FAILURE
        //
        // Dropping this wait would still be correct in the capability sense:
        // the pipelined session is already broken and every later call would
        // carry the refusal. It would just carry it at a time the caller
        // cannot act on. A supervisor loop wants to hear about a permanent
        // refusal where it asked to connect, so the round trip is paid once,
        // here, and the throw lands in the catch below with the engine's own
        // sentence in it.
        pending.wait(io.waitScope);

        phase = Phase::Serving;
        announce(Status{});

        // Runs until the destructor fulfills this from inside an executeSync.
        // A lost connection does not end it: the session goes on failing
        // calls, and it is the owner who decides when to stop.
        shutdown.promise.wait(io.waitScope);
    } catch (const kj::Exception& failure) {
        Error translated = translate(context, failure);
        switch (phase) {
            case Phase::Connecting:
                translated.category = ErrorCategory::Unreachable;
                break;
            case Phase::LoggingIn:
                // Only where kj called it FAILED, which translate left
                // Unclassified. A DISCONNECTED or an OVERLOADED during login is
                // the engine's state and not a verdict on the token, and it
                // already carries the category that says so.
                if (translated.category == ErrorCategory::Unclassified) {
                    translated.category = ErrorCategory::Unauthenticated;
                }
                break;
            case Phase::Serving:
                break;
        }
        announce(std::unexpected(std::move(translated)));
    } catch (const std::exception& failure) {
        announce(fail(std::format("{}: {}", context, failure.what())));
    } catch (...) {
        announce(fail(std::format("{}: the client event loop failed in a way it could not "
                                  "describe",
                                  context)));
    }

    // Reached with announced already true on every ordinary path. It is here
    // so that a failure between the handshake and the wait cannot leave
    // start() blocked on a future nobody will ever set.
    announce(fail("the client event loop stopped without saying why"));
}

Expected<EngineInfo> ClientImpl::info() {
    return on_loop("info", [](LoopState& state) {
        return state.session.infoRequest().send().then(
            [](auto&& response) { return read_engine_info(response.getInfo()); });
    });
}

Expected<bool> ClientImpl::running() {
    return on_loop("running", [](LoopState& state) {
        return state.session.runningRequest().send().then(
            [](auto&& response) { return response.getRunning(); });
    });
}

Expected<std::vector<SourceDescriptor>> ClientImpl::list_sources() {
    return on_loop("list_sources", [](LoopState& state) {
        return state.session.listSourcesRequest().send().then([](auto&& response) {
            auto sources = response.getSources();
            std::vector<SourceDescriptor> out;
            out.reserve(sources.size());
            for (auto source : sources) {
                out.push_back(read_source_descriptor(source));
            }
            return out;
        });
    });
}

Expected<SourceStats> ClientImpl::source_stats() {
    return on_loop("source_stats", [](LoopState& state) {
        return state.session.sourceStatsRequest().send().then(
            [](auto&& response) { return read_source_stats(response.getStats()); });
    });
}

Expected<std::int64_t> ClientImpl::set_source_center(std::int64_t center_hz) {
    return on_loop("set_source_center", [center_hz](LoopState& state) {
        auto request = state.session.setSourceCenterRequest();
        request.setCenterHz(center_hz);
        return request.send().then(
            [](auto&& response) { return response.getGrantedHz(); });
    });
}

Expected<double> ClientImpl::set_source_gain(std::string_view stage, double db) {
    // The stage is copied into a std::string before the lambda, not captured
    // as a view. on_loop runs the body on the event loop thread and the
    // caller's storage is not this call's to rely on by then.
    return on_loop("set_source_gain", [stage = std::string(stage), db](LoopState& state) {
        auto request = state.session.setSourceGainRequest();
        request.setStage(stage);
        request.setDb(db);
        return request.send().then([](auto&& response) { return response.getGrantedDb(); });
    });
}

Status ClientImpl::set_source_gain_auto(std::string_view stage, bool on) {
    auto done = on_loop("set_source_gain_auto",
                        [stage = std::string(stage), on](LoopState& state) {
                            auto request = state.session.setSourceGainAutoRequest();
                            request.setStage(stage);
                            request.setOn(on);
                            return request.send().then([](auto&&) { return 0; });
                        });
    if (!done) {
        return std::unexpected(done.error());
    }
    return {};
}

Expected<std::optional<SourceDescriptor>> ClientImpl::source_descriptor() {
    return on_loop("source_descriptor", [](LoopState& state) {
        return state.session.sourceDescriptorRequest().send().then(
            [](auto&& response) -> std::optional<SourceDescriptor> {
                if (!response.getOpen()) {
                    return std::nullopt;
                }
                return read_source_descriptor(response.getSource());
            });
    });
}

Expected<SourceTuning> ClientImpl::source_can_retune() {
    return on_loop("source_can_retune", [](LoopState& state) {
        return state.session.sourceCanRetuneRequest().send().then([](auto&& response) {
            SourceTuning out;
            out.can_retune = response.getCanRetune();
            out.low_hz = response.getLowHz();
            out.high_hz = response.getHighHz();
            return out;
        });
    });
}

Status ClientImpl::open_source(std::string_view uri) {
    // Copied into the request rather than referenced, because the lambda runs
    // on the loop thread and on_loop's contract does not promise the caller's
    // buffer outlives the round trip. capnp::Text::Reader wants a NUL, which a
    // string_view does not carry, so the std::string is not avoidable either.
    const std::string owned(uri);
    return on_loop("open_source", [&owned](LoopState& state) {
        auto request = state.session.openSourceRequest();
        request.setUri(capnp::Text::Reader(owned.c_str(), owned.size()));
        return request.send().ignoreResult();
    });
}

Status ClientImpl::close_source() {
    return on_loop("close_source", [](LoopState& state) {
        return state.session.closeSourceRequest().send().ignoreResult();
    });
}

Expected<std::uint64_t> ClientImpl::add_vrx(const VrxParams& params) {
    return on_loop("add_vrx", [&params](LoopState& state) {
        auto request = state.session.addVrxRequest();
        write_vrx_params(request.initParams(), params);
        return request.send().then([](auto&& response) { return response.getId(); });
    });
}

Status ClientImpl::remove_vrx(std::uint64_t id) {
    return on_loop("remove_vrx", [id](LoopState& state) {
        auto request = state.session.removeVrxRequest();
        request.setId(id);
        return request.send().ignoreResult();
    });
}

Status ClientImpl::set_vrx_params(std::uint64_t id, const VrxParams& params) {
    return on_loop("set_vrx_params", [id, &params](LoopState& state) {
        auto request = state.session.setVrxParamsRequest();
        request.setId(id);
        write_vrx_params(request.initParams(), params);
        return request.send().ignoreResult();
    });
}

Expected<VrxStatus> ClientImpl::vrx_status(std::uint64_t id) {
    // Two Expecteds deep, and they mean different things. The outer one is
    // whether the call happened; the inner one is whether what came back is
    // something this client can name, which is read_demod's problem. Flattened
    // here rather than in on_loop, because this is the only call that carries
    // a field the schema can legally hold and this build cannot interpret.
    auto response = on_loop("vrx_status", [id](LoopState& state) {
        auto request = state.session.vrxStatusRequest();
        request.setId(id);
        return request.send().then(
            [](auto&& reply) { return read_vrx_status(reply.getStatus()); });
    });

    if (!response) {
        return std::unexpected(response.error());
    }
    return std::move(*response);
}

Expected<std::vector<std::uint64_t>> ClientImpl::vrx_ids() {
    return on_loop("vrx_ids", [](LoopState& state) {
        return state.session.vrxIdsRequest().send().then([](auto&& response) {
            auto ids = response.getIds();
            std::vector<std::uint64_t> out;
            out.reserve(ids.size());
            for (auto id : ids) {
                out.push_back(id);
            }
            return out;
        });
    });
}

Expected<DetectionList> ClientImpl::detections(double min_confidence) {
    // Two Expecteds deep and flattened, the same shape as vrx_status and for
    // the same reason: the outer one is whether the call happened, the inner
    // one is whether every row carried a state this build can name.
    auto response = on_loop("detections", [min_confidence](LoopState& state) {
        auto request = state.session.detectionsRequest();
        request.setMinConfidence(min_confidence);
        return request.send().then(
            [](auto&& reply) { return read_detection_list(reply.getDetections()); });
    });

    if (!response) {
        return std::unexpected(response.error());
    }
    return std::move(*response);
}

Status ClientImpl::set_detection_threshold(double threshold_db) {
    return on_loop("set_detection_threshold", [threshold_db](LoopState& state) {
        auto request = state.session.setDetectionThresholdRequest();
        request.setThresholdDb(threshold_db);
        return request.send().ignoreResult();
    });
}

// Nothing is checked here and everything is checked at the server, which is
// the same arrangement detections has. A receiver that cannot carry a
// composite is refused by the one side that can see the placement, and a
// copy of the four conditions in this file would be a second policy that
// could disagree with the first.
Expected<RdsStation> ClientImpl::rds_station(std::uint64_t vrx) {
    // Two Expecteds deep and flattened, the same shape as detections and for
    // the same reason: the outer one is whether the call happened, the inner
    // one is whether every enum in the reply carried an ordinal this build
    // can name.
    auto response = on_loop("rds_station", [vrx](LoopState& state) {
        auto request = state.session.rdsStationRequest();
        request.setVrx(vrx);
        return request.send().then(
            [](auto&& reply) { return read_rds_station(reply.getStation()); });
    });

    if (!response) {
        return std::unexpected(response.error());
    }
    return std::move(*response);
}

Status ClientImpl::set_rds_region(std::uint64_t vrx, RdsRegion region) {
    return on_loop("set_rds_region", [vrx, region](LoopState& state) {
        auto request = state.session.setRdsRegionRequest();
        request.setVrx(vrx);
        request.setRegion(static_cast<schema::RdsRegion>(region));
        return request.send().ignoreResult();
    });
}

kj::Promise<void> ClientImpl::end_subscription(LoopState& state) {
    if (state.subscription.get() == nullptr) {
        return kj::READY_NOW;
    }

    // Cancel and then drop. Dropping alone ends the subscription, as the
    // schema says, but waiting for the cancel to return is what makes the stop
    // ordered: Cap'n Proto delivers calls and returns on one connection in
    // order, so any frame() the engine sent before answering this has already
    // been dispatched by the time the answer arrives. A caller that
    // unsubscribes and then tears down what its callback touches is therefore
    // not racing a frame already on the wire.
    auto cancelled = state.subscription->cancelRequest().send().ignoreResult();
    state.subscription = nullptr;
    callback_ = nullptr;

    // A cancel that failed because the connection died has ended the
    // subscription just as thoroughly.
    return cancelled.catch_([](kj::Exception&&) {});
}

Status ClientImpl::subscribe_spectrum(std::uint32_t every_nth, FrameCallback callback) {
    if (!callback) {
        return fail("subscribe_spectrum: the callback is empty. unsubscribe_spectrum is how a "
                    "subscription ends");
    }

    return on_loop("subscribe_spectrum", [this, every_nth, &callback](LoopState& state) {
        // Replacing, per client.h, and the old one is ended first so that a
        // frame from it cannot arrive at the new callback.
        return end_subscription(state).then([this, &state, every_nth, &callback]() {
            // Set before the request goes out, not after it returns. The
            // engine may call frame() on the receiver before it answers the
            // subscribe, and a callback installed afterwards would miss it.
            callback_ = std::move(callback);

            auto request = state.session.subscribeSpectrumRequest();
            request.setReceiver(
                schema::SpectrumReceiver::Client(kj::heap<SpectrumReceiverImpl>(*this)));
            request.setEveryNth(every_nth);

            return request.send()
                .then([&state](auto&& response) {
                    state.subscription = kj::heap<schema::SpectrumSubscription::Client>(
                        response.getSubscription());
                })
                .catch_([this](kj::Exception&& failure) -> kj::Promise<void> {
                    // A subscription that never took has nothing to call back
                    // into, so do not hold the caller's closure alive on the
                    // strength of it.
                    callback_ = nullptr;
                    return kj::Promise<void>(kj::mv(failure));
                });
        });
    });
}

kj::Promise<void> ClientImpl::end_passband(LoopState& state, std::uint64_t vrx) {
    passband_callbacks_.erase(vrx);
    passband_scratch_.erase(vrx);

    auto found = state.passbands.find(vrx);
    if (found == state.passbands.end()) {
        return kj::READY_NOW;
    }

    // Cancel, then drop, for the same ordering reason end_subscription has:
    // a frame the engine sent before answering the cancel has already been
    // dispatched by the time the answer arrives, so a caller that
    // unsubscribes and then tears down what its callback touched is not
    // racing one already on the wire.
    auto cancelled = found->second->cancelRequest().send().ignoreResult();
    state.passbands.erase(found);
    return cancelled.catch_([](kj::Exception&&) {});
}

Status ClientImpl::subscribe_passband(std::uint64_t vrx, std::uint32_t every_nth,
                                      PassbandCallback callback) {
    if (!callback) {
        return fail("subscribe_passband: the callback is empty. unsubscribe_passband is how a "
                    "subscription ends");
    }

    return on_loop("subscribe_passband", [this, vrx, every_nth,
                                          &callback](LoopState& state) {
        return end_passband(state, vrx)
            .then([this, &state, vrx, every_nth, &callback]() {
                // Installed before the request goes out, because the engine
                // may call frame() on the receiver before it answers the
                // subscribe and a callback installed afterwards would miss
                // it.
                passband_callbacks_[vrx] = std::move(callback);
                passband_scratch_[vrx] = PassbandFrame{};

                auto request = state.session.subscribePassbandRequest();
                request.setVrx(vrx);
                request.setReceiver(schema::PassbandReceiver::Client(
                    kj::heap<PassbandReceiverImpl>(*this, vrx)));
                request.setEveryNth(every_nth);

                return request.send()
                    .then([&state, vrx](auto&& response) {
                        state.passbands[vrx] =
                            kj::heap<schema::PassbandSubscription::Client>(
                                response.getSubscription());
                    })
                    .catch_([this, vrx](kj::Exception&& failure) -> kj::Promise<void> {
                        passband_callbacks_.erase(vrx);
                        passband_scratch_.erase(vrx);
                        return kj::Promise<void>(kj::mv(failure));
                    });
            });
    });
}

void ClientImpl::forget_audio(std::uint64_t vrx) {
    audio_callbacks_.erase(vrx);
    audio_ended_.erase(vrx);
    audio_scratch_.erase(vrx);

    // THE CAPABILITY IS THE HALF THAT USED TO BE LEFT BEHIND
    //
    // Until 2026-09-20 the three maps above were erased in two places and
    // this one only in end_audio, so a stream the server ended left its
    // AudioSubscription in place. audio_stats then found it, sent statsRequest
    // and came back Ok with a dead subscription's frozen counters, where
    // every other teardown path answers "this client holds no audio
    // subscription on that receiver". A UI polling stats saw a healthy
    // subscription on a receiver that had been removed. The surviving
    // reference also held the server's AudioSubscriptionImpl, its AudioNode
    // and that node's queue open until the connection dropped, so a client
    // cycling receivers accumulated one per removal.
    //
    // state_ is null only while the loop's stack is unwinding, and LoopState
    // is what is being destroyed at that point, so there is nothing to erase.
    if (state_ != nullptr) {
        state_->audios.erase(vrx);
    }
}

kj::Promise<void> ClientImpl::end_audio(LoopState& state, std::uint64_t vrx) {
    auto found = state.audios.find(vrx);
    if (found == state.audios.end()) {
        forget_audio(vrx);
        return kj::READY_NOW;
    }

    // Cancel, then drop, for the ordering reason end_subscription has: a
    // chunk the engine sent before answering the cancel has already been
    // dispatched by the time the answer arrives, so a caller that
    // unsubscribes and then tears down what its callback touched is not
    // racing one already on the wire.
    //
    // The cancel is also what keeps ended() from firing. The schema says a
    // client never hears ended for a cancel it asked for, and the server
    // honours that by never sending one down a subscription it was told to
    // stop; dropping the capability alone would end the subscription just as
    // thoroughly and would leave the same guarantee resting on destruction
    // order.
    auto cancelled = found->second->cancelRequest().send().ignoreResult();
    forget_audio(vrx);
    return cancelled.catch_([](kj::Exception&&) {});
}

Expected<std::uint32_t> ClientImpl::subscribe_audio(std::uint64_t vrx,
                                                    std::uint32_t buffer_millis,
                                                    AudioCallback on_chunk,
                                                    AudioEndedCallback on_ended) {
    if (!on_chunk) {
        return fail("subscribe_audio: the chunk callback is empty. unsubscribe_audio is how a "
                    "subscription ends");
    }

    return on_loop("subscribe_audio", [this, vrx, buffer_millis, &on_chunk,
                                       &on_ended](LoopState& state) {
        // Replacing, per client.h, and the old one is ended first so that a
        // chunk from it cannot arrive at the new callback.
        return end_audio(state, vrx)
            .then([this, &state, vrx, buffer_millis, &on_chunk, &on_ended]() {
                // Installed before the request goes out, because the engine
                // may call chunk() on the receiver before it answers the
                // subscribe and a callback installed afterwards would miss
                // it.
                audio_callbacks_[vrx] = std::move(on_chunk);
                audio_ended_[vrx] = std::move(on_ended);
                audio_scratch_[vrx] = AudioChunk{};

                auto request = state.session.subscribeAudioRequest();
                request.setVrx(vrx);
                request.setReceiver(
                    schema::AudioReceiver::Client(kj::heap<AudioReceiverImpl>(*this, vrx)));
                request.setBufferMillis(buffer_millis);

                return request.send()
                    .then([&state, vrx](auto&& response) -> std::uint32_t {
                        state.audios[vrx] = kj::heap<schema::AudioSubscription::Client>(
                            response.getSubscription());
                        return response.getBufferMillisGranted();
                    })
                    .catch_([this, vrx](kj::Exception&& failure)
                                -> kj::Promise<std::uint32_t> {
                        // A subscription that never took has nothing to call
                        // back into, so do not hold the caller's closures
                        // alive on the strength of it.
                        forget_audio(vrx);
                        return kj::Promise<std::uint32_t>(kj::mv(failure));
                    });
            });
    });
}

void ClientImpl::unsubscribe_audio(std::uint64_t vrx) {
    // No error channel, for the reason unsubscribe_spectrum gives: the only
    // failure is a connection that has already ended the subscription.
    static_cast<void>(on_loop("unsubscribe_audio", [this, vrx](LoopState& state) {
        return end_audio(state, vrx);
    }));
}

Expected<AudioStats> ClientImpl::audio_stats(std::uint64_t vrx) {
    return on_loop("audio_stats", [vrx](LoopState& state) -> kj::Promise<AudioStats> {
        auto found = state.audios.find(vrx);
        if (found == state.audios.end()) {
            kj::throwFatalException(KJ_EXCEPTION(
                FAILED, "this client holds no audio subscription on that receiver"));
        }
        return found->second->statsRequest().send().then(
            [](auto&& response) { return read_audio_stats(response.getStats()); });
    });
}

void ClientImpl::deliver_audio(std::uint64_t vrx, schema::AudioChunk::Reader in) {
    auto callback = audio_callbacks_.find(vrx);
    if (callback == audio_callbacks_.end() || !callback->second) {
        // An unsubscribe that crossed a chunk already on the wire.
        return;
    }

    auto scratch = audio_scratch_.find(vrx);
    if (scratch == audio_scratch_.end()) {
        return;
    }

    read_audio_chunk(scratch->second, in);
    callback->second(scratch->second);
}

void ClientImpl::deliver_audio_ended(std::uint64_t vrx, capnp::Text::Reader reason) {
    auto ended = audio_ended_.find(vrx);
    if (ended == audio_ended_.end()) {
        return;
    }

    // Copied out before the teardown runs, because the callback is the
    // client's and may unsubscribe from inside itself. client.h forbids
    // calling back into the Client from here, but erasing the entry this
    // iterator names is this file's own footgun rather than the caller's.
    AudioEndedCallback callable = ended->second;
    const std::string text = read_text(reason);

    // The same teardown a cancel goes through, minus the cancel itself. The
    // server has already ended this subscription, so there is nothing left to
    // stop; what is left is to stop holding it. Dropping the capability is
    // what releases the server's AudioSubscriptionImpl and the AudioNode
    // under it, and it is what makes a later audio_stats on this receiver
    // refuse rather than report a corpse.
    forget_audio(vrx);

    if (callable) {
        callable(text);
    }
}

void ClientImpl::unsubscribe_passband(std::uint64_t vrx) {
    // No error channel, for the reason unsubscribe_spectrum gives: the only
    // failure is a connection that has already ended the subscription.
    static_cast<void>(on_loop("unsubscribe_passband", [this, vrx](LoopState& state) {
        return end_passband(state, vrx);
    }));
}

void ClientImpl::deliver_passband(std::uint64_t vrx, schema::PassbandFrame::Reader in) {
    auto callback = passband_callbacks_.find(vrx);
    if (callback == passband_callbacks_.end() || !callback->second) {
        // An unsubscribe that crossed a frame already on the wire.
        return;
    }

    auto scratch = passband_scratch_.find(vrx);
    if (scratch == passband_scratch_.end()) {
        return;
    }

    read_passband_frame(scratch->second, in);
    callback->second(scratch->second);
}

void ClientImpl::unsubscribe_spectrum() {
    // No error channel here, and none is wanted. The only failure this could
    // report is a connection that has already died, which has already ended
    // the subscription, so there would be nothing for a caller to do with it.
    static_cast<void>(
        on_loop("unsubscribe_spectrum", [this](LoopState& state) {
            return end_subscription(state);
        }));
}

std::uint64_t ClientImpl::frames_received() const {
    return frames_received_.load(std::memory_order_relaxed);
}

std::uint64_t ClientImpl::frames_dropped() const {
    return frames_dropped_.load(std::memory_order_relaxed);
}

void ClientImpl::deliver(schema::SpectrumFrame::Reader in) {
    frames_received_.fetch_add(1, std::memory_order_relaxed);

    if (!callback_) {
        // Sent, but there is nobody to hand it to: an unsubscribe that crossed
        // a frame already on the wire. Not a drop in the sense client.h
        // counts, which is this client failing to keep up.
        return;
    }

    if (callback_active_) {
        // Unreachable, and kept as a guard rather than as a statistic.
        //
        // The callback runs inline on this thread and frame() is not answered
        // until it returns, so the engine cannot deliver a second frame while
        // the first is in the callback, and the loop thread is the only
        // thread that dispatches frame() at all. Nothing re-enters here short
        // of a callback driving the event loop itself, which it is given no
        // handle to do. tests/rpc/test_rpc_spectrum.cpp asserts
        // frames_dropped() reads zero even against a subscriber slow enough
        // to make the engine drop, and that zero is the assertion's point: a
        // client that grew a queue would make it move.
        //
        // So this is not the number a display wants, and client.h's summary
        // of it as frames the callback could not keep up with is the shape of
        // the intent rather than of the mechanism. A slow subscriber does
        // cause drops, but the engine executes them under the backpressure
        // core/rpc/server.h describes and counts them on its own side. The
        // only trace that reaches a client is SpectrumFrame::sequence, the
        // engine's own frame counter, jumping by more than the every_nth this
        // subscription asked for. ui/models/engine_link.cpp measures it from
        // there, because Client exposes no accessor to carry it and client.h
        // is a contract between two separate builds rather than somewhere to
        // grow one casually.
        frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const ScopedSet<bool> active{callback_active_, true, false};

    read_spectrum_frame(scratch_, in);
    callback_(scratch_);
}

}  // namespace

Expected<std::unique_ptr<Client>> Client::connect(std::string_view address, std::uint16_t port,
                                                  std::span<const std::uint8_t> token) {
    if (address.empty()) {
        return fail("cannot connect: no address was given");
    }
    // Before the socket, so a caller that forgot the token does not get a
    // refusal from the far end that reads like the engine's fault.
    //
    // Unauthenticated and not Unclassified, on the same ground the far end's
    // refusal sits on: a credential of the wrong shape cannot become the right
    // one by being offered again, so a supervisor that retries this writes one
    // line a second until somebody reads it.
    if (token.size() != kTokenBytes) {
        return fail(std::format(
                        "cannot connect: the token is {} bytes and it has to be exactly {}. "
                        "core/rpc/token.h reads the engine's token file into that shape",
                        token.size(), kTokenBytes),
                    ErrorCategory::Unauthenticated);
    }
    if (port == 0) {
        // ServerOptions::port defaults to zero meaning "bind whatever is
        // free", so a caller that forwards its own options here asks the OS to
        // connect to port zero and gets a message about nothing listening.
        // Server::port() reports the port actually bound.
        return fail("cannot connect: zero is not a port to connect to. A server started on an "
                    "ephemeral port reports the real one through Server::port()");
    }

    Token copied{};
    std::copy_n(token.begin(), kTokenBytes, copied.begin());

    auto client = std::make_unique<ClientImpl>();
    if (auto started = client->start(std::string(address), port, copied); !started) {
        return std::unexpected(started.error());
    }
    return std::unique_ptr<Client>(std::move(client));
}

}  // namespace revenant::rpc
