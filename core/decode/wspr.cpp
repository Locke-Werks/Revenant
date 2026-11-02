#include "core/decode/wspr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include "core/decode/fano.h"

namespace revenant::decode {

namespace {

// "Merge With Sync Vector", the 162-bit table as printed (see the header on
// its "0.0,0").
constexpr std::array<std::uint8_t, 162> kSync = {
    1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0,
    0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1, 0,
    1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1,
    1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0,
};

constexpr int kTail = 31;

// "Source Coding": '0'-'9' are 0-9, 'A'-'Z' 10-35, space 36.
int char_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    if (c == ' ') return 36;
    return -1;
}

char value_char(int v) {
    if (v < 10) return static_cast<char>('0' + v);
    if (v < 36) return static_cast<char>('A' + v - 10);
    return ' ';
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return out;
}

SlowProtocol make_protocol() {
    SlowProtocol p;
    p.name = "WSPR";
    p.symbols = 162;
    p.tones = 4;
    // "Modulation": 12000 / 8192 Hz, and a symbol the reciprocal of that.
    p.tone_spacing_hz = 12000.0 / 8192.0;
    p.symbol_seconds = 8192.0 / 12000.0;
    // NOT SPECIFIED by the document; see the header.
    p.slot_seconds = 120.0;
    p.nominal_start_seconds = 1.0;
    p.start_min_seconds = 0.0;
    p.start_max_seconds = 4.0;
    // Symbol = Sync + 2 Data: tones {sync, sync + 2} carry energy.
    for (const auto s : kSync) {
        p.sync_on.push_back({s, s + 2});
        p.sync_off.push_back({1 - s, 3 - s});
    }
    return p;
}

}  // namespace

const SlowProtocol& wspr_protocol() {
    static const SlowProtocol p = make_protocol();
    return p;
}

std::span<const std::uint8_t> wspr_sync_vector() { return kSync; }

Expected<WsprPayload> wspr_pack(std::string_view call_in, std::string_view grid_in, int power) {
    std::string call = upper(call_in);
    // "The third character is forced to be always a number": a space goes in
    // front when the second is the digit, then spaces pad to six.
    if (call.size() >= 2 && call[1] >= '0' && call[1] <= '9') call.insert(call.begin(), ' ');
    if (call.size() > 6 || call.size() < 3) return fail(std::format("WSPR callsign {} is not 3 to 6 characters", call_in));
    call.resize(6, ' ');
    const int c1 = char_value(call[0]);
    const int c2 = char_value(call[1]);
    const int c3 = char_value(call[2]);
    if (c1 < 0 || c2 < 0 || c2 == 36 || c3 < 0 || c3 > 9) return fail(std::format("WSPR cannot encode callsign {}", call_in));
    std::uint32_t n = static_cast<std::uint32_t>(c1);
    n = n * 36 + static_cast<std::uint32_t>(c2);
    n = n * 10 + static_cast<std::uint32_t>(c3);
    for (int i = 3; i < 6; ++i) {
        const int c = char_value(call[static_cast<std::size_t>(i)]);
        if (c < 10) return fail(std::format("WSPR callsign {} ends in a digit", call_in));
        n = 27 * n + static_cast<std::uint32_t>(c - 10);
    }
    const std::string grid = upper(grid_in);
    if (grid.size() != 4 || grid[0] < 'A' || grid[0] > 'R' || grid[1] < 'A' || grid[1] > 'R' || grid[2] < '0' ||
        grid[2] > '9' || grid[3] < '0' || grid[3] > '9') {
        return fail(std::format("WSPR locator {} is not four characters AA00 to RR99", grid_in));
    }
    if (power < 0 || power > 60) return fail(std::format("WSPR power {} dBm is outside 0 to 60", power));
    const int m1 = (179 - 10 * (grid[0] - 'A') - (grid[2] - '0')) * 180 + 10 * (grid[1] - 'A') + (grid[3] - '0');
    const auto m = static_cast<std::uint32_t>(m1 * 128 + power + 64);
    WsprPayload bits{};
    for (int i = 0; i < 28; ++i) bits[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((n >> (27 - i)) & 1U);
    for (int i = 0; i < 22; ++i) bits[static_cast<std::size_t>(28 + i)] = static_cast<std::uint8_t>((m >> (21 - i)) & 1U);
    return bits;
}

std::optional<WsprMessage> wspr_unpack(const WsprPayload& bits) {
    std::uint32_t n = 0;
    std::uint32_t m = 0;
    for (int i = 0; i < 28; ++i) n = (n << 1U) | bits[static_cast<std::size_t>(i)];
    for (int i = 0; i < 22; ++i) m = (m << 1U) | bits[static_cast<std::size_t>(28 + i)];
    if (n >= 262177560U) return std::nullopt;
    std::string call(6, ' ');
    for (int i = 5; i >= 3; --i) {
        call[static_cast<std::size_t>(i)] = value_char(static_cast<int>(n % 27) + 10);
        n /= 27;
    }
    call[2] = value_char(static_cast<int>(n % 10));
    n /= 10;
    call[1] = value_char(static_cast<int>(n % 36));
    n /= 36;
    call[0] = value_char(static_cast<int>(n));
    // A space may only lead or trail.
    const auto first = call.find_first_not_of(' ');
    const auto last = call.find_last_not_of(' ');
    if (first > 1) return std::nullopt;
    const std::string trimmed = call.substr(first, last - first + 1);
    if (trimmed.find(' ') != std::string::npos) return std::nullopt;

    const int pwr = static_cast<int>(m % 128) - 64;
    if (pwr < 0 || pwr > 60) return std::nullopt;
    const int m1 = static_cast<int>(m / 128);
    if (m1 < 179 || m1 > 32220) return std::nullopt;
    const int q = 179 - m1 / 180;
    const int r = m1 % 180;
    const int l1 = q / 10;
    const int l3 = q % 10;
    const int l2 = r / 10;
    const int l4 = r % 10;
    if (l1 > 17 || l2 > 17) return std::nullopt;
    WsprMessage msg;
    msg.call = trimmed;
    msg.grid = {static_cast<char>('A' + l1), static_cast<char>('A' + l2), static_cast<char>('0' + l3),
                static_cast<char>('0' + l4)};
    msg.power_dbm = pwr;
    const int last_digit = pwr % 10;
    msg.odd_power = !(last_digit == 0 || last_digit == 3 || last_digit == 7);
    msg.text = std::format("{} {} {}", msg.call, msg.grid, msg.power_dbm);
    return msg;
}

std::vector<int> wspr_channel_tones(const WsprPayload& payload) {
    std::vector<std::uint8_t> bits(payload.begin(), payload.end());
    bits.resize(kWsprBits + kTail, 0);
    const auto coded = wsjt_conv_encode(bits);
    const auto data = wsjt_interleave(coded);
    std::vector<int> out(162);
    for (std::size_t i = 0; i < 162; ++i) out[i] = kSync[i] + 2 * data[i];
    return out;
}

WsprDecoder::WsprDecoder(WsprConfig config, SlowStream stream)
    : config_(std::move(config)), stream_(std::move(stream)) {}

Expected<WsprDecoder> WsprDecoder::create(WsprConfig config) {
    if (!(config.low_hz >= 0.0 && config.high_hz > config.low_hz && config.high_hz < 5000.0)) {
        return fail("WSPR search range must lie within 0 to 5000 Hz");
    }
    auto stream = SlowStream::create(wspr_protocol().slot_seconds, config.rate, config.utc_of_sample);
    if (!stream) return std::unexpected(stream.error());
    return WsprDecoder(std::move(config), std::move(*stream));
}

Status WsprDecoder::process(ConstRealSpan audio, std::vector<WsprDecode>& out) {
    stream_.process(audio, [&](std::span<const float> slot, double utc, SampleIndex start) {
        decode_slot(slot, utc, start, out);
    });
    return {};
}

void WsprDecoder::decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                              std::vector<WsprDecode>& out) const {
    const SlowProtocol& p = wspr_protocol();
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

    std::map<std::string, WsprDecode> found;
    for (const auto& c : slot.candidates(search)) {
        const SlowSpectra s = slot.demodulate(c, search);
        const double a = slow_amplitude(s, 0, 3);
        std::vector<float> llr(162);
        for (int i = 0; i < 162; ++i) {
            const int q = kSync[static_cast<std::size_t>(i)];
            const double p0 = s.at(i, q);
            const double p1 = s.at(i, q + 2);
            llr[static_cast<std::size_t>(i)] = static_cast<float>(slow_log_i0(2.0 * a * std::sqrt(p0)) - slow_log_i0(2.0 * a * std::sqrt(p1)));
        }
        const auto soft = wsjt_deinterleave(llr);
        auto r = fano_decode(fano, soft);
        if (!r || !r->decoded) continue;
        WsprPayload bits{};
        std::copy_n(r->bits.begin(), kWsprBits, bits.begin());
        auto msg = wspr_unpack(bits);
        if (!msg) continue;
        // NOT SPECIFIED: a decode is kept only if no more than 30% of the hard
        // decisions on its data bits disagree with its own code word.
        const auto tones = wspr_channel_tones(bits);
        int wrong = 0;
        for (int i = 0; i < 162; ++i) {
            const int q = kSync[static_cast<std::size_t>(i)];
            const int hard = s.at(i, q + 2) > s.at(i, q) ? 1 : 0;
            if (hard != tones[static_cast<std::size_t>(i)] / 2) ++wrong;
        }
        if (wrong > 162 * 3 / 10) continue;
        WsprDecode d;
        d.slot_utc = slot_utc;
        d.slot_start_sample = slot_start;
        d.frequency_hz = s.where.frequency_hz;
        d.dt_seconds = s.where.start_seconds - p.nominal_start_seconds;
        d.drift_hz = s.where.drift_hz;
        d.snr_db = slow_snr_2500(p, s, tones);
        d.message = std::move(*msg);
        auto it = found.find(d.message.text);
        if (it == found.end() || it->second.snr_db < d.snr_db) found[d.message.text] = d;
    }
    std::vector<WsprDecode> got;
    for (auto& [k, v] : found) got.push_back(std::move(v));
    std::sort(got.begin(), got.end(), [](const WsprDecode& a, const WsprDecode& b) { return a.frequency_hz < b.frequency_hz; });
    out.insert(out.end(), got.begin(), got.end());
}

}  // namespace revenant::decode
