#include "core/decode/jt9.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <string>

#include "core/decode/fano.h"

namespace revenant::decode {

namespace {

// "Merge With Sync", the 85-bit vector as printed in three rows.
constexpr std::string_view kSyncText =
    "11001000010000010000001000000000"
    "10100000000000000011001000010000"
    "010000001000000000101";

constexpr std::array<std::uint8_t, 85> make_sync() {
    std::array<std::uint8_t, 85> s{};
    for (std::size_t i = 0; i < 85; ++i) s[i] = kSyncText[i] == '1' ? 1 : 0;
    return s;
}
constexpr auto kSync = make_sync();
static_assert(kSyncText.size() == 85);

constexpr int kTail = 31;
constexpr int kCoded = 2 * (kWsjt72Bits + kTail);  // 206

SlowProtocol make_protocol(int submode) {
    SlowProtocol p;
    p.name = "JT9";
    p.symbols = 85;
    p.tones = 9;
    // "Modulation": 12000 / 6912 Hz times a binary multiple per submode; the
    // baud rate stays 12000 / 6912 for the slow submodes.
    p.symbol_seconds = 6912.0 / 12000.0;
    p.tone_spacing_hz = 12000.0 / 6912.0 * static_cast<double>(1 << submode);
    // NOT SPECIFIED by the document; see the header.
    p.slot_seconds = 60.0;
    p.nominal_start_seconds = 1.0;
    p.start_min_seconds = 0.0;
    p.start_max_seconds = 3.0;
    for (const auto s : kSync) {
        if (s) {
            p.sync_on.push_back({0});
            p.sync_off.push_back({});
        } else {
            p.sync_on.push_back({});
            p.sync_off.push_back({0});
        }
    }
    return p;
}

int inverse_gray(int g) {
    int n = 0;
    for (; g; g >>= 1) n ^= g;
    return n;
}

double log_sum_exp(double a, double b) {
    const double m = std::max(a, b);
    return m + std::log(std::exp(a - m) + std::exp(b - m));
}

}  // namespace

std::span<const std::uint8_t> jt9_sync_vector() { return kSync; }

const SlowProtocol& jt9_protocol(int submode) {
    static const std::array<SlowProtocol, 8> all = [] {
        std::array<SlowProtocol, 8> a;
        for (int i = 0; i < 8; ++i) a[static_cast<std::size_t>(i)] = make_protocol(i);
        return a;
    }();
    return all[static_cast<std::size_t>(std::clamp(submode, 0, 7))];
}

std::vector<int> jt9_channel_tones(const Wsjt72& payload) {
    std::vector<std::uint8_t> bits(payload.begin(), payload.end());
    bits.resize(kWsjt72Bits + kTail, 0);
    auto d = wsjt_interleave(wsjt_conv_encode(bits));
    d.push_back(0);  // "Add a padding 0 onto the end"
    std::vector<int> data;
    for (std::size_t i = 0; i + 2 < d.size(); i += 3) {
        const int v = d[i] * 4 + d[i + 1] * 2 + d[i + 2];
        data.push_back(v ^ (v >> 1));
    }
    std::vector<int> out;
    std::size_t k = 0;
    for (const auto s : kSync) out.push_back(s ? 0 : data[k++] + 1);
    return out;
}

Jt9Decoder::Jt9Decoder(Jt9Config config, SlowStream stream) : config_(std::move(config)), stream_(std::move(stream)) {}

Expected<Jt9Decoder> Jt9Decoder::create(Jt9Config config) {
    if (config.submode < 0 || config.submode > 7) return fail("JT9 submode must be 0 to 7 (A to H)");
    if (!(config.low_hz >= 0.0 && config.high_hz > config.low_hz && config.high_hz < 5000.0)) {
        return fail("JT9 search range must lie within 0 to 5000 Hz");
    }
    auto stream = SlowStream::create(jt9_protocol(config.submode).slot_seconds, config.rate, config.utc_of_sample);
    if (!stream) return std::unexpected(stream.error());
    return Jt9Decoder(std::move(config), std::move(*stream));
}

Status Jt9Decoder::process(ConstRealSpan audio, std::vector<Wsjt72Decode>& out) {
    stream_.process(audio, [&](std::span<const float> slot, double utc, SampleIndex start) {
        decode_slot(slot, utc, start, out);
    });
    return {};
}

void Jt9Decoder::decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                             std::vector<Wsjt72Decode>& out) const {
    const SlowProtocol& p = jt9_protocol(config_.submode);
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
        const double a = slow_amplitude(s, 1, 8);
        std::vector<float> llr;
        llr.reserve(207);
        for (int i = 0; i < 85; ++i) {
            if (kSync[static_cast<std::size_t>(i)]) continue;
            std::array<double, 8> m{};
            for (int t = 0; t < 8; ++t) m[static_cast<std::size_t>(t)] = slow_log_i0(2.0 * a * std::sqrt(static_cast<double>(s.at(i, t + 1))));
            for (int bit = 2; bit >= 0; --bit) {
                double l0 = -1e300;
                double l1 = -1e300;
                for (int t = 0; t < 8; ++t) {
                    const int b = inverse_gray(t);
                    if ((b >> bit) & 1) l1 = log_sum_exp(l1, m[static_cast<std::size_t>(t)]);
                    else l0 = log_sum_exp(l0, m[static_cast<std::size_t>(t)]);
                }
                llr.push_back(static_cast<float>(l0 - l1));
            }
        }
        llr.resize(kCoded);  // the pad bit carries nothing
        const auto soft = wsjt_deinterleave(llr);
        auto r = fano_decode(fano, soft);
        if (!r || !r->decoded) continue;
        Wsjt72 bits{};
        std::copy_n(r->bits.begin(), kWsjt72Bits, bits.begin());
        auto msg = wsjt72_unpack(bits);
        if (!msg.standard) continue;
        const auto tones = jt9_channel_tones(bits);
        // NOT SPECIFIED: kept only if at most 60% of data symbols' strongest
        // tones disagree with the decode's own symbols (noise gives 7 in 8).
        int wrong = 0;
        for (int i = 0; i < 85; ++i) {
            if (kSync[static_cast<std::size_t>(i)]) continue;
            int best = 1;
            for (int t = 2; t <= 8; ++t) {
                if (s.at(i, t) > s.at(i, best)) best = t;
            }
            if (best != tones[static_cast<std::size_t>(i)]) ++wrong;
        }
        if (wrong > 69 * 6 / 10) continue;
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
