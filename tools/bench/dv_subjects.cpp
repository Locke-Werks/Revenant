// M17, P25 Phase 1, D-STAR and TETRA: the complex baseband modes, each through
// its transmitter in core/dsp/synth/m17_mod.h or dv_mod.h, white noise from
// core/dsp/synth/channel.h calibrated in 2500 Hz, and the decoder.
//
// WHAT IS MEASURED, AND WHY IT IS NOT THE SAME FOR ALL FOUR
//
// Each follows its error-rate case in tests/decode, so the curve extends a
// figure docs/modes.md already publishes rather than inventing a new one.
// M17's case counts stream frames, because its decoder delivers payloads and
// they are what a user loses. The other three measure the demodulator on a
// raw stream: P25's and D-STAR's voice is IMBE and AMBE, and a stream of known
// bits through the same front end is the part of the chain every frame type
// shares. TETRA's payload rides in block 2 of synchronisation bursts, because
// a bare pi/4-DQPSK stream has no training sequence for the burst decoder to
// lock to; tests/decode/test_tetra.cpp says the same.
//
// Where the test excluded something from its count, this does too and for
// the same reason, and where the test left something out that a curve cannot,
// the difference is stated at the mode.

// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dstar.h"
#include "core/decode/dv_phy.h"
#include "core/decode/m17.h"
#include "core/decode/p25p1.h"
#include "core/decode/tetra.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/synth/m17_mod.h"
#include "tools/bench/mode_support.h"

namespace revenant::bench::detail {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/dv_subjects.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr dsp::SampleRate kRate48 = 48000;
constexpr dsp::SampleRate kTetraRate = 72000;

bool add_noise(std::vector<dsp::Complex32>& samples, double snr_2500_db, dsp::SampleRate rate, std::uint64_t seed) {
    return siggen::add_awgn(dsp::ComplexSpan(samples), siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db), rate,
                            seed)
        .has_value();
}

// Symbols the timing estimator spends pulling in, excluded from the count as
// tests/decode/test_p25p1.cpp excludes them: its first window is part
// transmitter turn-on and part signal, and counting it would put a floor
// under the curve set by the trial length rather than the channel.
constexpr std::size_t kSettledSymbols = 64;

// Where `pattern` sits in `recovered`, if it sits anywhere with a
// normalised correlation of at least 0.5, the tests' threshold.
std::optional<decode::SyncHit> align(std::span<const float> recovered, std::span<const float> pattern) {
    if (recovered.size() < pattern.size()) {
        return std::nullopt;
    }
    auto hit = decode::correlate_pattern(dsp::ConstRealSpan(recovered), dsp::ConstRealSpan(pattern));
    if (!hit || std::abs(hit->score) < 0.5) {
        return std::nullopt;
    }
    return *hit;
}

// ---------------------------------------------------------------------------
// M17
// ---------------------------------------------------------------------------

std::vector<std::array<std::uint8_t, decode::kM17StreamPayloadBytes>> m17_payloads(
    std::span<const std::uint8_t> payload) {
    std::vector<std::array<std::uint8_t, decode::kM17StreamPayloadBytes>> out;
    for (std::size_t at = 0; at + decode::kM17StreamPayloadBytes <= payload.size();
         at += decode::kM17StreamPayloadBytes) {
        std::array<std::uint8_t, decode::kM17StreamPayloadBytes> frame{};
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(at), frame.size(), frame.begin());
        out.push_back(frame);
    }
    return out;
}

ModeSubject m17(std::uint64_t destination, std::uint64_t source) {
    ModeSubject mode;
    mode.mode = "m17";
    mode.subject = "m17 stream mode 4FSK at 48000 S/s, 16-byte stream frames after one LSF; frame error rate "
                   "against SNR in 2500 Hz";
    mode.unit = "frame";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 25 * decode::kM17StreamPayloadBytes;
    mode.minimum_payload_bytes = decode::kM17StreamPayloadBytes;
    mode.payload_multiple = decode::kM17StreamPayloadBytes;
    mode.snr_start_db = 13.0;
    mode.snr_stop_db = 22.0;
    mode.trials = 512;
    mode.real_audio = false;
    mode.generator = [destination, source](std::span<const std::uint8_t> payload, double snr_db,
                                           std::uint64_t seed) {
        siggen::M17StreamMessage message;
        message.destination = destination;
        message.source = source;
        message.payloads = m17_payloads(payload);
        auto samples = siggen::m17_render_stream(siggen::M17ModConfig{}, message);
        if (!samples) {
            return std::vector<dsp::Complex32>{};
        }
        // A fifth of a second of carrier after the end marker, as the test
        // pads, so the receiver's filters and timing window finish with it.
        samples->insert(samples->end(), static_cast<std::size_t>(kRate48 / 5), dsp::Complex32{1.0F, 0.0F});
        if (!add_noise(*samples, snr_db, kRate48, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*samples);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const auto sent = m17_payloads(payload);
        auto decoder = decode::M17::create(decode::M17Config{});
        if (samples.empty() || !decoder) {
            return score_units(sent.size(), 0);
        }
        std::vector<decode::M17Frame> frames;
        if (!decoder->process(samples, frames)) {
            return score_units(sent.size(), 0);
        }
        std::vector<bool> seen(sent.size(), false);
        std::size_t good = 0;
        for (const decode::M17Frame& frame : frames) {
            if (!frame.stream || frame.stream->frame_number >= sent.size()) {
                continue;
            }
            const std::size_t number = frame.stream->frame_number;
            if (!seen[number] && frame.stream->payload == sent[number]) {
                seen[number] = true;
                ++good;
            }
        }
        return score_units(sent.size(), good);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        return hex_lines(payload, decode::kM17StreamPayloadBytes);
    };
    return mode;
}

// ---------------------------------------------------------------------------
// P25 Phase 1
// ---------------------------------------------------------------------------

// Four dibits per payload byte, most significant pair first.
std::vector<std::uint8_t> p25_dibits(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> dibits;
    dibits.reserve(payload.size() * 4);
    for (const std::uint8_t byte : payload) {
        for (int shift = 6; shift >= 0; shift -= 2) {
            dibits.push_back(static_cast<std::uint8_t>((byte >> shift) & 0x3U));
        }
    }
    return dibits;
}

std::uint8_t p25_slice(float value) {
    if (value >= 2.0F) {
        return 0b01;
    }
    if (value >= 0.0F) {
        return 0b00;
    }
    if (value >= -2.0F) {
        return 0b10;
    }
    return 0b11;
}

ModeSubject p25p1() {
    ModeSubject mode;
    mode.mode = "p25p1";
    mode.subject = "p25p1 C4FM at 48000 S/s, a raw dibit stream through the demodulator, the first 64 symbols "
                   "excluded; bit error rate against SNR in 2500 Hz";
    mode.unit = "bit";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 1500;
    mode.minimum_payload_bytes = 64;
    mode.snr_start_db = 12.0;
    mode.snr_stop_db = 26.0;
    mode.trials = 128;
    mode.real_audio = false;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        siggen::P25ModConfig mod;
        mod.rate = kRate48;
        auto samples = siggen::p25_render_dibits(mod, p25_dibits(payload));
        if (!samples || !add_noise(*samples, snr_db, kRate48, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*samples);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::vector<std::uint8_t> sent = p25_dibits(payload);
        const std::size_t scored_bits = 2 * (sent.size() - std::min(sent.size(), kSettledSymbols));
        decode::P25Config config;
        config.rate = kRate48;
        auto decoder = decode::P25Phase1::create(config);
        if (samples.empty() || !decoder) {
            return failed_bits(scored_bits);
        }
        std::vector<decode::P25Frame> frames;
        if (!decoder->process(samples, frames)) {
            return failed_bits(scored_bits);
        }
        const std::span<const float> recovered = decoder->last_symbols();
        std::vector<float> head(std::min<std::size_t>(128, sent.size()));
        for (std::size_t i = 0; i < head.size(); ++i) {
            head[i] = static_cast<float>(decode::kP25DibitToSymbol[sent[i]]);
        }
        const auto hit = align(recovered, head);
        if (!hit) {
            return failed_bits(scored_bits);
        }
        const std::size_t compare = std::min(sent.size(), recovered.size() - hit->offset);
        if (compare < sent.size() / 2) {
            return failed_bits(scored_bits);
        }
        TrialResult result;
        for (std::size_t i = kSettledSymbols; i < compare; ++i) {
            const float value = recovered[hit->offset + i];
            const std::uint8_t got = p25_slice(hit->inverted ? -value : value);
            result.bits_wrong += static_cast<std::uint64_t>(((got >> 1U) & 1U) != ((sent[i] >> 1U) & 1U));
            result.bits_wrong += static_cast<std::uint64_t>((got & 1U) != (sent[i] & 1U));
            result.bits_total += 2;
        }
        result.decoded = true;
        return result;
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        for (const std::uint8_t dibit : p25_dibits(payload)) {
            out.push_back(static_cast<char>('0' + dibit));
        }
        return out + "\n";
    };
    return mode;
}

// ---------------------------------------------------------------------------
// D-STAR
// ---------------------------------------------------------------------------

ModeSubject dstar() {
    ModeSubject mode;
    mode.mode = "dstar";
    mode.subject = "dstar GMSK BT 0.5 at 48000 S/s, a raw bit stream through the demodulator, the first 64 bits "
                   "excluded; bit error rate against SNR in 2500 Hz";
    mode.unit = "bit";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 1500;
    mode.minimum_payload_bytes = 32;
    mode.snr_start_db = 10.0;
    mode.snr_stop_db = 24.0;
    mode.trials = 128;
    mode.real_audio = false;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        siggen::DStarModConfig mod;
        mod.rate = kRate48;
        auto samples = siggen::dstar_render_bits(mod, payload_bits(payload));
        if (!samples || !add_noise(*samples, snr_db, kRate48, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*samples);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::vector<std::uint8_t> sent = payload_bits(payload);
        const std::size_t scored_bits = sent.size() - std::min(sent.size(), kSettledSymbols);
        decode::DStarConfig config;
        config.rate = kRate48;
        auto decoder = decode::DStar::create(config);
        if (samples.empty() || !decoder) {
            return failed_bits(scored_bits);
        }
        std::vector<decode::DStarTransmission> transmissions;
        if (!decoder->process(samples, transmissions)) {
            return failed_bits(scored_bits);
        }
        const std::span<const std::uint8_t> got = decoder->last_bits();
        std::vector<float> recovered(got.size());
        for (std::size_t i = 0; i < got.size(); ++i) {
            recovered[i] = got[i] != 0 ? 1.0F : -1.0F;
        }
        std::vector<float> head(std::min<std::size_t>(128, sent.size()));
        for (std::size_t i = 0; i < head.size(); ++i) {
            head[i] = sent[i] != 0 ? 1.0F : -1.0F;
        }
        const auto hit = align(recovered, head);
        if (!hit) {
            return failed_bits(scored_bits);
        }
        const std::size_t compare = std::min(sent.size(), recovered.size() - hit->offset);
        if (compare < sent.size() / 2) {
            return failed_bits(scored_bits);
        }
        TrialResult result;
        for (std::size_t i = kSettledSymbols; i < compare; ++i) {
            const std::uint8_t raw = got[hit->offset + i];
            const std::uint8_t value = hit->inverted ? static_cast<std::uint8_t>(1U - raw) : raw;
            result.bits_wrong += static_cast<std::uint64_t>(value != sent[i]);
            ++result.bits_total;
        }
        result.decoded = true;
        return result;
    };
    mode.truth = [](std::span<const std::uint8_t> payload) { return bits_as_text(payload_bits(payload)) + "\n"; };
    return mode;
}

// ---------------------------------------------------------------------------
// TETRA
// ---------------------------------------------------------------------------

// Block 2 of a synchronisation burst is 216 bits, 27 payload bytes.
constexpr std::size_t kTetraPayloadBits = decode::kTetraSyncBurstBlock2.length;
constexpr std::size_t kTetraBurstBytes = kTetraPayloadBits / 8;

// tests/decode/test_tetra.cpp's cell.
decode::TetraSyncPdu tetra_pdu(std::size_t burst) {
    decode::TetraSyncPdu pdu;
    pdu.system_code = 0b0011;
    pdu.colour_code = 37;
    pdu.timeslot = 0;
    pdu.frame_number = static_cast<std::uint8_t>(1 + (burst % 18));
    pdu.multiframe_number = 42;
    pdu.uplane_dtx_allowed = true;
    pdu.mobile_country_code = 234;
    pdu.mobile_network_code = 1'234;
    pdu.neighbour_cell_broadcast = 1;
    pdu.cell_load = 2;
    pdu.late_entry_supported = true;
    return pdu;
}

ModeSubject tetra() {
    ModeSubject mode;
    mode.mode = "tetra";
    mode.subject = "tetra pi/4-DQPSK at 72000 S/s, 216 payload bits in block 2 of each synchronisation burst, a "
                   "burst not found scored half wrong; bit error rate against SNR in 2500 Hz";
    mode.unit = "bit";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 24 * kTetraBurstBytes;
    mode.minimum_payload_bytes = 4 * kTetraBurstBytes;
    mode.payload_multiple = kTetraBurstBytes;
    mode.snr_start_db = 12.0;
    mode.snr_stop_db = 26.0;
    mode.trials = 128;
    mode.rate = kTetraRate;
    mode.real_audio = false;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        const std::vector<std::uint8_t> sent = payload_bits(payload);
        const std::size_t bursts = sent.size() / kTetraPayloadBits;
        std::vector<std::uint8_t> stream;
        stream.reserve((bursts + 1) * decode::kTetraBurstBits);
        // One burst more than the payload fills, carrying filler and not
        // scored. Without it the last payload burst is never found at any
        // SNR: the matched filter's delay and the timing window take the end
        // of a capture, and at 30 dB 24 bursts gave 23 back in every trial.
        // A real downlink never stops after the burst being read.
        for (std::size_t burst = 0; burst <= bursts; ++burst) {
            auto bits = siggen::tetra_sync_burst_bits(tetra_pdu(burst), 0xA11CE + burst);
            if (!bits) {
                return std::vector<dsp::Complex32>{};
            }
            if (burst < bursts) {
                std::copy_n(sent.begin() + static_cast<std::ptrdiff_t>(burst * kTetraPayloadBits),
                            kTetraPayloadBits,
                            bits->begin() + static_cast<std::ptrdiff_t>(decode::kTetraSyncBurstBlock2.offset));
            }
            stream.insert(stream.end(), bits->begin(), bits->end());
        }
        siggen::TetraModConfig mod;
        mod.rate = kTetraRate;
        auto samples = siggen::tetra_render_bits(mod, stream);
        if (!samples || !add_noise(*samples, snr_db, kTetraRate, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*samples);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::vector<std::uint8_t> sent = payload_bits(payload);
        const std::size_t bursts = sent.size() / kTetraPayloadBits;
        decode::TetraConfig config;
        config.rate = kTetraRate;
        auto decoder = decode::Tetra::create(config);
        if (samples.empty() || !decoder) {
            return failed_bits(sent.size());
        }
        std::vector<decode::TetraBurst> found;
        if (!decoder->process(samples, found)) {
            return failed_bits(sent.size());
        }
        // Matched to the burst it came from by where it started, which is
        // exact because bursts are a fixed 255 symbols. The test leaves a
        // burst that was not found out of its count, since it has no bits to
        // be wrong; a curve cannot, or a decoder that finds fewer bursts
        // would score better, so a missing burst is scored as a trial that
        // never locked is everywhere else in this harness.
        std::vector<bool> matched(bursts, false);
        TrialResult result;
        for (const decode::TetraBurst& burst : found) {
            const std::size_t index = (burst.first_symbol * 2) / decode::kTetraBurstBits;
            if (index >= bursts || matched[index]) {
                continue;
            }
            matched[index] = true;
            for (std::size_t i = 0; i < kTetraPayloadBits; ++i) {
                result.bits_wrong += static_cast<std::uint64_t>(
                    burst.bits[decode::kTetraSyncBurstBlock2.offset + i] != sent[index * kTetraPayloadBits + i]);
            }
            result.bits_total += kTetraPayloadBits;
        }
        const auto missing = static_cast<std::size_t>(std::count(matched.begin(), matched.end(), false));
        result.bits_total += missing * kTetraPayloadBits;
        result.bits_wrong += missing * kTetraPayloadBits / 2;
        result.decoded = missing == 0;
        return result;
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        const std::vector<std::uint8_t> bits = payload_bits(payload);
        for (std::size_t at = 0; at + kTetraPayloadBits <= bits.size(); at += kTetraPayloadBits) {
            out += bits_as_text(std::span(bits).subspan(at, kTetraPayloadBits)) + "\n";
        }
        return out;
    };
    return mode;
}

}  // namespace

Expected<ModeSubject> make_dv_subject(std::string_view mode) {
    if (mode == "m17") {
        auto destination = decode::m17_encode_callsign("M17-M17 C");
        auto source = decode::m17_encode_callsign("SP5WWP");
        if (!destination || !source) {
            return fail("the M17 subject's callsigns do not encode");
        }
        return m17(*destination, *source);
    }
    if (mode == "p25p1") {
        return p25p1();
    }
    if (mode == "dstar") {
        return dstar();
    }
    if (mode == "tetra") {
        return tetra();
    }
    return fail(std::format("'{}' is not a digital voice mode", mode));
}

}  // namespace revenant::bench::detail
