// Protocol identification. core/identify/identify.h has the rule and the
// count each row needs; this file is the rows.

#include "core/identify/identify.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <format>
#include <numbers>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/cw.h"
#include "core/decode/dstar.h"
#include "core/decode/dv_phy.h"
#include "core/decode/m17.h"
#include "core/decode/p25p1.h"
#include "core/decode/pocsag.h"
#include "core/decode/psk31.h"
#include "core/decode/rtty.h"
#include "core/decode/sitor_b.h"
#include "core/decode/tetra.h"

namespace revenant::identify {
namespace {

using characterise::ModulationFamily;

// The verifications each row needs. identify.h says why each is what it is.
constexpr std::uint32_t kP25Required = 2;
constexpr std::uint32_t kDStarRequired = 2;
constexpr std::uint32_t kTetraRequired = 1;
constexpr std::uint32_t kM17Required = 2;
constexpr std::uint32_t kPocsagRequired = 2;
constexpr std::uint32_t kAx25Required = 1;
constexpr std::uint32_t kRttyRequired = 10;
constexpr std::uint32_t kSitorRequired = 8;
constexpr std::uint32_t kPsk31Required = 6;
constexpr std::uint32_t kCwRequired = 4;

// The RTTY row's reading of a clean character, and the share of a run's
// characters that has to reach it. attempt_rtty has the measurement.
constexpr float kRttyMargin = 0.5F;
constexpr double kRttyCleanShare = 0.8;

// Where each audio-domain decoder expects its signal in the passband, as the
// receivers it was written for put it there: rtty.h's mark 2125 with 170 Hz
// shift centres on 2210, sitor_b.h's clause 1.3 centre is 1700, psk31.h's
// default tone is 1000 and cw.h's is 700.
constexpr double kRttyCentreHz = 2125.0 + 85.0;
constexpr double kSitorCentreHz = 1700.0;
constexpr double kPsk31CentreHz = 1000.0;
constexpr double kCwCentreHz = 700.0;

[[nodiscard]] bool family_is(ModulationFamily family, std::initializer_list<ModulationFamily> any) {
    return std::find(any.begin(), any.end(), family) != any.end();
}

// The upper sideband receiver's audio for a signal centred at DC in the
// extract: move it up to `centre_hz` and keep the real part, which is what a
// USB demodulator does to a signal that far above its dial frequency.
[[nodiscard]] std::vector<float> sideband_audio(dsp::ConstComplexSpan samples,
                                                dsp::SampleRate rate, double centre_hz) {
    std::vector<float> out(samples.size());
    const double step = 2.0 * std::numbers::pi * centre_hz / static_cast<double>(rate);
    for (std::size_t n = 0; n < samples.size(); ++n) {
        // Reduced per sample from an integer index so the phase does not
        // accumulate rounding over a long extract.
        const double phase = std::fmod(step * static_cast<double>(n), 2.0 * std::numbers::pi);
        const std::complex<double> moved =
            std::complex<double>(samples[n].real(), samples[n].imag()) * std::polar(1.0, phase);
        out[n] = static_cast<float>(moved.real());
    }
    return out;
}

// An FM receiver's audio: the discriminator core/decode already uses for P25
// and D-STAR, whose output scale the audio decoders do not care about.
[[nodiscard]] Expected<std::vector<float>> fm_audio(dsp::ConstComplexSpan samples,
                                                    dsp::SampleRate rate) {
    std::vector<float> out(samples.size());
    if (auto ran = decode::fm_discriminate(samples, dsp::RealSpan(out), rate); !ran) {
        return std::unexpected(ran.error());
    }
    return out;
}

struct Counted {
    std::uint32_t verified = 0;
};

// --- the rows ---------------------------------------------------------------

[[nodiscard]] Expected<Counted> attempt_p25(dsp::ConstComplexSpan samples, dsp::SampleRate rate) {
    decode::P25Config config;
    config.rate = rate;
    auto decoder = decode::P25Phase1::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::P25Frame> frames;
    if (auto ran = decoder->process(samples, frames); !ran) {
        return std::unexpected(ran.error());
    }
    // P25Phase1 appends a data unit only once its sync correlated and its NID
    // decoded, which is the check this row counts.
    return Counted{.verified = static_cast<std::uint32_t>(frames.size())};
}

[[nodiscard]] Expected<Counted> attempt_dstar(dsp::ConstComplexSpan samples,
                                              dsp::SampleRate rate) {
    decode::DStarConfig config;
    config.rate = rate;
    auto decoder = decode::DStar::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::DStarTransmission> pieces;
    if (auto ran = decoder->process(samples, pieces); !ran) {
        return std::unexpected(ran.error());
    }
    decoder->flush(pieces);
    Counted counted;
    for (const decode::DStarTransmission& piece : pieces) {
        if (piece.header.has_value() && piece.header->fcs_valid) {
            counted.verified += 2;
        }
        for (const decode::DStarVoiceFrame& frame : piece.frames) {
            if (frame.carried_resync) {
                ++counted.verified;
            }
        }
    }
    return counted;
}

[[nodiscard]] Expected<Counted> attempt_tetra(dsp::ConstComplexSpan samples,
                                              dsp::SampleRate rate) {
    decode::TetraConfig config;
    config.rate = rate;
    auto decoder = decode::Tetra::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::TetraBurst> bursts;
    if (auto ran = decoder->process(samples, bursts); !ran) {
        return std::unexpected(ran.error());
    }
    Counted counted;
    for (const decode::TetraBurst& burst : bursts) {
        if (burst.block_code_verified) {
            ++counted.verified;
        }
    }
    return counted;
}

[[nodiscard]] Expected<Counted> attempt_m17(dsp::ConstComplexSpan samples, dsp::SampleRate rate) {
    decode::M17Config config;
    config.rate = rate;
    auto decoder = decode::M17::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::M17Frame> frames;
    if (auto ran = decoder->process(samples, frames); !ran) {
        return std::unexpected(ran.error());
    }
    Counted counted;
    for (const decode::M17Frame& frame : frames) {
        if (frame.kind == decode::M17FrameKind::LinkSetup && frame.lsf.has_value()) {
            counted.verified += 2;
        } else if (frame.kind == decode::M17FrameKind::Stream && frame.stream.has_value()) {
            ++counted.verified;
        }
    }
    return counted;
}

// DMR. The one line to change when core/decode/dmr.h lands: build its decoder
// here, feed it the samples, and count what its sync and its own codes verify,
// the way the P25 row above counts NIDs. Until then there is nothing in this
// tree that can verify a DMR burst, and a row that claimed one on its
// modulation alone would be the thing identify.h rules out.
[[nodiscard]] Expected<Counted> attempt_dmr(dsp::ConstComplexSpan, dsp::SampleRate) {
    return fail("core/decode/dmr.h does not exist yet");
}

[[nodiscard]] Expected<Counted> attempt_pocsag(dsp::ConstComplexSpan samples,
                                               dsp::SampleRate rate) {
    auto audio = fm_audio(samples, rate);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    // All three rates, since nothing upstream measured which. The best of
    // them is the count.
    Counted best;
    for (const double bit_rate : {decode::kPocsag512, decode::kPocsag1200, decode::kPocsag2400}) {
        decode::PocsagConfig config;
        config.rate = rate;
        config.bit_rate = bit_rate;
        auto decoder = decode::PocsagDecoder::create(config);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::vector<decode::PocsagPage> pages;
        decoder->process(dsp::ConstRealSpan(*audio), pages);
        decoder->flush(pages);
        const decode::PocsagStats& stats = decoder->stats();
        const std::uint64_t confirmed =
            stats.batches >= stats.batches_unconfirmed ? stats.batches - stats.batches_unconfirmed
                                                       : 0;
        const auto verified = static_cast<std::uint32_t>(confirmed + pages.size());
        best.verified = std::max(best.verified, verified);
    }
    return best;
}

[[nodiscard]] Expected<Counted> attempt_ax25(dsp::ConstComplexSpan samples, dsp::SampleRate rate) {
    auto audio = fm_audio(samples, rate);
    if (!audio) {
        return std::unexpected(audio.error());
    }
    decode::Ax25Config config;
    config.rate = rate;
    auto decoder = decode::Ax25Decoder::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::Ax25Frame> frames;
    decoder->process(dsp::ConstRealSpan(*audio), frames);
    return Counted{.verified = static_cast<std::uint32_t>(frames.size())};
}

[[nodiscard]] Expected<Counted> attempt_rtty(dsp::ConstComplexSpan samples, dsp::SampleRate rate) {
    const std::vector<float> audio = sideband_audio(samples, rate, kRttyCentreHz);
    // Both polarities, because which tone is mark depends on the sideband the
    // transmitter keyed and the extract carries no dial to say.
    Counted best;
    for (const bool space_above : {true, false}) {
        decode::RttyConfig config;
        config.rate = rate;
        config.space_above_mark = space_above;
        auto decoder = decode::RttyDecoder::create(config);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::vector<decode::RttyCharacter> characters;
        decoder->process(dsp::ConstRealSpan(audio), characters);
        const auto framed = static_cast<std::uint32_t>(characters.size());
        // RTTY has no code, so the framing is the whole check, and framing
        // alone is not enough: noise frames characters too, and so does a
        // 100 baud SITOR-B signal on the same 170 Hz shift. What separates
        // them is how cleanly each character read. A character counts when the
        // weakest of its seven soft readings, which the discriminator bounds to
        // one, is at least kRttyMargin; the run counts when at least
        // kRttyCleanShare of its characters do and its framing errors are under
        // a tenth of them. Measured in the "identify survey" case of
        // tests/characterise/test_identify.cpp over five seconds at 12000 S/s:
        // RTTY at 20, 10 and 5 dB in 2500 Hz framed 29 characters with none
        // under 0.5 and mean margins of 0.94 to 0.99; SITOR-B framed 28 and 31
        // with 16 to 18 over 0.5, 0.44 to 0.49 on average, and 3 to 5 framing
        // errors; noise framed 23 to 27 with at most 3 over 0.5 and 5 to 9
        // framing errors.
        std::uint32_t clean = 0;
        for (const decode::RttyCharacter& character : characters) {
            clean += character.margin >= kRttyMargin ? 1U : 0U;
        }
        if (10U * decoder->framing_errors() > framed ||
            static_cast<double>(clean) < kRttyCleanShare * static_cast<double>(framed)) {
            continue;
        }
        best.verified = std::max(best.verified, clean);
    }
    return best;
}

[[nodiscard]] Expected<Counted> attempt_sitor(dsp::ConstComplexSpan samples,
                                              dsp::SampleRate rate) {
    const std::vector<float> audio = sideband_audio(samples, rate, kSitorCentreHz);
    decode::SitorConfig config;
    config.rate = rate;
    // Clause 4.6.4's wait for a line end is for printing, and a probe's dwell
    // is too short to spend on it.
    config.wait_for_line_end = false;
    auto decoder = decode::SitorBDecoder::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::SitorCharacter> characters;
    decoder->process(dsp::ConstRealSpan(audio), characters);
    const decode::SitorStats& stats = decoder->stats();
    if (stats.phasings == 0) {
        return Counted{};
    }
    Counted counted;
    for (const decode::SitorCharacter& character : characters) {
        if (!character.mutilated) {
            ++counted.verified;
        }
    }
    return counted;
}

[[nodiscard]] Expected<Counted> attempt_psk31(dsp::ConstComplexSpan samples,
                                              dsp::SampleRate rate) {
    const std::vector<float> audio = sideband_audio(samples, rate, kPsk31CentreHz);
    Counted best;
    for (const decode::Psk31Mode mode : {decode::Psk31Mode::Bpsk31, decode::Psk31Mode::Bpsk63}) {
        decode::Psk31Config config;
        config.rate = rate;
        config.centre_hz = static_cast<dsp::Hertz>(kPsk31CentreHz);
        config.mode = mode;
        // Half the decoder's default, one second of PSK31, so a two second
        // dwell has a second left to decode in.
        config.acquisition_symbols = 32;
        auto decoder = decode::Psk31::create(config);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::vector<decode::Psk31Character> characters;
        if (auto ran = decoder->process(dsp::ConstRealSpan(audio), characters); !ran) {
            return std::unexpected(ran.error());
        }
        decoder->flush(characters);
        std::uint32_t recognised = 0;
        std::uint32_t unrecognised = 0;
        for (const decode::Psk31Character& character : characters) {
            (character.recognised ? recognised : unrecognised) += 1;
        }
        if (4U * unrecognised >= recognised && recognised > 0) {
            continue;
        }
        best.verified = std::max(best.verified, recognised);
    }
    return best;
}

[[nodiscard]] Expected<Counted> attempt_cw(dsp::ConstComplexSpan samples, dsp::SampleRate rate) {
    const std::vector<float> audio = sideband_audio(samples, rate, kCwCentreHz);
    decode::CwConfig config;
    config.rate = rate;
    config.centre_hz = static_cast<dsp::Hertz>(kCwCentreHz);
    // A quarter of the decoder's default, for the same reason as PSK31's.
    config.acquisition_seconds = 0.5;
    auto decoder = decode::Cw::create(config);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    std::vector<decode::CwCharacter> characters;
    if (auto ran = decoder->process(dsp::ConstRealSpan(audio), characters); !ran) {
        return std::unexpected(ran.error());
    }
    decoder->flush(characters);
    if (!decoder->timing().locked()) {
        return Counted{};
    }
    std::uint32_t recognised = 0;
    std::uint32_t unrecognised = 0;
    for (const decode::CwCharacter& character : characters) {
        if (character.code.empty()) {
            continue;  // a word space
        }
        (character.recognised ? recognised : unrecognised) += 1;
    }
    if (unrecognised > 1) {
        return Counted{};
    }
    return Counted{.verified = recognised};
}

[[nodiscard]] std::uint32_t required_for(Protocol protocol) {
    switch (protocol) {
        case Protocol::P25Phase1: return kP25Required;
        case Protocol::DStar: return kDStarRequired;
        case Protocol::Tetra: return kTetraRequired;
        case Protocol::M17: return kM17Required;
        case Protocol::Pocsag: return kPocsagRequired;
        case Protocol::Ax25: return kAx25Required;
        case Protocol::Rtty: return kRttyRequired;
        case Protocol::SitorB: return kSitorRequired;
        case Protocol::Psk31: return kPsk31Required;
        case Protocol::Cw: return kCwRequired;
        case Protocol::Dmr:
        case Protocol::Rds:
        case Protocol::None: return 0;
    }
    return 0;
}

}  // namespace

const char* protocol_name(Protocol protocol) {
    switch (protocol) {
        case Protocol::None: return "";
        case Protocol::P25Phase1: return "P25";
        case Protocol::DStar: return "D-STAR";
        case Protocol::Tetra: return "TETRA";
        case Protocol::M17: return "M17";
        case Protocol::Dmr: return "DMR";
        case Protocol::Pocsag: return "POCSAG";
        case Protocol::Ax25: return "AX.25";
        case Protocol::Rtty: return "RTTY";
        case Protocol::SitorB: return "SITOR-B";
        case Protocol::Psk31: return "PSK31";
        case Protocol::Cw: return "CW";
        case Protocol::Rds: return "RDS";
    }
    return "";
}

// THE PLAUSIBILITY TABLE. Widths are the detection's occupied bandwidth, and
// each window is the protocol's channel with room either side for the
// detector's 99 percent trim and for a signal measured at low SNR. A family
// the characteriser named rules a row out only where the physics does: a
// constant-envelope FSK mode is never an unmodulated carrier or OFDM, and
// TETRA's pi/4-DQPSK is never constant envelope. Unknown rules nothing out.
bool plausible(Protocol protocol, const IdentifyHints& hints, dsp::SampleRate rate) {
    const double width = hints.occupied_hz;
    const ModulationFamily family = hints.family;
    const auto within = [width](double low, double high) { return width >= low && width <= high; };
    const bool not_line_or_ofdm =
        !family_is(family, {ModulationFamily::Unmodulated, ModulationFamily::Ofdm});

    switch (protocol) {
        case Protocol::P25Phase1:
        case Protocol::M17:
        case Protocol::Dmr:
            // 4800 symbols a second of 4FSK in a 12.5 kHz channel.
            return rate >= 9600 && within(4000.0, 16000.0) && not_line_or_ofdm;
        case Protocol::DStar:
            // 4800 bit/s GMSK in a 6.25 kHz channel.
            return rate >= 9600 && within(2500.0, 10000.0) && not_line_or_ofdm;
        case Protocol::Tetra:
            // 18 ksym/s pi/4-DQPSK in 25 kHz.
            return rate >= 36000 && within(12000.0, 30000.0) &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::Psk});
        case Protocol::Pocsag:
        case Protocol::Ax25:
            // Direct FSK, and AFSK on an FM carrier, in 12.5 to 25 kHz.
            return rate >= 9600 && within(2500.0, 20000.0) &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::Fsk,
                                      ModulationFamily::AnalogueFm});
        case Protocol::Rtty:
        case Protocol::SitorB:
            // 170 Hz shift at 45.45 or 100 baud.
            return within(100.0, 600.0) &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::Fsk});
        case Protocol::Psk31:
            // 31.25 or 62.5 baud BPSK, under 150 Hz.
            return width <= 150.0 &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::Psk});
        case Protocol::Cw:
            // A keyed carrier.
            return width <= 300.0 &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::Unmodulated});
        case Protocol::Rds:
            // A broadcast FM carrier, 100 kHz and up.
            return width >= 100'000.0 &&
                   family_is(family, {ModulationFamily::Unknown, ModulationFamily::AnalogueFm});
        case Protocol::None: return false;
    }
    return false;
}

Expected<Identification> identify(dsp::ConstComplexSpan samples, const IdentifyHints& hints,
                                  const IdentifyConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("identify: the rate must be positive, was {}", config.rate));
    }

    Identification out;
    for (const Protocol protocol : kIdentifyOrder) {
        if (protocol == Protocol::Dmr && !config.dmr) {
            continue;
        }
        Attempt attempt;
        attempt.protocol = protocol;
        attempt.required = required_for(protocol);

        if (!plausible(protocol, hints, config.rate)) {
            out.attempts.push_back(attempt);
            continue;
        }
        if (protocol == Protocol::Rds) {
            attempt.result = AttemptResult::Unavailable;
            out.attempts.push_back(attempt);
            continue;
        }

        Expected<Counted> counted = fail("no row");
        switch (protocol) {
            case Protocol::P25Phase1: counted = attempt_p25(samples, config.rate); break;
            case Protocol::DStar: counted = attempt_dstar(samples, config.rate); break;
            case Protocol::Tetra: counted = attempt_tetra(samples, config.rate); break;
            case Protocol::M17: counted = attempt_m17(samples, config.rate); break;
            case Protocol::Dmr: counted = attempt_dmr(samples, config.rate); break;
            case Protocol::Pocsag: counted = attempt_pocsag(samples, config.rate); break;
            case Protocol::Ax25: counted = attempt_ax25(samples, config.rate); break;
            case Protocol::Rtty: counted = attempt_rtty(samples, config.rate); break;
            case Protocol::SitorB: counted = attempt_sitor(samples, config.rate); break;
            case Protocol::Psk31: counted = attempt_psk31(samples, config.rate); break;
            case Protocol::Cw: counted = attempt_cw(samples, config.rate); break;
            case Protocol::Rds:
            case Protocol::None: break;
        }

        if (!counted) {
            // A decoder that cannot be built at this rate, or the DMR row
            // before its decoder exists: nothing verified, and not a fault of
            // the probe that asked.
            attempt.result = AttemptResult::Unavailable;
            out.attempts.push_back(attempt);
            continue;
        }

        attempt.verified = counted->verified;
        attempt.result = attempt.verified >= attempt.required && attempt.required > 0
                             ? AttemptResult::Verified
                             : AttemptResult::NotVerified;
        out.attempts.push_back(attempt);

        // The first row to verify wins, because kIdentifyOrder runs from the
        // strongest check to the weakest. Every row is still run and listed.
        if (attempt.result == AttemptResult::Verified) {
            if (out.protocol == Protocol::None) {
                out.protocol = protocol;
                out.verified = attempt.verified;
                out.confidence =
                    1.0 - 0.1 * std::pow(0.5, static_cast<double>(attempt.verified -
                                                                  attempt.required));
            }
        }
    }
    return out;
}

}  // namespace revenant::identify
