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
// same attach_audio_sink the RDS decoder is fed by, on the completion thread,
// behind the same retune fence, because to the engine a complex tap and a
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
//   A p25p1, dstar or tetra receiver goes through the fine stage, mixed to
//   DC, filtered to the mode's channel and resampled to 48000, 48000 or
//   72000, which is the rate each decoder was written at.
//   VrxStatus::demod_rate states it for these three.
//
//   A raw receiver is RawTapStage: one coarse grid channel at the channel
//   rate, with the carrier wherever the grid left it. VrxStatus::demod_rate
//   is the planner's figure there and is NOT the rate the tap delivers;
//   core/engine/vrx.h says so. A decoder attached to one runs at the channel
//   rate and hears the residual as a carrier offset. The P25 path removes
//   it per data unit, from a least-squares fit of that unit's sync word, so
//   its answer does not depend on how the stream is blocked. The D-STAR path
//   still removes a constant offset as each call's mean of the discriminator,
//   and core/decode/tetra.cpp has no offset correction at all.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/decode/aprs.h"
#include "core/decode/ax25.h"
#include "core/decode/cw.h"
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
    // means any receiver whose output is `input`, which is how the three
    // complex decoders have always been attached: p25p1 on a raw tap is a
    // choice an operator can make on purpose.
    //
    // NOT ON THE WIRE AS A LIST. DecoderInfo has no field for it and adding
    // one is a schema change for something the description already says in
    // words, so each description names its modes and subscribeDecoded refuses
    // a receiver outside them naming the modes again.
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
// the engine's completion thread. A raw tap at a coarse channel's full rate
// is the expensive case; a p25p1 receiver's fine stage delivers the design
// rate.
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
// which the completion thread's in-order delivery gives; a chunk the graph
// dropped would shift every later position by its length, and the graph
// reports a drop through its own counter rather than here.
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

// Characters gathered into lines, for the two start-stop text modes.
//
// A MESSAGE PER CHARACTER WOULD FILL THE QUEUE, which is why this exists.
// RTTY runs at six characters a second and a subscription holds 256 messages,
// so a client that stalled for 42 seconds would lose text, and a person reads
// a line rather than a character. A line ends at a carriage return or line
// feed, at kMaxLineCharacters, when the transmitter goes quiet for
// kIdleCharacters character times, or when the decoder has a reason to think
// what follows is another transmission.
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
enum class LineEnd : std::uint8_t { LineEnd, Length, Idle, NewTransmission };

[[nodiscard]] inline std::string_view line_end_name(LineEnd why) {
    switch (why) {
        case LineEnd::LineEnd: return "line_end";
        case LineEnd::Length: return "length";
        case LineEnd::Idle: return "idle";
        case LineEnd::NewTransmission: return "new_transmission";
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
// One message per transmission whose frame sync and radio header were
// recovered, core/decode/dstar.h's DStarTransmission. kind is "header".
//
// Callsign fields are text because JARL Ver 7.0 clause 4.1.1 f through j
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
//   fcs_valid            flag   the P_FCS checked. A header that failed it is
//                               still reported, so the callsigns may be wrong
//   sync_score           real
//   inverted             flag
//   voice_frames         int    voice frames recovered by the time the header
//                               was reported, which is those already buffered
//   ended                flag   the clause 4.1.2 h last frame was seen too
//
// The voice payload is AMBE and is never rendered; docs/modes.md has why.
class DStarDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "dstar";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
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

        for (const decode::DStarTransmission& transmission : transmissions_) {
            if (!transmission.header.has_value()) {
                continue;
            }
            using namespace decoders_detail;
            const decode::DStarHeader& header = *transmission.header;
            DecodedMessage message = stamped(kName, "header", chunk);
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
            message.fields.push_back(real_field("sync_score", transmission.sync_score));
            message.fields.push_back(flag_field("inverted", transmission.inverted));
            message.fields.push_back(integer_field(
                "voice_frames", static_cast<std::int64_t>(transmission.frames.size())));
            message.fields.push_back(flag_field("ended", transmission.ended));

            message.text = std::format("MY {}{}{} UR {} RPT1 {} RPT2 {}{}", header.own_callsign,
                                       header.own_suffix.empty() ? "" : "/", header.own_suffix,
                                       header.companion, header.departure_repeater,
                                       header.destination_repeater,
                                       header.fcs_valid ? "" : " (FCS failed)");
            out.push_back(std::move(message));
        }
        return {};
    }

    void reset() override { decoder_.reset(); }

private:
    DStarDecoder(dsp::SampleRate rate, decode::DStar decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    dsp::SampleRate rate_;
    decode::DStar decoder_;
    std::vector<dsp::Complex32> iq_;
    std::vector<decode::DStarTransmission> transmissions_;
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
//   ended                text   line_end, length or idle; TextLine has why
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
//
// A PAGE STILL OPEN WHEN THE AUDIO STOPS IS NOT REPORTED. PocsagDecoder::flush
// exists for the end of a capture, and nothing on this seam says the stream
// has ended: a receiver goes on delivering noise, which ends the page on the
// loss of sync a few codewords later.
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
        for (std::size_t i = 0; i < decoders_.size(); ++i) {
            pages_.clear();
            decoders_[i].process(chunk.samples, pages_);
            for (const decode::PocsagPage& page : pages_) {
                out.push_back(describe(page, static_cast<std::int64_t>(kRates[i]), chunk));
            }
        }
        return {};
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
                                          const DecoderChunk& chunk) const {
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
    std::vector<decode::PocsagPage> pages_;
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
//   lost                 int    characters lost in both copies
//   from_rx              int    characters whose DX copy was lost and whose
//                               RX copy was used, clause 4.3
//   phasing              int    sitor_b.h's count of phasings when the line
//                               ended; a change is a new transmission
//   ended                text   line_end, length, idle or new_transmission
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
        characters_.clear();
        decoder_.process(chunk.samples, characters_);

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
            if (line_.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, LineEnd::Length, out);
            }
        }
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, LineEnd::Idle, out);
        }
        return {};
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

    void clear_line() {
        line_.clear();
        lost_ = 0;
        from_rx_ = 0;
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
    std::vector<decode::SitorCharacter> characters_;
    decoders_detail::TextLine line_;
    std::size_t lost_ = 0;
    std::size_t from_rx_ = 0;
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
        messages_.clear();
        decoder_.process(chunk.samples, messages_);
        for (const decode::NavtexMessage& navtex : messages_) {
            out.push_back(describe(navtex, chunk));
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
    }

private:
    NavtexChunkDecoder(dsp::SampleRate rate, decode::NavtexDecoder decoder)
        : rate_(rate), decoder_(std::move(decoder)) {}

    [[nodiscard]] DecodedMessage describe(const decode::NavtexMessage& navtex,
                                          const DecoderChunk& chunk) const {
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
            message.text += " (incomplete)";
        }
        return message;
    }

    dsp::SampleRate rate_;
    decode::NavtexDecoder decoder_;
    decoders_detail::StreamBase base_;
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
//   ended                text   line_end, length or idle
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
        characters_.clear();
        if (auto processed = decoder_.process(chunk.samples, characters_); !processed) {
            return processed;
        }

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
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, LineEnd::Idle, out);
        }
        return {};
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
    std::vector<decode::Psk31Character> characters_;
    decoders_detail::TextLine line_;
    std::size_t unrecognised_ = 0;
};

// ---------------------------------------------------------------------------
// CW
// ---------------------------------------------------------------------------
//
// Reads a cw receiver's audio, or a usb or lsb receiver's, core/decode/cw.h,
// with the tone at 700 Hz, CwConfig's centre, give or take the 100 Hz its
// acquisition captures. A cw receiver puts the carrier at its cw_pitch, whose
// default is 700, so a cw receiver left at its default pitch and tuned to the
// signal is the case this was written for. A pitch set elsewhere moves the
// tone out of the capture range and nothing decodes; the pitch is not in
// DecoderBuild, because it is the one parameter a retune can change without
// rebuilding the decoder, and a decoder that followed it would have to be told
// about retunes, which this seam does not do.
//
// One message per line: M.1677-1 has no line end, so a line ends at
// kMaxLineCharacters or when the key has been up for five word spaces at the
// speed being read, 2.1 s at 20 WPM. kind is "line".
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
//   frequency_offset_hz  real   the tone's measured offset from 700 Hz
//   level_deviations     real   cw.h's signal meter
//   ended                text   length or idle
//   began_sample         int    receiver-stream index of the first mark
class CwChunkDecoder final : public ChunkDecoder {
public:
    static constexpr std::string_view kName = "cw";

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(const DecoderBuild& build) {
        decode::CwConfig config;
        config.rate = build.rate;
        auto built = decode::Cw::create(config);
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
        characters_.clear();
        if (auto processed = decoder_.process(chunk.samples, characters_); !processed) {
            return processed;
        }

        using decoders_detail::LineEnd;
        for (const decode::CwCharacter& c : characters_) {
            const std::uint64_t at = base_.at(c.first_sample);
            if (line_.characters > 0 && at > line_.last_sample + idle_samples()) {
                emit(chunk, LineEnd::Idle, out);
            }
            if (c.text == " ") {
                // A word space opens nothing, so a line never starts with one.
                if (line_.characters > 0) {
                    line_.add_text(" ", at);
                    code_ += " /";
                }
                continue;
            }
            if (c.recognised) {
                line_.add_text(c.text, at);
            } else {
                line_.add(char32_t{0xFFFD}, at);
                ++unrecognised_;
            }
            code_ += (code_.empty() ? "" : " ") + c.code;
            if (line_.characters >= decoders_detail::kMaxLineCharacters) {
                emit(chunk, LineEnd::Length, out);
            }
        }
        if (line_.quiet_at(chunk.start + chunk.frames(), idle_samples())) {
            emit(chunk, LineEnd::Idle, out);
        }
        return {};
    }

    void reset() override {
        decoder_.reset();
        base_.reset();
        clear_line();
    }

private:
    CwChunkDecoder(const decode::CwConfig& config, decode::Cw decoder)
        : config_(config), decoder_(std::move(decoder)) {}

    // Five word spaces of seven units each at the unit being read, or at 20
    // WPM's before the decoder has locked. Measured from the start of the
    // line's last character, so it includes that character's own length.
    [[nodiscard]] std::uint64_t idle_samples() const {
        const double unit = decoder_.timing().locked()
                                ? decoder_.timing().unit_seconds()
                                : decode::kParisUnitSecondsTimesWpm / 20.0;
        return static_cast<std::uint64_t>(5.0 * decode::kMorseWordSpaceDots * unit *
                                          static_cast<double>(config_.rate));
    }

    void clear_line() {
        line_.clear();
        code_.clear();
        unrecognised_ = 0;
    }

    void emit(const DecoderChunk& chunk, decoders_detail::LineEnd why,
              std::vector<DecodedMessage>& out) {
        if (!line_.visible) {
            clear_line();
            return;
        }
        // A trailing word space is the gap before the line ended, not text.
        while (!line_.text.empty() && line_.text.back() == ' ') {
            line_.text.pop_back();
        }
        using namespace decoders_detail;
        DecodedMessage message = stamped(kName, "line", chunk);
        message.fields.push_back(text_field("text", line_.text));
        message.fields.push_back(
            integer_field("characters", static_cast<std::int64_t>(line_.characters)));
        message.fields.push_back(
            integer_field("unrecognised", static_cast<std::int64_t>(unrecognised_)));
        message.fields.push_back(text_field("code", code_));
        message.fields.push_back(real_field("wpm", decoder_.wpm()));
        message.fields.push_back(real_field("overall_wpm", decoder_.overall_wpm()));
        message.fields.push_back(real_field("frequency_offset_hz", decoder_.frequency_offset_hz()));
        message.fields.push_back(real_field("level_deviations", decoder_.level_deviations()));
        message.fields.push_back(text_field("ended", std::string(line_end_name(why))));
        message.fields.push_back(
            integer_field("began_sample", static_cast<std::int64_t>(line_.first_sample)));
        message.text = line_.text;
        out.push_back(std::move(message));
        clear_line();
    }

    decode::CwConfig config_;
    decode::Cw decoder_;
    decoders_detail::StreamBase base_;
    std::vector<decode::CwCharacter> characters_;
    decoders_detail::TextLine line_;
    std::string code_;
    std::size_t unrecognised_ = 0;
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
// BERT frames m17.h recognises by their sync bursts and does not decode. A
// sync burst is eight symbols and noise matches it: through the engine on
// 2026-09-22, the 2.5 s of noise after one 30 dB transmission produced two
// LSFs whose CRC failed, with callsigns like "6BFI/5ALB", one BERT burst and
// two packet bursts. The CRC is the only check an LSF carries, and a packet
// or BERT frame has none this decoder reads, so none of them is an event a
// client could tell from noise. lsf_crc_failures on the next "lsf" counts
// the LSFs dropped, so a channel producing nothing but failures is visible
// once one good one arrives.
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
// as p25p1, dstar, tetra and cw are, is also what an empty name resolves to on
// a receiver in that mode. The other audio decoders are named after their
// protocol, because a usb receiver may be carrying any of seven of them, and
// an empty name on a usb receiver is refused with the list of those that read
// it.
//
// AN AUDIO DECODER NAMES ITS MODES, and its description says them again in
// words, because DecoderInfo carries no list and that is where a client
// reading decoders() looks.
[[nodiscard]] inline std::span<const DecoderSpec> decoder_registry() {
    using decoders_detail::kCwModes;
    using decoders_detail::kFmModes;
    using decoders_detail::kM17Modes;
    using decoders_detail::kSidebandModes;
    static constexpr DecoderSpec kRegistry[] = {
        {P25p1Decoder::kName, DecoderInput::ComplexBaseband,
         "P25 Phase 1 C4FM framing, TIA-102.BAAA-A: NAC and DUID of every data unit, and the "
         "Header Data Unit's talkgroup, algorithm, key and encrypted flag",
         &P25p1Decoder::make, {}},
        {DStarDecoder::kName, DecoderInput::ComplexBaseband,
         "D-STAR DV radio header, JARL Ver 7.0: the four callsigns, the suffix and the flags",
         &DStarDecoder::make, {}},
        {TetraDecoder::kName, DecoderInput::ComplexBaseband,
         "TETRA V+D synchronisation bursts, EN 300 392-2: colour code, MCC, MNC, timeslot and "
         "the frame and multiframe numbers",
         &TetraDecoder::make, {}},
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
         "CW, ITU-R M.1677-1 International Morse code: lines of text with the character and "
         "overall speed in PARIS words per minute, the tone at 700 Hz in the audio. Reads a cw "
         "receiver at its default pitch, or a usb or lsb receiver's audio",
         &CwChunkDecoder::make, kCwModes},
        {M17ChunkDecoder::kName, DecoderInput::ComplexBaseband,
         "M17, Protocol Specification Part I 2.0.4: each link setup frame's callsigns, type and "
         "encrypted flag, the end of each stream, and the end of transmission. Reads the complex "
         "baseband of a p25p1 receiver, 48000 S/s in a 12.5 kHz channel, or a raw tap",
         &M17ChunkDecoder::make, kM17Modes},
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
