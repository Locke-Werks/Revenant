// GPS L2C and L5 CNAV. See gps_cnav.h for the documents.

#include "core/decode/gps_cnav.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "core/decode/gps_l1c.h"  // crc24q, the same CRC-24Q

namespace revenant::decode::gnss {

namespace {

// IS-GPS-200N Table 3-IIa, PRN 1 to 37: L2 CM and L2 CL initial and end
// shift register states, octal.
struct L2cRow {
    std::uint32_t cm_initial, cl_initial, cm_end, cl_end;
};
constexpr std::array<L2cRow, 37> kL2c = {{
    {0742417664, 0624145772, 0552566002, 0267724236},
    {0756014035, 0506610362, 034445034, 0167516066},
    {02747144, 0220360016, 0723443711, 0771756405},
    {066265724, 0710406104, 0511222013, 047202624},
    {0601403471, 01143345, 0463055213, 052770433},
    {0703232733, 053023326, 0667044524, 0761743665},
    {0124510070, 0652521276, 0652322653, 0133015726},
    {0617316361, 0206124777, 0505703344, 0610611511},
    {047541621, 015563374, 0520302775, 0352150323},
    {0733031046, 0561522076, 0244205506, 051266046},
    {0713512145, 023163525, 0236174002, 0305611373},
    {024437606, 0117776450, 0654305531, 0504676773},
    {021264003, 0606516355, 0435070571, 0272572634},
    {0230655351, 03037343, 0630431251, 0731320771},
    {01314400, 046515565, 0234043417, 0631326563},
    {0222021506, 0671511621, 0535540745, 0231516360},
    {0540264026, 0605402220, 043056734, 030367366},
    {0205521705, 02576207, 0731304103, 0713543613},
    {064022144, 0525163451, 0412120105, 0232674654},
    {0120161274, 0266527765, 0365636111, 0641733155},
    {044023533, 06760703, 0143324657, 0730125345},
    {0724744327, 0501474556, 0110766462, 0316074},
    {045743577, 0743747443, 0602405203, 0171313614},
    {0741201660, 0615534726, 0177735650, 01523662},
    {0700274134, 0763621420, 0630177560, 023457250},
    {010247261, 0720727474, 0653467107, 0330733254},
    {0713433445, 0700521043, 0406576630, 0625055726},
    {0737324162, 0222567263, 0221777100, 0476524061},
    {0311627434, 0132765304, 0773266673, 0602066031},
    {0710452007, 0746332245, 0100010710, 012412526},
    {0722462133, 0102300466, 0431037132, 0705144501},
    {050172213, 0255231716, 0624127475, 0615373171},
    {0500653703, 0437661701, 0154624012, 041637664},
    {0755077436, 0717047302, 0275636742, 0100107264},
    {0136717361, 0222614207, 0644341556, 0634251723},
    {0756675453, 0561123307, 0514260662, 0257012032},
    {0435506112, 0240713073, 0133501670, 0703702423},
}};

// IS-GPS-705J Table 3-Ia, PRN 1 to 37: XB code advance in chips for I5 and
// Q5, and the initial XB code states as printed (rightmost bit out first).
struct L5Row {
    std::uint16_t i5_advance, q5_advance, i5_state, q5_state;
};
constexpr std::array<L5Row, 37> kL5 = {{
    {266, 1701, 0b0101011100100, 0b1001011001100},
    {365, 323, 0b1100000110101, 0b0100011110110},
    {804, 5292, 0b0100000001000, 0b1111000100011},
    {1138, 2020, 0b1011000100110, 0b0011101101010},
    {1509, 5429, 0b1110111010111, 0b0011110110010},
    {1559, 7136, 0b0110011111010, 0b0101010101001},
    {1756, 1041, 0b1010010011111, 0b1111110000001},
    {2084, 5947, 0b1011110100100, 0b0110101101000},
    {2170, 4315, 0b1111100101011, 0b1011101000011},
    {2303, 148, 0b0111111011110, 0b0010010000110},
    {2527, 535, 0b0000100111010, 0b0001000000101},
    {2687, 1939, 0b1110011111001, 0b0101011000101},
    {2930, 5206, 0b0001110011100, 0b0100110100101},
    {3471, 5910, 0b0100000100111, 0b1010000111111},
    {3940, 3595, 0b0110101011010, 0b1011110001111},
    {4132, 5135, 0b0001111001001, 0b1101001011111},
    {4332, 6082, 0b0100110001111, 0b1110011001000},
    {4924, 6990, 0b1111000011110, 0b1011011100100},
    {5343, 3546, 0b1100100011111, 0b0011001011011},
    {5443, 1523, 0b0110101101101, 0b1100001110001},
    {5641, 4548, 0b0010000001000, 0b0110110010000},
    {5816, 4484, 0b1110111101111, 0b0010110001110},
    {5898, 1893, 0b1000011111110, 0b1000101111101},
    {5918, 3961, 0b1100010110100, 0b0110111110011},
    {5955, 7106, 0b1101001101101, 0b0100010011011},
    {6243, 5299, 0b1010110010110, 0b0101010111100},
    {6345, 4660, 0b0101011011110, 0b1000011111010},
    {6477, 276, 0b0111101010110, 0b1111101000010},
    {6518, 4389, 0b0101111100001, 0b0101000100100},
    {6875, 3783, 0b1000010110111, 0b1000001111001},
    {7168, 1591, 0b0001010011110, 0b0101111100101},
    {7187, 1601, 0b0000010111001, 0b1001000101010},
    {7329, 749, 0b1101010000001, 0b1011001000100},
    {7577, 1387, 0b1101111111001, 0b1111001000100},
    {7720, 1661, 0b1111011011100, 0b0110010110011},
    {7777, 3210, 0b1001011001000, 0b0011110101111},
    {8057, 708, 0b0011010010000, 0b0010011010001},
}};

constexpr std::uint32_t kL2cPoly = 01112225171;
constexpr std::uint32_t kL2cMask = (1U << 27) - 1U;

std::vector<std::uint8_t> l2c_bits(std::uint32_t initial, std::size_t chips) {
    // Figure 3-13, modular: the register shifts towards its output end, and
    // a one shifted out is fed back into every stage the polynomial taps.
    // With the state's bit 0 as the output stage this reproduces every end
    // state of Table 3-IIa.
    std::vector<std::uint8_t> out(chips);
    std::uint32_t s = initial;
    for (std::size_t n = 0; n < chips; ++n) {
        const std::uint32_t o = s & 1U;
        out[n] = static_cast<std::uint8_t>(o);
        s >>= 1;
        if (o != 0) {
            s ^= (kL2cPoly >> 1) & kL2cMask;
        }
    }
    return out;
}

std::vector<std::int8_t> to_pm(const std::vector<std::uint8_t>& bits) {
    std::vector<std::int8_t> out(bits.size());
    for (std::size_t i = 0; i < bits.size(); ++i) {
        out[i] = bits[i] != 0 ? std::int8_t{-1} : std::int8_t{1};
    }
    return out;
}

std::vector<std::int8_t> from_string(const char* s) {
    std::vector<std::int8_t> out;
    for (; *s != 0; ++s) {
        out.push_back(*s == '1' ? std::int8_t{-1} : std::int8_t{1});
    }
    return out;
}

// A 13-stage Fibonacci register as Figures 3-4 and 3-5 draw it: stages 1 to
// 13 shifting towards 13, output from 13, feedback into 1 from the taps.
struct Reg13 {
    std::array<std::uint8_t, 14> st{};
    std::uint32_t taps = 0;  // bit k for tap k
    void ones() { st.fill(1); }
    std::uint8_t clock() {
        const std::uint8_t out = st[13];
        std::uint8_t fb = 0;
        for (unsigned k = 1; k <= 13; ++k) {
            if (((taps >> k) & 1U) != 0) {
                fb ^= st[k];
            }
        }
        for (unsigned k = 13; k > 1; --k) {
            st[k] = st[k - 1];
        }
        st[1] = fb;
        return out;
    }
};

constexpr std::uint32_t kXaTaps = (1U << 9) | (1U << 10) | (1U << 12) | (1U << 13);
constexpr std::uint32_t kXbTaps =
    (1U << 1) | (1U << 3) | (1U << 4) | (1U << 6) | (1U << 7) | (1U << 8) | (1U << 12) | (1U << 13);

}  // namespace

std::uint32_t l2c_register_end(std::uint32_t initial, std::size_t chips) {
    std::uint32_t s = initial;
    for (std::size_t n = 0; n + 1 < chips; ++n) {
        const std::uint32_t o = s & 1U;
        s >>= 1;
        if (o != 0) {
            s ^= (kL2cPoly >> 1) & kL2cMask;
        }
    }
    return s;
}

std::uint32_t l2c_initial_state(unsigned prn, bool cl) {
    if (prn < 1 || prn > kL2c.size()) {
        return 0;
    }
    return cl ? kL2c[prn - 1].cl_initial : kL2c[prn - 1].cm_initial;
}

std::uint32_t l2c_table_end_state(unsigned prn, bool cl) {
    if (prn < 1 || prn > kL2c.size()) {
        return 0;
    }
    return cl ? kL2c[prn - 1].cl_end : kL2c[prn - 1].cm_end;
}

std::vector<std::int8_t> l2cm_code(unsigned prn) {
    if (prn < 1 || prn > kL2c.size()) {
        return {};
    }
    return to_pm(l2c_bits(kL2c[prn - 1].cm_initial, 10230));
}

std::vector<std::int8_t> l2cl_code(unsigned prn) {
    if (prn < 1 || prn > kL2c.size()) {
        return {};
    }
    return to_pm(l2c_bits(kL2c[prn - 1].cl_initial, 767250));
}

std::vector<std::int8_t> l2cm_replica(unsigned prn) {
    const auto cm = l2cm_code(prn);
    std::vector<std::int8_t> out(cm.size() * 2, 0);
    for (std::size_t i = 0; i < cm.size(); ++i) {
        out[2 * i] = cm[i];
    }
    return out;
}

std::vector<std::int8_t> l5_code(unsigned prn, bool q5) {
    if (prn < 1 || prn > kL5.size()) {
        return {};
    }
    // 3.3.2.2: XA from all ones, short cycled after 8190 chips; XB from all
    // ones advanced by the table's count, left to run its natural 8191.
    Reg13 xa;
    xa.taps = kXaTaps;
    xa.ones();
    Reg13 xb;
    xb.taps = kXbTaps;
    xb.ones();
    const unsigned adv = q5 ? kL5[prn - 1].q5_advance : kL5[prn - 1].i5_advance;
    for (unsigned k = 0; k < adv; ++k) {
        (void)xb.clock();
    }
    std::vector<std::uint8_t> bits(10230);
    for (std::size_t n = 0; n < bits.size(); ++n) {
        if (n == 8190) {
            xa.ones();
        }
        bits[n] = static_cast<std::uint8_t>(xa.clock() ^ xb.clock());
    }
    return to_pm(bits);
}

unsigned l5_table_xb_state(unsigned prn, bool q5) {
    if (prn < 1 || prn > kL5.size()) {
        return 0;
    }
    return q5 ? kL5[prn - 1].q5_state : kL5[prn - 1].i5_state;
}

std::vector<std::int8_t> l5_nh10() {
    return from_string("0000110101");  // IS-GPS-705J 3.3.3.1.2
}

std::vector<std::int8_t> l5_nh20() {
    return from_string("00000100110101001110");  // IS-GPS-705J 3.3.2.3
}

// ---------------------------------------------------------------------------

unsigned CnavMessage::field(unsigned first, unsigned count) const {
    unsigned v = 0;
    for (unsigned i = 0; i < count; ++i) {
        v = (v << 1U) | bits[first - 1 + i];
    }
    return v;
}

CnavFramer::CnavFramer(unsigned prn) : prn_(prn) {
    for (auto& ph : phases_) {
        StreamingViterbiConfig vc;
        vc.memory = 6;
        vc.generators = {kCnavG1, kCnavG2};
        auto v = StreamingViterbi::create(vc);
        if (v) {
            ph.viterbi.emplace(std::move(*v));
        }
    }
}

void CnavFramer::push(float soft, SampleIndex sample, std::vector<CnavMessage>& out) {
    // Keep the symbols near unit size whatever the correlator gain.
    const double mag = std::abs(static_cast<double>(soft));
    scale_ = symbols_ == 0 ? mag : scale_ + 0.01 * (mag - scale_);
    const float v = scale_ > 0.0 ? static_cast<float>(soft / scale_) : 0.0F;
    symbol_samples_.push_back(sample);
    const std::size_t n = symbols_++;
    for (std::size_t phi = 0; phi < 2; ++phi) {
        if (n < phi) {
            continue;
        }
        auto& ph = phases_[phi];
        if (!ph.viterbi) {
            continue;
        }
        std::vector<std::uint8_t> bits;
        ph.viterbi->push(std::span<const float>(&v, 1), bits);
        for (auto b : bits) {
            const std::size_t sym = phi + 2 * ph.decided++;
            ph.bits.push_back(b);
            ph.samples.push_back(sym < symbol_samples_.size() ? symbol_samples_[sym] : sample);
        }
        if (!bits.empty()) {
            scan(ph, out);
        }
    }
}

void CnavFramer::scan(Phase& ph, std::vector<CnavMessage>& out) {
    static constexpr std::array<std::uint8_t, 8> kPreamble = {1, 0, 0, 0, 1, 0, 1, 1};
    std::size_t i = 0;
    while (i + 300 <= ph.bits.size()) {
        bool match = true, inv = true;
        for (std::size_t k = 0; k < 8; ++k) {
            match = match && ph.bits[i + k] == kPreamble[k];
            inv = inv && ph.bits[i + k] != kPreamble[k];
        }
        if (match || inv) {
            CnavMessage m;
            for (std::size_t k = 0; k < 300; ++k) {
                m.bits[k] = static_cast<std::uint8_t>(ph.bits[i + k] ^ (inv ? 1U : 0U));
            }
            if (crc24q(m.bits) == 0) {
                m.prn = prn_;
                m.sample = ph.samples[i];
                m.msg_prn = m.field(9, 6);
                m.type = m.field(15, 6);
                m.tow = m.field(21, 17);
                m.alert = m.field(38, 1) != 0;
                if (m.type == 10) {
                    m.wn = m.field(39, 13);
                    m.top = m.field(55, 11);
                    m.toe = m.field(71, 11);
                }
                // Both pairings can only both pass the CRC on the same
                // message; report it once.
                const SampleIndex key = (static_cast<SampleIndex>(m.tow) << 6) | m.type;
                if (!have_last_ || key != last_message_) {
                    out.push_back(m);
                    last_message_ = key;
                    have_last_ = true;
                }
                i += 300;
                continue;
            }
        }
        ++i;
    }
    // Everything before i has been looked at; keep what a message could
    // still start in.
    if (i > 0) {
        ph.bits.erase(ph.bits.begin(), ph.bits.begin() + static_cast<std::ptrdiff_t>(i));
        ph.samples.erase(ph.samples.begin(), ph.samples.begin() + static_cast<std::ptrdiff_t>(i));
    }
}

// ---------------------------------------------------------------------------

FrontEndConfig cnav_front_end(CnavBand band, dsp::SampleRate rate) {
    FrontEndConfig fc;
    fc.sample_rate = rate;
    if (band == CnavBand::L2C) {
        // One 20 ms period coherent: 25 Hz bins keep the worst-case loss under
        // 1 dB. Three periods give the epoch-aligned refinement in
        // gnss_acq.cpp two whole symbols to work on. The threshold sits above the largest of the
        // noise-only cells in tests/decode/test_gps_cnav.cpp.
        fc.acquisition.doppler_step_hz = 25.0;
        fc.acquisition.noncoherent_periods = 3;
        fc.acquisition.threshold = 15.0;
        fc.acquisition.retry_periods = 1500;
        fc.tracking = TrackingConfig{2.0, 8.0, 0.5, 0.5, 25, 50, 24.0, 100};
    }
    return fc;
}

Expected<GpsCnavDecoder> GpsCnavDecoder::create(GpsCnavConfig config) {
    if (config.sample_rate <= 0) {
        return fail("gps cnav: sample rate must be positive");
    }
    if (config.prns.empty()) {
        for (unsigned p = 1; p <= 32; ++p) {
            config.prns.push_back(p);
        }
    }
    if (!config.keep_front_end) {
        config.front_end = cnav_front_end(config.band, config.sample_rate);
    }
    config.front_end.sample_rate = config.sample_rate;
    if (config.status_periods == 0) {
        config.status_periods = config.band == CnavBand::L2C ? 50 : 1000;
    }
    GpsCnavDecoder d;
    std::vector<SignalSpec> specs;
    for (auto prn : config.prns) {
        if (prn < 1 || prn > 37) {
            return fail("gps cnav: PRN out of 1 to 37");
        }
        SignalSpec s;
        s.id = prn;
        if (config.band == CnavBand::L2C) {
            s.code = l2cm_replica(prn);
            s.chip_rate_hz = 1.023e6;
            s.carrier_hz = 1227.60e6;
        } else {
            s.code = l5_code(prn, false);
            s.chip_rate_hz = 10.23e6;
            s.carrier_hz = 1176.45e6;
        }
        s.offset_hz = config.offset_hz;
        specs.push_back(std::move(s));
        d.sats_[prn].framer.emplace(prn);
    }
    auto fe = FrontEnd::create(config.front_end, std::move(specs));
    if (!fe) {
        return std::unexpected(fe.error());
    }
    d.fe_.emplace(std::move(*fe));
    d.nh10_ = l5_nh10();
    d.config_ = std::move(config);
    return d;
}

void GpsCnavDecoder::handle(const Prompt& p, GpsCnavOutput& out) {
    auto it = sats_.find(p.id);
    if (it == sats_.end()) {
        return;
    }
    Sat& sat = it->second;
    if (p.period >= sat.last_status + config_.status_periods) {
        sat.last_status = p.period;
        out.status.push_back({p.id, p.cn0_dbhz, p.doppler_hz, p.phase_locked,
                              config_.band == CnavBand::L2C ? sat.started : sat.synced});
    }
    if (!sat.started) {
        if (!p.phase_locked) {
            return;
        }
        sat.started = true;
    }
    if (config_.band == CnavBand::L2C) {
        // One symbol per 20 ms CM period.
        sat.framer->push(static_cast<float>(p.prompt.real()), p.sample, out.messages);
        return;
    }
    // L5: find the NH10 phase, then sum each symbol's ten periods against it.
    if (!sat.synced) {
        if (sat.re.empty()) {
            sat.first_period = p.period;
        }
        sat.re.push_back(p.prompt.real());
        sat.samples.push_back(p.sample);
        if (sat.re.size() < 200) {
            return;
        }
        double best = -1.0;
        std::size_t shift = 0;
        for (std::size_t s = 0; s < 10; ++s) {
            double total = 0.0;
            for (std::size_t k = s; k + 10 <= sat.re.size(); k += 10) {
                double c = 0.0;
                for (std::size_t j = 0; j < 10; ++j) {
                    c += sat.re[k + j] * nh10_[j];
                }
                total += std::abs(c);
            }
            if (total > best) {
                best = total;
                shift = s;
            }
        }
        sat.synced = true;
        sat.sym_period = sat.first_period + shift;
        const auto re = std::move(sat.re);
        const auto samples = std::move(sat.samples);
        sat.re.clear();
        sat.samples.clear();
        for (std::size_t k = shift; k < re.size(); ++k) {
            const std::size_t j = (k - shift) % 10;
            if (j == 0) {
                sat.acc = 0.0;
                sat.acc_sample = samples[k];
            }
            sat.acc += re[k] * nh10_[j];
            if (j == 9) {
                sat.framer->push(static_cast<float>(sat.acc), sat.acc_sample, out.messages);
            }
        }
        return;
    }
    if (p.period < sat.sym_period) {
        return;
    }
    const std::size_t j = (p.period - sat.sym_period) % 10;
    if (j == 0) {
        sat.acc = 0.0;
        sat.acc_sample = p.sample;
    }
    sat.acc += p.prompt.real() * nh10_[j];
    if (j == 9) {
        sat.framer->push(static_cast<float>(sat.acc), sat.acc_sample, out.messages);
    }
}

void GpsCnavDecoder::feed(std::span<const dsp::Complex32> samples, GpsCnavOutput& out) {
    FrontEndOutput fo;
    fe_->feed(samples, fo);
    out.acquisitions.insert(out.acquisitions.end(), fo.acquisitions.begin(),
                            fo.acquisitions.end());
    for (const auto& l : fo.losses) {
        auto it = sats_.find(l.id);
        if (it != sats_.end()) {
            const unsigned prn = it->first;
            it->second = Sat{};
            it->second.framer.emplace(prn);
        }
    }
    for (const auto& p : fo.prompts) {
        handle(p, out);
    }
}

}  // namespace revenant::decode::gnss
