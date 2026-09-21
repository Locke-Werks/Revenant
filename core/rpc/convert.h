// Engine structs to schema readers and back.
//
// Every one of these is mechanical, and that is the point of having them in
// one file: a field that is silently dropped on the way out is a field the
// UI renders as zero with nothing to say it was ever set. Reviewing one file
// against core/engine/engine.h and core/rpc/revenant.capnp is a thing a
// person can actually do; reviewing the same conversions smeared through
// eight RPC method bodies is not.
//
// TWO INVARIANTS THIS FILE EXISTS TO HOLD
//
// Frequencies stay rational. docs/conventions.md refuses integer hertz for
// a channel centre because k*rate/M is usually not a whole number, and a
// conversion layer is exactly where that rule gets quietly broken by
// somebody reaching for the double. Nothing here calls bin_width_hz() or
// divides a rational.
//
// The demodulator enum is checked, not cast on faith. The schema's ordinals
// are declared to match engine::Demod, so the conversion is a cast, and a
// static_assert per mode below is what keeps that true. Reorder either side
// without the other and the build stops here rather than retuning every
// receiver in a saved session to a different mode.

#pragma once

#include "core/decode/rds_bits.h"
#include "core/decode/rds_groups.h"
#include "core/detect/detector.h"
#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "core/rpc/revenant.capnp.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::rpc {

// The cast the conversions below rely on, made a compile error to break.
static_assert(static_cast<std::uint16_t>(schema::Demod::RAW) ==
              static_cast<std::uint16_t>(engine::Demod::Raw));
static_assert(static_cast<std::uint16_t>(schema::Demod::AM) ==
              static_cast<std::uint16_t>(engine::Demod::Am));
static_assert(static_cast<std::uint16_t>(schema::Demod::NFM) ==
              static_cast<std::uint16_t>(engine::Demod::Nfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::WFM) ==
              static_cast<std::uint16_t>(engine::Demod::Wfm));
static_assert(static_cast<std::uint16_t>(schema::Demod::USB) ==
              static_cast<std::uint16_t>(engine::Demod::Usb));
static_assert(static_cast<std::uint16_t>(schema::Demod::LSB) ==
              static_cast<std::uint16_t>(engine::Demod::Lsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::DSB) ==
              static_cast<std::uint16_t>(engine::Demod::Dsb));
static_assert(static_cast<std::uint16_t>(schema::Demod::CW) ==
              static_cast<std::uint16_t>(engine::Demod::Cw));

// The same check for the detector's track state. It matters less than the
// demodulator's, because a state is read and never written back, but it fails
// the same silent way: a renumbering makes a held track draw as live, and a
// display that stopped decaying held tracks would look right until somebody
// noticed it never removes anything.
static_assert(static_cast<std::uint16_t>(schema::TrackState::PENDING) ==
              static_cast<std::uint16_t>(detect::TrackState::Pending));
static_assert(static_cast<std::uint16_t>(schema::TrackState::LIVE) ==
              static_cast<std::uint16_t>(detect::TrackState::Live));
static_assert(static_cast<std::uint16_t>(schema::TrackState::HELD) ==
              static_cast<std::uint16_t>(detect::TrackState::Held));
static_assert(static_cast<std::uint16_t>(schema::TrackState::MERGED) ==
              static_cast<std::uint16_t>(detect::TrackState::Merged));

// And the three RDS enums, which is the assert core/rpc/types.h and
// core/rpc/client.cpp both said would land here: they hold the schema
// against the client's mirror, this file holds it against the decoder, and
// nothing else in the tree sees both ends of the second pair.
//
// The region is the one that matters, because it is the only one written IN
// rather than read out. Renumber decode::Region without renumbering the
// schema and every station on one continent is decoded with the other's PTY
// table and call sign rules, which draws and is wrong, and nothing
// downstream can tell.
static_assert(static_cast<std::uint16_t>(schema::RdsRegion::RDS) ==
              static_cast<std::uint16_t>(decode::Region::kRds));
static_assert(static_cast<std::uint16_t>(schema::RdsRegion::RBDS) ==
              static_cast<std::uint16_t>(decode::Region::kRbds));

static_assert(static_cast<std::uint16_t>(schema::RdsLock::UNLOCKED) ==
              static_cast<std::uint16_t>(decode::RdsLock::Unlocked));
static_assert(static_cast<std::uint16_t>(schema::RdsLock::ACQUIRING) ==
              static_cast<std::uint16_t>(decode::RdsLock::Acquiring));
static_assert(static_cast<std::uint16_t>(schema::RdsLock::LOCKED) ==
              static_cast<std::uint16_t>(decode::RdsLock::Locked));

static_assert(static_cast<std::uint16_t>(schema::RdsSync::HUNTING) ==
              static_cast<std::uint16_t>(decode::SyncState::kHunting));
static_assert(static_cast<std::uint16_t>(schema::RdsSync::PRE_SYNC) ==
              static_cast<std::uint16_t>(decode::SyncState::kPreSync));
static_assert(static_cast<std::uint16_t>(schema::RdsSync::SYNCED) ==
              static_cast<std::uint16_t>(decode::SyncState::kSynced));

[[nodiscard]] schema::Demod to_schema(engine::Demod mode);
[[nodiscard]] schema::TrackState to_schema(detect::TrackState state);
[[nodiscard]] schema::RdsRegion to_schema(decode::Region region);
[[nodiscard]] schema::RdsLock to_schema(decode::RdsLock lock);
[[nodiscard]] schema::RdsSync to_schema(decode::SyncState sync);

// Rejects an out-of-range ordinal rather than casting it.
//
// A Cap'n Proto enum field can legally hold a value the reader's schema has
// never heard of, which is how a newer client reaches an older engine. The
// generated C++ hands that back as an enum with no matching name, and a
// blind cast turns it into whichever mode happens to sit at that ordinal.
// Mode is the one receiver parameter where being wrong is inaudible until
// somebody notices the recording is unintelligible.
[[nodiscard]] Expected<engine::Demod> from_schema(schema::Demod mode);

// The same, for the one RDS enum a client sends rather than reads. An
// ordinal nobody here knows is refused rather than cast, because the two
// readings of the bitstream are mutually incompatible and picking the wrong
// one is silent: PTY 26 renders as National Music in one and Hip-Hop in the
// other, both draw, and nothing downstream can tell.
[[nodiscard]] Expected<decode::Region> from_schema(schema::RdsRegion region);

void write_rational(schema::Rational::Builder out, std::int64_t numerator,
                    std::int64_t denominator);

void write_device(schema::DeviceInfo::Builder out, const gpu::DeviceInfo& in);
void write_source_descriptor(schema::SourceDescriptor::Builder out,
                             const source::SourceCapabilities& in);
void write_grid(schema::GridParams::Builder out, const dsp::GridParams& in);
void write_spectrum_geometry(schema::SpectrumGeometry::Builder out,
                             const engine::SpectrumGeometry& in);
// Two engine structs and not one, because two of the wire's fields are a
// live measurement rather than what the engine settled on when it opened the
// source. Engine::info() hands back a reference that is fixed for the run;
// Engine::source_pacing() is read fresh per call. Taking both here rather
// than letting the caller set two fields afterwards keeps the rule at the
// top of this file: one place to review a wire struct against its sources.
void write_engine_info(schema::EngineInfo::Builder out, const engine::EngineInfo& in,
                       const engine::SourcePacing& pacing);

void write_source_stats(schema::SourceStats::Builder out, const source::SourceStats& in);
void write_vrx_params(schema::VrxParams::Builder out, const engine::VrxParams& in);

// The placement, plus the sentence VrxPlacement::clampReason carries.
//
// It takes the request as well because the sentence is a comparison: the
// engine struct holds what was granted and VrxParams holds what was asked
// for, and neither on its own can say by how much. Composed here rather
// than in the engine because there is nowhere in core/engine/vrx.h to put a
// string, and because it is a rendering of two facts rather than a third
// fact.
void write_vrx_placement(schema::VrxPlacement::Builder out, const engine::VrxPlacement& in,
                         const engine::VrxParams& request);

void write_vrx_status(schema::VrxStatus::Builder out, const engine::VrxStatus& in);

// Copies the whole frame, including its power_db span, so the result
// outlives the sink call. engine::SpectrumFrame documents that span as valid
// for the duration of the call and not after, which for a wire format means
// the copy is not an inefficiency to be optimised away later.
void write_spectrum_frame(schema::SpectrumFrame::Builder out,
                          const engine::SpectrumFrame& in);

// The same for one receiver's passband, whose span carries the same lifetime
// rule and therefore the same copy.
void write_passband_geometry(schema::PassbandGeometry::Builder out,
                             const engine::PassbandGeometry& in);
void write_passband_frame(schema::PassbandFrame::Builder out,
                          const engine::PassbandFrame& in);

// One track, as the wire carries it. Deliberately lossy: see the note on
// schema::Detection for which fields of detect::Track are left behind and
// why.
//
// There is deliberately no write_detection_list beside it, and the absence
// is stated because the rule at the top of this file would otherwise say
// there should be. DetectionList's other five fields are not a conversion of
// any one engine struct: they come off Detector::stats(), Detector::config()
// and the filtering the server does per caller, and the struct holding them
// is local to core/rpc/server.cpp. A writer here would take those five as
// loose scalars, which is the mechanical-copy mistake this file exists to
// make reviewable, not an instance of it. The one place they are written is
// ServerImpl::detections, next to the code that reads them.
void write_detection(schema::Detection::Builder out, const detect::Track& in);

// One station's accumulated group-layer state.
//
// LOSSY IN ONE DIRECTION AND INCOMPLETE IN THE OTHER, and both halves of
// that have to be said or the rule at the top of this file is broken twice.
//
// Lossy: it leaves behind the decoder's own assembly state, the partially
// built AF pair, the pending LF/MF flag and the leaky-bucket error credit,
// for the reason schema::RdsStation gives. A schema is a contract rather
// than a mirror.
//
// Incomplete: six fields of schema::RdsStation are NOT written here, because
// they are not fields of decode::StationState and never will be. vrx,
// region, compositeRate, taChangedAt and lastGroupSample belong to the
// server's own bookkeeping, and health draws on two objects rather than one
// (RdsBitsStatus for the physical half and RdsDecoder's counters for the
// block half). They are written in core/rpc/server.cpp beside the code that
// keeps them, which is the same split ServerImpl::detections makes for
// DetectionList's five non-track fields and for the same reason: taking
// them here as loose scalars would be the mechanical-copy mistake this file
// exists to make reviewable rather than an instance of it.
//
// region is a parameter and not a field of the state because the PTY names
// and the call sign derivation are region dependent and StationState carries
// neither of them. Passing the decoder's own region is the caller's job, and
// passing a different one produces a struct whose names disagree with the
// bytes they came from.
void write_rds_station(schema::RdsStation::Builder out, const decode::StationState& in,
                       decode::Region region);

// The physical half of RdsHealth. The block half is nine counters off
// RdsDecoder and is written by the caller: see the note above.
void write_rds_bits_status(schema::RdsHealth::Builder out, const decode::RdsBitsStatus& in);

[[nodiscard]] Expected<engine::VrxParams> read_vrx_params(schema::VrxParams::Reader in);

// There is deliberately no read_spectrum_geometry here.
//
// One existed, nothing ever called it, and it could not be called by the one
// reader that needs the conversion: client.cpp is forbidden from including
// this header, because it compiles a second time against the dynamic CRT
// where revenant_core is not linked. So the live reader is client.cpp's own,
// over core/rpc/types.h.
//
// Two readers is worse than the duplication looks. This one guarded a zero
// denominator by substituting 1, and the live one copies it through on
// purpose, because types.h::hertz() already decides what a zero denominator
// means and a second policy here would make one wire value mean two
// different frequencies depending on which layer read it. Deleted rather
// than left dead, since dead code with a conflicting policy is a trap for
// whoever wires it up.

}  // namespace revenant::rpc
