// `siggen voice`. tools/siggen/voice.h has the scene and why.

#include "tools/siggen/voice.h"

#include <cmath>
#include <complex>
#include <cstdio>
#include <format>
#include <fstream>
#include <span>

#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/modulators.h"
#include "tools/siggen/speech.h"

namespace revenant::siggen_voice {
namespace {

constexpr dsp::SampleRate kEmitterRate = 48'000;

[[nodiscard]] std::vector<float> speech(std::size_t count, std::uint64_t seed) {
    siggen_speech::SpeechSpec spec;
    spec.rate = kEmitterRate;
    spec.samples = count;
    spec.seed = seed;
    return siggen_speech::synthesise_speech(spec).audio;
}

}  // namespace

Expected<VoiceScene> build_voice_scene(const VoiceSceneSpec& spec) {
    if (!(spec.seconds > 0.0) || spec.seconds > 600.0) {
        return fail("the voice scene's length must be between 0 and 600 seconds");
    }
    VoiceScene out;
    const auto count = static_cast<std::size_t>(spec.seconds * kEmitterRate);
    std::uint64_t salt = 1;
    const auto next_seed = [&] { return siggen::derive_seed(spec.seed, salt++); };

    siggen::ModulatorConfig common;
    common.rate = kEmitterRate;

    const auto add = [&](std::string name, dsp::Hertz offset, Expected<siggen::GeneratedSignal> made,
                         dsp::Hertz low, dsp::Hertz high, std::string label) -> Status {
        if (!made) {
            return std::unexpected(with_context(made.error(), std::format("the {} emitter", name)));
        }
        siggen_lift::LiftedEmitter emitter;
        emitter.name = name;
        emitter.offset_hz = offset;
        emitter.rate = kEmitterRate;
        emitter.samples = std::move(made->samples);
        emitter.snr_2500_db = spec.snr_2500_db;
        out.emitters.push_back(std::move(emitter));
        out.truth.push_back({std::move(name), offset, offset + low, offset + high, std::move(label)});
        return {};
    };

    {
        common.seed = next_seed();
        siggen::AmParams am;
        am.modulation_index = 0.8;
        if (auto s = add("am", -450'000, siggen::generate_am(common, am, count, speech(count, next_seed())),
                         -3'000, 3'000, "AM");
            !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::NfmParams nfm;
        nfm.deviation = 2'500;
        if (auto s = add("nfm2.5", -300'000,
                         siggen::generate_nfm(common, nfm, count, speech(count, next_seed())),
                         -5'500, 5'500, "NFM");
            !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::NfmParams nfm;
        nfm.deviation = 5'000;
        if (auto s = add("nfm5", -150'000,
                         siggen::generate_nfm(common, nfm, count, speech(count, next_seed())),
                         -8'000, 8'000, "NFM");
            !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::SsbParams ssb;
        if (auto s = add("usb", 50'000,
                         siggen::generate_ssb(common, ssb, true, count, speech(count, next_seed())),
                         300, 3'000, "USB");
            !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::SsbParams ssb;
        if (auto s = add("lsb", 200'000,
                         siggen::generate_ssb(common, ssb, false, count, speech(count, next_seed())),
                         -3'000, -300, "LSB");
            !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::CwParams cw;
        cw.words_per_minute = 20.0;
        if (auto s = add("cw", 300'000, siggen::generate_cw(common, cw, count), -100, 100, "CW"); !s) {
            return std::unexpected(s.error());
        }
    }
    {
        common.seed = next_seed();
        siggen::PskParams psk;
        psk.symbol_rate = 1200.0;
        psk.rolloff = 0.35;
        psk.symbol_count = static_cast<std::size_t>(spec.seconds * psk.symbol_rate) + 16;
        if (auto s = add("bpsk", 400'000, siggen::generate_bpsk(common, psk, count), -810, 810,
                         "BPSK");
            !s) {
            return std::unexpected(s.error());
        }
    }
    out.truth.push_back({"empty", 500'000, 498'500, 501'500, ""});
    return out;
}

Status render_voice_scene(const VoiceSceneSpec& spec, const siggen_lift::BlockSink& sink) {
    auto scene = build_voice_scene(spec);
    if (!scene) {
        return std::unexpected(scene.error());
    }
    siggen_lift::LiftSpec lift;
    lift.rate = kVoiceSceneRate;
    lift.total_samples = static_cast<std::uint64_t>(std::llround(spec.seconds * kVoiceSceneRate));
    lift.seed = spec.seed;
    lift.noise_dbfs = spec.noise_dbfs;
    return siggen_lift::render_lifted(scene->emitters, lift, sink);
}

Status write_voice_scene(const VoiceSceneSpec& spec, const std::string& out_path,
                         const std::string& truth_path) {
    if (out_path.empty()) {
        return fail("--out is required");
    }
    std::ofstream stream(out_path, std::ios::binary);
    if (!stream) {
        return fail(std::format("could not open '{}' for writing", out_path));
    }
    auto rendered = render_voice_scene(spec, [&](std::span<const std::complex<float>> block) -> Status {
        stream.write(reinterpret_cast<const char*>(block.data()),
                     static_cast<std::streamsize>(block.size() * sizeof(std::complex<float>)));
        if (!stream) {
            return fail(std::format("writing '{}' failed", out_path));
        }
        return {};
    });
    if (!rendered) {
        return rendered;
    }
    if (!truth_path.empty()) {
        auto scene = build_voice_scene(spec);
        if (!scene) {
            return std::unexpected(scene.error());
        }
        std::ofstream truth(truth_path);
        for (const VoiceTruth& row : scene->truth) {
            truth << std::format("{} {} {} {} {}\n", row.name, row.offset_hz, row.low_hz, row.high_hz,
                                 row.label.empty() ? "-" : row.label);
        }
    }
    std::printf("siggen voice: %.1f s at %lld S/s, %.1f dB in 2500 Hz each\n", spec.seconds,
                static_cast<long long>(kVoiceSceneRate), spec.snr_2500_db);
    return {};
}

}  // namespace revenant::siggen_voice
