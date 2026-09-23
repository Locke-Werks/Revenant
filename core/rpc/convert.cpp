#include "core/rpc/convert.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/vrx_reference.h"

namespace revenant::rpc {
namespace {

// dsp::SampleRate is Int64 and every rate in the schema is UInt32, so each
// rate written below narrows and /W4 is right to say so. Made explicit here
// rather than widened on the wire: UInt32 reaches 4.294967295 GS/s, which no
// SDR front end comes near, and core/rpc/types.h mirrors the schema, so
// widening one means widening all three.
//
// Deliberately not clamped. A saturating cast would report a rate the engine
// is not running at, and every frequency a client derives from it would then
// be wrong with nothing to say so.
[[nodiscard]] constexpr std::uint32_t to_wire_rate(dsp::SampleRate rate) {
    return static_cast<std::uint32_t>(rate);
}

// A fixed array of the decoder's characters as the bytes the schema carries.
//
// A reinterpret_cast and not a copy loop, because char and std::uint8_t have
// the same size and alignment and capnp::Data::Reader is a span. The cast is
// the one std::byte-ish aliasing the standard allows outright, and the
// alternative, std::string_view over char, is precisely what the field is
// declared Data to avoid: it invites a reader to treat Annex E code points
// as text.
template <std::size_t N>
[[nodiscard]] capnp::Data::Reader to_bytes(const std::array<char, N>& in) {
    return capnp::Data::Reader(reinterpret_cast<const kj::byte*>(in.data()), in.size());
}

// The AF lists, which are already whole hertz in the decoder and stay whole
// hertz on the wire. Nothing here divides or rounds: these are channel-plan
// frequencies the standard states as integers, which is the one class of
// frequency docs/conventions.md does not ask for a rational.
void write_frequencies(capnp::List<std::int64_t>::Builder out,
                       const std::vector<dsp::Hertz>& in) {
    for (unsigned i = 0; i < out.size(); ++i) {
        out.set(i, in[i]);
    }
}

}  // namespace

schema::Demod to_schema(engine::Demod mode) {
    // A switch rather than the cast the static_asserts would allow, and it is
    // spelled out for one reason: the asserts in convert.h catch a REORDER,
    // and cannot catch a mode ADDED to engine::Demod alone. Every existing
    // assert still passes while the cast puts an ordinal on the wire that no
    // reader has a name for, and the operator finds out by listening to an
    // unintelligible recording.
    //
    // Written exhaustively with no default label, which is a build error
    // because cmake/CompilerFlags.cmake puts /w14062 in the house warning set.
    // That option is load-bearing and this comment used to claim the guard
    // came free with /W4 and /WX. It does not. Measured 2026-09-19 with MSVC
    // 19.44.35228 and this project's exact flags, an unhandled enumerator with
    // no default compiled clean and silent, and only /w14062 produced the
    // diagnostic; C4062 is off by default at /W4. It was on for this target
    // alone until the same measurement turned it on tree-wide, which is what
    // now covers core/engine/vrx.h's demod_name and the rest of the family.
    //
    // The trailing return below is not the fallback that claim was wrong
    // about. It is there because MSVC cannot prove a switch over a scoped
    // enum is exhaustive, since the underlying type can hold values that are
    // not enumerators, and without it C4715 fires. A value that reaches it is
    // one that was never a valid engine::Demod, which is undefined behaviour
    // upstream of here rather than a mode this function failed to map.
    switch (mode) {
        case engine::Demod::Raw: return schema::Demod::RAW;
        case engine::Demod::Am:  return schema::Demod::AM;
        case engine::Demod::Nfm: return schema::Demod::NFM;
        case engine::Demod::Wfm: return schema::Demod::WFM;
        case engine::Demod::Usb: return schema::Demod::USB;
        case engine::Demod::Lsb: return schema::Demod::LSB;
        case engine::Demod::Dsb: return schema::Demod::DSB;
        case engine::Demod::Cw:  return schema::Demod::CW;
        case engine::Demod::P25p1: return schema::Demod::P25P1;
        case engine::Demod::Dstar: return schema::Demod::DSTAR;
        case engine::Demod::Tetra: return schema::Demod::TETRA;
        case engine::Demod::Dmr: return schema::Demod::DMR;
    }
    return schema::Demod::RAW;
}

schema::TrackState to_schema(detect::TrackState state) {
    // Exhaustive with no default, for the reason the switch above gives: the
    // asserts in convert.h catch a renumbering and cannot catch a state added
    // to detect::TrackState alone, and /w14062 in the house warning set is
    // what turns that into a build error.
    //
    // Pending is mapped rather than rejected even though Detector::tracks()
    // never publishes one. A function that refused it would need an error
    // path on a value that cannot arrive, and the state it would have to
    // invent instead is exactly the one a display would then draw wrong.
    switch (state) {
        case detect::TrackState::Pending: return schema::TrackState::PENDING;
        case detect::TrackState::Live:    return schema::TrackState::LIVE;
        case detect::TrackState::Held:    return schema::TrackState::HELD;
        case detect::TrackState::Merged:  return schema::TrackState::MERGED;
    }
    // Unreachable for any value that was ever a valid TrackState. Present
    // because MSVC cannot prove a switch over a scoped enum is exhaustive,
    // the underlying type being able to hold values that are not
    // enumerators, and C4715 fires without it.
    return schema::TrackState::PENDING;
}

schema::FrontEndState to_schema(detect::FrontEndVerdict verdict) {
    // Exhaustive with no default, for the reason the switch above gives.
    switch (verdict) {
        case detect::FrontEndVerdict::Unmeasured: return schema::FrontEndState::UNMEASURED;
        case detect::FrontEndVerdict::Steady:     return schema::FrontEndState::STEADY;
        case detect::FrontEndVerdict::SpanScales: return schema::FrontEndState::SPAN_SCALES;
        case detect::FrontEndVerdict::FloorFollowsSignal:
            return schema::FrontEndState::FLOOR_FOLLOWS_SIGNAL;
    }
    // Unreachable, and present for the C4715 reason to_schema(TrackState)
    // states. Unmeasured rather than any of the others: a value nobody
    // enumerated is not a measurement.
    return schema::FrontEndState::UNMEASURED;
}

schema::RdsRegion to_schema(decode::Region region) {
    // Exhaustive with no default, for the reason the two switches above
    // give. A third region added to decode::Region alone would otherwise
    // reach the wire as an ordinal no reader has a name for, and the reader
    // that matters is the one deciding which PTY table to draw.
    switch (region) {
        case decode::Region::kRds:  return schema::RdsRegion::RDS;
        case decode::Region::kRbds: return schema::RdsRegion::RBDS;
    }
    return schema::RdsRegion::RDS;
}

schema::RdsLock to_schema(decode::RdsLock lock) {
    switch (lock) {
        case decode::RdsLock::Unlocked:  return schema::RdsLock::UNLOCKED;
        case decode::RdsLock::Acquiring: return schema::RdsLock::ACQUIRING;
        case decode::RdsLock::Locked:    return schema::RdsLock::LOCKED;
    }
    return schema::RdsLock::UNLOCKED;
}

schema::RdsSync to_schema(decode::SyncState sync) {
    switch (sync) {
        case decode::SyncState::kHunting: return schema::RdsSync::HUNTING;
        case decode::SyncState::kPreSync: return schema::RdsSync::PRE_SYNC;
        case decode::SyncState::kSynced:  return schema::RdsSync::SYNCED;
    }
    return schema::RdsSync::HUNTING;
}

schema::RetuneCause to_schema(engine::RetuneCause cause) {
    // A switch and not an ordinal cast, because the schema has unknown at
    // zero and the engine has no such value, so the two are one apart.
    switch (cause) {
        case engine::RetuneCause::OutsideSpan:  return schema::RetuneCause::OUTSIDE_SPAN;
        case engine::RetuneCause::Unplaceable:  return schema::RetuneCause::UNPLACEABLE;
        case engine::RetuneCause::ShapeChanged: return schema::RetuneCause::SHAPE_CHANGED;
    }
    // Unreachable, and unknown rather than any cause: a value nobody
    // enumerated is not a cause a client should act on.
    return schema::RetuneCause::UNKNOWN;
}

Expected<decode::Region> from_schema(schema::RdsRegion region) {
    const auto ordinal = static_cast<std::uint16_t>(region);
    if (ordinal > static_cast<std::uint16_t>(decode::Region::kRbds)) {
        return fail(std::format(
            "RDS region ordinal {} is not one this engine knows; the caller was built "
            "against a newer schema",
            ordinal));
    }
    return static_cast<decode::Region>(ordinal);
}

Expected<engine::Demod> from_schema(schema::Demod mode) {
    const auto ordinal = static_cast<std::uint16_t>(mode);
    if (ordinal > static_cast<std::uint16_t>(engine::Demod::Dmr)) {
        return fail(std::format(
            "demodulator ordinal {} is not one this engine knows; the caller was built "
            "against a newer schema",
            ordinal));
    }
    return static_cast<engine::Demod>(ordinal);
}

namespace {

// What a truncated passband does to this mode, for the modes where it breaks
// the output rather than narrowing it, and nothing for the rest.
//
// PER MODE, because the reason differs. This used to be one sentence about a
// discriminator attached to every mode engine::clamp_breaks_demodulator
// names, which was true when that meant NFM and WFM. The digital voice modes
// joined the predicate with their fine stage on 2026-09-22, and TETRA is not
// discriminated at all: it is pi/4-DQPSK read through a root raised cosine
// matched filter, and what a clamp costs it is intersymbol interference.
// Exhaustive with no default, so a twelfth mode has to say something here.
[[nodiscard]] std::string_view clamp_consequence(engine::Demod mode) {
    switch (mode) {
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
            return "the discriminator recovers the instantaneous frequency of whatever reaches "
                   "it, so what comes out is the wrong audio rather than less of the right "
                   "audio";
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Dmr:
            return "core/decode discriminates it to recover the symbols, so a cut sideband "
                   "moves the symbol levels rather than lowering them, and the decoder reads "
                   "wrong symbols from a carrier the waterfall shows as clean";
        case engine::Demod::Tetra:
            return "its receive filter is matched to the whole root raised cosine channel, so "
                   "cutting into it spreads every symbol into its neighbours, and the decoder "
                   "reads that intersymbol interference as bit errors on a carrier the "
                   "waterfall shows as clean";
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw:
            return {};
    }
    return {};
}

// The sentence VrxPlacement::clampReason carries, or nothing.
//
// Built from the request and the grant together, which is why this is here
// and not in core/engine: the engine struct holds the grant, VrxParams holds
// the request, and neither alone can say by how much. It is composed once
// per vrxStatus call, which is a poll rather than the sample path.
[[nodiscard]] std::string clamp_sentence(const engine::VrxPlacement& placement,
                                         const engine::VrxParams& request) {
    if (!placement.bandwidth_clamped) {
        return {};
    }

    const std::int64_t got = placement.granted_high - placement.granted_low;
    const dsp::Hertz room = dsp::max_channel_bandwidth(placement);

    // The same expansion engine::place ran, so the numbers compared here are
    // the ones it compared. It resolved for this same request a moment ago,
    // so a failure is not reachable through vrxStatus; saying what is known
    // is still better than saying nothing.
    auto asked = dsp::resolve_passband(request);
    if (!asked) {
        return std::format(
            "this receiver's passband was fitted to one grid channel and came back as {} to "
            "{} Hz from its centre, {} Hz wide",
            placement.granted_low, placement.granted_high, got);
    }

    const std::int64_t wanted = asked->width();
    std::string out = std::format(
        "this receiver asked for {} Hz of passband, {} to {} Hz from its centre, and one "
        "channel of this grid could carry {} Hz of it, {} to {}",
        wanted, asked->low, asked->high, got, placement.granted_low, placement.granted_high);

    if (wanted > 0 && got < wanted) {
        out += std::format(", which is {} percent of what was asked for",
                           (100 * got) / wanted);
        // engine::clamp_breaks_demodulator, which used to be a copy here.
        // engine::place reads the same predicate to decide which clamps it
        // refuses outright, and two copies of that would be two policies:
        // this sentence would keep describing a receiver the engine had
        // already declined to build, or stop describing one it still does.
        if (engine::clamp_breaks_demodulator(request.demod)) {
            out += std::format(
                ". A {} receiver on a truncated passband is not a narrower version of the "
                "same receiver: {}",
                engine::demod_name(request.demod), clamp_consequence(request.demod));
        }
    }

    out += std::format(
        ". The widest a receiver placed here can be is {} Hz, which is a property of the "
        "channel grid rather than of this receiver: the grid is sized when the source is "
        "opened, so widening it is revenant-engine's --channels and nothing this session "
        "can set",
        room);
    return out;
}

}  // namespace

void write_rational(schema::Rational::Builder out, std::int64_t numerator,
                    std::int64_t denominator) {
    out.setNumerator(numerator);
    // Never zero on the wire. A reader dividing by it would get a NaN
    // frequency that shows up as a blank axis label three layers away.
    out.setDenominator(denominator == 0 ? 1 : denominator);
}

void write_device(schema::DeviceInfo::Builder out, const gpu::DeviceInfo& in) {
    out.setIndex(in.index);
    out.setName(in.name);
    out.setVendor(in.vendor_name());
    out.setDiscrete(in.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
    out.setApiVersion(in.api_version);
    out.setDriverVersion(in.driver_version);
}

// An enum written by name rather than by cast, on the same ground convert.h
// gives for every other ordinal in this file: a static_assert pins the two
// enumerations together and a switch means a value added to one of them without
// the other fails to compile rather than crossing the wire as a number the far
// side reads as something else.
[[nodiscard]] schema::SampleFormat to_schema(source::SampleFormat in) {
    switch (in) {
        case source::SampleFormat::Cu8:
            return schema::SampleFormat::CU8;
        case source::SampleFormat::Cs8:
            return schema::SampleFormat::CS8;
        case source::SampleFormat::Cs16:
            return schema::SampleFormat::CS16;
        case source::SampleFormat::Cf32:
            return schema::SampleFormat::CF32;
        case source::SampleFormat::Cs24:
            return schema::SampleFormat::CS24;
    }
    return schema::SampleFormat::CF32;
}

[[nodiscard]] schema::FlowControl to_schema(source::FlowControl in) {
    switch (in) {
        case source::FlowControl::Paced:
            return schema::FlowControl::PACED;
        case source::FlowControl::Demand:
            return schema::FlowControl::DEMAND;
    }
    return schema::FlowControl::DEMAND;
}

void write_source_descriptor(schema::SourceDescriptor::Builder out,
                             const source::SourceCapabilities& in) {
    out.setUri(in.uri);
    out.setBackend(in.backend);
    out.setDisplayName(in.display_name);
    out.setUnavailable(in.unavailable);

    auto notes = out.initNotes(static_cast<std::uint32_t>(in.notes.size()));
    for (std::uint32_t i = 0; i < notes.size(); ++i) {
        notes.set(i, in.notes[i]);
    }

    // A RANGE WHOSE HIGH IS BELOW ITS LOW IS DROPPED RATHER THAN CARRIED, which
    // is the same filter Engine::source_tuning applies for the same reason: a
    // backend that could not describe its tuner leaves an empty or inverted
    // entry rather than guessing, and librtlsdr has no driver for a tuner it did
    // not recognise, so nothing on such a dongle can be tuned at all. Carrying
    // the inverted pair would offer a client a control that refuses everything.
    std::uint32_t usable = 0;
    for (const source::TuneRange& range : in.tune_ranges) {
        if (range.high >= range.low) {
            ++usable;
        }
    }
    auto ranges = out.initTuneRanges(usable);
    std::uint32_t at = 0;
    for (const source::TuneRange& range : in.tune_ranges) {
        if (range.high < range.low) {
            continue;
        }
        auto one = ranges[at++];
        one.setLowHz(range.low);
        one.setHighHz(range.high);
        one.setStepHz(range.step);
    }

    auto rates = out.initSampleRates(static_cast<std::uint32_t>(in.sample_rates.size()));
    for (std::uint32_t i = 0; i < rates.size(); ++i) {
        rates.set(i, to_wire_rate(in.sample_rates[i]));
    }
    out.setMinRate(to_wire_rate(in.min_rate));
    out.setMaxRate(to_wire_rate(in.max_rate));

    out.setNativeFormat(to_schema(in.native_format));
    out.setBitsPerComponent(in.bits_per_component);

    auto stages = out.initGainStages(static_cast<std::uint32_t>(in.gain_stages.size()));
    for (std::uint32_t i = 0; i < stages.size(); ++i) {
        const source::GainStage& stage = in.gain_stages[i];
        auto one = stages[i];
        one.setName(stage.name);
        one.setMinDb(stage.min_db);
        one.setMaxDb(stage.max_db);
        one.setHasAuto(stage.has_auto);

        auto steps = one.initStepsDb(static_cast<std::uint32_t>(stage.steps_db.size()));
        for (std::uint32_t step = 0; step < steps.size(); ++step) {
            steps.set(step, stage.steps_db[step]);
        }
    }

    out.setFlow(to_schema(in.flow));
    out.setSeekable(in.seekable);
    out.setLengthSamples(in.length_samples);
    out.setSerial(in.serial);
}

source::DeviceCalibration read_calibration_settings(schema::CalibrationSettings::Reader in) {
    source::DeviceCalibration out;
    out.correction_ppb = in.getCorrectionPpb();
    out.dc_removal = in.getDcRemoval();
    out.iq_correction = in.getIqCorrection();
    return out;
}

void write_calibration(schema::Calibration::Builder out, const engine::CalibrationState& in) {
    out.setOpen(in.open);
    out.setKey(in.key);
    auto settings = out.initSettings();
    settings.setCorrectionPpb(in.settings.correction_ppb);
    settings.setDcRemoval(in.settings.dc_removal);
    settings.setIqCorrection(in.settings.iq_correction);
    out.setCorrectionApplied(in.correction_applied);
    out.setPersisted(in.persisted);
    out.setNote(in.note);
    out.setDeviceCenterHz(in.device_center);

    const dsp::FrontEndEstimate& estimate = in.front_end.estimate;
    out.setMeasured(estimate.measured);
    out.setBlocksMeasured(in.front_end.blocks_measured);
    if (estimate.measured) {
        out.setDcDbfs(estimate.dc_dbfs());
        out.setGainErrorDb(estimate.gain_error_db());
        out.setPhaseErrorDeg(estimate.phase_error_deg());
        out.setImageRejectionDb(estimate.image_rejection_db());
        out.setIqPlausible(estimate.iq_plausible);
    } else {
        out.setDcDbfs(-300.0);
    }
}

void write_grid(schema::GridParams::Builder out, const dsp::GridParams& in) {
    out.setChannels(in.channels);
    out.setTapsPerBranch(in.taps_per_branch);
    out.setDecimation(in.decimation);
}

void write_spectrum_geometry(schema::SpectrumGeometry::Builder out,
                             const engine::SpectrumGeometry& in) {
    out.setTransform(in.transform);
    out.setBinsPerChannel(in.bins_per_channel);
    out.setChannels(in.channels);
    out.setBins(in.bins);
    write_rational(out.initBinWidth(), in.bin_width_numerator, in.bin_width_denominator);
    write_rational(out.initBinZero(), in.bin_zero_numerator, in.bin_zero_denominator);
}

void write_engine_info(schema::EngineInfo::Builder out, const engine::EngineInfo& in,
                       const engine::SourcePacing& pacing) {
    write_device(out.initDevice(), in.device);
    write_grid(out.initGrid(), in.grid);
    out.setSourceRate(to_wire_rate(in.source_rate));
    out.setChannelRate(to_wire_rate(in.channel_rate));
    out.setChannelSpacing(in.channel_spacing);
    write_spectrum_geometry(out.initSpectrum(), in.spectrum);
    out.setSourceCenter(in.source_center);
    out.setRingSamples(in.ring.capacity_samples);
    out.setRingSeconds(in.ring.seconds_retained);
    out.setRingClamped(in.ring.clamped);
    out.setRingClampReason(in.ring.clamp_reason);
    out.setRealtimeFactor(pacing.realtime_factor);
    out.setSourcePacedBy(pacing.paced_by);
    out.setSourceEpoch(in.source_epoch);
    out.setRealtimeWindowSeconds(pacing.window_seconds);
}

void write_source_stats(schema::SourceStats::Builder out, const source::SourceStats& in,
                        const detect::FrontEndObservation& front_end,
                        const engine::GraphConditions& conditions) {
    out.setBlocksDelivered(in.blocks_delivered);
    out.setSamplesDelivered(in.samples_delivered);
    out.setOverrunEvents(in.overrun_events);
    out.setSamplesLost(in.samples_lost);
    out.setLastLossIndex(in.last_loss_index);
    out.setWriteIndex(in.write_index);
    out.setFrontEnd(to_schema(front_end.verdict));
    out.setFrontEndSlope(front_end.slope);
    out.setFrontEndFloorLiftDb(front_end.floor_lift_db);

    // The graph's two, on the source's message because that is the one this
    // wire already polls once a second and neither is worth a call of its own.
    // engine::GraphConditions is where the argument lives for why they are not
    // fields on source::SourceStats.
    out.setVrxRetuneRefusals(conditions.vrx_retune_refusals);
    out.setFrameStalls(conditions.frame_stalls);
}

void write_vrx_params(schema::VrxParams::Builder out, const engine::VrxParams& in) {
    out.setCenter(in.center);
    out.setBandwidth(in.bandwidth);
    out.setDemod(to_schema(in.demod));
    out.setAudioRate(to_wire_rate(in.audio_rate));
    out.setSquelchDbfs(in.squelch_dbfs);
    out.setAgcAttackMs(in.agc_attack_ms);
    out.setAgcDecayMs(in.agc_decay_ms);
    out.setAgcEnabled(in.agc_enabled);
    out.setCwPitch(in.cw_pitch);
    out.setPassbandLow(in.passband_low);
    out.setPassbandHigh(in.passband_high);
    out.setNoiseBlanker(in.nb_enabled);
    out.setNoiseBlankerThresholdDb(in.nb_threshold_db);
    out.setNotch(in.notch_enabled);
    out.setNotchHz(in.notch_hz);
    out.setNotchDepthDb(in.notch_depth_db);
    out.setNotchWidthHz(in.notch_width_hz);
    out.setAutoNotch(in.auto_notch_enabled);
    out.setNoiseReduction(in.nr_enabled);
    out.setNoiseReductionStrength(in.nr_strength);
}

void write_vrx_placement(schema::VrxPlacement::Builder out, const engine::VrxPlacement& in,
                         const engine::VrxParams& request) {
    out.setChannel(in.channel);
    write_rational(out.initChannelCentre(), in.channel_centre.numerator,
                   in.channel_centre.denominator);
    write_rational(out.initResidual(), in.residual_numerator, in.residual_denominator);
    out.setChannelRate(to_wire_rate(in.channel_rate));
    out.setBandwidthClamped(in.bandwidth_clamped);
    out.setGrantedLow(in.granted_low);
    out.setGrantedHigh(in.granted_high);
    out.setClampReason(clamp_sentence(in, request));
}

void write_vrx_status(schema::VrxStatus::Builder out, const engine::VrxStatus& in) {
    out.setId(in.id.value);
    write_vrx_params(out.initParams(), in.params);

    // The params as given, which is what the clamp sentence has to compare
    // the grant against. VrxStatus hands back the request verbatim, so this
    // is the request and not a reading of the result.
    write_vrx_placement(out.initPlacement(), in.placement, in.params);
    out.setDemodRate(to_wire_rate(in.demod_rate));
    out.setResolvedAudioRate(to_wire_rate(in.resolved_audio_rate));
    out.setLevelDbfs(in.level_dbfs);
    out.setSquelchOpen(in.squelch_open);
    out.setAudioSamples(in.audio_samples);
    out.setAudioDropped(in.audio_dropped);

    // The pair a client reads as a dropout, which no other field on this
    // message carries: the frames between a re-anchor's two cursors were
    // never produced, so audioSamples cannot show them and audioDropped
    // counts something else.
    out.setReanchors(in.reanchors);
    out.setReanchorFramesSkipped(in.reanchor_frames_skipped);
}

void write_spectrum_frame(schema::SpectrumFrame::Builder out,
                          const engine::SpectrumFrame& in) {
    auto bins = out.initPowerDb(static_cast<unsigned>(in.power_db.size()));
    for (unsigned i = 0; i < bins.size(); ++i) {
        bins.set(i, in.power_db[i]);
    }
    write_spectrum_geometry(out.initGeometry(), in.geometry);
    out.setStart(in.start);
    out.setCount(in.count);
    out.setSequence(in.sequence);
    out.setFloorDb(in.floor_db);
    out.setCeilingDb(in.ceiling_db);
    out.setPercentileLowDb(in.percentile_low_db);
    out.setPercentileHighDb(in.percentile_high_db);
}

void write_passband_geometry(schema::PassbandGeometry::Builder out,
                             const engine::PassbandGeometry& in) {
    out.setTransform(in.transform);
    out.setBins(in.bins);
    out.setRate(to_wire_rate(in.rate));
    write_rational(out.initBinWidth(), in.bin_width_numerator, in.bin_width_denominator);
    write_rational(out.initBinZero(), in.bin_zero_numerator, in.bin_zero_denominator);
}

void write_passband_frame(schema::PassbandFrame::Builder out,
                          const engine::PassbandFrame& in) {
    out.setVrx(in.vrx.value);
    auto bins = out.initPowerDb(static_cast<unsigned>(in.power_db.size()));
    for (unsigned i = 0; i < bins.size(); ++i) {
        bins.set(i, in.power_db[i]);
    }
    write_passband_geometry(out.initGeometry(), in.geometry);
    out.setStart(in.start);
    out.setCount(in.count);
    out.setSequence(in.sequence);
    out.setFloorDb(in.floor_db);
    out.setCeilingDb(in.ceiling_db);
    out.setPercentileLowDb(in.percentile_low_db);
    out.setPercentileHighDb(in.percentile_high_db);
}

void write_detection(schema::Detection::Builder out, const detect::Track& in) {
    out.setId(in.id);
    out.setCenterHz(in.center);
    out.setBandwidthHz(in.bandwidth);
    out.setSnr2500Db(in.snr_2500_db);
    out.setConfidence(in.confidence);
    out.setMarginConfidence(in.margin_confidence);
    out.setState(to_schema(in.state));
    out.setFirstSeen(in.first_seen);
    out.setLastSeen(in.last_seen);
    out.setLastDetected(in.last_detected);
    out.setChannel(in.channel);
    out.setChannelValid(in.channel_valid);
    out.setMergedInto(in.merged_into);
    out.setConcentration(in.shape.concentration);
    out.setShapeMeasured(in.shape.measured);
}

void write_rds_bits_status(schema::RdsHealth::Builder out, const decode::RdsBitsStatus& in) {
    out.setLock(to_schema(in.lock));
    out.setQuality(in.quality);
    out.setBiphaseConsistency(in.biphase_consistency);
    out.setCarrierCoherence(in.carrier_coherence);
    out.setCarrierOffsetHz(in.carrier_offset_hz);
    out.setBitRateHz(in.bit_rate_hz);
    out.setPilotLocked(in.pilot_locked);
    out.setPilotLevel(in.pilot_level);
    out.setSamplesConsumed(in.samples_consumed);
    out.setBitsEmitted(in.bits_emitted);
    out.setReacquisitions(in.reacquisitions);
}

void write_rds_station(schema::RdsStation::Builder out, const decode::StationState& in,
                       decode::Region region) {
    out.setPi(in.pi);
    out.setPiValid(in.pi_valid);

    // Derived here rather than left to the client, because the derivation is
    // region dependent and a client with one rule shows the wrong call sign
    // on the other continent. An empty string means no call sign is
    // derivable from this PI in this region, which is a different answer
    // from "no PI yet" and piValid is what separates them.
    if (in.pi_valid) {
        if (auto call = decode::callsign_from_pi(region, in.pi); call.has_value()) {
            out.setCallSign(*call);
        }
    }

    out.setPty(in.pty);
    out.setPtyValid(in.pty_valid);

    // Both widths, always, and not only when ptyValid. A reader that checks
    // the flag gets the same answer either way, and a name computed for code
    // zero is the standard's own row for zero rather than a placeholder.
    const std::string_view short_name =
        decode::pty_name(region, in.pty, decode::PtyWidth::kShort);
    const std::string_view long_name =
        decode::pty_name(region, in.pty, decode::PtyWidth::kLong);
    out.setPtyShortName(capnp::Text::Reader(short_name.data(), short_name.size()));
    out.setPtyLongName(capnp::Text::Reader(long_name.data(), long_name.size()));

    out.setTp(in.tp);
    out.setTpValid(in.tp_valid);
    out.setTa(in.ta);
    out.setTaValid(in.ta_valid);

    out.setMusic(in.music);
    out.setMusicValid(in.music_valid);

    out.setDiStereo(in.di.stereo);
    out.setDiArtificialHead(in.di.artificial_head);
    out.setDiCompressed(in.di.compressed);
    out.setDiDynamicPty(in.di.dynamic_pty);
    out.setDiReceived(in.di.received);

    // Data and not Text for the three text fields, because they are EN 50067
    // Annex E code points. See the note on schema::RdsStation::ps: a station
    // transmitting an accented character would put invalid UTF-8 on the wire
    // through a Text field, the C++ runtime would not notice, and a Python
    // or Rust client would fail to decode it or silently replace it.
    out.setPs(to_bytes(in.ps));
    out.setPsReceived(in.ps_received);

    out.setRt(to_bytes(in.rt));
    out.setRtReceived(in.rt_received);
    out.setRtLength(static_cast<std::uint32_t>(in.rt_length));
    out.setRtAb(in.rt_ab);
    out.setRtAbValid(in.rt_ab_valid);
    out.setRtVersionB(in.rt_version_b);

    out.setPtyn(to_bytes(in.ptyn));
    out.setPtynReceived(in.ptyn_received);
    out.setPtynAb(in.ptyn_ab);
    out.setPtynAbValid(in.ptyn_ab_valid);
    out.setPtynCorrected(in.ptyn_corrected);

    auto tmc = out.initTmc();
    tmc.setAnnounced(in.tmc.announced);
    tmc.setAid(in.tmc.aid);
    tmc.setGroupType(in.tmc.group_type);
    tmc.setVersionB(in.tmc.version_b);
    tmc.setOdaMessage(in.tmc.oda_message);
    tmc.setIdentification(in.tmc.identification);
    tmc.setIdentificationValid(in.tmc.identification_valid);
    tmc.setGroups(in.tmc.groups);
    tmc.setOdaGroups(in.tmc.oda_groups);
    tmc.setIncomplete(in.tmc.incomplete);
    tmc.setEvicted(in.tmc.evicted);
    auto messages = tmc.initMessages(static_cast<unsigned>(in.tmc.messages.size()));
    for (unsigned i = 0; i < messages.size(); ++i) {
        const decode::TmcMessage& row = in.tmc.messages[i];
        messages[i].setX(row.x);
        messages[i].setY(row.y);
        messages[i].setZ(row.z);
        messages[i].setReceptions(row.receptions);
        messages[i].setCorrectedReceptions(row.corrected_receptions);
    }

    auto ews = out.initEws();
    ews.setGroups(in.ews.groups);
    ews.setGroupType(in.ews.last.group_type);
    ews.setVersionB(in.ews.last.version_b);
    ews.setBlock2Low(in.ews.last.block2_low);
    ews.setBlock3(in.ews.last.block3);
    ews.setBlock3Valid(in.ews.last.block3_valid);
    ews.setBlock4(in.ews.last.block4);
    ews.setBlock4Valid(in.ews.last.block4_valid);
    ews.setCorrected(in.ews.last.corrected);
    out.setEwsChannelIdentification(in.ews_channel_identification);
    out.setEwsChannelIdentificationValid(in.ews_channel_identification_valid);

    auto clock = out.initClock();
    clock.setMjd(in.clock.mjd);
    clock.setYear(in.clock.date.year);
    clock.setMonth(in.clock.date.month);
    clock.setDay(in.clock.date.day);
    clock.setHour(in.clock.hour);
    clock.setMinute(in.clock.minute);
    clock.setOffsetHalfHours(in.clock.offset_half_hours);
    clock.setValid(in.clock.valid);

    out.setPinDay(in.pin.day);
    out.setPinHour(in.pin.hour);
    out.setPinMinute(in.pin.minute);
    out.setPinValid(in.pin.valid);

    out.setEcc(in.ecc);
    out.setEccValid(in.ecc_valid);
    out.setEccContradictsRegion(in.ecc_contradicts_region);

    out.setLanguage(in.language);
    out.setLanguageValid(in.language_valid);

    out.setLinkageActuator(in.linkage_actuator);
    out.setLinkageActuatorValid(in.linkage_actuator_valid);

    write_frequencies(out.initAf(static_cast<unsigned>(in.af.size())), in.af);
    out.setAfAnnounced(in.af_announced);
    out.setAfRepeats(in.af_repeats);

    auto odas = out.initOda(static_cast<unsigned>(in.oda.size()));
    for (unsigned i = 0; i < odas.size(); ++i) {
        const decode::OdaAnnouncement& row = in.oda[i];
        odas[i].setGroupType(row.group_type);
        odas[i].setVersionB(row.version_b);
        odas[i].setMessage(row.message);
        odas[i].setAid(row.aid);
    }

    auto networks = out.initEon(static_cast<unsigned>(in.eon.size()));
    for (unsigned i = 0; i < networks.size(); ++i) {
        const decode::EonEntry& row = in.eon[i];
        networks[i].setPi(row.pi);
        networks[i].setPs(to_bytes(row.ps));
        networks[i].setPsReceived(row.ps_received);
        networks[i].setTp(row.tp);
        networks[i].setTa(row.ta);
        networks[i].setTaValid(row.ta_valid);
        networks[i].setPty(row.pty);
        networks[i].setPtyValid(row.pty_valid);
        networks[i].setLinkage(row.linkage);
        networks[i].setLinkageValid(row.linkage_valid);
        write_frequencies(networks[i].initAf(static_cast<unsigned>(row.af.size())), row.af);
    }
}

Expected<engine::VrxParams> read_vrx_params(schema::VrxParams::Reader in) {
    auto mode = from_schema(in.getDemod());
    if (!mode) {
        return std::unexpected(mode.error());
    }

    engine::VrxParams out;
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
    out.nb_enabled = in.getNoiseBlanker();
    out.nb_threshold_db = in.getNoiseBlankerThresholdDb();
    out.notch_enabled = in.getNotch();
    out.notch_hz = in.getNotchHz();
    out.notch_depth_db = in.getNotchDepthDb();
    out.notch_width_hz = in.getNotchWidthHz();
    out.auto_notch_enabled = in.getAutoNotch();
    out.nr_enabled = in.getNoiseReduction();
    out.nr_strength = in.getNoiseReductionStrength();
    return out;
}

void write_decoded_message(schema::DecodedMessage::Builder out, const DecodedMessage& in) {
    out.setVrx(in.vrx);
    out.setDecoder(in.decoder);
    out.setKind(in.kind);
    out.setStartSample(in.start_sample);
    out.setEndSample(in.end_sample);
    out.setSampleRate(in.sample_rate);
    out.setText(in.text);
    out.setSequence(in.sequence);
    out.setDroppedBefore(in.dropped_before);

    auto fields = out.initFields(static_cast<unsigned>(in.fields.size()));
    for (unsigned i = 0; i < fields.size(); ++i) {
        const DecodedField& field = in.fields[i];
        fields[i].setKey(field.key);
        auto value = fields[i].initValue();

        // By index and exhaustively, so a sixth alternative added to
        // DecodedValue fails to compile here rather than crossing as the
        // union's default, which is an integer zero.
        static_assert(std::variant_size_v<DecodedValue> == 5,
                      "a new DecodedValue alternative needs a case here and in client.cpp");
        switch (field.value.index()) {
            case 0: value.setInteger(std::get<0>(field.value)); break;
            case 1: value.setReal(std::get<1>(field.value)); break;
            case 2: value.setFlag(std::get<2>(field.value)); break;
            case 3: value.setText(std::get<3>(field.value)); break;
            case 4: {
                const std::vector<std::uint8_t>& bytes = std::get<4>(field.value);
                value.setBytes(capnp::Data::Reader(bytes.data(), bytes.size()));
                break;
            }
            default: break;
        }
    }
}

void write_decoder_info(schema::DecoderInfo::Builder out, std::string_view name,
                        DecoderInput input, std::string_view description,
                        std::span<const std::string_view> modes) {
    out.setName(std::string(name));
    out.setDescription(std::string(description));

    std::vector<std::string> named;
    if (modes.empty()) {
        const bool complex = input == DecoderInput::ComplexBaseband;
        for (auto ordinal = static_cast<std::uint16_t>(engine::Demod::Raw);
             ordinal <= static_cast<std::uint16_t>(engine::Demod::Dmr); ++ordinal) {
            const auto mode = static_cast<engine::Demod>(ordinal);
            if (engine::is_complex_tap(mode) == complex) {
                named.emplace_back(engine::demod_name(mode));
            }
        }
    } else {
        for (const std::string_view mode : modes) {
            named.emplace_back(mode);
        }
    }
    auto list = out.initModes(static_cast<unsigned>(named.size()));
    for (unsigned i = 0; i < list.size(); ++i) {
        list.set(i, named[i].c_str());
    }

    // Exhaustive and with no default, per docs/conventions.md, so a third
    // input kind stops the build here.
    switch (input) {
        case DecoderInput::ComplexBaseband:
            out.setInput(schema::DecoderInput::COMPLEX_BASEBAND);
            return;
        case DecoderInput::RealAudio:
            out.setInput(schema::DecoderInput::REAL_AUDIO);
            return;
    }
}

}  // namespace revenant::rpc
