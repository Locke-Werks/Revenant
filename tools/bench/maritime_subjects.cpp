// The maritime modes: AIS and DSC on VHF, each through its transmitter in
// core/dsp/synth and its decoder in core/decode, read the way the engine hands
// them over: an nfm receiver's channel filter, 8 kHz either side, and its FM
// discriminator.
//
// Both are complex baseband subjects, noise on the RF rather than the audio,
// because the discriminator has a threshold and noise after it would miss
// it; the SNR is the RF signal's in 2500 Hz, as for POCSAG. Every setting is
// the one the mode's case in tests/decode uses.

// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "core/decode/ais.h"
#include "core/decode/dsc.h"
#include "core/decode/dv_phy.h"
#include "core/dsp/synth/ais_mod.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dsc_mod.h"
#include "tools/bench/mode_support.h"

namespace revenant::bench::detail {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/maritime_subjects.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr dsp::SampleRate kRate = 48000;

// core/dsp/vrx_reference.cpp's default nfm passband.
double nfm_channel(double hertz, const void*) {
    return std::abs(hertz) <= 8000.0 ? 1.0 : 0.0;
}

// What an nfm receiver hands its audio decoders.
bool discriminate(dsp::ConstComplexSpan samples, std::span<const float> taps, std::vector<float>& audio) {
    std::vector<dsp::Complex32> filtered(samples.size());
    audio.resize(samples.size());
    return decode::filter_complex(samples, dsp::ConstRealSpan(taps), dsp::ComplexSpan(filtered)).has_value() &&
           decode::fm_discriminate(dsp::ConstComplexSpan(filtered), dsp::RealSpan(audio), kRate).has_value();
}

std::uint32_t read32(std::span<const std::uint8_t> b, std::size_t at) {
    return (static_cast<std::uint32_t>(b[at]) << 24U) | (static_cast<std::uint32_t>(b[at + 1]) << 16U) |
           (static_cast<std::uint32_t>(b[at + 2]) << 8U) | static_cast<std::uint32_t>(b[at + 3]);
}

// ---------------------------------------------------------------------------
// AIS
// ---------------------------------------------------------------------------

// A Message 1 per 14 payload bytes: MMSI, longitude and latitude from four
// bytes each, and SOG, COG and heading from the last two. 168 bits on the
// air, one slot, which is what most of a real channel carries.
constexpr std::size_t kAisReportBytes = 14;

std::vector<decode::AisMessage> ais_reports(std::span<const std::uint8_t> payload) {
    std::vector<decode::AisMessage> out;
    for (std::size_t at = 0; at + kAisReportBytes <= payload.size(); at += kAisReportBytes) {
        decode::AisMessage m;
        m.message_id = 1;
        m.mmsi = read32(payload, at) % 1'000'000'000U;
        // Table 48's units, 1/10000 minute, inside plus and minus 180 and 90
        // degrees.
        const auto lon_units = static_cast<std::int64_t>(read32(payload, at + 4) % 216'000'001U) - 108'000'000;
        const auto lat_units = static_cast<std::int64_t>(read32(payload, at + 8) % 108'000'001U) - 54'000'000;
        m.position = decode::AisPosition{static_cast<double>(lon_units) / decode::kAisPositionUnitsPerDegree,
                                         static_cast<double>(lat_units) / decode::kAisPositionUnitsPerDegree};
        m.navigational_status = static_cast<std::uint8_t>(payload[at + 12] % 9U);
        m.sog_tenths = static_cast<std::uint16_t>(payload[at + 12] * 4U % 1023U);
        m.cog_tenths = static_cast<std::uint16_t>(payload[at + 13] * 14U % 3600U);
        m.heading = static_cast<std::uint16_t>(payload[at + 13] % 360U);
        m.timestamp = static_cast<std::uint8_t>(payload[at + 12] % 60U);
        out.push_back(m);
    }
    return out;
}

// The burst's own power: the carrier is off between bursts, and noise stated
// against a mean that counted the gaps would sit below the burst by the duty
// cycle.
double burst_power(std::span<const dsp::Complex32> rf) {
    double sum = 0.0;
    std::size_t on = 0;
    for (const dsp::Complex32& v : rf) {
        const double p = static_cast<double>(std::norm(v));
        if (p > 0.0) {
            sum += p;
            ++on;
        }
    }
    return on == 0 ? 0.0 : sum / static_cast<double>(on);
}

Expected<ModeSubject> ais() {
    auto taps = decode::design_from_response(kRate, 127, nfm_channel, nullptr);
    if (!taps) {
        return std::unexpected(taps.error());
    }
    ModeSubject mode;
    mode.mode = "ais";
    mode.subject = "ais Message 1 bursts, 9600 bit/s GMSK BT 0.4 as complex baseband at 48000 S/s with the "
                   "carrier off between them, through an 8 kHz nfm channel filter and FM discriminator; "
                   "packet error rate against SNR in 2500 Hz of the burst";
    mode.unit = "frame";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 4 * kAisReportBytes;
    mode.minimum_payload_bytes = kAisReportBytes;
    mode.payload_multiple = kAisReportBytes;
    mode.snr_start_db = 14.0;
    mode.snr_stop_db = 28.0;
    mode.trials = 256;
    mode.real_audio = false;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        siggen::AisModConfig mod;
        mod.rate = kRate;
        auto rf = siggen::ais_render(mod, ais_reports(payload));
        if (!rf) {
            return std::vector<dsp::Complex32>{};
        }
        auto noise = siggen::awgn_power_for(siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), burst_power(*rf), kRate);
        if (!noise || !siggen::add_awgn_at_power(dsp::ComplexSpan(*rf), *noise, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*rf);
    };
    mode.score = [filter = std::move(*taps)](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const auto sent = ais_reports(payload);
        auto decoder = decode::AisDecoder::create(decode::AisConfig{});
        std::vector<float> audio;
        if (samples.empty() || !decoder || !discriminate(samples, filter, audio)) {
            return score_units(sent.size(), 0);
        }
        std::vector<decode::AisMessage> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        std::vector<bool> seen(sent.size(), false);
        std::size_t good = 0;
        for (const decode::AisMessage& g : got) {
            for (std::size_t i = 0; i < sent.size(); ++i) {
                auto octets = siggen::ais_encode(sent[i]);
                if (!seen[i] && octets && g.octets == *octets) {
                    seen[i] = true;
                    ++good;
                    break;
                }
            }
        }
        return score_units(sent.size(), good);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        for (const decode::AisMessage& m : ais_reports(payload)) {
            auto octets = siggen::ais_encode(m);
            if (octets) {
                out += hex_lines(*octets, octets->size());
            }
        }
        return out;
    };
    return mode;
}

// ---------------------------------------------------------------------------
// DSC
// ---------------------------------------------------------------------------

// A routine individual call per 9 payload bytes, Table A1-4.9's "All mode
// RT": the called and calling identities from four bytes each and a working
// channel from the ninth: 19 information characters, 56 slots on the air
// with the phasing and the closing characters, 0.48 s with the dot pattern.
constexpr std::size_t kDscCallBytes = 9;

std::vector<decode::DscCall> dsc_calls(std::span<const std::uint8_t> payload) {
    std::vector<decode::DscCall> out;
    for (std::size_t at = 0; at + kDscCallBytes <= payload.size(); at += kDscCallBytes) {
        decode::DscCall c;
        c.format = decode::kDscFormatIndividual;
        c.address = read32(payload, at) % 1'000'000'000U;
        c.category = decode::kDscCategoryRoutine;
        c.self_id = read32(payload, at + 4) % 1'000'000'000U;
        c.telecommand1 = 100;
        c.telecommand2 = decode::kDscNoInformation;
        c.frequencies = {decode::DscFrequency{decode::DscFrequency::Kind::VhfChannel, 1U + payload[at + 8] % 88U, {}}};
        c.eos = decode::kDscEosAcknowledgeRq;
        out.push_back(c);
    }
    return out;
}

Expected<ModeSubject> dsc() {
    auto taps = decode::design_from_response(kRate, 127, nfm_channel, nullptr);
    if (!taps) {
        return std::unexpected(taps.error());
    }
    ModeSubject mode;
    mode.mode = "dsc";
    mode.subject = "dsc VHF individual calls, 1200 baud on 1300 and 2100 Hz tones phase modulated at index 2.0 "
                   "as complex baseband at 48000 S/s, through an 8 kHz nfm channel filter and FM discriminator; "
                   "call error rate against SNR in 2500 Hz";
    mode.unit = "message";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 4 * kDscCallBytes;
    mode.minimum_payload_bytes = kDscCallBytes;
    mode.payload_multiple = kDscCallBytes;
    mode.snr_start_db = 10.0;
    mode.snr_stop_db = 20.0;
    mode.trials = 256;
    mode.real_audio = false;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        std::vector<std::vector<std::uint8_t>> bits;
        for (const decode::DscCall& c : dsc_calls(payload)) {
            auto information = siggen::dsc_encode(c);
            if (!information) {
                return std::vector<dsp::Complex32>{};
            }
            bits.push_back(siggen::dsc_bits(*information));
        }
        siggen::DscModConfig mod;
        mod.rate = kRate;
        auto rf = siggen::dsc_render_baseband(mod, bits);
        if (!rf || !siggen::add_awgn(dsp::ComplexSpan(*rf), siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), kRate,
                                     seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*rf);
    };
    mode.score = [filter = std::move(*taps)](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const auto sent = dsc_calls(payload);
        auto decoder = decode::DscDecoder::create(decode::DscConfig{});
        std::vector<float> audio;
        if (samples.empty() || !decoder || !discriminate(samples, filter, audio)) {
            return score_units(sent.size(), 0);
        }
        std::vector<decode::DscCall> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        std::vector<bool> seen(sent.size(), false);
        std::size_t good = 0;
        for (const decode::DscCall& g : got) {
            for (std::size_t i = 0; i < sent.size(); ++i) {
                auto information = siggen::dsc_encode(sent[i]);
                if (!seen[i] && information && g.symbols == *information) {
                    seen[i] = true;
                    ++good;
                    break;
                }
            }
        }
        return score_units(sent.size(), good);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        for (const decode::DscCall& c : dsc_calls(payload)) {
            out += std::format("{:09} to {:09} channel {}\n", c.self_id, c.address.value_or(0),
                               c.frequencies.front().value);
        }
        return out;
    };
    return mode;
}

}  // namespace

Expected<ModeSubject> make_maritime_subject(std::string_view mode) {
    if (mode == "ais") {
        return ais();
    }
    if (mode == "dsc") {
        return dsc();
    }
    return fail(std::format("'{}' is not a maritime mode", mode));
}

}  // namespace revenant::bench::detail
