#include "core/decode/jt4.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

#include "core/decode/fano.h"

namespace revenant::decode {

namespace {

// "207 bit Synchronisation Vector", as printed.
constexpr std::array<std::uint8_t, 207> kSync = {
    0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0,
    1, 1, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1,
    0, 1, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 0,
    1, 0, 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 0, 1, 1, 0, 1,
    1, 1, 1, 0, 1, 0, 1,
};

constexpr int kTail = 31;
constexpr int kCoded = 2 * (kWsjt72Bits + kTail);  // 206
// "Tone spacing multipliers", JT4a to JT4g.
constexpr std::array<int, 7> kK = {1, 2, 4, 9, 18, 36, 72};

SlowProtocol make_protocol(int submode) {
    SlowProtocol p;
    p.name = "JT4";
    p.symbols = 207;
    p.tones = 4;
    // "11025 / 2520, or 4.375Hz ... multiplied by a constant K"; "the
    // transmitted symbol length is the reciprocal of the tone spacing".
    p.symbol_seconds = 2520.0 / 11025.0;
    p.tone_spacing_hz = 11025.0 / 2520.0 * kK[static_cast<std::size_t>(submode)];
    // NOT SPECIFIED by the document; see the header.
    p.slot_seconds = 60.0;
    p.nominal_start_seconds = 1.0;
    p.start_min_seconds = 0.0;
    p.start_max_seconds = 3.0;
    for (const auto s : kSync) {
        p.sync_on.push_back({s, s + 2});
        p.sync_off.push_back({1 - s, 3 - s});
    }
    return p;
}

}  // namespace

std::span<const std::uint8_t> jt4_sync_vector() { return kSync; }

const SlowProtocol& jt4_protocol(int submode) {
    static const std::array<SlowProtocol, 7> all = [] {
        std::array<SlowProtocol, 7> a;
        for (int i = 0; i < 7; ++i) a[static_cast<std::size_t>(i)] = make_protocol(i);
        return a;
    }();
    return all[static_cast<std::size_t>(std::clamp(submode, 0, 6))];
}

double jt4_conventional_tone0_hz(int submode) {
    return 11025.0 / 1024.0 * 118.0 - 1.5 * 4.375 * kK[static_cast<std::size_t>(std::clamp(submode, 0, 6))];
}

std::vector<int> jt4_channel_tones(const Wsjt72& payload) {
    std::vector<std::uint8_t> bits(payload.begin(), payload.end());
    bits.resize(kWsjt72Bits + kTail, 0);
    auto d = wsjt_interleave(wsjt_conv_encode(bits));
    d.insert(d.begin(), 0);  // "A single zero bit is now appended to the start"
    std::vector<int> out(207);
    for (std::size_t i = 0; i < 207; ++i) out[i] = kSync[i] + 2 * d[i];
    return out;
}

Jt4Decoder::Jt4Decoder(Jt4Config config, SlowStream stream) : config_(std::move(config)), stream_(std::move(stream)) {}

Expected<Jt4Decoder> Jt4Decoder::create(Jt4Config config) {
    if (config.submode < 0 || config.submode > 6) return fail("JT4 submode must be 0 to 6 (A to G)");
    if (!(config.low_hz >= 0.0 && config.high_hz > config.low_hz && config.high_hz < 5000.0)) {
        return fail("JT4 search range must lie within 0 to 5000 Hz");
    }
    auto stream = SlowStream::create(jt4_protocol(config.submode).slot_seconds, config.rate, config.utc_of_sample);
    if (!stream) return std::unexpected(stream.error());
    return Jt4Decoder(std::move(config), std::move(*stream));
}

Status Jt4Decoder::process(ConstRealSpan audio, std::vector<Wsjt72Decode>& out) {
    stream_.process(audio, [&](std::span<const float> slot, double utc, SampleIndex start) {
        decode_slot(slot, utc, start, out);
    });
    return {};
}

void Jt4Decoder::decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                             std::vector<Wsjt72Decode>& out) const {
    const SlowProtocol& p = jt4_protocol(config_.submode);
    SlowSlot slot(p, slot_12k);
    SlowSearch search;
    search.low_hz = config_.low_hz;
    search.high_hz = config_.high_hz;
    search.max_candidates = config_.max_candidates;
    search.sync_threshold = config_.sync_threshold;
    search.max_drift_hz = config_.max_drift_hz;

    FanoConfig fano;
    fano.generator0 = kWsjtConvG0;
    fano.generator1 = kWsjtConvG1;
    fano.tail_bits = kTail;
    fano.max_cycles_per_bit = config_.fano_cycles_per_bit;

    std::map<std::string, Wsjt72Decode> found;
    for (const auto& c : slot.candidates(search)) {
        const SlowSpectra s = slot.demodulate(c, search);
        const double a = slow_amplitude(s, 0, 3);
        std::vector<float> llr;
        llr.reserve(kCoded);
        for (int i = 1; i < 207; ++i) {  // symbol 0 carries the fixed zero bit
            const int q = kSync[static_cast<std::size_t>(i)];
            llr.push_back(static_cast<float>(slow_log_i0(2.0 * a * std::sqrt(static_cast<double>(s.at(i, q)))) -
                                             slow_log_i0(2.0 * a * std::sqrt(static_cast<double>(s.at(i, q + 2))))));
        }
        const auto soft = wsjt_deinterleave(llr);
        auto r = fano_decode(fano, soft);
        if (!r || !r->decoded) continue;
        Wsjt72 bits{};
        std::copy_n(r->bits.begin(), kWsjt72Bits, bits.begin());
        auto msg = wsjt72_unpack(bits);
        if (!msg.standard) continue;
        const auto tones = jt4_channel_tones(bits);
        // NOT SPECIFIED: as WSPR, at most 30% of hard data-bit decisions wrong.
        int wrong = 0;
        for (int i = 0; i < 207; ++i) {
            const int q = kSync[static_cast<std::size_t>(i)];
            const int hard = s.at(i, q + 2) > s.at(i, q) ? 1 : 0;
            if (hard != tones[static_cast<std::size_t>(i)] / 2) ++wrong;
        }
        if (wrong > 207 * 3 / 10) continue;
        Wsjt72Decode d;
        d.slot_utc = slot_utc;
        d.slot_start_sample = slot_start;
        d.frequency_hz = s.where.frequency_hz;
        d.dt_seconds = s.where.start_seconds - p.nominal_start_seconds;
        d.drift_hz = s.where.drift_hz;
        d.snr_db = slow_snr_2500(p, s, tones);
        d.payload = bits;
        d.message = std::move(msg);
        auto it = found.find(d.message.text);
        if (it == found.end() || it->second.snr_db < d.snr_db) found[d.message.text] = d;
    }
    std::vector<Wsjt72Decode> got;
    for (auto& [k, v] : found) got.push_back(std::move(v));
    std::sort(got.begin(), got.end(), [](const Wsjt72Decode& a, const Wsjt72Decode& b) { return a.frequency_hz < b.frequency_hz; });
    out.insert(out.end(), got.begin(), got.end());
}

}  // namespace revenant::decode
