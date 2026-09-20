#include "core/rpc/convert.h"

#include <format>

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

Expected<engine::Demod> from_schema(schema::Demod mode) {
    const auto ordinal = static_cast<std::uint16_t>(mode);
    if (ordinal > static_cast<std::uint16_t>(engine::Demod::Cw)) {
        return fail(std::format(
            "demodulator ordinal {} is not one this engine knows; the caller was built "
            "against a newer schema",
            ordinal));
    }
    return static_cast<engine::Demod>(ordinal);
}

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

void write_source_descriptor(schema::SourceDescriptor::Builder out,
                             const source::SourceCapabilities& in) {
    out.setUri(in.uri);
    out.setBackend(in.backend);
    out.setDisplayName(in.display_name);
    out.setUnavailable(in.unavailable);
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

void write_engine_info(schema::EngineInfo::Builder out, const engine::EngineInfo& in) {
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
}

void write_source_stats(schema::SourceStats::Builder out, const source::SourceStats& in) {
    out.setBlocksDelivered(in.blocks_delivered);
    out.setSamplesDelivered(in.samples_delivered);
    out.setOverrunEvents(in.overrun_events);
    out.setSamplesLost(in.samples_lost);
    out.setLastLossIndex(in.last_loss_index);
    out.setWriteIndex(in.write_index);
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
}

void write_vrx_placement(schema::VrxPlacement::Builder out, const engine::VrxPlacement& in) {
    out.setChannel(in.channel);
    write_rational(out.initChannelCentre(), in.channel_centre.numerator,
                   in.channel_centre.denominator);
    write_rational(out.initResidual(), in.residual_numerator, in.residual_denominator);
    out.setChannelRate(to_wire_rate(in.channel_rate));
    out.setBandwidthClamped(in.bandwidth_clamped);
}

void write_vrx_status(schema::VrxStatus::Builder out, const engine::VrxStatus& in) {
    out.setId(in.id.value);
    write_vrx_params(out.initParams(), in.params);
    write_vrx_placement(out.initPlacement(), in.placement);
    out.setLevelDbfs(in.level_dbfs);
    out.setSquelchOpen(in.squelch_open);
    out.setAudioSamples(in.audio_samples);
    out.setAudioDropped(in.audio_dropped);
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

void write_detection(schema::Detection::Builder out, const detect::Track& in) {
    out.setId(in.id);
    out.setCenterHz(in.center);
    out.setBandwidthHz(in.bandwidth);
    out.setSnr2500Db(in.snr_2500_db);
    out.setConfidence(in.confidence);
    out.setState(to_schema(in.state));
    out.setFirstSeen(in.first_seen);
    out.setLastSeen(in.last_seen);
    out.setLastDetected(in.last_detected);
    out.setChannel(in.channel);
    out.setChannelValid(in.channel_valid);
    out.setMergedInto(in.merged_into);
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
    return out;
}

}  // namespace revenant::rpc
