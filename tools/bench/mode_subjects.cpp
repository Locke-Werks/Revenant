// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include "tools/bench/mode_subjects.h"

#include <algorithm>
#include <array>
#include <format>

#include "tools/bench/mode_support.h"

namespace revenant::bench {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/mode_subjects.cpp must include core/dsp/reference_fp.h first");

namespace {

// The order the nightly sweeps them, which is also the order of the table in
// docs/sensitivity.md.
constexpr std::array<std::string_view, 18> kModes = {
    "rtty",   "ax25",   "pocsag-512", "pocsag-1200", "pocsag-2400", "sitor-b", "navtex", "psk31",
    "psk63",  "qpsk31", "cw",         "m17",         "p25p1",       "dstar",   "tetra",
    "dmr",    "ais",    "dsc",
};

enum class Family { Fsk, Tone, Dv, Maritime };

Family family_of(std::string_view mode) {
    if (mode == "ais" || mode == "dsc") {
        return Family::Maritime;
    }
    if (mode == "psk31" || mode == "psk63" || mode == "qpsk31" || mode == "cw") {
        return Family::Tone;
    }
    if (mode == "m17" || mode == "p25p1" || mode == "dstar" || mode == "tetra" || mode == "dmr") {
        return Family::Dv;
    }
    return Family::Fsk;
}

}  // namespace

std::span<const std::string_view> mode_subject_names() {
    return kModes;
}

Expected<ModeSubject> make_mode_subject(std::string_view mode) {
    if (std::find(kModes.begin(), kModes.end(), mode) == kModes.end()) {
        std::string known;
        for (const std::string_view name : kModes) {
            known += known.empty() ? "" : ", ";
            known += name;
        }
        return fail(std::format("unknown mode '{}': {}", mode, known));
    }
    switch (family_of(mode)) {
    case Family::Fsk:
        return detail::make_fsk_subject(mode);
    case Family::Tone:
        return detail::make_tone_subject(mode);
    case Family::Dv:
        return detail::make_dv_subject(mode);
    case Family::Maritime:
        return detail::make_maritime_subject(mode);
    }
    return fail(std::format("mode '{}' has no family", mode));
}

namespace detail {

std::vector<dsp::Complex32> audio_as_baseband(std::span<const float> audio) {
    std::vector<dsp::Complex32> out;
    out.reserve(audio.size());
    for (const float sample : audio) {
        out.emplace_back(sample, 0.0F);
    }
    return out;
}

std::vector<float> real_part(dsp::ConstComplexSpan samples) {
    std::vector<float> out(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        out[i] = samples[i].real();
    }
    return out;
}

double real_mean_power(std::span<const float> audio) {
    double sum = 0.0;
    for (const float v : audio) {
        sum += static_cast<double>(v) * static_cast<double>(v);
    }
    return audio.empty() ? 0.0 : sum / static_cast<double>(audio.size());
}

std::string text_from_payload(std::span<const std::uint8_t> payload, std::string_view pool) {
    std::string text;
    text.reserve(payload.size());
    for (const std::uint8_t byte : payload) {
        text.push_back(pool[byte % pool.size()]);
    }
    return text;
}

std::u32string widen(std::string_view text) {
    std::u32string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char32_t>(static_cast<unsigned char>(c)));
    }
    return out;
}

std::size_t edit_distance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> row(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) {
        row[j] = j;
    }
    for (std::size_t i = 1; i <= a.size(); ++i) {
        std::size_t diagonal = row[0];
        row[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const std::size_t above = row[j];
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0U : 1U)});
            diagonal = above;
        }
    }
    return row[b.size()];
}

TrialResult score_text(std::string_view sent, std::string_view got) {
    const std::size_t distance = edit_distance(sent, got);
    TrialResult result;
    result.bits_total = sent.size();
    result.bits_wrong = std::min(distance, sent.size());
    result.decoded = distance == 0;
    return result;
}

TrialResult score_units(std::size_t sent, std::size_t received) {
    TrialResult result;
    result.bits_total = sent;
    result.bits_wrong = sent - std::min(sent, received);
    result.decoded = received >= sent;
    return result;
}

TrialResult failed_bits(std::size_t bits) {
    TrialResult result;
    result.bits_total = bits;
    result.bits_wrong = bits / 2;
    result.decoded = false;
    return result;
}

std::vector<std::uint8_t> payload_bits(std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> bits(payload.size() * 8);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        bits[i] = payload_bit(payload, i) ? 1 : 0;
    }
    return bits;
}

std::string bits_as_text(std::span<const std::uint8_t> bits) {
    std::string text(bits.size(), '0');
    for (std::size_t i = 0; i < bits.size(); ++i) {
        text[i] = bits[i] != 0 ? '1' : '0';
    }
    return text;
}

std::string hex_lines(std::span<const std::uint8_t> payload, std::size_t bytes_per_line) {
    std::string out;
    for (std::size_t i = 0; i < payload.size(); ++i) {
        out += std::format("{:02X}", payload[i]);
        if ((i + 1) % bytes_per_line == 0 || i + 1 == payload.size()) {
            out += '\n';
        }
    }
    return out;
}

}  // namespace detail

}  // namespace revenant::bench
