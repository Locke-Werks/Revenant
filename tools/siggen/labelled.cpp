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

namespace revenant::siggen_labelled {
namespace {

using dsp::Complex32;
using dsp::SampleRate;

constexpr SampleRate kVoiceRate = 48'000;
constexpr SampleRate kTetraRate = 72'000;

// Taps per polyphase branch of the interpolator that lifts each emitter onto
// the wideband rate, and its Kaiser beta. The same figures
// tests/engine/test_engine_probe.cpp's interpolator uses for its OFDM burst.
constexpr std::size_t kTapsPerPhase = 24;
constexpr double kKaiserBeta = 8.0;

struct Emitter {
    std::string name;
    dsp::Hertz offset_hz = 0;
    SampleRate rate = 0;
    std::vector<Complex32> samples;  // one loop of it, unit mean power
};

// Abramowitz and Stegun 9.6.12.
[[nodiscard]] double bessel_i0(double x) {
    const double quarter_square = 0.25 * x * x;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 256; ++k) {
        term *= quarter_square / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < 1e-18 * sum) {
            break;
        }
    }
    return sum;
}

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

// Gaussian noise band-limited to 300 to 3000 Hz at kVoiceRate, scaled to an
// RMS of 0.3 and clipped at one, which a modulator reads as full scale.
[[nodiscard]] std::vector<float> voice_shaped(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 engine(seed);
    std::vector<double> white(count);
    for (std::size_t n = 0; n < count; n += 2) {
        const double u1 = (static_cast<double>(engine() >> 11) + 1.0) / 9007199254740993.0;
        const double u2 = static_cast<double>(engine() >> 11) / 9007199254740992.0;
        const double radius = std::sqrt(-2.0 * std::log(u1));
        white[n] = radius * std::cos(2.0 * std::numbers::pi * u2);
        if (n + 1 < count) {
            white[n + 1] = radius * std::sin(2.0 * std::numbers::pi * u2);
        }
    }
    const auto high = low_pass(96, 3000.0 / kVoiceRate);
    const auto low = low_pass(96, 300.0 / kVoiceRate);
    std::vector<double> band(count, 0.0);
    for (std::size_t n = 0; n < count; ++n) {
        double acc = 0.0;
        for (int k = -96; k <= 96; ++k) {
            const auto j = static_cast<std::ptrdiff_t>(n) + k;
            if (j < 0 || j >= static_cast<std::ptrdiff_t>(count)) {
                continue;
            }
            const auto t = static_cast<std::size_t>(k + 96);
            acc += (high[t] - low[t]) * white[static_cast<std::size_t>(j)];
        }
        band[n] = acc;
    }
    double power = 0.0;
    for (const double v : band) {
        power += v * v;
    }
    const double scale = 0.3 / std::sqrt(power / static_cast<double>(count));
    std::vector<float> out(count);
    for (std::size_t n = 0; n < count; ++n) {
        out[n] = static_cast<float>(std::clamp(band[n] * scale, -1.0, 1.0));
    }
    return out;
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

void normalise(std::vector<Complex32>& samples) {
    double power = 0.0;
    for (const Complex32 sample : samples) {
        power += static_cast<double>(std::norm(sample));
    }
    power /= static_cast<double>(std::max<std::size_t>(samples.size(), 1));
    if (!(power > 0.0)) {
        return;
    }
    const auto scale = static_cast<float>(1.0 / std::sqrt(power));
    for (Complex32& sample : samples) {
        sample *= scale;
    }
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

    for (Emitter& emitter : out) {
        normalise(emitter.samples);
    }
    return out;
}

// One emitter's interpolator, lifting its loop by `factor` with a Kaiser
// windowed sinc cut at 0.45 of its own rate, and its mixer to the offset.
struct Lift {
    const Emitter* emitter = nullptr;
    std::size_t factor = 1;
    std::vector<float> taps;  // kTapsPerPhase * factor, scaled so power is kept
    double gain = 1.0;
};

[[nodiscard]] Lift make_lift(const Emitter& emitter, double gain) {
    Lift lift;
    lift.emitter = &emitter;
    lift.factor = static_cast<std::size_t>(kLabelledRate / emitter.rate);
    lift.gain = gain;
    const std::size_t length = kTapsPerPhase * lift.factor;
    const double centre = (static_cast<double>(length) - 1.0) / 2.0;
    const double cutoff = 0.45 * static_cast<double>(emitter.rate) / kLabelledRate;
    const double i0 = bessel_i0(kKaiserBeta);
    std::vector<double> taps(length);
    double sum = 0.0;
    for (std::size_t i = 0; i < length; ++i) {
        const double position = static_cast<double>(i) - centre;
        const double ratio = 2.0 * static_cast<double>(i) / static_cast<double>(length - 1) - 1.0;
        const double window =
            bessel_i0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - ratio * ratio))) / i0;
        const double x = 2.0 * cutoff * position;
        const double sinc = x == 0.0 ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
        taps[i] = 2.0 * cutoff * sinc * window;
        sum += taps[i];
    }
    const double scale = static_cast<double>(lift.factor) / sum;
    lift.taps.resize(length);
    for (std::size_t i = 0; i < length; ++i) {
        lift.taps[i] = static_cast<float>(taps[i] * scale);
    }
    return lift;
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
    const double noise_power = std::pow(10.0, spec.noise_dbfs / 10.0);
    const double noise_in_reference = noise_power * 2500.0 / static_cast<double>(kLabelledRate);
    const double signal_power = noise_in_reference * std::pow(10.0, spec.snr_2500_db / 10.0);
    std::vector<Lift> lifts;
    for (const Emitter& emitter : *emitters) {
        lifts.push_back(make_lift(emitter, std::sqrt(signal_power)));
    }

    std::ofstream stream(spec.out_path, std::ios::binary);
    if (!stream) {
        return fail(std::format("could not open '{}' for writing", spec.out_path));
    }

    const auto total = static_cast<std::uint64_t>(spec.seconds * kLabelledRate);
    constexpr std::size_t kBlock = 1U << 16;
    std::vector<std::complex<float>> block(kBlock);
    std::mt19937_64 noise_engine(siggen::derive_seed(spec.seed, 0x4E015E));
    const double sigma = std::sqrt(noise_power / 2.0);

    for (std::uint64_t first = 0; first < total; first += kBlock) {
        const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(kBlock, total - first));
        for (std::size_t n = 0; n < count; n += 2) {
            const double u1 = (static_cast<double>(noise_engine() >> 11) + 1.0) / 9007199254740993.0;
            const double u2 = static_cast<double>(noise_engine() >> 11) / 9007199254740992.0;
            const double radius = sigma * std::sqrt(-2.0 * std::log(u1));
            block[n] = {static_cast<float>(radius * std::cos(2.0 * std::numbers::pi * u2)),
                        static_cast<float>(radius * std::sin(2.0 * std::numbers::pi * u2))};
            if (n + 1 < count) {
                const double u3 = (static_cast<double>(noise_engine() >> 11) + 1.0) / 9007199254740993.0;
                const double u4 = static_cast<double>(noise_engine() >> 11) / 9007199254740992.0;
                const double r2 = sigma * std::sqrt(-2.0 * std::log(u3));
                block[n + 1] = {static_cast<float>(r2 * std::cos(2.0 * std::numbers::pi * u4)),
                                static_cast<float>(r2 * std::sin(2.0 * std::numbers::pi * u4))};
            }
        }

        for (const Lift& lift : lifts) {
            const Emitter& emitter = *lift.emitter;
            const std::size_t loop = emitter.samples.size();
            const std::size_t length = lift.taps.size();
            for (std::size_t n = 0; n < count; ++n) {
                const std::uint64_t m = first + n;
                // y[m] = sum over input k of x[k] h[m - k L], h of `length`
                // taps, so k runs from ceil((m - length + 1) / L) to m / L.
                const std::uint64_t k_high = m / lift.factor;
                std::complex<float> acc(0.0F, 0.0F);
                for (std::uint64_t k = k_high + 1; k-- > 0;) {
                    const std::uint64_t offset = m - k * lift.factor;
                    if (offset >= length) {
                        break;
                    }
                    acc += lift.taps[offset] * emitter.samples[static_cast<std::size_t>(k % loop)];
                }
                // Integer reduction of the mixer's phase, so it does not
                // drift over a long capture.
                std::int64_t turns = (emitter.offset_hz * static_cast<std::int64_t>(m)) %
                                     static_cast<std::int64_t>(kLabelledRate);
                if (turns < 0) {
                    turns += kLabelledRate;
                }
                const double angle = 2.0 * std::numbers::pi * static_cast<double>(turns) /
                                     static_cast<double>(kLabelledRate);
                const std::complex<float> mixer(static_cast<float>(std::cos(angle)),
                                                static_cast<float>(std::sin(angle)));
                block[n] += static_cast<float>(lift.gain) * acc * mixer;
            }
        }

        stream.write(reinterpret_cast<const char*>(block.data()),
                     static_cast<std::streamsize>(count * sizeof(std::complex<float>)));
        if (!stream) {
            return fail(std::format("writing '{}' failed", spec.out_path));
        }
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
