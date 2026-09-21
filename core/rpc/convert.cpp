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
    if (ordinal > static_cast<std::uint16_t>(engine::Demod::Tetra)) {
        return fail(std::format(
            "demodulator ordinal {} is not one this engine knows; the caller was built "
            "against a newer schema",
            ordinal));
    }
    return static_cast<engine::Demod>(ordinal);
}

namespace {

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
                "same receiver: the discriminator recovers the instantaneous frequency of "
                "whatever reaches it, so what comes out is the wrong audio rather than less "
                "of the right audio",
                engine::demod_name(request.demod));
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
}

void write_source_stats(schema::SourceStats::Builder out, const source::SourceStats& in,
                        const detect::FrontEndObservation& front_end) {
    out.setBlocksDelivered(in.blocks_delivered);
    out.setSamplesDelivered(in.samples_delivered);
    out.setOverrunEvents(in.overrun_events);
    out.setSamplesLost(in.samples_lost);
    out.setLastLossIndex(in.last_loss_index);
    out.setWriteIndex(in.write_index);
    out.setFrontEnd(to_schema(front_end.verdict));
    out.setFrontEndSlope(front_end.slope);
    out.setFrontEndFloorLiftDb(front_end.floor_lift_db);
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
    out.setState(to_schema(in.state));
    out.setFirstSeen(in.first_seen);
    out.setLastSeen(in.last_seen);
    out.setLastDetected(in.last_detected);
    out.setChannel(in.channel);
    out.setChannelValid(in.channel_valid);
    out.setMergedInto(in.merged_into);
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
    return out;
}

}  // namespace revenant::rpc
