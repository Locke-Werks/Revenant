// `siggen labelled`. tools/siggen/labelled.h has the scene and why.

#include "tools/siggen/labelled.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <format>
#include <fstream>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/dmr.h"
#include "core/decode/dstar.h"
#include "core/decode/m17.h"
#include "core/decode/p25p1.h"
#include "core/decode/tetra.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/dmr_mod.h"
#include "core/dsp/synth/dv_mod.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/m17_mod.h"
#include "core/dsp/synth/modulators.h"
#include "tools/siggen/lift.h"
#include "tools/siggen/speech.h"

namespace revenant::siggen_labelled {
namespace {

using dsp::Complex32;
using dsp::SampleRate;

constexpr SampleRate kVoiceRate = 48'000;
constexpr SampleRate kTetraRate = 72'000;

// One loop of each emitter at its own rate; tools/siggen/lift.h lifts them.
using Emitter = siggen_lift::LiftedEmitter;

// A windowed-sinc low pass, `half` taps either side of centre, cutoff as a
// fraction of the rate, unity gain at DC.
[[nodiscard]] std::vector<double> low_pass(int half, double cutoff) {
    std::vector<double> taps(static_cast<std::size_t>(2 * half + 1));
    double sum = 0.0;
    for (int k = -half; k <= half; ++k) {
        const double x = 2.0 * cutoff * static_cast<double>(k);
        const double sinc = k == 0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        const double window = 0.54 + 0.46 * std::cos(std::numbers::pi * k / half);
        taps[static_cast<std::size_t>(k + half)] = sinc * window;
        sum += sinc * window;
    }
    for (double& tap : taps) {
        tap /= sum;
    }
    return taps;
}

// Real audio around `centre_hz` moved to DC as complex baseband, with the
// image at twice the centre filtered off. How a probe centred on an RTTY
// signal sees it.
[[nodiscard]] std::vector<Complex32> audio_to_baseband(const std::vector<float>& audio,
                                                       SampleRate rate, double centre_hz) {
    const auto taps = low_pass(128, 600.0 / static_cast<double>(rate));
    std::vector<std::complex<double>> mixed(audio.size());
    const double step = -2.0 * std::numbers::pi * centre_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        const double phase = std::fmod(step * static_cast<double>(n), 2.0 * std::numbers::pi);
        mixed[n] = 2.0 * static_cast<double>(audio[n]) * std::polar(1.0, phase);
    }
    std::vector<Complex32> out(audio.size());
    for (std::size_t n = 0; n < audio.size(); ++n) {
        std::complex<double> acc(0.0, 0.0);
        for (int k = -128; k <= 128; ++k) {
            const auto j = static_cast<std::ptrdiff_t>(n) + k;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(audio.size())) {
                continue;
            }
            acc += taps[static_cast<std::size_t>(k + 128)] * mixed[static_cast<std::size_t>(j)];
        }
        out[n] = Complex32(static_cast<float>(acc.real()), static_cast<float>(acc.imag()));
    }
    return out;
}

// Audio frequency-modulated onto a carrier at DC.
[[nodiscard]] std::vector<Complex32> fm(const std::vector<float>& audio, SampleRate rate,
                                        double deviation_hz) {
    std::vector<Complex32> out(audio.size());
    double phase = 0.0;
    const double scale = 2.0 * std::numbers::pi * deviation_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < audio.size(); ++n) {
        phase = std::fmod(phase + scale * static_cast<double>(audio[n]), 2.0 * std::numbers::pi);
        out[n] = Complex32(static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase)));
    }
    return out;
}

// Speech-shaped audio at kVoiceRate: syllables, pauses and a pitch, from
// tools/siggen/speech.h.
[[nodiscard]] std::vector<float> voice_shaped(std::size_t count, std::uint64_t seed) {
    siggen_speech::SpeechSpec speech;
    speech.rate = kVoiceRate;
    speech.samples = count;
    speech.seed = seed;
    return siggen_speech::synthesise_speech(speech).audio;
}

template <typename T>
[[nodiscard]] Expected<T> must(Expected<T> value, std::string_view what) {
    if (!value) {
        return std::unexpected(with_context(value.error(), std::string(what)));
    }
    return value;
}

[[nodiscard]] Expected<std::vector<Emitter>> build_emitters(const LabelledSceneSpec& spec) {
    std::vector<Emitter> out;
    const auto voice_count = static_cast<std::size_t>(spec.seconds * kVoiceRate);
    std::uint64_t salt = 1;
    const auto next_seed = [&] { return siggen::derive_seed(spec.seed, salt++); };

    siggen::ModulatorConfig common;
    common.rate = kVoiceRate;

    {
        common.seed = next_seed();
        siggen::AmParams am;
        am.modulation_index = 0.8;
        auto made = must(siggen::generate_am(common, am, voice_count,
                                             voice_shaped(voice_count, next_seed())),
                         "the AM emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"AM", -800'000, kVoiceRate, std::move(made->samples)});
    }
    {
        common.seed = next_seed();
        siggen::NfmParams nfm;
        nfm.deviation = 2500;
        auto made = must(siggen::generate_nfm(common, nfm, voice_count,
                                              voice_shaped(voice_count, next_seed())),
                         "the NFM emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"NFM", -600'000, kVoiceRate, std::move(made->samples)});
    }
    {
        common.seed = next_seed();
        siggen::CwParams cw;
        cw.words_per_minute = 20.0;
        auto made = must(siggen::generate_cw(common, cw, voice_count), "the CW emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"CW", -450'000, kVoiceRate, std::move(made->samples)});
    }
    {
        common.seed = next_seed();
        siggen::PskParams psk;
        psk.symbol_rate = 2400.0;
        psk.rolloff = 0.35;
        psk.symbol_count = static_cast<std::size_t>(spec.seconds * psk.symbol_rate) + 16;
        auto made = must(siggen::generate_bpsk(common, psk, voice_count), "the BPSK emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"BPSK", -300'000, kVoiceRate, std::move(made->samples)});
    }
    {
        // Twenty LDUs of voice, looped. The IMBE frames are pseudorandom
        // bits: the decoder counts what the framing verifies, and nothing
        // here is to be listened to.
        siggen::P25VoiceMessage message;
        std::mt19937_64 bits(next_seed());
        for (std::size_t frame = 0; frame < 9 * 20; ++frame) {
            std::array<std::uint8_t, decode::kP25VoiceFrameBits> voice{};
            for (std::uint8_t& bit : voice) {
                bit = static_cast<std::uint8_t>(bits() & 1U);
            }
            message.voice.push_back(voice);
        }
        auto dibits = must(siggen::p25_voice_message_dibits(message), "the P25 message");
        if (!dibits) {
            return std::unexpected(dibits.error());
        }
        siggen::P25ModConfig mod;
        mod.rate = kVoiceRate;
        auto made = must(siggen::p25_render_dibits(mod, *dibits), "the P25 emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"P25", -150'000, kVoiceRate, std::move(*made)});
    }
    {
        siggen::DStarMessage message;
        message.header.own_callsign = "N0CALL";
        message.header.companion = "CQCQCQ";
        message.header.destination_repeater = "DIRECT";
        message.header.departure_repeater = "DIRECT";
        message.header.own_suffix = "TEST";
        std::mt19937_64 bits(next_seed());
        for (std::size_t frame = 0; frame < 84; ++frame) {
            std::array<std::uint8_t, decode::kDStarVoiceBits> voice{};
            for (std::uint8_t& bit : voice) {
                bit = static_cast<std::uint8_t>(bits() & 1U);
            }
            message.voice_frames.push_back(voice);
        }
        siggen::DStarModConfig mod;
        mod.rate = kVoiceRate;
        auto made = must(siggen::dstar_render(mod, message), "the D-STAR emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"D-STAR", 100'000, kVoiceRate, std::move(*made)});
    }
    {
        // Continuous synchronisation bursts as one differential stream, the
        // way tests/decode/test_tetra.cpp renders them.
        decode::TetraSyncPdu pdu;
        pdu.colour_code = 5;
        pdu.mobile_country_code = 234;
        pdu.mobile_network_code = 14;
        std::vector<std::uint8_t> bits;
        const std::uint64_t filler = next_seed();
        for (std::uint64_t burst = 0; burst < 64; ++burst) {
            pdu.frame_number = static_cast<std::uint8_t>(1 + burst % 18);
            auto one = must(siggen::tetra_sync_burst_bits(pdu, filler + burst), "a TETRA burst");
            if (!one) {
                return std::unexpected(one.error());
            }
            bits.insert(bits.end(), one->begin(), one->end());
        }
        siggen::TetraModConfig mod;
        mod.rate = kTetraRate;
        auto made = must(siggen::tetra_render_bits(mod, bits), "the TETRA emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"TETRA", 250'000, kTetraRate, std::move(*made)});
    }
    {
        siggen::M17StreamMessage message;
        message.destination = 0xFFFFFFFFFFFFULL;
        message.source = 0x00000061D0D1ULL;
        for (std::size_t frame = 0; frame < 60; ++frame) {
            std::array<std::uint8_t, decode::kM17StreamPayloadBytes> payload{};
            payload[0] = static_cast<std::uint8_t>(frame);
            message.payloads.push_back(payload);
        }
        siggen::M17ModConfig mod;
        mod.rate = kVoiceRate;
        auto made = must(siggen::m17_render_stream(mod, message), "the M17 emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"M17", 400'000, kVoiceRate, std::move(*made)});
    }
    {
        siggen::Ax25ModConfig mod;
        mod.rate = kVoiceRate;
        siggen::Ax25FrameSpec frame;
        frame.destination = decode::Ax25Address{"APRS", 0};
        frame.source = decode::Ax25Address{"N0CALL", 7};
        const std::string text = "!4903.50N/07201.75W-labelled scene";
        frame.information.assign(text.begin(), text.end());
        auto octets = must(siggen::ax25_frame_octets(frame), "the AX.25 frame");
        if (!octets) {
            return std::unexpected(octets.error());
        }
        const std::vector<std::vector<std::uint8_t>> frames(4, *octets);
        auto audio = must(siggen::ax25_render(mod, frames), "the AX.25 audio");
        if (!audio) {
            return std::unexpected(audio.error());
        }
        out.push_back({"AX.25", 550'000, kVoiceRate, fm(*audio, kVoiceRate, 6000.0)});
    }
    {
        siggen::RttyModConfig mod;
        mod.rate = kVoiceRate;
        auto combinations =
            must(siggen::ita2_encode_text(U"RYRYRY CQ CQ DE N0CALL THE QUICK BROWN FOX 73 "),
                 "the RTTY text");
        if (!combinations) {
            return std::unexpected(combinations.error());
        }
        auto audio = must(siggen::rtty_render(mod, *combinations), "the RTTY audio");
        if (!audio) {
            return std::unexpected(audio.error());
        }
        out.push_back({"RTTY", 700'000, kVoiceRate, audio_to_baseband(*audio, kVoiceRate, 2210.0)});
    }
    {
        // A base station's channel with nothing to say: every slot an idle
        // burst behind its CACH, which is what a repeater keys up to between
        // calls, from the transmitter tests/decode/test_dmr.cpp uses.
        std::vector<siggen::DmrSlot> slots;
        const std::array<std::uint8_t, decode::kDmrCachPayloadBits> payload{};
        for (std::size_t i = 0; i < 144; ++i) {
            siggen::DmrSlot slot;
            slot.cach = siggen::dmr_cach(false, i % 2 == 0 ? 1 : 2, 0, payload);
            slot.burst = siggen::dmr_idle_burst(decode::DmrSyncType::BsData, 1);
            slots.push_back(slot);
        }
        siggen::DmrModConfig mod;
        mod.rate = kVoiceRate;
        auto made = must(siggen::dmr_render_slots(mod, slots), "the DMR emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"DMR", -950'000, kVoiceRate, std::move(*made)});
    }
    {
        common.seed = next_seed();
        siggen::SsbParams ssb;
        auto made = must(siggen::generate_ssb(common, ssb, true, voice_count,
                                              voice_shaped(voice_count, next_seed())),
                         "the USB emitter");
        if (!made) {
            return std::unexpected(made.error());
        }
        out.push_back({"USB", 850'000, kVoiceRate, std::move(made->samples)});
    }

    return out;
}

}  // namespace

Status render_labelled_scene(const LabelledSceneSpec& spec) {
    if (spec.out_path.empty()) {
        return fail("--out is required");
    }
    if (!(spec.seconds > 0.0) || spec.seconds > 600.0) {
        return fail("--seconds must be between 0 and 600");
    }

    auto emitters = build_emitters(spec);
    if (!emitters) {
        return std::unexpected(emitters.error());
    }

    // Every emitter at the same SNR in 2500 Hz against the noise's density.
    for (Emitter& emitter : *emitters) {
        emitter.snr_2500_db = spec.snr_2500_db;
    }

    std::ofstream stream(spec.out_path, std::ios::binary);
    if (!stream) {
        return fail(std::format("could not open '{}' for writing", spec.out_path));
    }

    siggen_lift::LiftSpec lift;
    lift.rate = kLabelledRate;
    lift.total_samples = static_cast<std::uint64_t>(spec.seconds * kLabelledRate);
    lift.seed = spec.seed;
    lift.noise_dbfs = spec.noise_dbfs;
    auto rendered = siggen_lift::render_lifted(
        *emitters, lift, [&](std::span<const std::complex<float>> block) -> Status {
            stream.write(reinterpret_cast<const char*>(block.data()),
                         static_cast<std::streamsize>(block.size() * sizeof(std::complex<float>)));
            if (!stream) {
                return fail(std::format("writing '{}' failed", spec.out_path));
            }
            return {};
        });
    if (!rendered) {
        return rendered;
    }

    if (!spec.truth_path.empty()) {
        std::ofstream truth(spec.truth_path);
        for (const Emitter& emitter : *emitters) {
            truth << std::format("{} {} {} {}\n", emitter.name, emitter.offset_hz, emitter.rate,
                                 emitter.samples.size());
        }
    }
    std::printf("siggen labelled: %zu emitters, %.1f s at %lld S/s, %.1f dB in 2500 Hz each\n",
                emitters->size(), spec.seconds, static_cast<long long>(kLabelledRate),
                spec.snr_2500_db);
    return {};
}

}  // namespace revenant::siggen_labelled
