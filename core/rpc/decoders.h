// The decoder seam: one interface every event decoder is adapted to, and the
// registry the server attaches them from by name.
//
// WHAT A DECODER IS HERE
//
// Something that eats one receiver's stream a chunk at a time and hands back
// DecodedMessages, the generic event core/rpc/types.h defines and the schema
// carries. The libraries in core/decode are pure functions of a span of
// samples and know nothing of receivers, chunks or the wire; an adapter below
// is the few dozen lines between one of them and this interface, and it is
// the only thing a new decoder needs in order to be served. The next one lands
// as one more adapter and one more registry row, naming the modes whose output
// it reads, with nothing in the schema, the client or the server changing for
// it; PSK31, CW and M17 were the first three to arrive that way.
//
// WHAT THIS PARAGRAPH USED TO SAY, until the audio-domain decoders arrived on
// 2026-09-22: that RTTY, APRS and POCSAG would each land "with nothing in the
// schema, the client or the server moving". The schema and the client did not
// move. The server did, once: an audio decoder reads one kind of receiver and
// not another, RTTY a sideband and AX.25 an FM discriminator, and before
// DecoderSpec::modes the server could only tell complex from audio, so RTTY on
// a WFM receiver was accepted and decoded broadcast audio as teleprinter
// noise. subscribeDecoded now refuses that in words naming the modes. A
// decoder added after this is one adapter and one row again.
//
// RDS IS NOT ON THIS SEAM and does not need to be. It is polled as a
// station's accumulated state, which is a different shape from a stream of
// events; core/rpc/revenant.capnp has the argument beside DecodedField.
//
// WHY THIS IS A HEADER AND NOT A .cpp IN revenant_rpc_server
//
// Two programs run these adapters and only one of them links the wire.
// core/rpc/server.cpp attaches them to a receiver a client asked about, and
// tools/cli/main.cpp attaches them to the receivers on its own command line,
// in the same process as the engine, linking revenant_core and nothing of
// core/rpc's. A .cpp here would have to be compiled into one of the two
// libraries and would drag capnp into the CLI or the adapters into revenant_core.
// Inline in a header, both reach the same code with neither link changing,
// which is the arrangement core/engine/engine.h already uses for its fan-out.
//
// It includes core/rpc/types.h, which depends on nothing, and core/decode,
// which is in revenant_core. It must never include the generated schema:
// the CLI has no capnp.
//
// TWO INPUTS, ONE MECHANISM
//
// A complex decoder reads a complex tap: interleaved I and Q, two floats a
// sample. An audio decoder reads what a demodulator made of the channel: one
// float a sample, mono, at the receiver's audio rate. Both arrive through the
// same attach_audio_sink the RDS decoder is fed by, handed to a decode lane
// (core/rpc/decode_lane.h), behind the same retune fence, because to the
// engine a complex tap and a
// demodulator are both a receiver's output and differ only in the channel
// count. DecoderSpec::input says which a decoder reads and DecoderSpec::modes
// says which demodulators produce it.
//
// WHAT THE ADAPTERS DO NOT DO
//
// They do not mix, filter to a channel or resample. They take the complex
// baseband or the audio a receiver delivers at whatever rate the chunk says,
// and build the decoder at that rate on the first chunk. An audio receiver's
// rate is VrxStatus::resolved_audio_rate, 48000 unless the receiver asked for
// another, and every audio decoder below was written and measured at 48000.
// Two complex paths reach them, and the rate is the chunk's on both rather
// than anything VrxStatus says:
//
//   A p25p1, dstar, tetra or dmr receiver goes through the fine stage, mixed
//   to DC, filtered to the mode's channel and resampled to 48000, 48000,
//   72000 or 48000, which is the rate each decoder was written at.
//   VrxStatus::demod_rate states it for these four.
//
//   A raw receiver is RawTapStage: one coarse grid channel at the channel
//   rate, with the carrier wherever the grid left it. VrxStatus::demod_rate
//   is the planner's figure there and is NOT the rate the tap delivers;
//   core/engine/vrx.h says so. A decoder attached to one runs at the channel
//   rate, no faster than decoders_detail::kRawTapRateCap, and hears the
//   residual as a carrier offset. The P25 path removes
//   it per data unit, from a least-squares fit of that unit's sync word, and
//   the D-STAR path per transmission, from a fit of its frame sync refreshed
//   on every resynchronisation signal, so neither answer depends on how the
//   stream is blocked. core/decode/tetra.cpp has no offset correction at all.
//
//   WHAT THIS USED TO SAY: "The D-STAR path still removes a constant offset
//   as each call's mean of the discriminator". True until "Decode D-STAR the
//   same however its input is blocked".

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/decode/ais.h"
#include "core/decode/aprs.h"
#include "core/decode/ax25.h"
#include "core/decode/cw.h"
#include "core/decode/dmr.h"
#include "core/decode/dsc.h"
#include "core/decode/dstar.h"
#include "core/decode/m17.h"
#include "core/decode/navtex.h"
#include "core/decode/p25p1.h"
#include "core/decode/pocsag.h"
#include "core/decode/psk31.h"
#include "core/decode/rtty.h"
#include "core/decode/sitor_b.h"
#include "core/decode/tetra.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/rpc/types.h"

namespace revenant::rpc {

// One delivery from a receiver, in the terms an adapter needs and no more.
//
// Not engine::AudioChunk, so that this header does not pull the engine into a
// translation unit that only wants a decoder, and because the three fields
// the engine carries beyond these, the receiver id, the squelch gate and the
// tuning epoch, are the caller's business: the server fences on the epoch and
// stamps the id, and neither is a decoder's question.
struct DecoderChunk {
    // Interleaved, `channels` floats per frame. A complex tap is two, I then
    // Q, which core/engine/engine.h's AudioChunk says is how the graph hands
    // one out.
    std::span<const float> samples;
    std::uint32_t channels = 1;
    dsp::SampleRate rate = 0;

    // The receiver-stream index of the first frame, AudioChunk::start.
    dsp::SampleIndex start = 0;

    [[nodiscard]] std::uint64_t frames() const {
        return channels == 0 ? 0 : samples.size() / channels;
    }
};

// What every decoder on this seam is.
//
// consume appends every message the chunk completed. A refusal is terminal:
// the caller stops feeding this decoder and says why, because the only things
// an adapter refuses are shape, a channel count or a rate it was not built for,
// and core/engine refuses a retune that changes either. reset discards
// everything accumulated about the transmitter, for a retune.
//
// flush is for the end of the stream, when the receiver goes or the run
// finishes: it appends what a decoder holding a message open has recovered of
// it, rather than let it go with the decoder. Every adapter below that holds
// something takes it: D-STAR a transmission handed over one superframe at a
// time, POCSAG a page whose closing codeword the stream cut short, NAVTEX a
// message still waiting for its "NNNN", CW the character being keyed, and
// RTTY, SITOR-B, the PSK modes and CW the line they were gathering. P25,
// TETRA, DMR, AX.25 and M17
// hold nothing a client could read, a partial frame or burst, and take the
// default here, which appends nothing.
//
// WHAT THIS PARAGRAPH USED TO SAY after the D-STAR sentence: "POCSAG, CW and
// NAVTEX have a flush of their own in core/decode that their adapters do not
// call yet, so they take the default here, which appends nothing."
class ChunkDecoder {
public:
    ChunkDecoder() = default;
    virtual ~ChunkDecoder() = default;

    ChunkDecoder(const ChunkDecoder&) = delete;
    ChunkDecoder& operator=(const ChunkDecoder&) = delete;
    ChunkDecoder(ChunkDecoder&&) = delete;
    ChunkDecoder& operator=(ChunkDecoder&&) = delete;

    [[nodiscard]] virtual Status consume(const DecoderChunk& chunk,
                                         std::vector<DecodedMessage>& out) = 0;
    virtual void flush(std::vector<DecodedMessage>&) {}
    virtual void reset() = 0;
};

// What an adapter is built for, known on the first chunk.
//
// mode is the receiver's demodulator in engine::demod_name's spelling, which
// is the one spelling core/engine/vrx_place.cpp parses back. A string rather
// than engine::Demod so that this header stays clear of the engine, for the
// reason DecoderChunk gives. Fixed for the life of the receiver:
// Graph::set_vrx_params refuses a change of demodulator, so a decoder built for
// one never meets another.
//
// Only the sideband decoders read it today, because the sideband is what
// decides which audio tone the higher radio frequency lands on.
struct DecoderBuild {
    dsp::SampleRate rate = 0;
    std::string_view mode;
};

using DecoderFactory = Expected<std::unique_ptr<ChunkDecoder>> (*)(const DecoderBuild& build);

struct DecoderSpec {
    std::string_view name;
    DecoderInput input = DecoderInput::ComplexBaseband;
    std::string_view description;
    DecoderFactory make = nullptr;

    // The demodulators whose output this decoder reads, by demod_name. Empty
    // means any receiver whose output is `input`, and no row uses it today:
    // every complex decoder names its own mode and raw, and a raw tap is held
    // to decoders_detail::kRawTapRateCap.
    //
    // WHAT THIS PARAGRAPH USED TO SAY after "is `input`": "which is how the
    // three complex decoders have always been attached: p25p1 on a raw tap is
    // a choice an operator can make on purpose." It still is, below the cap.
    //
    // ON THE WIRE AS DecoderInfo::modes since 2026-09-23, written out whole by
    // core/rpc/convert.cpp so empty never crosses as "any". The receiver
    // window offers an operator the decoders a receiver can feed, and filling
    // that menu from the descriptions would have meant parsing prose.
    //
    // WHAT THIS PARAGRAPH USED TO SAY: "NOT ON THE WIRE AS A LIST. DecoderInfo
    // has no field for it and adding one is a schema change for something the
    // description already says in words". The descriptions still say it, for a
    // person reading the list.
    std::span<const std::string_view> modes;
};

// Whether `spec` reads the output of a receiver in `mode`: listed, or nothing
// listed. The input check against a complex tap is the caller's, since this
// header knows modes by name and not which of them are complex.
[[nodiscard]] inline bool decoder_accepts(const DecoderSpec& spec, std::string_view mode) {
    if (spec.modes.empty()) {
        return true;
    }
    return std::ranges::find(spec.modes, mode) != spec.modes.end();
}

// The modes as a refusal says them: "usb or lsb", "nfm".
[[nodiscard]] inline std::string decoder_modes_text(const DecoderSpec& spec) {
    std::string out;
    for (std::size_t i = 0; i < spec.modes.size(); ++i) {
        if (i > 0) {
            out += (i + 1 == spec.modes.size()) ? " or " : ", ";
        }
        out += spec.modes[i];
    }
    return out;
}

// What a refusal says the decoder reads: "the audio of a usb or lsb receiver",
// "the complex baseband of a p25p1 or raw receiver".
[[nodiscard]] inline std::string decoder_needs_text(const DecoderSpec& spec) {
    return std::format("the {} of a {} receiver",
                       spec.input == DecoderInput::RealAudio ? "audio" : "complex baseband",
                       decoder_modes_text(spec));
}

namespace decoders_detail {

[[nodiscard]] inline DecodedField integer_field(std::string key, std::int64_t value) {
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<0>, value}};
}
[[nodiscard]] inline DecodedField real_field(std::string key, double value) {
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<1>, value}};
}
[[nodiscard]] inline DecodedField flag_field(std::string key, bool value) {
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<2>, value}};
}
[[nodiscard]] inline DecodedField text_field(std::string key, std::string value) {
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<3>, std::move(value)}};
}
[[nodiscard]] inline DecodedField bytes_field(std::string key, std::vector<std::uint8_t> value) {
    return DecodedField{std::move(key), DecodedValue{std::in_place_index<4>, std::move(value)}};
}

// A new message stamped with where in the stream it completed. See
// DecodedMessage::start_sample for why the span is the chunk's and not the
// message's own.
[[nodiscard]] inline DecodedMessage stamped(std::string_view decoder, std::string kind,
                                            const DecoderChunk& chunk) {
    DecodedMessage out;
    out.decoder = std::string(decoder);
    out.kind = std::move(kind);
    out.start_sample = chunk.start;
    out.end_sample = chunk.start + chunk.frames();
    out.sample_rate = static_cast<std::uint32_t>(chunk.rate);
    return out;
}

// The receive filter's tap count at `rate`, holding the span in time the
// decoder's own default gives at its design rate.
//
// AN ENGINEERING CHOICE AND NOT A CITATION. Each decoder in core/decode sizes
// its filter for one rate, 48000 or 72000, and states the span that buys:
// 121 taps at 48 kHz is twelve P25 symbol periods. The count is fixed, so the
// same 121 on a raw tap at 144000 S/s spans four, and a filter that short
// cannot hold the response the clause specifies. Scaling the count with the
// rate keeps the span. Rounded up to odd, because every design routine in
// core/decode/dv_phy.h wants a whole-sample group delay, and never under
// seven. At the design rate it is the identity, which is the fine stage's
// case for all three modes.
//
// WHAT IT COSTS. The filter is a direct-form FIR per sample, so work grows
// with the square of the rate: the P25 filter is 121 multiplies a sample at
// 48000, 5.8 million a second, and 363 at 144000, 52.3 million a second, on
// the server's decode lane. A raw tap at a coarse channel's full rate
// is the expensive case, and kRawTapRateCap below is where it stops being
// allowed; a p25p1 receiver's fine stage delivers the design rate.
[[nodiscard]] inline std::size_t scaled_taps(std::size_t design_taps,
                                             dsp::SampleRate design_rate,
                                             dsp::SampleRate rate) {
    const double scaled = std::ceil(static_cast<double>(design_taps) *
                                    static_cast<double>(rate) /
                                    static_cast<double>(design_rate));
    auto taps = static_cast<std::size_t>(std::max(scaled, 7.0));
    if (taps % 2 == 0) {
        ++taps;
    }
    return taps;
}

// The shape check every complex adapter makes, in words naming both halves.
[[nodiscard]] inline Status require_complex(std::string_view decoder, const DecoderChunk& chunk,
                                            dsp::SampleRate built_for) {
    if (chunk.channels != 2) {
        return fail(std::format(
            "the {} decoder reads complex baseband, two floats a sample, and the receiver "
            "delivered {} channel{}. That is audio, so this receiver's mode is not a complex tap",
            decoder, chunk.channels, chunk.channels == 1 ? "" : "s"));
    }
    if (chunk.rate != built_for) {
        return fail(std::format(
            "the {} decoder was built for {} S/s and the receiver delivered {}. Its filter and "
            "its symbol timing are sized by the rate, so it stopped rather than decode at the "
            "wrong one",
            decoder, built_for, chunk.rate));
    }
    return {};
}

// De-interleaves into a buffer the adapter keeps, so the steady state does not
// allocate. A copy rather than a cast over the floats: the standard makes a
// complex<float> viewable as two floats and says nothing about the reverse.
inline void to_complex(const DecoderChunk& chunk, std::vector<dsp::Complex32>& out) {
    const std::size_t frames = chunk.samples.size() / 2;
    out.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i] = dsp::Complex32(chunk.samples[2 * i], chunk.samples[2 * i + 1]);
    }
}

// The modes the audio decoders read, by demod_name.
//
// A sideband receiver for the HF tone modes, because on SSB the RF shift
// lands in the audio passband as a pair of tones, which is what
// decode::ToneDiscriminator reads. AM and DSB would pass the tones too, and
// are left out because they fold both sidebands onto one audio spectrum and a
// receiver a few hertz off the carrier beats the two images against each
// other; nothing here has been measured through them.
//
// The FM discriminator alone for AFSK and POCSAG. AFSK is audio tones on an FM
// voice channel and POCSAG is direct FSK whose shift the discriminator turns
// into a level. WFM is left out: a broadcast receiver is 200 kHz wide for a
// signal about 16 kHz wide, it de-emphasises by default, and nothing here has
// been measured through one.
inline constexpr std::string_view kSidebandModes[] = {"usb", "lsb"};
inline constexpr std::string_view kFmModes[] = {"nfm"};

// CW reads a cw receiver, whose demodulator puts the carrier at its pitch, or
// a sideband receiver the operator has tuned to put the tone there.
inline constexpr std::string_view kCwModes[] = {"cw", "usb", "lsb"};

// M17 is complex baseband like P25 and has no demodulator of its own; see its
// adapter for why these two and not dstar or tetra.
inline constexpr std::string_view kM17Modes[] = {"p25p1", "raw"};

// The four digital voice decoders read their own mode's fine stage, which
// delivers the rate each was written at, or a raw tap at no more than
// kRawTapRateCap. DMR is the fourth, since 2026-09-23.
//
// WHAT THIS USED TO BE: no list, which reads as any complex tap, so p25p1
// was offered on a dstar or tetra receiver's tap as well as its own, and on a
// raw tap at whatever rate the grid gave it.
inline constexpr std::string_view kP25Modes[] = {"p25p1", "raw"};
inline constexpr std::string_view kDStarModes[] = {"dstar", "raw"};
inline constexpr std::string_view kTetraModes[] = {"tetra", "raw"};
inline constexpr std::string_view kDmrModes[] = {"dmr", "raw"};

// THE FASTEST RAW TAP A COMPLEX DECODER WILL READ.
//
// A raw tap is one coarse channel at the grid's channel rate, which the
// source and the grid decide and nothing here caps. The receive filters in
// core/decode are sized at 48000 or 72000 S/s and scaled_taps grows them with
// the rate to hold their span, so a decoder's work grows with the square of
// the rate, and it is done on a decode lane the server shares between
// receivers. A tap fast enough to leave that lane behind makes it drop
// chunks for every decoder pinned to it, not only this one.
//
// WHAT THIS PARAGRAPH USED TO SAY: "it is done on the engine's completion
// thread, which delivers every receiver's output. A tap fast enough to stall
// that thread makes the source drop samples for every receiver". Decoders
// left the completion thread on 2026-09-23, core/rpc/decode_lane.h has why,
// so a decoder that cannot keep up now costs its lane's decoders chunks
// rather than costing the radio samples. The cap stands on that narrower
// ground.
//
// Measured on 2026-09-23 on the RTX 4090 machine, one core, milliseconds per
// second of a raw tap of noise fed a twentieth of a second at a time:
//
//   rate       p25p1   m17    dstar  tetra
//   48000        5.6    5.7    3.9    4.3
//   144000      38.8   30.2   23.2   17.4
//   192000      66.2   54.3   39.0   29.4
//   288000     145.8  116.4   81.3   57.1
//   600000     601.6  488.0  324.8  249.1
//
// AN ENGINEERING CHOICE AND NOT A CITATION: 192000 is four times the 48000
// that P25, D-STAR and M17 were written at, and there the costliest of the
// four takes under a fifteenth of a core. Above it a raw tap is refused in
// words naming the mode that feeds the decoder at its own rate. DMR, added
// the same day and not in the table, took 76.9 ms at 192000 in a run where
// P25 took 64.4, which is under a twelfth.
inline constexpr dsp::SampleRate kRawTapRateCap = 192'000;

// The mode and rate that feed `decoder` without a raw tap, for the refusal.
struct OwnTap {
    std::string_view mode;
    dsp::SampleRate rate = 0;
};

[[nodiscard]] inline OwnTap own_tap(std::string_view decoder) {
    if (decoder == "tetra") {
        return {"tetra", 72'000};
    }
    if (decoder == "dstar") {
        return {"dstar", 48'000};
    }
    if (decoder == "dmr") {
        return {"dmr", 48'000};
    }
    // p25p1 and m17, which reads a p25p1 receiver's fine stage.
    return {"p25p1", 48'000};
}

// Whether a complex decoder may read a receiver in `mode` delivering `rate`.
// Only a raw tap is ever refused here; a mode's own fine stage runs at the
// rate its decoder was written at.
[[nodiscard]] inline Status raw_tap_allowed(std::string_view decoder, std::string_view mode,
                                            dsp::SampleRate rate) {
    if (mode != "raw" || rate <= kRawTapRateCap) {
        return {};
    }
    const OwnTap own = own_tap(decoder);
    return fail(std::format(
        "the {} decoder reads a raw tap at no more than {} S/s, and this one runs at {}, the "
        "grid's channel rate. Its receive filter grows with the rate and runs on a decode "
        "thread shared with other receivers' decoders, so a tap this fast would leave them all "
        "behind. Add a receiver in {} on the signal instead: its fine stage mixes the "
        "carrier to DC and resamples to the {} S/s this decoder was written for",
        decoder, kRawTapRateCap, rate, own.mode, own.rate));
}

// The shape check every audio adapter makes, in words naming both halves.
[[nodiscard]] inline Status require_audio(std::string_view decoder, const DecoderChunk& chunk,
                                          dsp::SampleRate built_for) {
    if (chunk.channels != 1) {
        return fail(std::format(
            "the {} decoder reads mono audio, one float a sample, and the receiver delivered {} "
            "interleaved channels. That is a complex tap or stereo audio, and reading a pair as "
            "consecutive samples decodes a signal that does not exist, so this decoder stopped",
            decoder, chunk.channels));
    }
    if (chunk.rate != built_for) {
        return fail(std::format(
            "the {} decoder was built for {} S/s of audio and the receiver delivered {}. Its "
            "filters and its bit timing are sized by the rate, so it stopped rather than decode "
            "at the wrong one",
            decoder, built_for, chunk.rate));
    }
    return {};
}

inline void append_utf8(std::string& out, char32_t c) {
    const auto v = static_cast<std::uint32_t>(c);
    if (v < 0x80U) {
        out.push_back(static_cast<char>(v));
    } else if (v < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (v >> 6U)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    } else if (v < 0x10000U) {
        out.push_back(static_cast<char>(0xE0U | (v >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((v >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xF0U | (v >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((v >> 12U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | ((v >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    }
}

// Where the decoder's own sample count starts in the receiver's stream.
//
// The libraries in core/decode count samples from the first one they were
// handed, and a message's own position is in that count. The receiver's
// stream index of the first chunk after a build or a reset is what turns one
// into the other. Exact as long as every chunk after it reaches the decoder,
// which the completion thread's in-order delivery and a decode lane's
// first-in first-out run give; a chunk the graph or a full lane dropped would
// shift every later position by its length, and each reports a drop through
// its own counter rather than here.
class StreamBase {
public:
    void observe(const DecoderChunk& chunk) {
        if (!base_.has_value()) {
            base_ = chunk.start;
        }
    }
    [[nodiscard]] std::uint64_t at(std::uint64_t decoder_index) const {
        return base_.value_or(0) + decoder_index;
    }
    void reset() { base_.reset(); }

private:
    std::optional<std::uint64_t> base_;
};

// Where the stream stood when it ended, for what flush hands over.
//
// Nothing completed a message a flush recovers, so no chunk's span is its
// own. It is stamped where the last chunk ended, with no length, as the
// D-STAR adapter stamps a flushed superframe: the message was still open at
// that sample, which is the one position the stream can vouch for.
class StreamEnd {
public:
    void observe(const DecoderChunk& chunk) {
        channels_ = chunk.channels;
        rate_ = chunk.rate;
        end_ = chunk.start + chunk.frames();
    }
    [[nodiscard]] DecoderChunk at_end() const {
        return DecoderChunk{.samples = {}, .channels = channels_, .rate = rate_, .start = end_};
    }

private:
    std::uint32_t channels_ = 1;
    dsp::SampleRate rate_ = 0;
    dsp::SampleIndex end_ = 0;
};

// Characters gathered into lines, for the two start-stop text modes.
//
// A MESSAGE PER CHARACTER WOULD FILL THE QUEUE, which is why this exists.
// RTTY runs at six characters a second and a subscription holds 256 messages,
// so a client that stalled for 42 seconds would lose text, and a person reads
// a line rather than a character. A line ends at a carriage return or line
// feed, at kMaxLineCharacters, when the transmitter goes quiet for
// kIdleCharacters character times, when the decoder has a reason to think
// what follows is another transmission, or when the stream ends and
// ChunkDecoder::flush hands over the line in progress.
//
// Both limits are engineering choices and not citations: 80 is a terminal's
// width, and ten character times is 1.65 s of RTTY and 1.4 s of SITOR-B, long
// enough that a typist pausing mid-line does not split it and short enough
// that the last line of a transmission does not wait for the next one.
inline constexpr std::size_t kMaxLineCharacters = 80;
inline constexpr double kIdleCharacters = 10.0;

struct TextLine {
    std::string text;
    std::uint64_t first_sample = 0;
    std::uint64_t last_sample = 0;
    std::size_t characters = 0;
    bool visible = false;

    // The end of the chunk that delivered the latest character, and whether
    // one has been added since that was last set.
    std::uint64_t arrived = 0;
    bool fresh = false;

    void add(char32_t glyph, std::uint64_t at) {
        begin(at);
        append_utf8(text, glyph);
        visible = visible || glyph != U' ';
    }
    // A token already in UTF-8, which is how CW hands out a character: a
    // service signal is a word in angle brackets and E-acute is two bytes.
    void add_text(std::string_view token, std::uint64_t at) {
        begin(at);
        text += token;
        visible = visible || token.find_first_not_of(' ') != std::string_view::npos;
    }

    // Whether the channel has been quiet for `idle` samples by the end of a
    // chunk, with that chunk's characters already added.
    //
    // FROM WHEN THE LAST CHARACTER ARRIVED, NOT FROM WHERE IT SITS. A decoder
    // hands a character over some time after its position: RTTY after its
    // stop element, PSK31 and CW after the two seconds they spend finding the
    // tone, which they then release in one batch. Measured from the
    // positions, that batch ended the line the moment it arrived; through
    // the engine on 2026-09-22, CW split "CQ CQ DE N0CALL K" after "N0".
    // Between two characters in one delivery the positions are compared
    // instead, by the adapters, because there the latency cancels.
    [[nodiscard]] bool quiet_at(std::uint64_t chunk_end, std::uint64_t idle) {
        if (fresh) {
            arrived = chunk_end;
            fresh = false;
        }
        return characters > 0 && chunk_end > arrived + idle;
    }

    void clear() { *this = TextLine{}; }

private:
    void begin(std::uint64_t at) {
        if (characters == 0) {
            first_sample = at;
        }
        last_sample = at;
        ++characters;
        fresh = true;
    }
};

// Why a line was handed out, as the "ended" field spells it.
enum class LineEnd : std::uint8_t { LineEnd, Length, Idle, NewTransmission, StreamEnd };

[[nodiscard]] inline std::string_view line_end_name(LineEnd why) {
    switch (why) {
        case LineEnd::LineEnd: return "line_end";
        case LineEnd::Length: return "length";
        case LineEnd::Idle: return "idle";
        case LineEnd::NewTransmission: return "new_transmission";
        case LineEnd::StreamEnd: return "stream_end";
    }
    return "unknown";
}

}  // namespace decoders_detail

// ---------------------------------------------------------------------------
// P25 Phase 1
// ---------------------------------------------------------------------------
//
// One message per data unit whose frame sync and Network Identifier were
// recovered, core/decode/p25p1.h's P25Frame. kind is p25_duid_name's answer,
// "hdu", "ldu1", "ldu2", "tdu", "tdulc", "pdu" or "reserved".
//
// Every message:
//   nac                  int    TIA-102.BAAA-A clause 8.5, 12 bits
//   duid                 int    Table 8-4, 4 bits
//   nid_corrected_bits   int    bits the BCH decode changed; above 11 the NID
//                               is the nearest code word, not the sent one
//   sync_score           real   normalised correlation of the frame sync
//   inverted             flag   the discriminator polarity was reversed
//   carrier_offset_hz    real   the carrier's offset from DC as the
//                               discriminator saw it, from the least-squares
//                               fit of this unit's 24 sync symbols
//   deviation_ratio      real   the deviation against Table 9-1's nominal
//                               from the same fit
//
// The last two are what the decoder sliced this unit with, so a client can
// see a transmitter drifting off frequency or deviating wide before the
// headers stop decoding. The deviation reads under 1.0 for a transmitter on
// specification, because the outer symbols land a little inside +/-3 after
// the filters: tests/decode/test_p25p1_blocking.cpp measured 0.919 to 0.974
// without the engine, and the six-header capture in
// tests/rpc/test_rpc_decode.cpp read 0.8478 to 0.9671, with the carrier
// within 6.62 Hz of DC, over 12 data units through the engine on 2026-09-23.
//
// A Header Data Unit adds, from clause 10.2:
//   talkgroup            int    TGID
//   algorithm_id         int    ALGID, 0x80 unencrypted per TIA-102.BAAC 2.8
//   key_id               int    KID
//   manufacturer_id      int    MFID
//   encrypted            flag   ALGID is not 0x80. Reported, never decrypted,
//                               per docs/modes.md
//   message_indicator    bytes  the 72-bit MI, most significant first
//   golay_worst_correction int  worst correction across the 36 Golay words
//
// The Link Control word in LDU1 is not decoded by core/decode/p25p1.h, so a
// receiver joining mid-call sees the NAC and the DUIDs and no talkgroup until
// the next header. p25p1.h has why.
class P25p1Decoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "p25p1";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        if (auto allowed = decoders_detail::raw_tap_allowed(kName, build.mode, build.rate);
            !allowed) {
            return std::unexpected(allowed.error());
        }
        const dsp::SampleRate rate = build.rate;
        decode::P25Config config;
        config.filter_taps =
            decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
        config.rate = rate;
        auto built = decode::P25Phase1::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the p25p1 decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new P25p1Decoder(rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_complex(kName, chunk, rate_); !shape) {
            return shape;
        }
        decoders_detail::to_complex(chunk, iq_);
        frames_.clear();
        if (auto processed = decoder_.process(iq_, frames_); !processed) {
            return processed;
        }

        for (const decode::P25Frame& frame : frames_) {
            using namespace decoders_detail;
            DecodedMessage message =
                stamped(kName, std::string(decode::p25_duid_name(frame.nid.duid)), chunk);
            message.fields.push_back(integer_field("nac", frame.nid.network_access_code));
            message.fields.push_back(integer_field("duid", frame.nid.duid));
            message.fields.push_back(
                integer_field("nid_corrected_bits", frame.nid.corrected_bits));
            message.fields.push_back(real_field("sync_score", frame.sync_score));
            message.fields.push_back(flag_field("inverted", frame.inverted));
            message.fields.push_back(real_field("carrier_offset_hz", frame.carrier_offset_hz));
            message.fields.push_back(real_field("deviation_ratio", frame.deviation_ratio));

            message.text = std::format("NAC 0x{:03X} {}", frame.nid.network_access_code,
                                       message.kind);
            if (frame.header.has_value()) {
                const decode::P25Header& header = *frame.header;
                message.fields.push_back(integer_field("talkgroup", header.talkgroup_id));
                message.fields.push_back(integer_field("algorithm_id", header.algorithm_id));
                message.fields.push_back(integer_field("key_id", header.key_id));
                message.fields.push_back(integer_field("manufacturer_id", header.manufacturer_id));
                message.fields.push_back(flag_field("encrypted", header.encrypted));
                message.fields.push_back(bytes_field(
                    "message_indicator", std::vector<std::uint8_t>(
                                             header.message_indicator.begin(),
                                             header.message_indicator.end())));
                message.fields.push_back(
                    integer_field("golay_worst_correction", header.worst_golay_correction));
                message.text += std::format(" TG {} ALGID 0x{:02X} KID 0x{:04X} {}",
                                            header.talkgroup_id, header.algorithm_id,
                                            header.key_id,
                                            header.encrypted ? "encrypted" : "clear");
            }
            out.push_back(std::move(message));
        }
        return {};
    }

    void reset() override { decoder_.reset(); }

private:
    P25p1Decoder(dsp::SampleRate rate, decode::P25Phase1 decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    dsp::SampleRate rate_;
    decode::P25Phase1 decoder_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::P25Frame> frames_;
};

// ---------------------------------------------------------------------------
// D-STAR DV
// ---------------------------------------------------------------------------
//
// One message per superframe, the pieces core/decode/dstar.h's DStar hands a
// transmission over in: the 21 voice frames from one clause 4.1.2 c
// resynchronisation signal to the next, or fewer when the transmission closed
// sooner. kind is "header" for the first piece, which carries the radio
// header, and "superframe" for every piece after it.
//
// Every message:
//   superframe           int    0 for the header's piece, counting up by one
//   voice_frames         int    voice frames in this piece, 21 unless the
//                               transmission closed inside it
//   voice_frames_total   int    voice frames in the transmission so far
//   ended                flag   the clause 4.1.2 h last frame closed it here
//   flushed              flag   the stream ended with the transmission still
//                               open, and this is what it had recovered
//   sync_score           real   the first frame sync's
//   inverted             flag
//   carrier_offset_hz    real   the carrier's offset from DC as the
//                               discriminator saw it, from the least-squares
//                               fit of the first frame sync's 15 bits
//   deviation_ratio      real   the deviation against a quarter of the bit
//                               rate from the same fit
//
// Every superframe carries the first frame sync's two figures, as
// core/decode/dstar.h hands them over. The decoder refits on each
// resynchronisation signal to slice with, and does not report the refit.
//
// BOTH READ LOW FOR A TRANSMITTER ON NOMINAL, so read them as a trend rather
// than against 0 Hz and 1.0. At BT 0.5 a lone bit falls short of full
// deviation and the frame sync is mostly lone bits, so the straight-line fit
// reads a smaller gain and moves the offset with it: without the engine,
// tests/decode/test_dstar_blocking.cpp measured 0.646 to 0.693 of nominal
// and 40 to 80 Hz under the carrier, and the fifty-frame transmission in
// tests/rpc/test_rpc_decode.cpp read 0.6621 and -46.77 Hz through the
// engine on 2026-09-23.
//
// A transmission is closed by the piece with `ended` or `flushed`, or by one
// with fewer than 21 voice frames: that is a resynchronisation signal missing
// where clause 4.1.2 c puts one, the receiver losing the carrier. A
// transmission lost exactly on a superframe boundary sends nothing more, since
// the decoder has no frames to hand over; the next header is the only sign.
//
// The header adds, as text because JARL Ver 7.0 clause 4.1.1 f through j
// makes them ASCII, trailing padding removed by the decoder:
//   my                   text   own callsign 1 (自局コールサイン1)
//   my_suffix            text   own callsign 2, four characters
//   ur                   text   companion (相手局コールサイン)
//   rpt1                 text   departure repeater (送り元レピータ局)
//   rpt2                 text   destination repeater (送り先レピータ局)
//   flag1, flag2, flag3  int    the three flag bytes as received
//   data, via_repeater, interrupted, control, emergency
//                        flag   clause 4.1.1 c, flag 1 bits 7 to 3
//   response             int    flag 1 bits 2 to 0
//   fcs_valid            flag   true on every header this decoder reports
//
// A superframe repeats my and ur from its header, so a client that joined
// after the header, or dropped it from a full queue, can still say whose it is.
//
// WHAT THIS LIST USED TO SAY. fcs_valid read "the P_FCS checked. A header that
// failed it is still reported, so the callsigns may be wrong". It never was:
// core/decode/dstar.cpp refuses a header whose P_FCS does not check and goes
// on searching, and did from the commit that added it. The field stays on the
// wire and is always true. voice_frames read "voice frames recovered by the
// time the header was reported, which is those already buffered", which was
// the block length's answer rather than the transmission's; the decoder has
// handed over one superframe at a time since 2026-09-23, and every superframe
// after the first was dropped here until this adapter reported them.
//
// The voice payload is AMBE and is never rendered; docs/modes.md has why. Each
// frame's 24-bit data slot is not reported either: clause 4.1.2 e leaves its
// content to the user, as core/decode/dstar.h says, and nothing here parses it.
class DStarDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "dstar";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        if (auto allowed = decoders_detail::raw_tap_allowed(kName, build.mode, build.rate);
            !allowed) {
            return std::unexpected(allowed.error());
        }
        const dsp::SampleRate rate = build.rate;
        decode::DStarConfig config;
        config.filter_taps =
            decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
        config.rate = rate;
        auto built = decode::DStar::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the dstar decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new DStarDecoder(rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_complex(kName, chunk, rate_); !shape) {
            return shape;
        }
        decoders_detail::to_complex(chunk, iq_);
        transmissions_.clear();
        if (auto processed = decoder_.process(iq_, transmissions_); !processed) {
            return processed;
        }
        last_end_ = chunk.start + chunk.frames();

        for (const decode::DStarTransmission& piece : transmissions_) {
            report(piece, chunk, false, out);
        }
        return {};
    }

    // What a transmission still open has recovered, stamped at the end of the
    // last chunk, since no chunk completed it.
    void flush(std::vector<DecodedMessage>& out) override {
        transmissions_.clear();
        decoder_.flush(transmissions_);
        const DecoderChunk at_end{.samples = {}, .channels = 2, .rate = rate_, .start = last_end_};
        for (const decode::DStarTransmission& piece : transmissions_) {
            report(piece, at_end, true, out);
        }
    }

    void reset() override {
        decoder_.reset();
        open_.reset();
    }

private:
    // The transmission the pieces belong to, which the decoder identifies by
    // the bit its frame sync started at. Every piece carries its first
    // piece's first_bit, so a piece that does not match is from a
    // transmission whose header this adapter never saw.
    struct Open {
        std::size_t first_bit = 0;
        std::string my;
        std::string ur;
        std::int64_t superframes = 0;
        std::int64_t voice_frames = 0;
    };

    DStarDecoder(dsp::SampleRate rate, decode::DStar decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    void report(const decode::DStarTransmission& piece, const DecoderChunk& chunk, bool flushed,
                std::vector<DecodedMessage>& out) {
        using namespace decoders_detail;

        if (piece.header.has_value()) {
            open_ = Open{.first_bit = piece.first_bit,
                         .my = piece.header->own_callsign,
                         .ur = piece.header->companion};
        } else if (!open_.has_value() || open_->first_bit != piece.first_bit) {
            open_ = Open{.first_bit = piece.first_bit};
            // Numbered from one rather than zero: it is not the header's.
            open_->superframes = 1;
        }

        const std::int64_t index = open_->superframes++;
        const auto frames = static_cast<std::int64_t>(piece.frames.size());
        open_->voice_frames += frames;

        DecodedMessage message =
            stamped(kName, piece.header.has_value() ? "header" : "superframe", chunk);
        message.fields.push_back(integer_field("superframe", index));
        message.fields.push_back(integer_field("voice_frames", frames));
        message.fields.push_back(integer_field("voice_frames_total", open_->voice_frames));
        message.fields.push_back(flag_field("ended", piece.ended));
        message.fields.push_back(flag_field("flushed", flushed));
        message.fields.push_back(real_field("sync_score", piece.sync_score));
        message.fields.push_back(flag_field("inverted", piece.inverted));
        message.fields.push_back(real_field("carrier_offset_hz", piece.carrier_offset_hz));
        message.fields.push_back(real_field("deviation_ratio", piece.deviation_ratio));

        const char* closing = piece.ended ? ", ended" : flushed ? ", stream ended" : "";
        if (piece.header.has_value()) {
            const decode::DStarHeader& header = *piece.header;
            message.fields.push_back(text_field("my", header.own_callsign));
            message.fields.push_back(text_field("my_suffix", header.own_suffix));
            message.fields.push_back(text_field("ur", header.companion));
            message.fields.push_back(text_field("rpt1", header.departure_repeater));
            message.fields.push_back(text_field("rpt2", header.destination_repeater));
            message.fields.push_back(integer_field("flag1", header.flag1));
            message.fields.push_back(integer_field("flag2", header.flag2));
            message.fields.push_back(integer_field("flag3", header.flag3));
            message.fields.push_back(flag_field("data", header.flags.data));
            message.fields.push_back(flag_field("via_repeater", header.flags.via_repeater));
            message.fields.push_back(flag_field("interrupted", header.flags.interrupted));
            message.fields.push_back(flag_field("control", header.flags.control));
            message.fields.push_back(flag_field("emergency", header.flags.emergency));
            message.fields.push_back(integer_field("response", header.flags.response));
            message.fields.push_back(flag_field("fcs_valid", header.fcs_valid));

            message.text = std::format("MY {}{}{} UR {} RPT1 {} RPT2 {}, {} voice frames{}",
                                       header.own_callsign, header.own_suffix.empty() ? "" : "/",
                                       header.own_suffix, header.companion,
                                       header.departure_repeater, header.destination_repeater,
                                       frames, closing);
        } else {
            if (!open_->my.empty() || !open_->ur.empty()) {
                message.fields.push_back(text_field("my", open_->my));
                message.fields.push_back(text_field("ur", open_->ur));
            }
            message.text = std::format("MY {} superframe {}, {} voice frames, {} in all{}",
                                       open_->my.empty() ? "?" : open_->my, index, frames,
                                       open_->voice_frames, closing);
        }
        out.push_back(std::move(message));

        // Closed here, so the next piece is a new transmission's.
        if (piece.ended || flushed || frames < static_cast<std::int64_t>(
                                                  decode::kDStarResyncInterval)) {
            open_.reset();
        }
    }

    dsp::SampleRate rate_;
    decode::DStar decoder_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::DStarTransmission> transmissions_;
    std::optional<Open> open_;
    dsp::SampleIndex last_end_ = 0;
};

// ---------------------------------------------------------------------------
// TETRA synchronisation bursts
// ---------------------------------------------------------------------------
//
// One message per synchronisation downlink burst found by its training
// sequence, core/decode/tetra.h's TetraBurst. kind is "sync" when the
// broadcast synchronisation channel decoded and its block code verified, and
// "burst" when the training sequence matched and the BSCH did not, which is
// still a TETRA carrier and is reported as one.
//
// Every message:
//   sync_score           real
//   block_code_verified  flag
//
// A "sync" adds the SYNC PDU, EN 300 392-2 table 21.76, and the D-MLE-SYNC it
// carries, table 18.17:
//   mcc                  int    mobile country code, 10 bits
//   mnc                  int    mobile network code, 14 bits
//   colour_code          int    6 bits
//   timeslot             int    1 to 4. Table 21.76 sends 00 to 11 for
//                               timeslots 1 to 4, and this is the timeslot
//   frame_number         int    1 to 18
//   multiframe_number    int    1 to 60
//   system_code, sharing_mode, reserved_frames, neighbour_cell_broadcast,
//   cell_load            int    as table 21.76 and table 18.17 send them
//   uplane_dtx_allowed, frame18_extension, late_entry_supported
//                        flag
//
// Nothing here decrypts or reads traffic; tetra.h and docs/modes.md.
class TetraDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "tetra";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        if (auto allowed = decoders_detail::raw_tap_allowed(kName, build.mode, build.rate);
            !allowed) {
            return std::unexpected(allowed.error());
        }
        const dsp::SampleRate rate = build.rate;
        decode::TetraConfig config;
        config.filter_taps =
            decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
        config.rate = rate;
        auto built = decode::Tetra::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the tetra decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new TetraDecoder(rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_complex(kName, chunk, rate_); !shape) {
            return shape;
        }
        decoders_detail::to_complex(chunk, iq_);
        bursts_.clear();
        if (auto processed = decoder_.process(iq_, bursts_); !processed) {
            return processed;
        }

        for (const decode::TetraBurst& burst : bursts_) {
            using namespace decoders_detail;
            DecodedMessage message = stamped(kName, burst.sync ? "sync" : "burst", chunk);
            message.fields.push_back(real_field("sync_score", burst.sync_score));
            message.fields.push_back(flag_field("block_code_verified", burst.block_code_verified));
            if (!burst.sync.has_value()) {
                message.text = "synchronisation burst, BSCH did not verify";
                out.push_back(std::move(message));
                continue;
            }

            const decode::TetraSyncPdu& pdu = *burst.sync;
            const int timeslot = pdu.timeslot + 1;
            message.fields.push_back(integer_field("mcc", pdu.mobile_country_code));
            message.fields.push_back(integer_field("mnc", pdu.mobile_network_code));
            message.fields.push_back(integer_field("colour_code", pdu.colour_code));
            message.fields.push_back(integer_field("timeslot", timeslot));
            message.fields.push_back(integer_field("frame_number", pdu.frame_number));
            message.fields.push_back(integer_field("multiframe_number", pdu.multiframe_number));
            message.fields.push_back(integer_field("system_code", pdu.system_code));
            message.fields.push_back(integer_field("sharing_mode", pdu.sharing_mode));
            message.fields.push_back(integer_field("reserved_frames", pdu.reserved_frames));
            message.fields.push_back(
                integer_field("neighbour_cell_broadcast", pdu.neighbour_cell_broadcast));
            message.fields.push_back(integer_field("cell_load", pdu.cell_load));
            message.fields.push_back(flag_field("uplane_dtx_allowed", pdu.uplane_dtx_allowed));
            message.fields.push_back(flag_field("frame18_extension", pdu.frame18_extension));
            message.fields.push_back(flag_field("late_entry_supported", pdu.late_entry_supported));

            message.text = std::format("MCC {} MNC {} CC {} TN {} FN {} MN {}",
                                       pdu.mobile_country_code, pdu.mobile_network_code,
                                       pdu.colour_code, timeslot, pdu.frame_number,
                                       pdu.multiframe_number);
            out.push_back(std::move(message));
        }
        return {};
    }

    void reset() override { decoder_.reset(); }

private:
    TetraDecoder(dsp::SampleRate rate, decode::Tetra decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    dsp::SampleRate rate_;
    decode::Tetra decoder_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::TetraBurst> bursts_;
};

// ---------------------------------------------------------------------------
// DMR
// ---------------------------------------------------------------------------
//
// Reads a dmr receiver's complex baseband, core/decode/dmr.h, or a raw tap
// below kRawTapRateCap. One message per event rather than one per burst: a
// base station channel carries 33 bursts a second, most of them Idle or
// voice, and a subscription holds 256 messages. So:
//
//   "voice_header"   a voice LC header's Full LC, TS 102 361-1 clause 7.1.1
//   "embedded_lc"    a Full LC completed from a superframe's embedded
//                    signalling, clause 7.1.3, when it differs from the last
//                    Full LC this slot reported: a receiver that joined after
//                    the header learns the call from it, and one that did not
//                    hears nothing new
//   "terminator"     a terminator with LC, clause 7.1.2, ending the call
//   "csbk"           a CSBK, clause 7.2, and "mbc_header" an MBC header
//   "data_header"    a data header block, clause 8.2.1
//   "pi_header"      a PI header: privacy is in use on the call
//   "short_lc"       a Short LC from the CACH, clause 7.1.4, when it differs
//                    from the last one reported
//
// Idle bursts, voice bursts, data continuations and anything whose CRC or
// Reed-Solomon code failed are not reported. payload_failures on the next
// message counts the failures, cumulative, as m17's lsf_crc_failures does.
//
// Every message:
//   slot                 int    1 or 2, or 0 where nothing on the air has
//                               named the timeslot: an MS or simplex
//                               transmission, whose sync names none
//   sync                 text   Table 9.2's pattern at the burst's centre,
//                               core/decode/dmr.h's dmr_sync_name, or "none"
//   colour_code          int    from the slot type or EMB; absent on a burst
//                               that carries neither
//   data_type            text   Table 9.22's name, on a data burst
//   sync_score, carrier_offset_hz, deviation_ratio
//                        real   from the least-squares fit of the slot's last
//                               sync, which the burst was sliced with
//   began_sample         int    receiver-stream index of the burst's first
//                               symbol
//   payload_failures     int    cumulative
//
// A Full LC adds, from clause 7.1, figure 7.1 and TS 102 361-2 clause 7.1.1:
//   flco, fid            int    and flco_name, text
//   protect_flag         flag
//   lc                   bytes  the nine octets
// and for the two voice channel user opcodes with FID 0:
//   group                flag   Grp_V_Ch_Usr rather than UU_V_Ch_Usr
//   talkgroup            int    the group address, when group
//   destination          int    the target address, when not
//   source               int
//   service_options      int    Table 7.11, bit 7 first, and parsed:
//   emergency, privacy, broadcast, open_voice_call_mode
//                        flag
//   priority             int
//   encrypted            flag   privacy is set, or a PI header arrived on
//                               this slot since the call began. Reported,
//                               never decrypted, per docs/modes.md
//
// A csbk or mbc_header adds opcode, opcode_name, fid, last_block,
// protect_flag and the ten octets as `csbk`; target, source and
// service_options where TS 102 361-2 Tables 7.5a to 7.7 give them; and for a
// Preamble CSBK data_follows, group and blocks_to_follow.
//
// A data_header adds format and format_name (Table 9.30), sap and sap_name
// (Table 9.31), group, response_requested, destination, source and
// blocks_to_follow where the format has them, or for the second block of a
// proprietary packet, proprietary_second and manufacturer_id.
//
// A short_lc adds slco and data, and for an Activity Update, TS 102 361-2
// Table 7.10, activity_slot_1, activity_slot_2, hashed_address_1 and
// hashed_address_2.
class DmrChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "dmr";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        if (auto allowed = decoders_detail::raw_tap_allowed(kName, build.mode, build.rate);
            !allowed) {
            return std::unexpected(allowed.error());
        }
        const dsp::SampleRate rate = build.rate;
        decode::DmrConfig config;
        config.filter_taps = decoders_detail::scaled_taps(config.filter_taps, config.rate, rate);
        config.rate = rate;
        auto built = decode::Dmr::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the dmr decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new DmrChunkDecoder(rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_complex(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        decoders_detail::to_complex(chunk, iq_);
        bursts_.clear();
        if (auto processed = decoder_.process(iq_, bursts_); !processed) {
            return processed;
        }
        for (const decode::DmrBurst& burst : bursts_) {
            describe(burst, chunk, out);
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        slots_ = {};
        last_short_lc_.reset();
        payload_failures_ = 0;
    }

private:
    // What this adapter remembers about a timeslot, to report a Full LC once
    // per change and to carry a PI header into the call's encrypted flag.
    struct Slot {
        std::optional<std::array<std::uint8_t, 9>> last_lc;
        bool pi_header = false;
    };

    DmrChunkDecoder(dsp::SampleRate rate, decode::Dmr decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] Slot& slot_of(const decode::DmrBurst& burst) {
        return slots_[burst.slot <= 2 ? burst.slot : 0];
    }

    [[nodiscard]] DecodedMessage start(std::string kind, const decode::DmrBurst& burst,
                                       const DecoderChunk& chunk) const {
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, std::move(kind), chunk);
        message.fields.push_back(integer_field("slot", burst.slot));
        message.fields.push_back(text_field(
            "sync", burst.sync ? std::string(decode::dmr_sync_name(*burst.sync)) : "none"));
        if (burst.colour_code) {
            message.fields.push_back(integer_field("colour_code", *burst.colour_code));
        }
        if (burst.data_type) {
            message.fields.push_back(
                text_field("data_type", std::string(decode::dmr_data_type_name(*burst.data_type))));
        }
        message.fields.push_back(real_field("sync_score", burst.sync_score));
        message.fields.push_back(real_field("carrier_offset_hz", burst.carrier_offset_hz));
        message.fields.push_back(real_field("deviation_ratio", burst.deviation_ratio));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(burst.first_sample))));
        message.fields.push_back(
            integer_field("payload_failures", static_cast<std::int64_t>(payload_failures_)));
        return message;
    }

    // "TS1 CC7", the prefix every text line starts with.
    [[nodiscard]] static std::string where(const decode::DmrBurst& burst) {
        std::string out = burst.slot == 0 ? "TS?" : std::format("TS{}", burst.slot);
        if (burst.colour_code) {
            out += std::format(" CC{}", *burst.colour_code);
        }
        return out;
    }

    static void options_fields(const decode::DmrServiceOptions& options, DecodedMessage& message) {
        using namespace decoders_detail;
        message.fields.push_back(integer_field("service_options", options.raw));
        message.fields.push_back(flag_field("emergency", options.emergency));
        message.fields.push_back(flag_field("privacy", options.privacy));
        message.fields.push_back(flag_field("broadcast", options.broadcast));
        message.fields.push_back(flag_field("open_voice_call_mode", options.open_voice_call_mode));
        message.fields.push_back(integer_field("priority", options.priority));
    }

    void full_lc(const decode::DmrBurst& burst, const DecoderChunk& chunk,
                 std::vector<DecodedMessage>& out) {
        using namespace decoders_detail;
        const decode::DmrFullLc& lc = *burst.full_lc;
        Slot& slot = slot_of(burst);
        if (lc.carrier == decode::DmrLcCarrier::VoiceHeader) {
            // A new call, whatever the last one said.
            slot.pi_header = false;
        }
        if (lc.carrier == decode::DmrLcCarrier::Embedded && slot.last_lc == lc.octets) {
            return;
        }
        slot.last_lc = lc.octets;

        const char* kind = lc.carrier == decode::DmrLcCarrier::VoiceHeader ? "voice_header"
                           : lc.carrier == decode::DmrLcCarrier::Terminator ? "terminator"
                                                                            : "embedded_lc";
        DecodedMessage message = start(kind, burst, chunk);
        message.fields.push_back(integer_field("flco", lc.flco));
        message.fields.push_back(text_field("flco_name", std::string(decode::dmr_flco_name(lc.fid, lc.flco))));
        message.fields.push_back(integer_field("fid", lc.fid));
        message.fields.push_back(flag_field("protect_flag", lc.protect_flag));
        message.fields.push_back(
            bytes_field("lc", std::vector<std::uint8_t>(lc.octets.begin(), lc.octets.end())));

        message.text = std::format("{} {}", where(burst), kind);
        if (lc.service_options && lc.source) {
            const bool encrypted = lc.service_options->privacy || slot.pi_header;
            message.fields.push_back(flag_field("group", lc.group));
            message.fields.push_back(integer_field(lc.group ? "talkgroup" : "destination",
                                                   lc.destination.value_or(0)));
            message.fields.push_back(integer_field("source", *lc.source));
            options_fields(*lc.service_options, message);
            message.fields.push_back(flag_field("encrypted", encrypted));
            message.text = std::format("{} {} {} {} from {}{}{}", where(burst), kind,
                                       lc.group ? "TG" : "to", lc.destination.value_or(0), *lc.source,
                                       lc.service_options->emergency ? ", emergency" : "",
                                       encrypted ? ", privacy" : "");
        } else {
            message.text += std::format(" FLCO {} FID 0x{:02X}", lc.flco, lc.fid);
        }
        if (lc.carrier == decode::DmrLcCarrier::Terminator) {
            slot.last_lc.reset();
            slot.pi_header = false;
        }
        out.push_back(std::move(message));
    }

    void describe(const decode::DmrBurst& burst, const DecoderChunk& chunk,
                  std::vector<DecodedMessage>& out) {
        using namespace decoders_detail;
        if (burst.payload_failed) {
            ++payload_failures_;
        }
        if (burst.short_lc && last_short_lc_ != std::pair{burst.short_lc->slco, burst.short_lc->data}) {
            const decode::DmrShortLc& lc = *burst.short_lc;
            last_short_lc_ = std::pair{lc.slco, lc.data};
            DecodedMessage message = start("short_lc", burst, chunk);
            message.fields.push_back(integer_field("slco", lc.slco));
            message.fields.push_back(integer_field("data", lc.data));
            message.text = std::format("CACH Short LC {} 0x{:06X}", lc.slco, lc.data);
            if (lc.activity && lc.hashed_address) {
                message.fields.push_back(integer_field("activity_slot_1", (*lc.activity)[0]));
                message.fields.push_back(integer_field("activity_slot_2", (*lc.activity)[1]));
                message.fields.push_back(integer_field("hashed_address_1", (*lc.hashed_address)[0]));
                message.fields.push_back(integer_field("hashed_address_2", (*lc.hashed_address)[1]));
                message.text = std::format("CACH activity TS1 {} TS2 {}", (*lc.activity)[0],
                                           (*lc.activity)[1]);
            }
            out.push_back(std::move(message));
        }
        if (burst.pi_header) {
            slot_of(burst).pi_header = true;
            DecodedMessage message = start("pi_header", burst, chunk);
            message.text = std::format("{} PI header, privacy", where(burst));
            out.push_back(std::move(message));
        }
        if (burst.full_lc) {
            full_lc(burst, chunk, out);
        }
        if (burst.csbk) {
            const decode::DmrCsbk& csbk = *burst.csbk;
            DecodedMessage message = start(csbk.mbc_header ? "mbc_header" : "csbk", burst, chunk);
            message.fields.push_back(integer_field("opcode", csbk.opcode));
            message.fields.push_back(
                text_field("opcode_name", std::string(decode::dmr_csbko_name(csbk.fid, csbk.opcode))));
            message.fields.push_back(integer_field("fid", csbk.fid));
            message.fields.push_back(flag_field("last_block", csbk.last_block));
            message.fields.push_back(flag_field("protect_flag", csbk.protect_flag));
            message.fields.push_back(
                bytes_field("csbk", std::vector<std::uint8_t>(csbk.octets.begin(), csbk.octets.end())));
            if (csbk.target) {
                message.fields.push_back(integer_field("target", *csbk.target));
            }
            if (csbk.source) {
                message.fields.push_back(integer_field("source", *csbk.source));
            }
            if (csbk.service_options) {
                options_fields(*csbk.service_options, message);
            }
            if (csbk.preamble_data_follows) {
                message.fields.push_back(flag_field("data_follows", *csbk.preamble_data_follows));
            }
            if (csbk.group) {
                message.fields.push_back(flag_field("group", *csbk.group));
            }
            if (csbk.blocks_to_follow) {
                message.fields.push_back(integer_field("blocks_to_follow", *csbk.blocks_to_follow));
            }
            message.text = std::format("{} {} {}", where(burst), message.kind,
                                       decode::dmr_csbko_name(csbk.fid, csbk.opcode));
            if (csbk.target && csbk.source) {
                message.text += std::format(" to {} from {}", *csbk.target, *csbk.source);
            }
            out.push_back(std::move(message));
        }
        if (burst.data_header) {
            const decode::DmrDataHeader& header = *burst.data_header;
            DecodedMessage message = start("data_header", burst, chunk);
            message.fields.push_back(integer_field("format", header.format));
            message.fields.push_back(
                text_field("format_name", std::string(decode::dmr_dpf_name(header.format))));
            message.fields.push_back(integer_field("sap", header.sap));
            message.fields.push_back(text_field("sap_name", std::string(decode::dmr_sap_name(header.sap))));
            message.fields.push_back(flag_field("proprietary_second", header.proprietary_second));
            if (header.manufacturer_id) {
                message.fields.push_back(integer_field("manufacturer_id", *header.manufacturer_id));
            }
            if (header.destination && header.source) {
                message.fields.push_back(flag_field("group", header.group));
                message.fields.push_back(flag_field("response_requested", header.response_requested));
                message.fields.push_back(integer_field("destination", *header.destination));
                message.fields.push_back(integer_field("source", *header.source));
            }
            if (header.blocks_to_follow) {
                message.fields.push_back(integer_field("blocks_to_follow", *header.blocks_to_follow));
            }
            message.text = std::format("{} data header {} {}", where(burst), decode::dmr_dpf_name(header.format),
                                       decode::dmr_sap_name(header.sap));
            if (header.destination && header.source) {
                message.text += std::format(" {} {} from {}", header.group ? "TG" : "to",
                                            *header.destination, *header.source);
            }
            out.push_back(std::move(message));
        }
    }

    dsp::SampleRate rate_;
    decode::Dmr decoder_;
    decoders_detail::StreamBase base_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::DmrBurst> bursts_;
    std::array<Slot, 3> slots_{};
    std::optional<std::pair<std::uint8_t, std::uint32_t>> last_short_lc_;
    std::uint64_t payload_failures_ = 0;
};

// ---------------------------------------------------------------------------
// RTTY
// ---------------------------------------------------------------------------
//
// Reads a usb or lsb receiver's audio. One message per line of text,
// core/decode/rtty.h's characters gathered by decoders_detail::TextLine at
// 45.45 baud, 170 Hz shift and mark on 2125 Hz, the practice rtty.h cites.
// kind is "line".
//
// THE SIDEBAND SETS THE POLARITY. RttyConfig's default puts space 170 Hz above
// mark in the audio, and rtty.h says the other sideband turns that over. The
// practice this follows, not from any document read for this file, is that an
// RTTY transmitter sends mark on the higher radio frequency: an lsb receiver
// turns that into the lower audio tone, which is the default, and a usb
// receiver into the higher one, with space 170 Hz below mark. An operator
// tunes so mark lands on 2125 Hz in either case. RTTY has no error detection
// that could tell two polarities apart after the fact, so the receiver's mode
// decides it rather than a trial of both.
//
// Every message:
//   text                 text   the line, UTF-8, its line end removed
//   characters           int    printing characters in it
//   min_margin           real   the smallest soft margin among them, rtty.h's
//                               RttyCharacter::margin; near zero means at
//                               least one unit was a guess
//   mean_margin          real
//   figures              flag   figures case was in force at the end
//   ended                text   line_end, length, idle or stream_end;
//                               TextLine has why. stream_end is the line
//                               flush handed over, stamped where the stream
//                               stopped
//   began_sample         int    receiver-stream index of the first character's
//                               start element, where start_sample is the
//                               chunk that completed the line
//   space_above_mark     flag   the polarity the mode chose
//   framing_errors       int    cumulative, stop elements that read as space
//   false_starts         int    cumulative, start elements that were glitches
//
// RTTY ON NOISE PRINTS. A start-stop receiver with no signal still finds start
// elements in the noise and frames characters from them, which is what a
// teleprinter on an empty channel does too. min_margin is how a client tells
// those lines from real ones.
class RttyChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "rtty";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::RttyConfig config;
        config.rate = build.rate;
        config.space_above_mark = build.mode != "usb";
        auto built = decode::RttyDecoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the rtty decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new RttyChunkDecoder(config, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, config_.rate); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        characters_.clear();
        decoder_.process(chunk.samples, characters_);

        using decoders_detail::LineEnd;
        for (const decode::RttyCharacter& c : characters_) {
            const std::uint64_t at = base_.at(c.position);
            figures_ = c.figures;
            if (line_.characters > 0 && at > line_.last_sample + idle_samples()) {
                emit(chunk, LineEnd::Idle, out);
            }
            if (c.glyph == U'\r' || c.glyph == U'\n') {
                emit(chunk, LineEnd::LineEnd, out);
                continue;
            }
            if (c.glyph == 0) {
                continue;
            }
            line_.add(c.glyph, at);
            min_margin_ = std::min(min_margin_, c.margin);
            margin_sum_ += static_cast<double>(c.margin);
            if (line_.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, LineEnd::Length, out);
            }
        }
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, LineEnd::Idle, out);
        }
        return {};
    }

    // The line in progress. A character still being framed is not one yet:
    // it has no stop element, which is what rtty.h hands a character over on.
    void flush(std::vector<DecodedMessage>& out) override {
        emit(end_.at_end(), decoders_detail::LineEnd::StreamEnd, out);
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        clear_line();
        figures_ = false;
    }

private:
    RttyChunkDecoder(const decode::RttyConfig& config, decode::RttyDecoder decoder)
        : config_(config), decoder_(std::move(decoder)) {}

    // Ten characters of 7.5 units, S.3 Table 1's character at the configured
    // rate, in samples.
    [[nodiscard]] std::uint64_t idle_samples() const {
        return static_cast<std::uint64_t>(decoders_detail::kIdleCharacters * 7.5 *
                                          static_cast<double>(config_.rate) / config_.baud);
    }

    void clear_line() {
        line_.clear();
        min_margin_ = std::numeric_limits<float>::max();
        margin_sum_ = 0.0;
    }

    void emit(const DecoderChunk& chunk, decoders_detail::LineEnd why,
              std::vector<DecodedMessage>& out) {
        if (!line_.visible) {
            clear_line();
            return;
        }
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, "line", chunk);
        message.fields.push_back(text_field("text", line_.text));
        message.fields.push_back(
            integer_field("characters", static_cast<std::int64_t>(line_.characters)));
        message.fields.push_back(real_field("min_margin", static_cast<double>(min_margin_)));
        message.fields.push_back(
            real_field("mean_margin", margin_sum_ / static_cast<double>(line_.characters)));
        message.fields.push_back(flag_field("figures", figures_));
        message.fields.push_back(text_field("ended", std::string(line_end_name(why))));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(line_.first_sample)));
        message.fields.push_back(flag_field("space_above_mark", config_.space_above_mark));
        message.fields.push_back(
            integer_field("framing_errors", static_cast<std::int64_t>(decoder_.framing_errors())));
        message.fields.push_back(
            integer_field("false_starts", static_cast<std::int64_t>(decoder_.false_starts())));
        message.text = line_.text;
        out.push_back(std::move(message));
        clear_line();
    }

    decode::RttyConfig config_;
    decode::RttyDecoder decoder_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::RttyCharacter> characters_;
    decoders_detail::TextLine line_;
    float min_margin_ = std::numeric_limits<float>::max();
    double margin_sum_ = 0.0;
    bool figures_ = false;
};

// ---------------------------------------------------------------------------
// AX.25, and APRS when a frame carries it
// ---------------------------------------------------------------------------
//
// Reads an nfm receiver's audio: 1200 baud Bell 202 AFSK, core/decode/ax25.h.
// One message per frame whose FCS checked. When the frame is UI and
// core/decode/aprs.h parses its information field, the message carries the
// APRS fields as well and its kind names what the packet is:
//   "aprs_position"  chapters 8 and 9, a position report
//   "aprs_mic_e"     chapter 10
//   "aprs_status"    chapter 16
//   "aprs_message", "aprs_ack", "aprs_reject"
//                    chapter 14
// and otherwise kind is "frame".
//
// Every message, from the frame, AX.25 2.2 clause 3:
//   source, destination  text   "N7LEM-1", ax25_address_text's form
//   path                 text   the repeaters, comma separated, each marked
//                               with "*" once its has-been-repeated bit is set
//   repeaters            int
//   control              int    the control octet as received
//   frame_type           text   "I", "S" or "U", Figure 4.1a
//   ui                   flag   Figure 4.4, P/F masked off
//   pid                  int    clause 3.4, I and UI frames only
//   information          bytes  the information field. Bytes and not text:
//                               AX.25 does not say what it holds
//   aprs                 flag   the APRS fields below are present
//   aprs_error           text   a UI frame the APRS parser refused, and why;
//                               objects, items, weather and telemetry are
//                               recognised and not parsed, aprs.h says which
//   fcs_failures         int    cumulative, candidate frames whose FCS failed
//   began_sample         int    receiver-stream index of the first bit after
//                               the opening flag
//
// An APRS position or Mic-E adds, APRS101 chapters 6 to 10:
//   latitude, longitude  real   degrees, north and east positive
//   ambiguity            int    trailing digits sent as spaces, 0 to 4
//   symbol_table         text   one character
//   symbol_code          text   one character
//   compressed           flag
//   course               int    degrees, when sent
//   speed_knots          real   when sent
//   altitude_ft          real   when sent; Mic-E sends metres, as altitude_m
//   comment              text   position: the comment; Mic-E: the status text
//   messaging            flag   position only
//   timestamp            text   position only, as sent: "092345z", "092345/"
//                               or "234517h"
//   mic_e_message        text   Mic-E only, mic_e_message_name's spelling
//   current              flag   Mic-E only, as sent
// A status adds status (text), timestamp and maidenhead (text) when sent. A
// message adds addressee (text), message (text) and message_id (text) when
// sent.
class Ax25ChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "ax25";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::Ax25Config config;
        config.rate = build.rate;
        auto built = decode::Ax25Decoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the ax25 decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new Ax25ChunkDecoder(build.rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        frames_.clear();
        decoder_.process(chunk.samples, frames_);
        for (const decode::Ax25Frame& frame : frames_) {
            out.push_back(describe(frame, chunk));
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    Ax25ChunkDecoder(dsp::SampleRate rate, decode::Ax25Decoder decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] static std::string path_text(const decode::Ax25Frame& frame) {
        std::string out;
        for (const decode::Ax25Address& hop : frame.repeaters) {
            if (!out.empty()) {
                out += ",";
            }
            out += decode::ax25_address_text(hop);
            if (hop.command_or_repeated) {
                out += "*";
            }
        }
        return out;
    }

    [[nodiscard]] static std::string timestamp_text(const decode::AprsTimestamp& t) {
        switch (t.format) {
            case decode::AprsTimeFormat::DayHourMinuteZulu:
                return std::format("{:02}{:02}{:02}z", t.day, t.hour, t.minute);
            case decode::AprsTimeFormat::DayHourMinuteLocal:
                return std::format("{:02}{:02}{:02}/", t.day, t.hour, t.minute);
            case decode::AprsTimeFormat::HourMinuteSecond:
                return std::format("{:02}{:02}{:02}h", t.hour, t.minute, t.second);
        }
        return {};
    }

    // The fields every position form shares, and the line an operator reads
    // for it: "49.0583N 72.0292W 088/36kt".
    [[nodiscard]] static std::string add_position(DecodedMessage& message,
                                                  const decode::AprsPosition& position) {
        using namespace decoders_detail;
        message.fields.push_back(real_field("latitude", position.latitude_deg));
        message.fields.push_back(real_field("longitude", position.longitude_deg));
        message.fields.push_back(integer_field("ambiguity", position.ambiguity));
        message.fields.push_back(text_field("symbol_table", std::string(1, position.symbol_table)));
        message.fields.push_back(text_field("symbol_code", std::string(1, position.symbol_code)));
        message.fields.push_back(flag_field("compressed", position.compressed));
        std::string line =
            std::format("{:.4f}{} {:.4f}{}", std::abs(position.latitude_deg),
                        position.latitude_deg >= 0.0 ? 'N' : 'S', std::abs(position.longitude_deg),
                        position.longitude_deg >= 0.0 ? 'E' : 'W');
        if (position.course_deg.has_value()) {
            message.fields.push_back(integer_field("course", *position.course_deg));
            line += std::format(" {:03}", *position.course_deg);
        }
        if (position.speed_knots.has_value()) {
            message.fields.push_back(real_field("speed_knots", *position.speed_knots));
            line += std::format("{}{:.0f}kt", position.course_deg.has_value() ? "/" : " ",
                                *position.speed_knots);
        }
        if (position.altitude_ft.has_value()) {
            message.fields.push_back(real_field("altitude_ft", *position.altitude_ft));
            line += std::format(" {:.0f}ft", *position.altitude_ft);
        }
        return line;
    }

    // APRS fields onto a message already carrying the frame's, and the kind
    // and the text line that go with them.
    static void add_aprs(DecodedMessage& message, const std::string& source,
                         const decode::AprsPacket& packet) {
        using namespace decoders_detail;
        const auto visit = [&](const auto& body) {
            using Body = std::decay_t<decltype(body)>;
            if constexpr (std::is_same_v<Body, decode::AprsPositionReport>) {
                message.kind = "aprs_position";
                const std::string where = add_position(message, body.position);
                message.fields.push_back(text_field("comment", body.comment));
                message.fields.push_back(flag_field("messaging", body.messaging));
                if (body.timestamp.has_value()) {
                    message.fields.push_back(
                        text_field("timestamp", timestamp_text(*body.timestamp)));
                }
                message.text = std::format("{} {}{}{}", source, where,
                                           body.comment.empty() ? "" : " ", body.comment);
            } else if constexpr (std::is_same_v<Body, decode::AprsMicE>) {
                message.kind = "aprs_mic_e";
                std::string where = add_position(message, body.position);
                if (body.altitude_m.has_value()) {
                    message.fields.push_back(real_field("altitude_m", *body.altitude_m));
                    where += std::format(" {:.0f}m", *body.altitude_m);
                }
                message.fields.push_back(text_field("comment", body.status_text));
                const std::string_view status = decode::mic_e_message_name(body.message);
                message.fields.push_back(text_field("mic_e_message", std::string(status)));
                message.fields.push_back(flag_field("current", body.current));
                message.text = std::format("{} {} {}{}{}", source, where, status,
                                           body.status_text.empty() ? "" : " ", body.status_text);
            } else if constexpr (std::is_same_v<Body, decode::AprsStatus>) {
                message.kind = "aprs_status";
                message.fields.push_back(text_field("status", body.text));
                if (body.timestamp.has_value()) {
                    message.fields.push_back(
                        text_field("timestamp", timestamp_text(*body.timestamp)));
                }
                if (body.maidenhead.has_value()) {
                    message.fields.push_back(text_field("maidenhead", *body.maidenhead));
                }
                message.text = std::format("{} status {}{}{}", source,
                                           body.maidenhead.value_or(""),
                                           body.maidenhead.has_value() ? " " : "", body.text);
            } else {
                static_assert(std::is_same_v<Body, decode::AprsMessage>);
                switch (body.kind) {
                    case decode::AprsMessage::Kind::Message: message.kind = "aprs_message"; break;
                    case decode::AprsMessage::Kind::Acknowledgement: message.kind = "aprs_ack"; break;
                    case decode::AprsMessage::Kind::Rejection: message.kind = "aprs_reject"; break;
                }
                message.fields.push_back(text_field("addressee", body.addressee));
                message.fields.push_back(text_field("message", body.text));
                if (body.id.has_value()) {
                    message.fields.push_back(text_field("message_id", *body.id));
                }
                const std::string id =
                    body.id.has_value() ? std::format(" {{{}}}", *body.id) : std::string();
                if (body.kind == decode::AprsMessage::Kind::Message) {
                    message.text = std::format("{} to {}: {}{}", source, body.addressee, body.text, id);
                } else {
                    message.text = std::format("{} to {}: {}{}", source, body.addressee,
                                               message.kind == "aprs_ack" ? "ack" : "reject", id);
                }
            }
        };
        std::visit(visit, packet.body);
    }

    [[nodiscard]] DecodedMessage describe(const decode::Ax25Frame& frame,
                                          const DecoderChunk& chunk) const {
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, "frame", chunk);
        const std::string source = decode::ax25_address_text(frame.source);
        const std::string destination = decode::ax25_address_text(frame.destination);
        const std::string path = path_text(frame);
        message.fields.push_back(text_field("source", source));
        message.fields.push_back(text_field("destination", destination));
        message.fields.push_back(text_field("path", path));
        message.fields.push_back(
            integer_field("repeaters", static_cast<std::int64_t>(frame.repeaters.size())));
        message.fields.push_back(integer_field("control", frame.control));
        std::string_view type = "U";
        switch (frame.kind) {
            case decode::Ax25FrameKind::Information: type = "I"; break;
            case decode::Ax25FrameKind::Supervisory: type = "S"; break;
            case decode::Ax25FrameKind::Unnumbered: type = "U"; break;
        }
        message.fields.push_back(text_field("frame_type", std::string(type)));
        message.fields.push_back(flag_field("ui", frame.is_ui));
        if (frame.pid.has_value()) {
            message.fields.push_back(integer_field("pid", *frame.pid));
        }
        message.fields.push_back(bytes_field("information", frame.information));
        message.fields.push_back(integer_field(
            "fcs_failures", static_cast<std::int64_t>(decoder_.stats().fcs_failures)));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(frame.first_sample))));

        // The header an operator reads it by, TNC2 style: SOURCE>DEST,PATH.
        const std::string header =
            std::format("{}>{}{}{}", source, destination, path.empty() ? "" : ",", path);

        bool aprs = false;
        if (frame.is_ui) {
            auto packet = decode::aprs_parse(frame);
            if (packet) {
                aprs = true;
                add_aprs(message, header, *packet);
            } else {
                message.fields.push_back(text_field("aprs_error", packet.error().message));
            }
        }
        message.fields.push_back(flag_field("aprs", aprs));
        if (!aprs) {
            // The information field with anything outside printable ASCII
            // shown as a dot, because this line is for reading and the bytes
            // field is for anything else.
            std::string shown;
            for (const std::uint8_t octet : frame.information) {
                shown.push_back(octet >= 0x20 && octet < 0x7F ? static_cast<char>(octet) : '.');
            }
            message.text = std::format("{} [{}]{}{}", header, type, shown.empty() ? "" : " ", shown);
        }
        return message;
    }

    dsp::SampleRate rate_;
    decode::Ax25Decoder decoder_;
    decoders_detail::StreamBase base_;
    std::vector<decode::Ax25Frame> frames_;
};

// ---------------------------------------------------------------------------
// POCSAG
// ---------------------------------------------------------------------------
//
// Reads an nfm receiver's audio, core/decode/pocsag.h. One message per page.
//
// THREE RATES AT ONCE. pocsag.h runs one bit rate per decoder and says a
// caller wanting all three runs three, and this adapter is that caller: 512
// and 1200 bit/s from ITU-R M.539-3 clause 4.3 and 2400 from practice, each a
// PocsagDecoder over the same audio. Networks choose their rate and an
// operator tuning a paging channel does not know it in advance. What it costs
// is three level discriminators and three bit clocks per sample, each a
// running sum; a sync codeword found at the wrong rate would need 30 of 32
// bits to line up by chance, which pocsag.h's tolerance of two holds to.
//
// kind is "alphanumeric" or "numeric" for function bits 11 and 00 with a
// message, "unspecified" for function bits 01 or 10 with message codewords
// M.584-2 gives no format for, and "tone" for an address with no message.
//
// Every message:
//   address              int    the 21-bit identity, 18 address bits and the
//                               3 frame bits, M.584-2 clause 1.3.2
//   function             int    0 to 3
//   bit_rate             int    512, 1200 or 2400: which decoder found it
//   message              text   numeric or alphanumeric only, decoded per
//                               clauses 2.1 and 2.2
//   message_bits         bytes  unspecified only: 20 bits a message codeword,
//                               one per byte, pocsag_numeric and
//                               pocsag_alphanumeric's input
//   corrected_bits       int    bits the BCH decode changed
//   uncorrectable_codewords int
//   inverted             flag   the sync codeword arrived complemented
//   began_sample         int    receiver-stream index of the address codeword
//   flushed              flag   the stream ended with the page still open,
//                               and flush handed it over
//
// A PAGE STILL OPEN WHEN THE STREAM ENDS IS REPORTED, by flush, from each of
// the three decoders that holds one. While the receiver lives nothing needs
// flushing: it goes on delivering noise, which ends the page on the loss of
// sync a few codewords later. What flush is for is the page whose closing
// idle codeword was the last thing the receiver heard: clause 1.2 ends a
// transmission on one, and the receiver's filter and bit clock delay leave
// it a few bits short, so without flush the last page of a capture, or of a
// receiver removed as the transmitter went quiet, went with the decoder.
// pocsag.h's flush drops a page still held in a batch nothing confirmed, so
// flush never hands over what the rule against noise paging would refuse.
//
// WHAT THIS PARAGRAPH USED TO SAY: "A PAGE STILL OPEN WHEN THE AUDIO STOPS IS
// NOT REPORTED. PocsagDecoder::flush exists for the end of a capture, and
// this adapter does not override ChunkDecoder::flush to call it." Before
// that it said "nothing on this seam says the stream has ended".
// ChunkDecoder::flush says so since 2026-09-23, for D-STAR first.
class PocsagChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "pocsag";
    static constexpr double kRates[] = {decode::kPocsag512, decode::kPocsag1200,
                                        decode::kPocsag2400};

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        std::vector<decode::PocsagDecoder> decoders;
        for (const double bit_rate : kRates) {
            decode::PocsagConfig config;
            config.rate = build.rate;
            config.bit_rate = bit_rate;
            auto built = decode::PocsagDecoder::create(config);
            if (!built) {
                return std::unexpected(with_context(
                    built.error(), std::format("building the pocsag decoder at {} bit/s", bit_rate)));
            }
            decoders.push_back(std::move(*built));
        }
        return std::unique_ptr<ChunkDecoder>(
            new PocsagChunkDecoder(build.rate, std::move(decoders)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        for (std::size_t i = 0; i < decoders_.size(); ++i) {
            pages_.clear();
            decoders_[i].process(chunk.samples, pages_);
            for (const decode::PocsagPage& page : pages_) {
                out.push_back(describe(page, static_cast<std::int64_t>(kRates[i]), chunk, false));
            }
        }
        return {};
    }

    void flush(std::vector<DecodedMessage>& out) override {
        const DecoderChunk at_end = end_.at_end();
        for (std::size_t i = 0; i < decoders_.size(); ++i) {
            pages_.clear();
            decoders_[i].flush(pages_);
            for (const decode::PocsagPage& page : pages_) {
                out.push_back(describe(page, static_cast<std::int64_t>(kRates[i]), at_end, true));
            }
        }
    }

    void reset() override {
        for (decode::PocsagDecoder& decoder : decoders_) {
            decoder.reset();
        }
        base_.reset();
    }

private:
    PocsagChunkDecoder(dsp::SampleRate rate, std::vector<decode::PocsagDecoder> decoders)
        : rate_(rate), decoders_(std::move(decoders)) {}

    [[nodiscard]] DecodedMessage describe(const decode::PocsagPage& page, std::int64_t bit_rate,
                                          const DecoderChunk& chunk, bool flushed) const {
        using namespace decoders_detail;
        std::string kind = "tone";
        if (!page.message_bits.empty()) {
            switch (page.format) {
                case decode::PocsagPage::Format::Numeric: kind = "numeric"; break;
                case decode::PocsagPage::Format::Alphanumeric: kind = "alphanumeric"; break;
                case decode::PocsagPage::Format::Unspecified: kind = "unspecified"; break;
            }
        }
        DecodedMessage message = stamped(kName, kind, chunk);
        message.fields.push_back(integer_field("address", page.identity));
        message.fields.push_back(integer_field("function", page.function));
        message.fields.push_back(integer_field("bit_rate", bit_rate));
        if (kind == "numeric" || kind == "alphanumeric") {
            message.fields.push_back(text_field("message", page.text));
        } else if (kind == "unspecified") {
            message.fields.push_back(bytes_field("message_bits", page.message_bits));
        }
        message.fields.push_back(integer_field("corrected_bits", page.corrected_bits));
        message.fields.push_back(
            integer_field("uncorrectable_codewords", page.uncorrectable_codewords));
        message.fields.push_back(flag_field("inverted", page.inverted));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(page.position))));
        message.fields.push_back(flag_field("flushed", flushed));

        message.text = std::format("{} bit/s RIC {} function {}", bit_rate, page.identity,
                                   page.function);
        if (kind == "tone") {
            message.text += ", tone only";
        } else if (kind == "unspecified") {
            message.text += std::format(", {} message bits in no format M.584 defines",
                                        page.message_bits.size());
        } else {
            message.text += std::format(": {}", page.text);
        }
        if (page.uncorrectable_codewords > 0) {
            message.text += std::format(" ({} codeword{} uncorrectable)",
                                        page.uncorrectable_codewords,
                                        page.uncorrectable_codewords == 1 ? "" : "s");
        }
        return message;
    }

    dsp::SampleRate rate_;
    std::vector<decode::PocsagDecoder> decoders_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::PocsagPage> pages_;
};

// ---------------------------------------------------------------------------
// AIS
// ---------------------------------------------------------------------------
//
// Reads an nfm receiver's audio, core/decode/ais.h: the discriminator's
// output, which the nfm kernel hands out with no de-emphasis and whose 8 kHz
// either side holds the 9600 bit/s GMSK. One message per AIS message whose
// FCS checked. The receiver is tuned to one channel, 161.975 or 162.025 MHz;
// a station hearing both runs two.
//
// kind is "position_report" for Messages 1 to 3, "base_station" for 4,
// "utc_response" for 11, "static_voyage" for 5, "class_b_position" for 18,
// "class_b_extended" for 19, "aid_to_navigation" for 21, "static_data" for
// 24, and "other" for any other message, or one shorter than its table,
// which carries the first three fields and its octets and nothing parsed.
//
// Every message, ITU-R M.1371-5 Annex 8 and Annex 2 clause 3.3.7:
//   message_id           int
//   repeat               int
//   mmsi                 int
//   octets               bytes  the data portion as received, FCS removed
//   began_sample         int    receiver-stream index of the first data bit
// Where the message's table has them, and only when not "not available":
//   latitude, longitude  real   degrees, north and east positive
//   position_accuracy    flag
//   raim                 flag
//   sog_knots            real
//   cog_degrees          real
//   heading              int    degrees true
//   navigational_status  int    Table 48, 0 to 15
//   rate_of_turn         int    ROTAIS as sent, -128 to 127
//   timestamp            int    UTC second of the report, 60 to 63 its
//                               unavailable cases
//   special_manoeuvre    int
//   communication_state  int    the 19 bits, raw
//   utc                  text   Message 4 and 11, "YYYY-MM-DDTHH:MM:SSZ" as
//                               sent, fields out of range included
//   epfd                 int    type of position fixing device
//   name, callsign       text   trailing "@" and spaces removed
//   destination          text
//   ship_type            int
//   to_bow, to_stern, to_port, to_starboard  int  metres, Figure 41
//   ais_version          int
//   imo_number           int
//   eta                  text   Message 5, "MM-DD HH:MM" as sent
//   draught_m            real
//   dte_not_available    flag
//   carrier_sense, display, dsc, whole_band, message_22  flag  Message 18
//   assigned_mode        flag
//   aton_type            int
//   off_position         flag
//   aton_status          int
//   virtual_aton         flag
//   part_number          int    Message 24
//   vendor               text   Message 24B, Table 79A's manufacturer
//   unit_model, unit_serial  int
class AisChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "ais";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::AisConfig config;
        config.rate = build.rate;
        auto built = decode::AisDecoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the ais decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new AisChunkDecoder(build.rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        messages_.clear();
        decoder_.process(chunk.samples, messages_);
        for (const decode::AisMessage& m : messages_) {
            out.push_back(describe(m, chunk));
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    AisChunkDecoder(dsp::SampleRate rate, decode::AisDecoder decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] static std::string_view kind_of(const decode::AisMessage& m) {
        if (!m.parsed) {
            return "other";
        }
        switch (m.message_id) {
            case 1:
            case 2:
            case 3: return "position_report";
            case 4: return "base_station";
            case 11: return "utc_response";
            case 5: return "static_voyage";
            case 18: return "class_b_position";
            case 19: return "class_b_extended";
            case 21: return "aid_to_navigation";
            case 24: return "static_data";
            default: return "other";
        }
    }

    // Table 48's navigational status, shortened.
    [[nodiscard]] static std::string_view status_text(std::uint8_t status) {
        switch (status) {
            case 0: return "under way using engine";
            case 1: return "at anchor";
            case 2: return "not under command";
            case 3: return "restricted manoeuvrability";
            case 4: return "constrained by draught";
            case 5: return "moored";
            case 6: return "aground";
            case 7: return "engaged in fishing";
            case 8: return "under way sailing";
            case 11: return "towing astern";
            case 12: return "pushing ahead or towing alongside";
            case 14: return "AIS-SART, MOB or EPIRB active";
            default: return {};
        }
    }

    [[nodiscard]] DecodedMessage describe(const decode::AisMessage& m,
                                          const DecoderChunk& chunk) const {
        using namespace decoders_detail;
        const std::string_view kind = kind_of(m);
        DecodedMessage message = stamped(kName, std::string(kind), chunk);
        message.fields.push_back(integer_field("message_id", m.message_id));
        message.fields.push_back(integer_field("repeat", m.repeat));
        message.fields.push_back(integer_field("mmsi", m.mmsi));
        message.fields.push_back(bytes_field("octets", m.octets));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(m.first_sample))));

        std::string line = std::format("MMSI {} message {}", m.mmsi, m.message_id);
        if (kind == "other") {
            message.text = line + std::format(", {} octets not parsed", m.octets.size());
            return message;
        }
        if (m.name) {
            message.fields.push_back(text_field("name", *m.name));
        }
        if (m.callsign) {
            message.fields.push_back(text_field("callsign", *m.callsign));
        }
        if (m.name && !m.name->empty()) {
            line += std::format(" {}", *m.name);
        }
        if (m.position) {
            message.fields.push_back(real_field("latitude", m.position->latitude));
            message.fields.push_back(real_field("longitude", m.position->longitude));
            line += std::format(" {:.4f}{} {:.4f}{}", std::abs(m.position->latitude),
                                m.position->latitude >= 0.0 ? 'N' : 'S',
                                std::abs(m.position->longitude),
                                m.position->longitude >= 0.0 ? 'E' : 'W');
        }
        if (m.position || m.utc) {
            message.fields.push_back(flag_field("position_accuracy", m.position_accuracy));
            message.fields.push_back(flag_field("raim", m.raim));
        }
        if (m.sog_tenths) {
            const double knots = *m.sog_tenths / 10.0;
            message.fields.push_back(real_field("sog_knots", knots));
            line += std::format(" {:.1f} kn", knots);
        }
        if (m.cog_tenths) {
            const double degrees = *m.cog_tenths / 10.0;
            message.fields.push_back(real_field("cog_degrees", degrees));
            line += std::format(" COG {:.1f}", degrees);
        }
        if (m.heading) {
            message.fields.push_back(integer_field("heading", *m.heading));
            line += std::format(" HDG {}", *m.heading);
        }
        if (m.navigational_status) {
            message.fields.push_back(integer_field("navigational_status", *m.navigational_status));
            const std::string_view status = status_text(*m.navigational_status);
            if (!status.empty()) {
                line += std::format(", {}", status);
            }
        }
        if (m.rate_of_turn) {
            message.fields.push_back(integer_field("rate_of_turn", *m.rate_of_turn));
        }
        if (m.timestamp) {
            message.fields.push_back(integer_field("timestamp", *m.timestamp));
        }
        if (m.special_manoeuvre) {
            message.fields.push_back(integer_field("special_manoeuvre", *m.special_manoeuvre));
        }
        if (m.communication_state) {
            message.fields.push_back(integer_field("communication_state", *m.communication_state));
        }
        if (m.utc) {
            const std::string utc = std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", m.utc->year,
                                                m.utc->month, m.utc->day, m.utc->hour,
                                                m.utc->minute, m.utc->second);
            message.fields.push_back(text_field("utc", utc));
            line += std::format(" {}", utc);
        }
        if (m.epfd) {
            message.fields.push_back(integer_field("epfd", *m.epfd));
        }
        if (m.ship_type) {
            message.fields.push_back(integer_field("ship_type", *m.ship_type));
        }
        if (m.dimensions) {
            message.fields.push_back(integer_field("to_bow", m.dimensions->to_bow));
            message.fields.push_back(integer_field("to_stern", m.dimensions->to_stern));
            message.fields.push_back(integer_field("to_port", m.dimensions->to_port));
            message.fields.push_back(integer_field("to_starboard", m.dimensions->to_starboard));
        }
        if (m.ais_version) {
            message.fields.push_back(integer_field("ais_version", *m.ais_version));
        }
        if (m.imo_number) {
            message.fields.push_back(integer_field("imo_number", *m.imo_number));
        }
        if (m.eta) {
            message.fields.push_back(text_field(
                "eta", std::format("{:02}-{:02} {:02}:{:02}", m.eta->month, m.eta->day,
                                   m.eta->hour, m.eta->minute)));
        }
        if (m.draught_tenths) {
            message.fields.push_back(real_field("draught_m", *m.draught_tenths / 10.0));
        }
        if (m.destination) {
            message.fields.push_back(text_field("destination", *m.destination));
            if (!m.destination->empty()) {
                line += std::format(" to {}", *m.destination);
            }
        }
        if (m.callsign && !m.callsign->empty()) {
            line += std::format(" ({})", *m.callsign);
        }
        if (m.dte_not_available) {
            message.fields.push_back(flag_field("dte_not_available", *m.dte_not_available));
        }
        if (m.class_b) {
            message.fields.push_back(flag_field("carrier_sense", m.class_b->carrier_sense));
            message.fields.push_back(flag_field("display", m.class_b->display));
            message.fields.push_back(flag_field("dsc", m.class_b->dsc));
            message.fields.push_back(flag_field("whole_band", m.class_b->whole_band));
            message.fields.push_back(flag_field("message_22", m.class_b->message_22));
        }
        if (m.assigned_mode) {
            message.fields.push_back(flag_field("assigned_mode", *m.assigned_mode));
        }
        if (m.aton_type) {
            message.fields.push_back(integer_field("aton_type", *m.aton_type));
        }
        if (m.off_position) {
            message.fields.push_back(flag_field("off_position", *m.off_position));
        }
        if (m.aton_status) {
            message.fields.push_back(integer_field("aton_status", *m.aton_status));
        }
        if (m.virtual_aton) {
            message.fields.push_back(flag_field("virtual_aton", *m.virtual_aton));
            if (*m.virtual_aton) {
                line += ", virtual";
            }
        }
        if (m.part_number) {
            message.fields.push_back(integer_field("part_number", *m.part_number));
        }
        if (m.vendor) {
            message.fields.push_back(text_field("vendor", *m.vendor));
        }
        if (m.unit_model) {
            message.fields.push_back(integer_field("unit_model", *m.unit_model));
        }
        if (m.unit_serial) {
            message.fields.push_back(integer_field("unit_serial", *m.unit_serial));
        }
        message.text = std::move(line);
        return message;
    }

    dsp::SampleRate rate_;
    decode::AisDecoder decoder_;
    decoders_detail::StreamBase base_;
    std::vector<decode::AisMessage> messages_;
};

// ---------------------------------------------------------------------------
// DSC
// ---------------------------------------------------------------------------
//
// Reads an nfm receiver's audio on VHF channel 70, core/decode/dsc.h: the
// 1300 and 2100 Hz tones of ITU-R M.493-15 Annex 1 clause 1.3.2. One message
// per call whose error-check character agreed.
//
// A DISTRESS ALERT IS REPORTED AND NOTHING ELSE. Nothing on this seam
// acknowledges, relays or sounds an alarm, and a decoded alert is not a
// substitute for a GMDSS watch.
//
// kind is "distress_alert" for format specifier 112; for a call of category
// distress, "distress_acknowledgement" when its telecommand is 110 and
// "distress_relay" when it is 112; otherwise "all_ships", "individual",
// "group", "geographic_area" or "automatic" by format specifier, clause 4.1.
//
// Every message:
//   format               int    clause 4.1's symbol
//   self_id              int    the caller's maritime identity, clause 7.1
//   eos                  int    117, 122 or 127, clause 9
//   symbols              bytes  the information characters, both format
//                               specifiers to the end of sequence
//   ecc                  int    the error-check character, which agreed
//   from_rx              int    characters taken from their RX copy
//   disagreements        int    characters whose copies disagreed and which
//                               the error-check character settled
//   began_sample         int    receiver-stream index of the phasing's start
// Where the format has them:
//   address              int    the called identity, clause 5.2
//   area                 text   a geographic area's ten digits, clause 5.3
//   category             int    100, 108, 110 or 112, clause 6
//   telecommand1, telecommand2  int  Table A1-3
//   distress_id          int    the station in distress, clause 8.2.1
//   nature_of_distress   int    100 to 112, clause 8.1.1
//   latitude, longitude  real   clause 8.1.2, degrees; absent when sent as
//                               ten nines
//   utc                  text   clause 8.1.3, "HH:MM"; absent for 8888
//   subsequent_communications  int  clause 8.1.4
//   channel              int    a VHF working channel, Table A1-5
//   frequency_hz         int    an MF or HF frequency, Table A1-5
//   message_symbols      bytes  Message 2 and 3 as sent, for what is not
//                               a channel or a frequency
class DscChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "dsc";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::DscConfig config;
        config.rate = build.rate;
        auto built = decode::DscDecoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the dsc decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new DscChunkDecoder(build.rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        calls_.clear();
        decoder_.process(chunk.samples, calls_);
        for (const decode::DscCall& c : calls_) {
            out.push_back(describe(c, chunk));
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    DscChunkDecoder(dsp::SampleRate rate, decode::DscDecoder decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] static std::string_view kind_of(const decode::DscCall& c) {
        if (c.format == decode::kDscFormatDistress) {
            return "distress_alert";
        }
        if (c.category == decode::kDscCategoryDistress) {
            if (c.telecommand1 == decode::kDscTelecommandDistressAcknowledgement) {
                return "distress_acknowledgement";
            }
            return "distress_relay";
        }
        switch (c.format) {
            case decode::kDscFormatAllShips: return "all_ships";
            case decode::kDscFormatIndividual: return "individual";
            case decode::kDscFormatGroup: return "group";
            case decode::kDscFormatGeographicArea: return "geographic_area";
            case decode::kDscFormatAutomatic: return "automatic";
            default: return "individual";
        }
    }

    // Table A1-3, the nature of distress column and clause 8.1.1.
    [[nodiscard]] static std::string_view nature_text(std::uint8_t nature) {
        switch (nature) {
            case 100: return "fire, explosion";
            case 101: return "flooding";
            case 102: return "collision";
            case 103: return "grounding";
            case 104: return "listing, in danger of capsizing";
            case 105: return "sinking";
            case 106: return "disabled and adrift";
            case 107: return "undesignated distress";
            case 108: return "abandoning ship";
            case 109: return "piracy or armed robbery attack";
            case 110: return "man overboard";
            case 112: return "EPIRB emission";
            default: return {};
        }
    }

    // Table A1-3, the category column.
    [[nodiscard]] static std::string_view category_text(std::uint8_t category) {
        switch (category) {
            case decode::kDscCategoryRoutine: return "routine";
            case decode::kDscCategorySafety: return "safety";
            case decode::kDscCategoryUrgency: return "urgency";
            case decode::kDscCategoryDistress: return "distress";
            default: return {};
        }
    }

    [[nodiscard]] DecodedMessage describe(const decode::DscCall& c,
                                          const DecoderChunk& chunk) const {
        using namespace decoders_detail;
        const std::string_view kind = kind_of(c);
        DecodedMessage message = stamped(kName, std::string(kind), chunk);
        message.fields.push_back(integer_field("format", c.format));
        message.fields.push_back(integer_field("self_id", static_cast<std::int64_t>(c.self_id)));
        message.fields.push_back(integer_field("eos", c.eos));
        message.fields.push_back(bytes_field("symbols", c.symbols));
        message.fields.push_back(integer_field("ecc", c.ecc));
        message.fields.push_back(integer_field("from_rx", c.from_rx));
        message.fields.push_back(integer_field("disagreements", c.disagreements));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(c.first_sample))));

        std::string line;
        if (kind == "distress_alert") {
            line = std::format("DISTRESS from {:09}", c.self_id);
        } else {
            std::string what(kind);
            std::ranges::replace(what, '_', ' ');
            line = std::format("{} from {:09}", what, c.self_id);
        }
        if (c.address) {
            message.fields.push_back(integer_field("address", static_cast<std::int64_t>(*c.address)));
            line += std::format(" to {:09}", *c.address);
        }
        if (c.area_digits) {
            message.fields.push_back(text_field("area", *c.area_digits));
            line += std::format(" to area {}", *c.area_digits);
        }
        if (c.category) {
            message.fields.push_back(integer_field("category", *c.category));
            const std::string_view category = category_text(*c.category);
            if (!category.empty()) {
                line += std::format(", {}", category);
            }
        }
        if (c.telecommand1) {
            message.fields.push_back(integer_field("telecommand1", *c.telecommand1));
        }
        if (c.telecommand2) {
            message.fields.push_back(integer_field("telecommand2", *c.telecommand2));
        }
        if (c.distress_id) {
            message.fields.push_back(
                integer_field("distress_id", static_cast<std::int64_t>(*c.distress_id)));
            line += std::format(", in distress {:09}", *c.distress_id);
        }
        if (c.nature_of_distress) {
            message.fields.push_back(integer_field("nature_of_distress", *c.nature_of_distress));
            const std::string_view nature = nature_text(*c.nature_of_distress);
            line += nature.empty() ? std::format(", nature {}", *c.nature_of_distress)
                                   : std::format(", {}", nature);
        }
        if (c.distress_position) {
            message.fields.push_back(real_field("latitude", c.distress_position->latitude));
            message.fields.push_back(real_field("longitude", c.distress_position->longitude));
            line += std::format(" {:.4f}{} {:.4f}{}", std::abs(c.distress_position->latitude),
                                c.distress_position->latitude >= 0.0 ? 'N' : 'S',
                                std::abs(c.distress_position->longitude),
                                c.distress_position->longitude >= 0.0 ? 'E' : 'W');
        }
        if (c.utc_hhmm) {
            const std::string utc = std::format("{:02}:{:02}", *c.utc_hhmm / 100, *c.utc_hhmm % 100);
            message.fields.push_back(text_field("utc", utc));
            line += std::format(" at {} UTC", utc);
        }
        if (c.subsequent_communications) {
            message.fields.push_back(
                integer_field("subsequent_communications", *c.subsequent_communications));
        }
        for (const decode::DscFrequency& f : c.frequencies) {
            switch (f.kind) {
                case decode::DscFrequency::Kind::VhfChannel:
                    message.fields.push_back(
                        integer_field("channel", static_cast<std::int64_t>(f.value)));
                    line += std::format(", channel {}", f.value);
                    break;
                case decode::DscFrequency::Kind::Frequency:
                    message.fields.push_back(
                        integer_field("frequency_hz", static_cast<std::int64_t>(f.value)));
                    line += std::format(", {:.1f} kHz", static_cast<double>(f.value) / 1000.0);
                    break;
                case decode::DscFrequency::Kind::HfChannel:
                case decode::DscFrequency::Kind::Other: break;
            }
        }
        if (!c.message_symbols.empty()) {
            message.fields.push_back(bytes_field("message_symbols", c.message_symbols));
        }
        if (c.eos == decode::kDscEosAcknowledgeRq) {
            line += ", acknowledgement requested";
        } else if (c.eos == decode::kDscEosAcknowledgeBq) {
            line += ", acknowledgement";
        }
        message.text = std::move(line);
        return message;
    }

    dsp::SampleRate rate_;
    decode::DscDecoder decoder_;
    decoders_detail::StreamBase base_;
    std::vector<decode::DscCall> calls_;
};

// ---------------------------------------------------------------------------
// SITOR-B
// ---------------------------------------------------------------------------
//
// Reads a usb or lsb receiver's audio, core/decode/sitor_b.h: collective
// mode B at 100 baud and 170 Hz about 1700 Hz, M.625-4. One message per line
// of text, gathered as RTTY's are. kind is "line".
//
// THE SIDEBAND SETS WHICH POLARITY IS TRIED FIRST, and only that:
// M.625-4 Table 1 note 2 puts B on the higher emitted frequency, which is the
// higher audio tone on usb and the lower on lsb, and sitor_b.h also finds the
// phasing signals complemented and inverts on its own. So a SITOR-B receiver
// in the wrong sideband still decodes; it spends one phasing attempt first.
//
// Every message:
//   text                 text   the line, UTF-8, its line end removed; a
//                               character lost in both copies is U+FFFD,
//                               clause 4.6.5's error character
//   characters           int
//   lost                 int    characters lost in both copies, or in the one
//                               copy a single_copy character had
//   from_rx              int    characters whose DX copy was lost and whose
//                               RX copy was used, clause 4.3
//   single_copy          int    characters flush decided from the DX copy
//                               alone because the stream ended before the RX
//                               copy, so only on a line flush produced
//   phasing              int    sitor_b.h's count of phasings when the line
//                               ended; a change is a new transmission
//   ended                text   line_end, length, idle, new_transmission or
//                               stream_end, the line flush handed over
//   began_sample         int    receiver-stream index of the first character
//   upper_sideband       flag   the polarity tried first
//   phasings, losses_of_phase, both_mutilated
//                        int    cumulative, SitorStats
//
// NOTHING PRINTS BEFORE THE FIRST LINE END after phasing, which is clause
// 4.6.4 and sitor_b.h's default. NAVTEX, whose "ZCZC" follows phasing with no
// line end, is the navtex decoder below rather than this one.
class SitorBChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "sitor_b";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::SitorConfig config;
        config.rate = build.rate;
        config.upper_sideband = build.mode != "lsb";
        auto built = decode::SitorBDecoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the sitor_b decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new SitorBChunkDecoder(config, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, config_.rate); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        characters_.clear();
        decoder_.process(chunk.samples, characters_);
        take(chunk, out);
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, decoders_detail::LineEnd::Idle, out);
        }
        return {};
    }

    // The characters sitor_b.h still holds, then the line in progress.
    // Clause 4.2 sends the RX copy 280 ms behind the DX, so a stream cut off
    // mid-transmission ends with DX copies whose RX copies never came.
    // sitor_b.h's flush decides each from its DX copy alone and marks it, and
    // they join the line before it goes, counted in single_copy. A stream that
    // ended on its own idle signals leaves none.
    //
    // WHAT THIS COMMENT USED TO SAY: "A character whose DX copy arrived and
    // whose RX copy had not is still held by sitor_b.h, which has no flush,
    // and goes with it".
    void flush(std::vector<DecodedMessage>& out) override {
        characters_.clear();
        decoder_.flush(characters_);
        const DecoderChunk at_end = end_.at_end();
        take(at_end, out);
        emit(at_end, decoders_detail::LineEnd::StreamEnd, out);
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        clear_line();
        phasing_ = 0;
    }

private:
    SitorBChunkDecoder(const decode::SitorConfig& config, decode::SitorBDecoder decoder)
        : config_(config), decoder_(std::move(decoder)) {}

    // Ten characters, each a DX and RX pair of 7-unit signals at 100 baud,
    // clause 4.2's 140 ms, in samples.
    [[nodiscard]] std::uint64_t idle_samples() const {
        return static_cast<std::uint64_t>(
            decoders_detail::kIdleCharacters * 2.0 *
            static_cast<double>(decode::kSitorSignalUnits) * static_cast<double>(config_.rate) /
            decode::kSitorBaud);
    }

    // characters_ into the line, closing it where a character says to. The
    // messages a close produces are stamped with `chunk`.
    void take(const DecoderChunk& chunk, std::vector<DecodedMessage>& out) {
        using decoders_detail::LineEnd;
        for (const decode::SitorCharacter& c : characters_) {
            const std::uint64_t at = base_.at(c.position);
            if (line_.characters > 0 && c.phasing != phasing_) {
                emit(chunk, LineEnd::NewTransmission, out);
            }
            phasing_ = c.phasing;
            if (line_.characters > 0 && at > line_.last_sample + idle_samples()) {
                emit(chunk, LineEnd::Idle, out);
            }
            if (!c.mutilated && (c.glyph == U'\r' || c.glyph == U'\n')) {
                emit(chunk, LineEnd::LineEnd, out);
                continue;
            }
            if (!c.mutilated && c.glyph == 0) {
                continue;
            }
            line_.add(c.glyph, at);
            lost_ += c.mutilated ? 1U : 0U;
            from_rx_ += c.from_rx ? 1U : 0U;
            single_copy_ += c.single_copy ? 1U : 0U;
            if (line_.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, LineEnd::Length, out);
            }
        }
    }

    void clear_line() {
        line_.clear();
        lost_ = 0;
        from_rx_ = 0;
        single_copy_ = 0;
    }

    void emit(const DecoderChunk& chunk, decoders_detail::LineEnd why,
              std::vector<DecodedMessage>& out) {
        if (!line_.visible) {
            clear_line();
            return;
        }
        using namespace decoders_detail;
        const decode::SitorStats& stats = decoder_.stats();
        DecodedMessage message = stamped(kName, "line", chunk);
        message.fields.push_back(text_field("text", line_.text));
        message.fields.push_back(
            integer_field("characters", static_cast<std::int64_t>(line_.characters)));
        message.fields.push_back(integer_field("lost", static_cast<std::int64_t>(lost_)));
        message.fields.push_back(integer_field("from_rx", static_cast<std::int64_t>(from_rx_)));
        message.fields.push_back(
            integer_field("single_copy", static_cast<std::int64_t>(single_copy_)));
        message.fields.push_back(integer_field("phasing", static_cast<std::int64_t>(phasing_)));
        message.fields.push_back(text_field("ended", std::string(line_end_name(why))));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(line_.first_sample)));
        message.fields.push_back(flag_field("upper_sideband", config_.upper_sideband));
        message.fields.push_back(
            integer_field("phasings", static_cast<std::int64_t>(stats.phasings)));
        message.fields.push_back(
            integer_field("losses_of_phase", static_cast<std::int64_t>(stats.losses_of_phase)));
        message.fields.push_back(
            integer_field("both_mutilated", static_cast<std::int64_t>(stats.both_mutilated)));
        message.text = line_.text;
        out.push_back(std::move(message));
        clear_line();
    }

    decode::SitorConfig config_;
    decode::SitorBDecoder decoder_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::SitorCharacter> characters_;
    decoders_detail::TextLine line_;
    std::size_t lost_ = 0;
    std::size_t from_rx_ = 0;
    std::size_t single_copy_ = 0;
    std::uint64_t phasing_ = 0;
};

// ---------------------------------------------------------------------------
// NAVTEX
// ---------------------------------------------------------------------------
//
// Reads a usb or lsb receiver's audio: SITOR-B, as above, framed into
// messages by core/decode/navtex.h per ITU-R M.540-2 Annex II. One message per
// NAVTEX message, when its "NNNN" arrives or when a new phasing or a new
// "ZCZC" shows it was cut off. kind is "message" or "incomplete".
//
// Every message:
//   area                 text   B1, one letter, "?" when it was lost
//   subject              text   B2, one letter, "?" when it was lost
//   serial               int    B3 B4, 0 to 99, or -1 when either was lost
//   preamble_clean       flag   clause 3: all four arrived and in their forms.
//                               A receiver following clause 3 prints only
//                               these, except serial 00, which clause 6 says
//                               is always printed
//   complete             flag   "NNNN" arrived
//   message              text   between the preamble's line end and "NNNN",
//                               UTF-8, line ends as sent
//   mutilated_characters int
//   began_sample         int    receiver-stream index of the first "Z"
//   flushed              flag   the stream ended before its "NNNN", and flush
//                               handed over what had arrived; kind is
//                               "incomplete"
//
// The text line is "ZCZC EA07" and the message with each line end shown as
// " / ", so it stays one line.
//
// WHAT THE LETTERS MEAN IS NOT HERE: navtex.h says why, and the reason is the
// same one, the IMO NAVTEX Manual was not held.
class NavtexChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "navtex";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::NavtexConfig config;
        config.sitor.rate = build.rate;
        config.sitor.upper_sideband = build.mode != "lsb";
        auto built = decode::NavtexDecoder::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the navtex decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new NavtexChunkDecoder(build.rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        messages_.clear();
        decoder_.process(chunk.samples, messages_);
        for (const decode::NavtexMessage& navtex : messages_) {
            out.push_back(describe(navtex, chunk, false));
        }
        return {};
    }

    void flush(std::vector<DecodedMessage>& out) override {
        messages_.clear();
        decoder_.flush(messages_);
        const DecoderChunk at_end = end_.at_end();
        for (const decode::NavtexMessage& navtex : messages_) {
            out.push_back(describe(navtex, at_end, true));
        }
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    NavtexChunkDecoder(dsp::SampleRate rate, decode::NavtexDecoder decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] DecodedMessage describe(const decode::NavtexMessage& navtex,
                                          const DecoderChunk& chunk, bool flushed) const {
        using namespace decoders_detail;
        const char area = navtex.area == 0 ? '?' : navtex.area;
        const char subject = navtex.subject == 0 ? '?' : navtex.subject;
        DecodedMessage message = stamped(kName, navtex.complete ? "message" : "incomplete", chunk);
        message.fields.push_back(text_field("area", std::string(1, area)));
        message.fields.push_back(text_field("subject", std::string(1, subject)));
        message.fields.push_back(integer_field("serial", navtex.serial));
        message.fields.push_back(flag_field("preamble_clean", navtex.preamble_clean));
        message.fields.push_back(flag_field("complete", navtex.complete));
        message.fields.push_back(text_field("message", navtex.text));
        message.fields.push_back(integer_field(
            "mutilated_characters", static_cast<std::int64_t>(navtex.mutilated_characters)));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(navtex.position))));
        message.fields.push_back(flag_field("flushed", flushed));

        // One line: CR LF pairs and lone line ends both become " / ".
        std::string flat;
        bool in_break = false;
        for (const char c : navtex.text) {
            if (c == '\r' || c == '\n') {
                in_break = true;
                continue;
            }
            if (in_break && !flat.empty()) {
                flat += " / ";
            }
            in_break = false;
            flat.push_back(c);
        }
        const std::string serial =
            navtex.serial >= 0 ? std::format("{:02}", navtex.serial) : std::string("??");
        message.text = std::format("ZCZC {}{}{} {}", area, subject, serial, flat);
        if (!navtex.preamble_clean) {
            message.text += " (preamble not clean)";
        }
        if (!navtex.complete) {
            message.text += flushed ? " (incomplete, stream ended)" : " (incomplete)";
        }
        return message;
    }

    dsp::SampleRate rate_;
    decode::NavtexDecoder decoder_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::NavtexMessage> messages_;
};

// ---------------------------------------------------------------------------
// PSK31, PSK63 and QPSK31
// ---------------------------------------------------------------------------
//
// Reads a usb or lsb receiver's audio, core/decode/psk31.h, with the tone put
// at 1000 Hz in the audio: Psk31Config's centre, with its AFC capturing 40 Hz
// either side. One message per line of text, gathered as RTTY's are; kind is
// "line". Three registry rows and one adapter, because the three modes are one
// decoder with a mode each and nothing in the audio says which a transmitter
// chose: BPSK31 read as QPSK31 is text of the wrong characters, not a refusal.
//
// THE SIDEBAND MATTERS TO QPSK ONLY. QEX page 8, as psk31.h cites it: a lower
// sideband demodulator mirrors the spectrum and turns every phase advance into
// a retard, so the adapter sets Psk31Config::lower_sideband from the mode.
//
// Every message:
//   text                 text   the line, its line end removed; a character
//                               Varicode does not recognise is U+FFFD, which
//                               covers the extended alphabet psk31.h reports
//                               as unrecognised
//   characters           int
//   unrecognised         int
//   ended                text   line_end, length, idle or stream_end, the
//                               line flush handed over
//   began_sample         int    receiver-stream index of the first character's
//                               first bit
//   frequency_offset_hz  real   the tone's measured offset from 1000 Hz
//   acquisition_strength real   psk31.h's line_to_mean when the carrier was
//                               found; the threshold is 6
//   lower_sideband       flag
//
// NOTHING PRINTS FOR THE FIRST TWO SECONDS of a transmission, which is the
// acquisition window psk31.h holds before decoding, and the decoder acquires
// once: a second transmitter on another tone later in the stream is not
// followed. psk31.h has both.
template <decode::Psk31Mode Mode>
class PskChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = Mode == decode::Psk31Mode::Bpsk31   ? "psk31"
                                              : Mode == decode::Psk31Mode::Bpsk63 ? "psk63"
                                                                                  : "qpsk31";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::Psk31Config config;
        config.rate = build.rate;
        config.mode = Mode;
        config.lower_sideband = build.mode == "lsb";
        auto built = decode::Psk31::create(config);
        if (!built) {
            return std::unexpected(
                with_context(built.error(), std::format("building the {} decoder", kName)));
        }
        return std::unique_ptr<ChunkDecoder>(new PskChunkDecoder(config, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, config_.rate); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        characters_.clear();
        if (auto processed = decoder_.process(chunk.samples, characters_); !processed) {
            return processed;
        }
        take(chunk, out);
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, decoders_detail::LineEnd::Idle, out);
        }
        return {};
    }

    // What psk31.h still holds, then the line in progress. psk31.h's flush
    // decides the bits QPSK31's Viterbi decoder was holding back, at least
    // Psk31Config::decision_delay_bits, so the characters they complete join
    // the line before it goes. A Varicode character whose two closing zeros
    // never arrived is not among them, and nor are the symbols its timing
    // recovery still holds; psk31.h says why.
    //
    // WHAT THIS COMMENT USED TO SAY: "What psk31.h still holds is not handed
    // over, because it offers no flush".
    void flush(std::vector<DecodedMessage>& out) override {
        characters_.clear();
        decoder_.flush(characters_);
        const DecoderChunk at_end = end_.at_end();
        take(at_end, out);
        emit(at_end, decoders_detail::LineEnd::StreamEnd, out);
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        line_.clear();
        unrecognised_ = 0;
    }

private:
    PskChunkDecoder(const decode::Psk31Config& config, decode::Psk31 decoder)
        : config_(config), decoder_(std::move(decoder)) {}

    // Ten characters of ten bits, a Varicode character of middling length
    // and its two-zero gap, at the mode's symbol rate: 3.2 s of PSK31.
    [[nodiscard]] std::uint64_t idle_samples() const {
        return static_cast<std::uint64_t>(decoders_detail::kIdleCharacters * 10.0 *
                                          static_cast<double>(config_.rate) /
                                          decode::psk31_symbol_rate(Mode));
    }

    // characters_ into the line, closing it where a character says to. The
    // messages a close produces are stamped with `chunk`.
    void take(const DecoderChunk& chunk, std::vector<DecodedMessage>& out) {
        using decoders_detail::LineEnd;
        for (const decode::Psk31Character& c : characters_) {
            const std::uint64_t at = base_.at(c.first_sample);
            if (line_.characters > 0 && at > line_.last_sample + idle_samples()) {
                emit(chunk, LineEnd::Idle, out);
            }
            if (c.recognised && (c.ascii == '\r' || c.ascii == '\n')) {
                emit(chunk, LineEnd::LineEnd, out);
                continue;
            }
            if (!c.recognised) {
                line_.add(char32_t{0xFFFD}, at);
                ++unrecognised_;
            } else if (c.ascii >= 0x20 || c.ascii == '\t') {
                line_.add(static_cast<char32_t>(c.ascii), at);
            } else {
                // The other control characters print nothing.
                continue;
            }
            if (line_.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, LineEnd::Length, out);
            }
        }
    }

    void emit(const DecoderChunk& chunk, decoders_detail::LineEnd why,
              std::vector<DecodedMessage>& out) {
        if (!line_.visible) {
            line_.clear();
            unrecognised_ = 0;
            return;
        }
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, "line", chunk);
        message.fields.push_back(text_field("text", line_.text));
        message.fields.push_back(
            integer_field("characters", static_cast<std::int64_t>(line_.characters)));
        message.fields.push_back(
            integer_field("unrecognised", static_cast<std::int64_t>(unrecognised_)));
        message.fields.push_back(text_field("ended", std::string(line_end_name(why))));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(line_.first_sample)));
        message.fields.push_back(real_field("frequency_offset_hz", decoder_.frequency_offset_hz()));
        message.fields.push_back(
            real_field("acquisition_strength", decoder_.acquisition_strength()));
        message.fields.push_back(flag_field("lower_sideband", config_.lower_sideband));
        message.text = line_.text;
        out.push_back(std::move(message));
        line_.clear();
        unrecognised_ = 0;
    }

    decode::Psk31Config config_;
    decode::Psk31 decoder_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::Psk31Character> characters_;
    decoders_detail::TextLine line_;
    std::size_t unrecognised_ = 0;
};

// ---------------------------------------------------------------------------
// CW
// ---------------------------------------------------------------------------
//
// Reads a cw receiver's audio, or a usb or lsb receiver's, through
// core/decode/cw.h's CwBand: every keyed tone from 200 to 2800 Hz of audio,
// wherever the tuning put it, each decoded as a stream of its own. A cw
// receiver's tone sits at its cw_pitch plus however far off zero beat the
// receiver was tuned, and a sideband receiver parked in a CW segment hears
// several stations at several pitches; both are what this reads. The pitch
// is not in DecoderBuild and does not need to be: the search finds the tone
// where it is, and a retune that moves it is found again.
//
// WHAT THIS PARAGRAPH USED TO SAY: "with the tone at 700 Hz, CwConfig's
// centre, give or take the 100 Hz its acquisition captures ... A pitch set
// elsewhere moves the tone out of the capture range and nothing decodes".
// Measured through the engine on 2026-09-23, a tone at 500 or 900 Hz printed
// nothing at 0 to 10 dB in 2500 Hz on any of cw, usb and lsb;
// docs/sensitivity.md has the grid before and after.
//
// One message per line, and a line belongs to one stream: M.1677-1 has no
// line end, so a line ends at kMaxLineCharacters, when its stream's key has
// been up for five word spaces at the speed being read, 2.1 s at 20 WPM, or
// when its stream ends. Lines from two streams interleave in the order they
// end. kind is "line".
//
// Every message:
//   text                 text   the line; a service signal is its clause
//                               1.1.3 name in angle brackets, as cw.h writes
//                               it, and a code the table does not hold is
//                               U+FFFD
//   characters           int    characters and word spaces
//   unrecognised         int
//   code                 text   the dots and dashes, one group a character,
//                               "/" between words
//   wpm                  real   character speed, PARIS, when the line ended
//   overall_wpm          real   the speed the letter spaces imply; lower than
//                               wpm under Farnsworth spacing
//   pitch_hz             real   the tone's audio frequency, where the stream
//                               was tracking it when the line ended
//   stream               int    which stream, numbered from 1 in the order
//                               the search found them; a tone that stops and
//                               comes back later is a new stream
//   frequency_offset_hz  real   pitch_hz less 700, the sidetone a cw
//                               receiver defaults to, which is what this
//                               field meant before pitch_hz existed
//   level_deviations     real   cw.h's signal meter
//   ended                text   length, idle or stream_end, the line flush
//                               handed over with the character being keyed
//   began_sample         int    receiver-stream index of the first mark
class CwChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "cw";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::CwBandConfig config;
        config.rate = build.rate;
        auto built = decode::CwBand::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the cw decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new CwChunkDecoder(config, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_audio(kName, chunk, config_.rate); !shape) {
            return shape;
        }
        base_.observe(chunk);
        end_.observe(chunk);
        characters_.clear();
        if (auto processed = decoder_.process(chunk.samples, characters_); !processed) {
            return processed;
        }
        take(chunk, out);
        refresh();
        const std::uint64_t chunk_end = chunk.start + chunk.frames();
        for (auto it = lines_.begin(); it != lines_.end();) {
            if (it->second.line.quiet_at(chunk_end, idle_samples(it->second))) {
                emit(chunk, it->first, it->second, decoders_detail::LineEnd::Idle, out);
            }
            if (it->second.line.characters == 0 && !running(it->first)) {
                it = lines_.erase(it);
            } else {
                ++it;
            }
        }
        return {};
    }

    // The characters being keyed, which cw.h's flush ends on the gap it has
    // seen so far, then every line with them in.
    void flush(std::vector<DecodedMessage>& out) override {
        const DecoderChunk at_end = end_.at_end();
        characters_.clear();
        decoder_.flush(characters_);
        take(at_end, out);
        refresh();
        for (auto& [stream, state] : lines_) {
            emit(at_end, stream, state, decoders_detail::LineEnd::StreamEnd, out);
        }
        lines_.clear();
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        lines_.clear();
    }

private:
    // One stream's line in progress, and what the stream last said about
    // itself, kept so a line that ends after its stream has gone still
    // carries it.
    struct StreamLine {
        decoders_detail::TextLine line;
        std::string code;
        std::size_t unrecognised = 0;
        double wpm = 0.0;
        double overall_wpm = 0.0;
        double pitch_hz = 0.0;
        double level_deviations = 0.0;
    };

    CwChunkDecoder(const decode::CwBandConfig& config, decode::CwBand decoder)
        : config_(config), decoder_(std::move(decoder)) {}

    [[nodiscard]] bool running(std::uint32_t stream) const {
        return std::ranges::any_of(decoder_.streams(), [stream](const decode::CwBand::StreamInfo& s) {
            return s.id == stream;
        });
    }

    // What each running stream says about itself now.
    void refresh() {
        for (const decode::CwBand::StreamInfo& info : decoder_.streams()) {
            auto found = lines_.find(info.id);
            if (found == lines_.end()) {
                continue;
            }
            StreamLine& state = found->second;
            state.pitch_hz = info.pitch_hz;
            state.wpm = info.wpm;
            state.overall_wpm = info.overall_wpm;
            state.level_deviations = info.level_deviations;
        }
    }

    // characters_ into each stream's line, ending it where a gap or its
    // length says.
    void take(const DecoderChunk& chunk, std::vector<DecodedMessage>& out) {
        using decoders_detail::LineEnd;
        for (const decode::CwCharacter& c : characters_) {
            StreamLine& state = lines_[c.stream];
            state.pitch_hz = c.pitch_hz;
            if (c.wpm > 0.0) {
                state.wpm = c.wpm;
            }
            const std::uint64_t at = base_.at(c.first_sample);
            if (state.line.characters > 0 && at > state.line.last_sample + idle_samples(state)) {
                emit(chunk, c.stream, state, LineEnd::Idle, out);
            }
            if (c.text == " ") {
                // A word space opens nothing, so a line never starts with one.
                if (state.line.characters > 0) {
                    state.line.add_text(" ", at);
                    state.code += " /";
                }
                continue;
            }
            if (c.recognised) {
                state.line.add_text(c.text, at);
            } else {
                state.line.add(char32_t{0xFFFD}, at);
                ++state.unrecognised;
            }
            state.code += (state.code.empty() ? "" : " ") + c.code;
            if (state.line.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, c.stream, state, LineEnd::Length, out);
            }
        }
    }

    // Five word spaces of seven units each at the stream's speed, or at 20
    // WPM's before it has one. Measured from the start of the line's last
    // character, so it includes that character's own length.
    [[nodiscard]] std::uint64_t idle_samples(const StreamLine& state) const {
        const double unit = state.wpm > 0.0 ? decode::kParisUnitSecondsTimesWpm / state.wpm
                                            : decode::kParisUnitSecondsTimesWpm / 20.0;
        return static_cast<std::uint64_t>(5.0 * decode::kMorseWordSpaceDots * unit *
                                          static_cast<double>(config_.rate));
    }

    void emit(const DecoderChunk& chunk, std::uint32_t stream, StreamLine& state,
              decoders_detail::LineEnd why, std::vector<DecodedMessage>& out) {
        if (!state.line.visible) {
            clear(state);
            return;
        }
        // A trailing word space is the gap before the line ended, not text.
        while (!state.line.text.empty() && state.line.text.back() == ' ') {
            state.line.text.pop_back();
        }
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, "line", chunk);
        message.fields.push_back(text_field("text", state.line.text));
        message.fields.push_back(
            integer_field("characters", static_cast<std::int64_t>(state.line.characters)));
        message.fields.push_back(
            integer_field("unrecognised", static_cast<std::int64_t>(state.unrecognised)));
        message.fields.push_back(text_field("code", state.code));
        message.fields.push_back(real_field("wpm", state.wpm));
        message.fields.push_back(real_field("overall_wpm", state.overall_wpm));
        message.fields.push_back(real_field("pitch_hz", state.pitch_hz));
        message.fields.push_back(integer_field("stream", static_cast<std::int64_t>(stream)));
        message.fields.push_back(
            real_field("frequency_offset_hz", state.pitch_hz - kSidetoneHz));
        message.fields.push_back(real_field("level_deviations", state.level_deviations));
        message.fields.push_back(text_field("ended", std::string(line_end_name(why))));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(state.line.first_sample)));
        message.text = state.line.text;
        out.push_back(std::move(message));
        clear(state);
    }

    static void clear(StreamLine& state) {
        state.line.clear();
        state.code.clear();
        state.unrecognised = 0;
    }

    // VrxParams::cw_pitch's default, which frequency_offset_hz is measured
    // from.
    static constexpr double kSidetoneHz = 700.0;

    decode::CwBandConfig config_;
    decode::CwBand decoder_;
    decoders_detail::StreamBase base_;
    decoders_detail::StreamEnd end_;
    std::vector<decode::CwCharacter> characters_;
    std::map<std::uint32_t, StreamLine> lines_;
};

// ---------------------------------------------------------------------------
// M17
// ---------------------------------------------------------------------------
//
// Reads complex baseband, core/decode/m17.h, like P25 and D-STAR, and has no
// demodulator of its own: the Demod enum does not grow for it. Two receivers
// feed it:
//
//   A p25p1 receiver. Its fine stage mixes to DC, filters to +/-6250 Hz and
//   resamples to 48000 S/s, and M17's 4800 symbols a second at 2.4 kHz peak
//   deviation occupy about 9 kHz, docs/modes.md's figure, so the P25 channel
//   carries it whole at the rate m17.h was measured at. This is the one to use.
//
//   A raw receiver, at the grid's channel rate with the carrier wherever the
//   grid left it, on the terms the top of this file gives for the complex
//   decoders on a raw tap. m17.h needs two samples a symbol, 9600 S/s, and
//   calibrates a carrier offset per frame against the sync burst.
//
// dstar is not listed because its channel is +/-3000 Hz and cuts M17's
// sidebands, and tetra because its 72000 S/s and +/-12.5 kHz were measured
// for nothing M17 does.
//
// Messages, one per event rather than one per frame. A stream frame arrives
// every 40 ms, 25 a second, which would fill a subscription's 256 in ten
// seconds of one call, and its payload is Codec 2 voice this tree does not
// render; docs/modes.md has why. So:
//
//   "lsf"          a Link Setup Frame whose CRC checked, or one rebuilt from six
//                  LICH chunks by a receiver that joined mid-stream
//   "stream_end"   the stream frame carrying 2.8.1's end-of-stream bit
//   "eot"          the End of Transmission marker, 1.4.5
//
// WHAT IS NOT REPORTED, AND WHY. An LSF whose CRC failed, and the packet and
// BERT frames m17.h recognises by their sync bursts and does not decode. The
// CRC is the only check an LSF carries, and a packet or BERT frame has none
// this decoder reads, so none of them is an event a client could tell from
// noise. lsf_crc_failures on the next "lsf" counts the LSFs dropped, so a
// channel producing nothing but failures is visible once one good one
// arrives.
//
// HOW OFTEN NOISE REACHES THIS DECODER NOW: never, in the two hours
// tests/decode/test_m17_noise.cpp measured on 2026-09-23 through a 12.5 kHz
// channel, 0 frames of any kind. m17.h requires 32 preamble symbols in front
// of an LSF or BERT burst, tracks a transmission only through the bursts
// clause 2.4 lets it carry, and joins a stream late only on two frames whose
// Frame Numbers count by one.
//
// WHAT THIS PARAGRAPH USED TO SAY, which is history from before those rules
// and no longer what noise does: "A sync burst is eight symbols and noise
// matches it: through the engine on 2026-09-22, the 2.5 s of noise after one
// 30 dB transmission produced two LSFs whose CRC failed, with callsigns like
// "6BFI/5ALB", one BERT burst and two packet bursts." An hour of noise then
// started 1615 LSFs and passed 9808 frames.
//
// An "lsf" carries, from 2.5.2, Appendix A and Table 3.2:
//   destination, source  text   the callsign for a standard address, "ALL"
//                               for broadcast, and the address in hex
//                               otherwise
//   destination_address, source_address
//                        int    the 48-bit values
//   destination_kind, source_kind
//                        text   standard, extended, broadcast or reserved
//   type                 int    the TYPE field as sent
//   stream               flag   bit 0
//   data_type, encryption_type, encryption_subtype, channel_access_number
//                        int    bits 2..1, 4..3, 6..5 and 10..7
//   encrypted            flag   encryption_type is not zero. Reported, never
//                               decrypted, per docs/modes.md
//   signed_stream        flag   bit 11
//   meta                 bytes  the 14 META bytes
//   from_lich            flag   rebuilt from LICH chunks
//   lsf_crc_failures     int    cumulative, LSFs dropped because their CRC
//                               failed
// Every message carries sync_score (real), inverted (flag) and began_sample
// (int, receiver-stream index of the sync burst's first symbol). A
// "stream_end" adds frame_number (int) and lich_worst_correction (int).
class M17ChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "m17";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        if (auto allowed = decoders_detail::raw_tap_allowed(kName, build.mode, build.rate);
            !allowed) {
            return std::unexpected(allowed.error());
        }
        decode::M17Config config;
        config.rate = build.rate;
        auto built = decode::M17::create(config);
        if (!built) {
            return std::unexpected(with_context(built.error(), "building the m17 decoder"));
        }
        return std::unique_ptr<ChunkDecoder>(new M17ChunkDecoder(build.rate, std::move(*built)));
    }

    [[nodiscard]] Status consume(const DecoderChunk& chunk,
                                 std::vector<DecodedMessage>& out) override {
        if (auto shape = decoders_detail::require_complex(kName, chunk, rate_); !shape) {
            return shape;
        }
        base_.observe(chunk);
        decoders_detail::to_complex(chunk, iq_);
        frames_.clear();
        if (auto processed = decoder_.process(iq_, frames_); !processed) {
            return processed;
        }
        for (const decode::M17Frame& frame : frames_) {
            describe(frame, chunk, out);
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    M17ChunkDecoder(dsp::SampleRate rate, decode::M17 decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] static std::string_view kind_name(decode::M17AddressKind kind) {
        switch (kind) {
            case decode::M17AddressKind::Reserved: return "reserved";
            case decode::M17AddressKind::Standard: return "standard";
            case decode::M17AddressKind::Extended: return "extended";
            case decode::M17AddressKind::Broadcast: return "broadcast";
        }
        return "unknown";
    }

    [[nodiscard]] static std::string address_text(const decode::M17Address& address) {
        switch (address.kind) {
            case decode::M17AddressKind::Standard: return address.callsign;
            case decode::M17AddressKind::Broadcast: return "ALL";
            case decode::M17AddressKind::Reserved:
            case decode::M17AddressKind::Extended: break;
        }
        return std::format("0x{:012X}", address.value);
    }

    [[nodiscard]] DecodedMessage start(std::string kind, const decode::M17Frame& frame,
                                       const DecoderChunk& chunk) const {
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, std::move(kind), chunk);
        message.fields.push_back(real_field("sync_score", frame.sync_score));
        message.fields.push_back(flag_field("inverted", frame.inverted));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(base_.at(frame.first_sample))));
        return message;
    }

    void describe(const decode::M17Frame& frame, const DecoderChunk& chunk,
                  std::vector<DecodedMessage>& out) {
        using namespace decoders_detail;
        if (frame.lsf.has_value() && !frame.lsf->crc_valid) {
            ++lsf_crc_failures_;
        } else if (frame.lsf.has_value()) {
            const decode::M17Lsf& lsf = *frame.lsf;
            DecodedMessage message = start("lsf", frame, chunk);
            const std::string destination = address_text(lsf.destination);
            const std::string source = address_text(lsf.source);
            message.fields.push_back(text_field("destination", destination));
            message.fields.push_back(text_field("source", source));
            message.fields.push_back(integer_field(
                "destination_address", static_cast<std::int64_t>(lsf.destination.value)));
            message.fields.push_back(
                integer_field("source_address", static_cast<std::int64_t>(lsf.source.value)));
            message.fields.push_back(
                text_field("destination_kind", std::string(kind_name(lsf.destination.kind))));
            message.fields.push_back(
                text_field("source_kind", std::string(kind_name(lsf.source.kind))));
            const decode::M17Type& type = lsf.type;
            message.fields.push_back(integer_field("type", type.raw));
            message.fields.push_back(flag_field("stream", type.stream));
            message.fields.push_back(integer_field("data_type", type.data_type));
            message.fields.push_back(integer_field("encryption_type", type.encryption_type));
            message.fields.push_back(integer_field("encryption_subtype", type.encryption_subtype));
            message.fields.push_back(
                integer_field("channel_access_number", type.channel_access_number));
            message.fields.push_back(flag_field("encrypted", type.encryption_type != 0));
            message.fields.push_back(flag_field("signed_stream", type.signed_stream));
            message.fields.push_back(bytes_field(
                "meta", std::vector<std::uint8_t>(lsf.meta.begin(), lsf.meta.end())));
            message.fields.push_back(flag_field("from_lich", frame.lsf_from_lich));
            message.fields.push_back(integer_field("lsf_crc_failures",
                                                   static_cast<std::int64_t>(lsf_crc_failures_)));
            message.text = std::format("{} > {} CAN {} type 0x{:04X} {}{}", source, destination,
                                       type.channel_access_number, type.raw,
                                       type.encryption_type != 0 ? "encrypted" : "clear",
                                       frame.lsf_from_lich ? ", from LICH" : "");
            out.push_back(std::move(message));
        }

        switch (frame.kind) {
            case decode::M17FrameKind::LinkSetup: break;
            case decode::M17FrameKind::Stream:
                if (frame.stream.has_value() && frame.stream->last) {
                    DecodedMessage message = start("stream_end", frame, chunk);
                    message.fields.push_back(integer_field("frame_number", frame.stream->frame_number));
                    message.fields.push_back(
                        integer_field("lich_worst_correction", frame.stream->lich_worst_correction));
                    message.text =
                        std::format("stream ended at frame {}", frame.stream->frame_number);
                    out.push_back(std::move(message));
                }
                break;
            case decode::M17FrameKind::Packet:
            case decode::M17FrameKind::Bert: break;
            case decode::M17FrameKind::EndOfTransmission: {
                DecodedMessage message = start("eot", frame, chunk);
                message.text = "end of transmission";
                out.push_back(std::move(message));
                break;
            }
        }
    }

    dsp::SampleRate rate_;
    decode::M17 decoder_;
    decoders_detail::StreamBase base_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::M17Frame> frames_;
    std::uint64_t lsf_crc_failures_ = 0;
};

// ---------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------

// Every decoder this build can attach, by name.
//
// A NEW DECODER IS ONE ROW HERE AND ONE ADAPTER ABOVE. The name is what a
// client passes to subscribeDecoded and what DecodedMessage::decoder carries,
// so it is permanent once published. A decoder named after an engine::Demod,
// as p25p1, dstar, tetra, dmr and cw are, is also what an empty name resolves
// to on a receiver in that mode. The other audio decoders are named after their
// protocol, because a usb receiver may be carrying any of seven of them, and
// an empty name on a usb receiver is refused with the list of those that read
// it.
//
// AN AUDIO DECODER NAMES ITS MODES, and its description says them again in
// words for a person reading the list. DecoderInfo::modes carries them to a
// client as a list. WHAT THIS PARAGRAPH USED TO SAY after "in words":
// "because DecoderInfo carries no list and that is where a client reading
// decoders() looks".
[[nodiscard]] inline std::span<const DecoderSpec> decoder_registry() {
    using decoders_detail::kCwModes;
    using decoders_detail::kDStarModes;
    using decoders_detail::kDmrModes;
    using decoders_detail::kFmModes;
    using decoders_detail::kM17Modes;
    using decoders_detail::kP25Modes;
    using decoders_detail::kSidebandModes;
    using decoders_detail::kTetraModes;
    static constexpr DecoderSpec kRegistry[] = {
        {P25p1Decoder::kName, DecoderInput::ComplexBaseband,
         "P25 Phase 1 C4FM framing, TIA-102.BAAA-A: NAC and DUID of every data unit, and the "
         "Header Data Unit's talkgroup, algorithm, key and encrypted flag. Reads the complex "
         "baseband of a p25p1 receiver, or a raw tap up to 192000 S/s",
         &P25p1Decoder::make, kP25Modes},
        {DStarDecoder::kName, DecoderInput::ComplexBaseband,
         "D-STAR DV, JARL Ver 7.0: the radio header's four callsigns, suffix and flags, then "
         "one message per superframe of voice frames. Reads the complex baseband of a dstar "
         "receiver, or a raw tap up to 192000 S/s",
         &DStarDecoder::make, kDStarModes},
        {TetraDecoder::kName, DecoderInput::ComplexBaseband,
         "TETRA V+D synchronisation bursts, EN 300 392-2: colour code, MCC, MNC, timeslot and "
         "the frame and multiframe numbers. Reads the complex baseband of a tetra receiver, or "
         "a raw tap up to 192000 S/s",
         &TetraDecoder::make, kTetraModes},
        {DmrChunkDecoder::kName, DecoderInput::ComplexBaseband,
         "DMR, ETSI TS 102 361-1 and -2: each timeslot's voice LC header, embedded LC and "
         "terminator with talkgroup, source, service options and privacy, CSBKs, data and PI "
         "headers, and Short LC from the CACH. Reads the complex baseband of a dmr receiver, or "
         "a raw tap up to 192000 S/s",
         &DmrChunkDecoder::make, kDmrModes},
        {RttyChunkDecoder::kName, DecoderInput::RealAudio,
         "RTTY, ITA2 over start-stop FSK, ITU-T S.1 and S.3: lines of text at 45.45 baud and "
         "170 Hz shift with mark on 2125 Hz. Reads a usb or lsb receiver's audio",
         &RttyChunkDecoder::make, kSidebandModes},
        {Ax25ChunkDecoder::kName, DecoderInput::RealAudio,
         "AX.25 2.2 over 1200 baud Bell 202 AFSK: every frame whose FCS checks, with its APRS "
         "position, Mic-E, status or message parsed when it carries one. Reads an nfm "
         "receiver's audio",
         &Ax25ChunkDecoder::make, kFmModes},
        {PocsagChunkDecoder::kName, DecoderInput::RealAudio,
         "POCSAG, ITU-R M.584-2, at 512, 1200 and 2400 bit/s at once: pages with their address, "
         "function and numeric or alphanumeric message. Reads an nfm receiver's audio",
         &PocsagChunkDecoder::make, kFmModes},
        {SitorBChunkDecoder::kName, DecoderInput::RealAudio,
         "SITOR-B collective FEC, ITU-R M.625-4: lines of text at 100 baud and 170 Hz shift "
         "about 1700 Hz, each character taken from whichever of its two copies arrived. Reads a "
         "usb or lsb receiver's audio",
         &SitorBChunkDecoder::make, kSidebandModes},
        {NavtexChunkDecoder::kName, DecoderInput::RealAudio,
         "NAVTEX, ITU-R M.540-2 over SITOR-B: each message with its B1 to B4 area, subject and "
         "serial and whether that preamble arrived clean. Reads a usb or lsb receiver's audio",
         &NavtexChunkDecoder::make, kSidebandModes},
        {PskChunkDecoder<decode::Psk31Mode::Bpsk31>::kName, DecoderInput::RealAudio,
         "PSK31, G3PLX in QEX July/August 1999: Varicode text at 31.25 baud, binary PSK, with "
         "the tone at 1000 Hz in the audio and 40 Hz of AFC. Reads a usb or lsb receiver's "
         "audio",
         &PskChunkDecoder<decode::Psk31Mode::Bpsk31>::make, kSidebandModes},
        {PskChunkDecoder<decode::Psk31Mode::Bpsk63>::kName, DecoderInput::RealAudio,
         "PSK63: PSK31's signal at 62.5 baud, with the tone at 1000 Hz in the audio. Reads a "
         "usb or lsb receiver's audio",
         &PskChunkDecoder<decode::Psk31Mode::Bpsk63>::make, kSidebandModes},
        {PskChunkDecoder<decode::Psk31Mode::Qpsk31>::kName, DecoderInput::RealAudio,
         "QPSK31, QEX July/August 1999: PSK31 with the K=5 convolutional code and Viterbi "
         "decoding, the tone at 1000 Hz in the audio. Reads a usb or lsb receiver's audio, and "
         "the sideband is taken from the receiver",
         &PskChunkDecoder<decode::Psk31Mode::Qpsk31>::make, kSidebandModes},
        {CwChunkDecoder::kName, DecoderInput::RealAudio,
         "CW, ITU-R M.1677-1 International Morse code: every keyed tone from 200 to 2800 Hz in "
         "the audio, each a stream of lines with its pitch and its character and overall speed "
         "in PARIS words per minute. Reads a cw, usb or lsb receiver's audio",
         &CwChunkDecoder::make, kCwModes},
        {M17ChunkDecoder::kName, DecoderInput::ComplexBaseband,
         "M17, Protocol Specification Part I 2.0.4: each link setup frame's callsigns, type and "
         "encrypted flag, the end of each stream, and the end of transmission. Reads the complex "
         "baseband of a p25p1 receiver, 48000 S/s in a 12.5 kHz channel, or a raw tap up to "
         "192000 S/s",
         &M17ChunkDecoder::make, kM17Modes},
        {AisChunkDecoder::kName, DecoderInput::RealAudio,
         "AIS, ITU-R M.1371-5, 9600 bit/s GMSK on 161.975 or 162.025 MHz: every message whose "
         "FCS checks, with Messages 1 to 5, 11, 18, 19, 21 and 24 parsed: MMSI, position, speed "
         "and course, heading, name, call sign, destination. Reads an nfm receiver's audio",
         &AisChunkDecoder::make, kFmModes},
        {DscChunkDecoder::kName, DecoderInput::RealAudio,
         "DSC on VHF channel 70, ITU-R M.493-15, 1200 baud on 1300 and 2100 Hz tones: distress "
         "alerts, relays and acknowledgements, all ships, individual and group calls with their "
         "identities, category, nature of distress, position and working channel. Reads an nfm "
         "receiver's audio",
         &DscChunkDecoder::make, kFmModes},
    };
    return kRegistry;
}

// The registry's audio decoders that read a receiver in `mode`, comma
// separated, or empty. For a refusal that has to say what would have worked.
[[nodiscard]] inline std::string decoders_reading(std::string_view mode) {
    std::string out;
    for (const DecoderSpec& spec : decoder_registry()) {
        if (spec.input != DecoderInput::RealAudio || spec.modes.empty() ||
            !decoder_accepts(spec, mode)) {
            continue;
        }
        if (!out.empty()) {
            out += ", ";
        }
        out += spec.name;
    }
    return out;
}

// The spec with this name, or null.
[[nodiscard]] inline const DecoderSpec* find_decoder(std::string_view name) {
    for (const DecoderSpec& spec : decoder_registry()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

// The registry's names, comma separated, for a refusal that has to list them.
[[nodiscard]] inline std::string decoder_names() {
    std::string out;
    for (const DecoderSpec& spec : decoder_registry()) {
        if (!out.empty()) {
            out += ", ";
        }
        out += spec.name;
    }
    return out;
}

}  // namespace revenant::rpc
