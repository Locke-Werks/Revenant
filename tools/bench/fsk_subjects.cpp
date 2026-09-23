// The two-level FSK modes: RTTY, AX.25, POCSAG, SITOR-B and NAVTEX, each
// through core/dsp/synth/fsk_mod.h and the matching decoder in core/decode.
//
// Every setting is the one the mode's error-rate case in tests/decode uses, so
// a point on a curve here and the figure that case prints are the same
// measurement at more SNRs and more trials. Where one differs it says so.

// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/dv_phy.h"
#include "core/decode/navtex.h"
#include "core/decode/pocsag.h"
#include "core/decode/rtty.h"
#include "core/decode/sitor_b.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"
#include "core/dsp/synth/rds_mod.h"
#include "tools/bench/mode_support.h"

namespace revenant::bench::detail {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/fsk_subjects.cpp must include core/dsp/reference_fp.h first");

namespace {

constexpr dsp::SampleRate kAudioRate = 48000;

// Noise on real audio, calibrated against the audio's own mean power in
// 2500 Hz, as every FSK case in tests/decode adds it.
bool add_audio_noise(std::vector<float>& audio, double snr_2500_db, std::uint64_t seed) {
    const double power = real_mean_power(audio);
    return siggen::add_real_awgn(dsp::RealSpan(audio), power, siggen::NoiseLevel::snr_in_2500_hz_db(snr_2500_db),
                                 kAudioRate, seed)
        .has_value();
}

// ---------------------------------------------------------------------------
// RTTY
// ---------------------------------------------------------------------------

// tests/decode/test_rtty.cpp's pool: letters and figures mixed, so the shifts
// are exercised and a lost shift costs what it costs on air.
constexpr std::string_view kRttyPool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ    0123456789-?:().,'=/+";

ModeSubject rtty() {
    ModeSubject mode;
    mode.mode = "rtty";
    mode.subject = "rtty 45.45 Bd 170 Hz shift at 48000 S/s; character error rate against SNR in 2500 Hz";
    mode.unit = "character";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 100;
    mode.minimum_payload_bytes = 16;
    mode.snr_start_db = -10.0;
    mode.snr_stop_db = 0.0;
    mode.trials = 256;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        const std::string text = text_from_payload(payload, kRttyPool);
        auto codes = siggen::ita2_encode_text(widen(text));
        if (!codes) {
            return std::vector<dsp::Complex32>{};
        }
        auto audio = siggen::rtty_render(siggen::RttyModConfig{}, *codes);
        if (!audio || !add_audio_noise(*audio, snr_db, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(*audio);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::string sent = text_from_payload(payload, kRttyPool);
        auto decoder = decode::RttyDecoder::create(decode::RttyConfig{});
        if (samples.empty() || !decoder) {
            return score_text(sent, "");
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::RttyCharacter> characters;
        decoder->process(dsp::ConstRealSpan(audio), characters);
        return score_text(sent, decode::rtty_text(characters));
    };
    mode.truth = [](std::span<const std::uint8_t> payload) { return text_from_payload(payload, kRttyPool) + "\n"; };
    return mode;
}

// ---------------------------------------------------------------------------
// AX.25
// ---------------------------------------------------------------------------

// tests/decode/test_ax25.cpp's frame: a UI frame from N7LEM to APRS via
// WIDE1-1 with 60 information octets, 83 octets in all before the FCS.
constexpr std::size_t kAx25InformationOctets = 60;

decode::Ax25Address ax25_address(const char* call, std::uint8_t ssid, bool c) {
    decode::Ax25Address address;
    address.callsign = call;
    address.ssid = ssid;
    address.command_or_repeated = c;
    return address;
}

Expected<std::vector<std::vector<std::uint8_t>>> ax25_frames(std::span<const std::uint8_t> payload) {
    std::vector<std::vector<std::uint8_t>> frames;
    for (std::size_t at = 0; at + kAx25InformationOctets <= payload.size(); at += kAx25InformationOctets) {
        siggen::Ax25FrameSpec spec;
        spec.destination = ax25_address("APRS", 0, true);
        spec.source = ax25_address("N7LEM", 0, false);
        spec.repeaters.push_back(ax25_address("WIDE1", 1, false));
        spec.information.assign(payload.begin() + static_cast<std::ptrdiff_t>(at),
                                payload.begin() + static_cast<std::ptrdiff_t>(at + kAx25InformationOctets));
        auto octets = siggen::ax25_frame_octets(spec);
        if (!octets) {
            return std::unexpected(octets.error());
        }
        frames.push_back(std::move(*octets));
    }
    return frames;
}

siggen::Ax25ModConfig ax25_mod() {
    // Sixteen flags between frames, as the test has, so a frame lost does
    // not take its neighbour with it.
    siggen::Ax25ModConfig mod;
    mod.flags_between = 16;
    return mod;
}

ModeSubject ax25() {
    ModeSubject mode;
    mode.mode = "ax25";
    mode.subject = "ax25 Bell 202 AFSK at 48000 S/s, UI frames of 60 information octets; frame error rate "
                   "against SNR in 2500 Hz";
    mode.unit = "frame";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 4 * kAx25InformationOctets;
    mode.minimum_payload_bytes = kAx25InformationOctets;
    mode.payload_multiple = kAx25InformationOctets;
    mode.snr_start_db = 6.0;
    mode.snr_stop_db = 16.0;
    // Four frames a trial, so this is 4096 frames a point, about forty lost
    // where the curve crosses 0.01.
    mode.trials = 1024;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        auto frames = ax25_frames(payload);
        if (!frames) {
            return std::vector<dsp::Complex32>{};
        }
        auto audio = siggen::ax25_render(ax25_mod(), *frames);
        if (!audio || !add_audio_noise(*audio, snr_db, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(*audio);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        auto frames = ax25_frames(payload);
        const std::size_t sent = payload.size() / kAx25InformationOctets;
        decode::Ax25Config config;
        config.rate = kAudioRate;
        auto decoder = decode::Ax25Decoder::create(config);
        if (!frames || samples.empty() || !decoder) {
            return score_units(sent, 0);
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::Ax25Frame> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        // In order, each sent frame matched at most once, as the test does.
        std::size_t good = 0;
        std::size_t next = 0;
        for (const decode::Ax25Frame& frame : got) {
            for (std::size_t i = next; i < frames->size(); ++i) {
                if (frame.octets == (*frames)[i]) {
                    ++good;
                    next = i + 1;
                    break;
                }
            }
        }
        return score_units(sent, good);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        auto frames = ax25_frames(payload);
        std::string out;
        if (frames) {
            for (const auto& frame : *frames) {
                out += hex_lines(frame, frame.size());
            }
        }
        return out;
    };
    return mode;
}

// ---------------------------------------------------------------------------
// POCSAG
// ---------------------------------------------------------------------------

// A page per 43 payload bytes: three for the 21-bit identity and forty
// alphanumeric characters, which is tests/decode/test_pocsag.cpp's page.
constexpr std::size_t kPocsagPageBytes = 43;
constexpr std::size_t kPocsagTextCharacters = 40;

struct SentPage {
    std::uint32_t identity = 0;
    std::string text;
};

std::vector<SentPage> pocsag_pages(std::span<const std::uint8_t> payload) {
    std::vector<SentPage> pages;
    for (std::size_t at = 0; at + kPocsagPageBytes <= payload.size(); at += kPocsagPageBytes) {
        SentPage page;
        page.identity = ((static_cast<std::uint32_t>(payload[at]) << 16U) |
                         (static_cast<std::uint32_t>(payload[at + 1]) << 8U) |
                         static_cast<std::uint32_t>(payload[at + 2])) &
                        0x1FFFFFU;
        for (std::size_t k = 0; k < kPocsagTextCharacters; ++k) {
            page.text.push_back(static_cast<char>(' ' + payload[at + 3 + k] % 94));
        }
        pages.push_back(std::move(page));
    }
    return pages;
}

Expected<std::vector<std::uint8_t>> pocsag_air_bits(std::span<const std::uint8_t> payload) {
    std::vector<siggen::PocsagPageSpec> specs;
    for (const SentPage& page : pocsag_pages(payload)) {
        siggen::PocsagPageSpec spec;
        spec.identity = page.identity;
        spec.function = decode::kPocsagFunctionAlphanumeric;
        auto bits = siggen::pocsag_alphanumeric_bits(page.text);
        if (!bits) {
            return std::unexpected(bits.error());
        }
        spec.message_bits = std::move(*bits);
        specs.push_back(std::move(spec));
    }
    return siggen::pocsag_bits(specs);
}

// The receiver's channel filter before the discriminator, 7 kHz either side,
// which passes the Carson bandwidth of 4.5 kHz deviation at 2400 bit/s,
// 2 x (4.5 + 2.4) = 13.8 kHz. The test's filter, and it serves all three
// rates, as one receiver's IF filter would.
double pocsag_channel_response(double hertz, const void*) {
    return std::abs(hertz) <= 7000.0 ? 1.0 : 0.0;
}

// `start` is the grid's first point; each rate's curve falls from all pages
// lost to none in about 4 dB, and the grid spans 10.
Expected<ModeSubject> pocsag(double bit_rate, std::string_view name, double start) {
    auto taps = decode::design_from_response(kAudioRate, 127, pocsag_channel_response, nullptr);
    if (!taps) {
        return std::unexpected(taps.error());
    }

    ModeSubject mode;
    mode.mode = std::string(name);
    mode.subject = std::format("pocsag {} bit/s, 4.5 kHz FSK as complex baseband at 48000 S/s through a 7 kHz "
                               "channel filter and FM discriminator, pages of 40 alphanumeric characters; page "
                               "error rate against SNR in 2500 Hz",
                               bit_rate);
    mode.unit = "page";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 4 * kPocsagPageBytes;
    mode.minimum_payload_bytes = kPocsagPageBytes;
    mode.payload_multiple = kPocsagPageBytes;
    mode.snr_start_db = start;
    mode.snr_stop_db = start + 10.0;
    mode.trials = 1024;
    mode.real_audio = false;
    mode.generator = [bit_rate](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        auto bits = pocsag_air_bits(payload);
        if (!bits) {
            return std::vector<dsp::Complex32>{};
        }
        siggen::PocsagModConfig mod;
        mod.rate = kAudioRate;
        mod.bit_rate = bit_rate;
        auto rf = siggen::pocsag_render_baseband(mod, *bits);
        if (!rf || !siggen::add_awgn(dsp::ComplexSpan(*rf), siggen::NoiseLevel::snr_in_2500_hz_db(snr_db),
                                     kAudioRate, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return std::move(*rf);
    };
    mode.score = [bit_rate, filter = std::move(*taps)](dsp::ConstComplexSpan samples,
                                                       std::span<const std::uint8_t> payload) {
        const std::vector<SentPage> sent = pocsag_pages(payload);
        decode::PocsagConfig config;
        config.rate = kAudioRate;
        config.bit_rate = bit_rate;
        auto decoder = decode::PocsagDecoder::create(config);
        if (samples.empty() || !decoder) {
            return score_units(sent.size(), 0);
        }
        std::vector<dsp::Complex32> filtered(samples.size());
        std::vector<float> audio(samples.size());
        if (!decode::filter_complex(samples, dsp::ConstRealSpan(filter), dsp::ComplexSpan(filtered)) ||
            !decode::fm_discriminate(dsp::ConstComplexSpan(filtered), dsp::RealSpan(audio), kAudioRate)) {
            return score_units(sent.size(), 0);
        }
        std::vector<decode::PocsagPage> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        decoder->flush(got);
        std::size_t good = 0;
        for (const SentPage& page : sent) {
            for (const decode::PocsagPage& g : got) {
                if (g.identity == page.identity && g.text == page.text) {
                    ++good;
                    break;
                }
            }
        }
        return score_units(sent.size(), good);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        for (const SentPage& page : pocsag_pages(payload)) {
            out += std::format("{} {}\n", page.identity, page.text);
        }
        return out;
    };
    return mode;
}

// ---------------------------------------------------------------------------
// SITOR-B
// ---------------------------------------------------------------------------

// tests/decode/test_sitor_b.cpp's pool.
constexpr std::string_view kSitorPool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ    0123456789.,";

// Printing characters, a character lost in both copies as '#', which the pool
// never sends, so a loss always costs one.
std::string sitor_text(const std::vector<decode::SitorCharacter>& characters) {
    std::string out;
    for (const decode::SitorCharacter& c : characters) {
        if (c.mutilated) {
            out.push_back('#');
        } else if (c.glyph != 0) {
            out.push_back(c.glyph < 0x80 ? static_cast<char>(c.glyph) : '?');
        }
    }
    return out;
}

ModeSubject sitor_b() {
    ModeSubject mode;
    mode.mode = "sitor-b";
    mode.subject = "sitor-b 100 Bd 170 Hz shift at 48000 S/s, 16 phasing pairs; character error rate against SNR "
                   "in 2500 Hz";
    mode.unit = "character";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 100;
    mode.minimum_payload_bytes = 16;
    mode.snr_start_db = -10.0;
    mode.snr_stop_db = 2.0;
    // About one trial in 250 loses most of its text between -1 and +2 dB,
    // so a point needs a thousand trials before that is a rate rather than
    // a coincidence.
    mode.trials = 1024;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        auto codes = siggen::ita2_encode_text(widen(text_from_payload(payload, kSitorPool)));
        if (!codes) {
            return std::vector<dsp::Complex32>{};
        }
        auto audio = siggen::sitor_b_render(siggen::SitorModConfig{}, *codes);
        if (!audio || !add_audio_noise(*audio, snr_db, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(*audio);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        // Clause 4.6.1 puts CR LF before the traffic and clause 4.6.4 starts
        // printing on it, so it is part of what a receiver should print.
        const std::string sent = "\r\n" + text_from_payload(payload, kSitorPool);
        auto decoder = decode::SitorBDecoder::create(decode::SitorConfig{});
        if (samples.empty() || !decoder) {
            return score_text(sent, "");
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::SitorCharacter> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        return score_text(sent, sitor_text(got));
    };
    mode.truth = [](std::span<const std::uint8_t> payload) { return text_from_payload(payload, kSitorPool) + "\n"; };
    return mode;
}

// ---------------------------------------------------------------------------
// NAVTEX
// ---------------------------------------------------------------------------

// tests/decode/test_navtex.cpp's pool and message length. Each message is its
// own transmission with M.540-2 Annex II Figure 1's ten seconds of phasing,
// back to back in one capture, serials 1 upward.
constexpr std::string_view kNavtexPool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ     0123456789.,";
constexpr std::size_t kNavtexBodyCharacters = 120;

siggen::SitorModConfig navtex_mod() {
    siggen::SitorModConfig mod;
    mod.phasing_pairs = 72;
    mod.line_end_first = false;
    return mod;
}

std::vector<std::string> navtex_bodies(std::span<const std::uint8_t> payload) {
    std::vector<std::string> bodies;
    for (std::size_t at = 0; at + kNavtexBodyCharacters <= payload.size(); at += kNavtexBodyCharacters) {
        bodies.push_back(text_from_payload(payload.subspan(at, kNavtexBodyCharacters), kNavtexPool));
    }
    return bodies;
}

ModeSubject navtex() {
    ModeSubject mode;
    mode.mode = "navtex";
    mode.subject = "navtex on sitor-b at 48000 S/s, 72 phasing pairs, messages of 120 characters each its own "
                   "transmission; message error rate against SNR in 2500 Hz";
    mode.unit = "message";
    mode.axis = "SNR in 2500 Hz";
    mode.default_payload_bytes = 2 * kNavtexBodyCharacters;
    mode.minimum_payload_bytes = kNavtexBodyCharacters;
    mode.payload_multiple = kNavtexBodyCharacters;
    mode.snr_start_db = -7.0;
    mode.snr_stop_db = 1.0;
    // Two messages a trial, 2048 a point.
    mode.trials = 1024;
    mode.generator = [](std::span<const std::uint8_t> payload, double snr_db, std::uint64_t seed) {
        std::vector<float> audio;
        const std::vector<std::string> bodies = navtex_bodies(payload);
        for (std::size_t m = 0; m < bodies.size(); ++m) {
            auto codes = siggen::ita2_encode_text(
                siggen::navtex_text('E', 'A', static_cast<int>(m + 1), widen(bodies[m] + "\r\n")));
            if (!codes) {
                return std::vector<dsp::Complex32>{};
            }
            auto one = siggen::sitor_b_render(navtex_mod(), *codes);
            if (!one) {
                return std::vector<dsp::Complex32>{};
            }
            audio.insert(audio.end(), one->begin(), one->end());
        }
        if (!add_audio_noise(audio, snr_db, seed)) {
            return std::vector<dsp::Complex32>{};
        }
        return audio_as_baseband(audio);
    };
    mode.score = [](dsp::ConstComplexSpan samples, std::span<const std::uint8_t> payload) {
        const std::vector<std::string> bodies = navtex_bodies(payload);
        auto decoder = decode::NavtexDecoder::create(decode::NavtexConfig{});
        if (samples.empty() || !decoder) {
            return score_units(bodies.size(), 0);
        }
        const std::vector<float> audio = real_part(samples);
        std::vector<decode::NavtexMessage> got;
        decoder->process(dsp::ConstRealSpan(audio), got);
        decoder->flush(got);
        // Received means what clause 3 lets a receiver print: a clean
        // preamble, and then the text exactly.
        std::size_t received = 0;
        for (std::size_t m = 0; m < bodies.size(); ++m) {
            for (const decode::NavtexMessage& message : got) {
                if (message.serial == static_cast<int>(m + 1) && message.preamble_clean &&
                    message.text == bodies[m]) {
                    ++received;
                    break;
                }
            }
        }
        return score_units(bodies.size(), received);
    };
    mode.truth = [](std::span<const std::uint8_t> payload) {
        std::string out;
        for (const std::string& body : navtex_bodies(payload)) {
            out += body + "\n";
        }
        return out;
    };
    return mode;
}

}  // namespace

Expected<ModeSubject> make_fsk_subject(std::string_view mode) {
    if (mode == "rtty") {
        return rtty();
    }
    if (mode == "ax25") {
        return ax25();
    }
    if (mode == "pocsag-512") {
        return pocsag(decode::kPocsag512, mode, 4.0);
    }
    if (mode == "pocsag-1200") {
        return pocsag(decode::kPocsag1200, mode, 6.0);
    }
    if (mode == "pocsag-2400") {
        return pocsag(decode::kPocsag2400, mode, 8.0);
    }
    if (mode == "sitor-b") {
        return sitor_b();
    }
    if (mode == "navtex") {
        return navtex();
    }
    return fail(std::format("'{}' is not an FSK mode", mode));
}

}  // namespace revenant::bench::detail
