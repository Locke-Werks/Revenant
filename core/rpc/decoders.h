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
// the only thing a new decoder needs in order to be served. RTTY, APRS,
// POCSAG, PSK31, CW and M17 are being written as such libraries, and each of
// them lands as one more adapter and one more registry row, with nothing in
// the schema, the client or the server moving.
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
// WHAT THE ADAPTERS DO NOT DO
//
// They do not mix, filter to a channel or resample. They take the complex
// baseband a receiver delivers at whatever rate the chunk says, and build the
// decoder at that rate on the first chunk. Two paths reach them, and the rate
// is the chunk's on both rather than anything VrxStatus says:
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
//   rate and hears the residual as a carrier offset. The P25 and D-STAR
//   paths remove a constant offset as the block mean of the discriminator;
//   core/decode/tetra.cpp has no offset correction at all.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/decode/dstar.h"
#include "core/decode/p25p1.h"
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

using DecoderFactory = Expected<std::unique_ptr<ChunkDecoder>> (*)(dsp::SampleRate rate);

struct DecoderSpec {
    std::string_view name;
    DecoderInput input = DecoderInput::ComplexBaseband;
    std::string_view description;
    DecoderFactory make = nullptr;
};

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

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(dsp::SampleRate rate) {
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

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(dsp::SampleRate rate) {
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

    [[nodiscard]] static Expected<std::unique_ptr<ChunkDecoder>> make(dsp::SampleRate rate) {
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
// The registry
// ---------------------------------------------------------------------------

// Every decoder this build can attach, by name.
//
// A NEW DECODER IS ONE ROW HERE AND ONE ADAPTER ABOVE. The name is what a
// client passes to subscribeDecoded and what DecodedMessage::decoder carries,
// so it is permanent once published. A decoder named after an engine::Demod,
// as these three are, is also what an empty name resolves to on a receiver in
// that mode.
[[nodiscard]] inline std::span<const DecoderSpec> decoder_registry() {
    static constexpr DecoderSpec kRegistry[] = {
        {P25p1Decoder::kName, DecoderInput::ComplexBaseband,
         "P25 Phase 1 C4FM framing, TIA-102.BAAA-A: NAC and DUID of every data unit, and the "
         "Header Data Unit's talkgroup, algorithm, key and encrypted flag",
         &P25p1Decoder::make},
        {DStarDecoder::kName, DecoderInput::ComplexBaseband,
         "D-STAR DV radio header, JARL Ver 7.0: the four callsigns, the suffix and the flags",
         &DStarDecoder::make},
        {TetraDecoder::kName, DecoderInput::ComplexBaseband,
         "TETRA V+D synchronisation bursts, EN 300 392-2: colour code, MCC, MNC, timeslot and "
         "the frame and multiframe numbers",
         &TetraDecoder::make},
    };
    return kRegistry;
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
