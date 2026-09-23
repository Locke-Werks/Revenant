// Floating-point discipline first, for the reason sweep.cpp gives: a stored
// curve is evidence of a regression only if the same source gives the same
// numbers on another host compiler.
#include "core/dsp/reference_fp.h"

#include "tools/bench/rds_subject.h"

#include <algorithm>
#include <span>
#include <vector>

#include "core/decode/rds_bits.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/rds_mod.h"

namespace revenant::bench {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/rds_subject.cpp must include core/dsp/reference_fp.h first");

namespace {

// How much of the recovered stream the alignment search scores. The same
// figure tests/decode/test_rds_bits.cpp uses, and for its reason: at a BER of
// 0.1 the right offset still beats every wrong one over 256 bits by a factor
// of five, and aligning on the whole stream would be circular.
constexpr std::size_t kAlignProbeBits = 256;

std::vector<std::uint8_t> payload_bits(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bits(payload.size() * 8);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = payload_bit(payload, i) ? 1 : 0;
    }
    return bits;
}

struct Alignment {
    bool found = false;
    std::size_t compared = 0;
    std::size_t errors = 0;
};

// The recovered stream is a contiguous run of the sent one starting wherever
// the decoder locked, so the offset is searched for rather than assumed.
Alignment align(std::span<const std::uint8_t> sent, std::span<const std::uint8_t> recovered) {
    Alignment best;
    const std::size_t window = std::min({kAlignProbeBits, recovered.size(), sent.size()});
    if (window == 0) {
        return best;
    }
    std::size_t best_errors = window + 1;
    std::size_t best_offset = 0;
    for (std::size_t offset = 0; offset + window <= sent.size(); ++offset) {
        std::size_t errors = 0;
        for (std::size_t i = 0; i < window; ++i) {
            errors += recovered[i] != sent[offset + i] ? 1U : 0U;
        }
        if (errors < best_errors) {
            best_errors = errors;
            best_offset = offset;
            best.found = true;
        }
    }
    if (!best.found) {
        return best;
    }
    best.compared = std::min(recovered.size(), sent.size() - best_offset);
    for (std::size_t i = 0; i < best.compared; ++i) {
        best.errors += recovered[i] != sent[best_offset + i] ? 1U : 0U;
    }
    return best;
}

siggen::RdsModSpec spec_for(const RdsSubjectConfig& config, std::span<const std::uint8_t> payload) {
    siggen::RdsModSpec spec;
    spec.rate = config.rate;
    spec.rds_deviation_hz = config.rds_deviation_hz;
    spec.mono_tone_hz = config.mono_tone_hz;
    spec.mono_deviation_hz = config.mono_deviation_hz;
    spec.bits = payload_bits(payload);
    return spec;
}

}  // namespace

Generator make_rds_generator(RdsSubjectConfig config) {
    return [config](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        std::vector<dsp::Complex32> out;
        auto composite = siggen::generate_rds(spec_for(config, payload));
        if (!composite) {
            return out;
        }
        std::vector<float>& samples = composite->samples;
        const auto level = siggen::NoiseLevel::eb_over_n0_db(snr_db, decode::kBitRateHz);
        if (!siggen::add_real_awgn(dsp::RealSpan(samples), composite->rds_mean_power, level, config.rate, seed)) {
            return out;
        }
        // The harness moves complex baseband. The composite is real, so it
        // rides in the real part and the subject reads only that.
        out.reserve(samples.size());
        for (const float sample : samples) {
            out.emplace_back(sample, 0.0F);
        }
        return out;
    };
}

Subject make_rds_subject(RdsSubjectConfig config) {
    return [config](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::vector<std::uint8_t> sent = payload_bits(payload);

        TrialResult failed;
        failed.bits_total = sent.size();
        failed.bits_wrong = sent.size() / 2;
        failed.decoded = false;

        if (samples.empty()) {
            return failed;
        }

        std::vector<float> composite(samples.size());
        for (std::size_t i = 0; i < samples.size(); ++i) {
            composite[i] = samples[i].real();
        }

        decode::RdsBitsConfig decoder_config;
        decoder_config.rate = config.rate;
        auto sync = decode::RdsBitSync::create(decoder_config);
        if (!sync) {
            return failed;
        }
        sync->process(dsp::ConstRealSpan(composite));
        const std::vector<std::uint8_t> recovered = sync->drain();

        const Alignment match = align(sent, recovered);
        if (!match.found || match.compared < sent.size() / 4) {
            return failed;
        }

        TrialResult result;
        result.bits_total = match.compared;
        result.bits_wrong = match.errors;
        result.decoded = true;
        return result;
    };
}

}  // namespace revenant::bench
