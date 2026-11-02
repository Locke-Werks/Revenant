// DVB-S decoder. See dvbs.h for the specification and what is decoded.

#include "core/decode/dvbs.h"

#include <algorithm>
#include <cmath>

#include "core/decode/crc.h"

namespace revenant::decode {

namespace {

// H.222.0 Annex A, equation A-1.
constexpr CrcParams kCrc32Mpeg{32, 0x04C11DB7, 0xFFFFFFFF, false, false, 0};

constexpr std::size_t kSyncSpacingBits = kDvbsCodedPacketBytes * 8;
// Sync bytes that must line up before acquisition declares lock.
constexpr std::size_t kAcquireSyncs = 5;
// Decoded bits skipped before the sync search: the Viterbi decoder starts
// with every state equally likely and its first decisions are unreliable.
constexpr std::size_t kWarmupBits = 96;

constexpr std::array<DvbsCodeRate, 5> kAllRates{DvbsCodeRate::R1_2, DvbsCodeRate::R2_3,
                                                DvbsCodeRate::R3_4, DvbsCodeRate::R5_6,
                                                DvbsCodeRate::R7_8};

std::size_t transmitted_period(DvbsCodeRate rate) {
    const auto pattern = dvbs_puncture_pattern(rate);
    return static_cast<std::size_t>(std::count(pattern.begin(), pattern.end(), 1));
}

std::uint8_t byte_at(const std::vector<std::uint8_t>& bits, std::size_t p) {
    std::uint8_t v = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        v = static_cast<std::uint8_t>((v << 1U) | bits[p + i]);
    }
    return v;
}

}  // namespace

std::vector<std::uint8_t> dvbs_puncture_pattern(DvbsCodeRate rate) {
    // EN 300 421 table 2. X and Y rows interleaved per encoder step.
    auto merge = [](std::initializer_list<int> x, std::initializer_list<int> y) {
        std::vector<std::uint8_t> out;
        auto xi = x.begin();
        auto yi = y.begin();
        for (; xi != x.end(); ++xi, ++yi) {
            out.push_back(static_cast<std::uint8_t>(*xi));
            out.push_back(static_cast<std::uint8_t>(*yi));
        }
        return out;
    };
    switch (rate) {
        case DvbsCodeRate::R1_2: return merge({1}, {1});
        case DvbsCodeRate::R2_3: return merge({1, 0}, {1, 1});
        case DvbsCodeRate::R3_4: return merge({1, 0, 1}, {1, 1, 0});
        case DvbsCodeRate::R5_6: return merge({1, 0, 1, 0, 1}, {1, 1, 0, 1, 0});
        case DvbsCodeRate::R7_8: return merge({1, 0, 0, 0, 1, 0, 1}, {1, 1, 1, 1, 0, 1, 0});
    }
    return {};
}

double dvbs_code_rate_value(DvbsCodeRate rate) {
    switch (rate) {
        case DvbsCodeRate::R1_2: return 1.0 / 2.0;
        case DvbsCodeRate::R2_3: return 2.0 / 3.0;
        case DvbsCodeRate::R3_4: return 3.0 / 4.0;
        case DvbsCodeRate::R5_6: return 5.0 / 6.0;
        case DvbsCodeRate::R7_8: return 7.0 / 8.0;
    }
    return 0.0;
}

const char* dvbs_code_rate_name(DvbsCodeRate rate) {
    switch (rate) {
        case DvbsCodeRate::R1_2: return "1/2";
        case DvbsCodeRate::R2_3: return "2/3";
        case DvbsCodeRate::R3_4: return "3/4";
        case DvbsCodeRate::R5_6: return "5/6";
        case DvbsCodeRate::R7_8: return "7/8";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Energy dispersal, EN 300 421 4.4.1
// ---------------------------------------------------------------------------

void DvbsPrbs::reset() {
    // Figure 2: registers 1 to 15 loaded with 1 0 0 1 0 1 0 1 0 0 0 0 0 0 0.
    constexpr std::array<int, 15> init{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    reg_ = 0;
    for (std::size_t i = 0; i < init.size(); ++i) {
        reg_ = static_cast<std::uint16_t>(reg_ | (init[i] << i));
    }
}

std::uint8_t DvbsPrbs::next_bit() {
    // 1 + X^14 + X^15: the output is register 14 XOR register 15, and is fed
    // back into register 1 as everything shifts one place along.
    const auto out = static_cast<std::uint8_t>(((reg_ >> 13U) ^ (reg_ >> 14U)) & 1U);
    reg_ = static_cast<std::uint16_t>(((reg_ << 1U) | out) & 0x7FFFU);
    return out;
}

std::uint8_t DvbsPrbs::next_byte() {
    std::uint8_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = static_cast<std::uint8_t>((v << 1U) | next_bit());
    }
    return v;
}

void dvbs_energy_dispersal(std::span<std::uint8_t> eight_packets, bool transmit) {
    DvbsPrbs prbs;
    for (std::size_t p = 0; p < 8 && (p + 1) * kTsPacketBytes <= eight_packets.size(); ++p) {
        auto packet = eight_packets.subspan(p * kTsPacketBytes, kTsPacketBytes);
        if (p == 0) {
            packet[0] = transmit ? kTsSyncInverted : kTsSync;
        } else {
            // The PRBS runs through the sync byte with its output disabled.
            (void)prbs.next_byte();
            packet[0] = kTsSync;
        }
        for (std::size_t i = 1; i < kTsPacketBytes; ++i) {
            packet[i] = static_cast<std::uint8_t>(packet[i] ^ prbs.next_byte());
        }
    }
}

// ---------------------------------------------------------------------------
// Convolutional interleaver, EN 300 421 4.4.2 and figure 4
// ---------------------------------------------------------------------------

DvbsInterleaver::DvbsInterleaver(bool deinterleave) : deinterleave_(deinterleave) { reset(); }

void DvbsInterleaver::reset() {
    for (std::size_t j = 0; j < kDvbsInterleaveDepth; ++j) {
        const std::size_t branch = deinterleave_ ? kDvbsInterleaveDepth - 1 - j : j;
        fifo_[j].assign(branch * kDvbsInterleaveCell, 0);
        head_[j] = 0;
    }
    branch_ = 0;
}

std::uint8_t DvbsInterleaver::push(std::uint8_t byte) {
    auto& fifo = fifo_[branch_];
    std::uint8_t out = byte;
    if (!fifo.empty()) {
        auto& head = head_[branch_];
        out = fifo[head];
        fifo[head] = byte;
        head = (head + 1) % fifo.size();
    }
    branch_ = (branch_ + 1) % kDvbsInterleaveDepth;
    return out;
}

const Rs8Codec& dvbs_rs() {
    // EN 300 421 4.4.2: p(x) = x^8 + x^4 + x^3 + x^2 + 1, roots lambda^0 to
    // lambda^15, 16 check bytes.
    static const Rs8Codec codec = *Rs8Codec::create({0x11D, 0, 16});
    return codec;
}

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

Expected<DvbsDecoder> DvbsDecoder::create(const DvbsConfig& config) {
    PskDemodConfig pc;
    pc.rate = config.rate;
    pc.symbol_rate = config.symbol_rate;
    pc.modulation = PskModulation::Qpsk;
    pc.roll_off = config.roll_off;
    pc.carrier_bandwidth = config.carrier_bandwidth;
    pc.timing_bandwidth = config.timing_bandwidth;
    pc.max_offset_hz = config.max_offset_hz;
    pc.bit_map = kDvbsQpskBitMap;
    auto demod = PskDemod::create(pc);
    if (!demod) {
        return std::unexpected(with_context(demod.error(), "DVB-S demodulator"));
    }
    if (config.sync_loss == 0) {
        return fail("DVB-S: sync_loss must be at least one");
    }
    DvbsDecoder d;
    d.config_ = config;
    d.demod_ = std::move(*demod);
    double lowest = 1.0;
    if (config.code_rate) {
        lowest = dvbs_code_rate_value(*config.code_rate);
    } else {
        lowest = 0.5;
    }
    const double decoded_bits =
        static_cast<double>(kWarmupBits + 8 + kSyncSpacingBits * kAcquireSyncs + config.traceback);
    d.acquisition_symbols_ = static_cast<std::size_t>(std::ceil(decoded_bits / (2.0 * lowest)));
    d.reset();
    return d;
}

void DvbsDecoder::reset() {
    demod_.reset();
    lose_lock();
    stats_ = {};
    out_.clear();
}

std::optional<DvbsCodeRate> DvbsDecoder::code_rate() const {
    if (!locked_) {
        return std::nullopt;
    }
    return hyp_.rate;
}

void DvbsDecoder::lose_lock() {
    locked_ = false;
    window_.clear();
    viterbi_.reset();
    deinterleaver_.reset();
    soft_skipped_ = 0;
    bit_count_ = 0;
    byte_acc_ = 0;
    byte_bits_ = 0;
    byte_count_ = 0;
    sync_misses_ = 0;
    deinterleaved_ = 0;
    coded_fill_ = 0;
    group_index_ = -1;
}

std::span<const DvbsPacket> DvbsDecoder::process(dsp::ConstComplexSpan in) {
    out_.clear();
    const auto symbols = demod_.process(in);
    for (const auto& s : symbols) {
        on_symbol(s.symbol);
    }
    return out_;
}

namespace {

Complex32 transform(Complex32 z, bool quarter, bool conjugate) {
    if (conjugate) {
        z = std::conj(z);
    }
    if (quarter) {
        z = Complex32{-z.imag(), z.real()};
    }
    return z;
}

}  // namespace

void DvbsDecoder::on_symbol(Complex32 symbol) {
    if (locked_) {
        feed_locked(symbol);
        return;
    }
    window_.push_back(symbol);
    if (window_.size() >= acquisition_symbols_) {
        try_acquire();
    }
}

std::optional<DvbsDecoder::Hypothesis> DvbsDecoder::test(DvbsCodeRate rate, bool quarter,
                                                         bool conjugate,
                                                         std::size_t skip) const {
    StreamingViterbiConfig vc;
    vc.memory = kDvbsMemory;
    vc.generators.assign(kDvbsGenerators.begin(), kDvbsGenerators.end());
    vc.traceback = config_.traceback;
    vc.puncture = dvbs_puncture_pattern(rate);
    auto viterbi = StreamingViterbi::create(vc);
    if (!viterbi) {
        return std::nullopt;
    }
    std::vector<float> soft;
    soft.reserve(window_.size() * 2);
    for (const auto& z : window_) {
        const auto t = transform(z, quarter, conjugate);
        // Figure 5: a zero bit is a positive rail, which is the SoftSpan sign.
        soft.push_back(t.real());
        soft.push_back(t.imag());
    }
    std::vector<std::uint8_t> bits;
    viterbi->push(std::span<const float>(soft).subspan(skip), bits);
    viterbi->flush(false, bits);

    const std::size_t span = kSyncSpacingBits * (kAcquireSyncs - 1) + 8;
    for (std::size_t p = kWarmupBits; p + span <= bits.size(); ++p) {
        std::size_t inverted = 0;
        bool ok = true;
        for (std::size_t k = 0; k < kAcquireSyncs; ++k) {
            const auto b = byte_at(bits, p + k * kSyncSpacingBits);
            if (b == kTsSyncInverted) {
                ++inverted;
            } else if (b != kTsSync) {
                ok = false;
                break;
            }
        }
        if (ok) {
            Hypothesis h;
            h.rate = rate;
            h.quarter = quarter;
            h.conjugate = conjugate;
            h.skip = skip;
            h.sync_bit = p;
            // A group of eight has one inverted sync; a half-turned stream
            // shows the complement, one plain to seven inverted.
            h.invert = inverted * 2 > kAcquireSyncs;
            return h;
        }
    }
    return std::nullopt;
}

void DvbsDecoder::try_acquire() {
    std::optional<Hypothesis> found;
    for (const auto rate : kAllRates) {
        if (config_.code_rate && *config_.code_rate != rate) {
            continue;
        }
        const std::size_t period = transmitted_period(rate);
        for (int conj = 0; conj < 2 && !found; ++conj) {
            for (int quarter = 0; quarter < 2 && !found; ++quarter) {
                for (std::size_t skip = 0; skip < period && !found; ++skip) {
                    found = test(rate, quarter != 0, conj != 0, skip);
                }
            }
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        // Slide by half a window so the next attempt sees new signal.
        window_.erase(window_.begin(),
                      window_.begin() + static_cast<std::ptrdiff_t>(window_.size() / 2));
        return;
    }

    hyp_ = *found;
    StreamingViterbiConfig vc;
    vc.memory = kDvbsMemory;
    vc.generators.assign(kDvbsGenerators.begin(), kDvbsGenerators.end());
    vc.traceback = config_.traceback;
    vc.puncture = dvbs_puncture_pattern(hyp_.rate);
    viterbi_ = *StreamingViterbi::create(vc);
    deinterleaver_.emplace(true);
    locked_ = true;
    // Replay the window through the persistent decoder, so the packets it
    // carried are not lost to acquisition.
    std::vector<Complex32> replay;
    replay.swap(window_);
    for (const auto& z : replay) {
        feed_locked(z);
        if (!locked_) {
            break;
        }
    }
}

void DvbsDecoder::feed_locked(Complex32 symbol) {
    const auto t = transform(symbol, hyp_.quarter, hyp_.conjugate);
    std::array<float, 2> soft{t.real(), t.imag()};
    std::span<const float> s(soft);
    if (soft_skipped_ < hyp_.skip) {
        const std::size_t drop = std::min<std::size_t>(hyp_.skip - soft_skipped_, 2);
        soft_skipped_ += drop;
        s = s.subspan(drop);
    }
    bits_scratch_.clear();
    viterbi_->push(s, bits_scratch_);
    on_bits(bits_scratch_);
}

void DvbsDecoder::on_bits(std::span<const std::uint8_t> bits) {
    for (const auto bit : bits) {
        if (!locked_) {
            return;
        }
        if (bit_count_++ < hyp_.sync_bit) {
            continue;
        }
        byte_acc_ = static_cast<std::uint8_t>((byte_acc_ << 1U) | ((bit ^ (hyp_.invert ? 1U : 0U)) & 1U));
        if (++byte_bits_ == 8) {
            byte_bits_ = 0;
            on_byte(byte_acc_);
        }
    }
}

void DvbsDecoder::on_byte(std::uint8_t byte) {
    if (byte_count_ % kDvbsCodedPacketBytes == 0) {
        if (byte == kTsSync || byte == kTsSyncInverted) {
            sync_misses_ = 0;
        } else if (++sync_misses_ >= config_.sync_loss) {
            ++stats_.sync_losses;
            lose_lock();
            return;
        }
    }
    ++byte_count_;
    const auto out = deinterleaver_->push(byte);
    // The first 11 packets out of the de-interleaver are its initial fill.
    if (deinterleaved_++ < kDvbsInterleaveDelayBytes) {
        return;
    }
    coded_[coded_fill_++] = out;
    if (coded_fill_ == kDvbsCodedPacketBytes) {
        coded_fill_ = 0;
        on_coded_packet();
    }
}

void DvbsDecoder::on_coded_packet() {
    const auto corrected = dvbs_rs().decode(coded_);
    const bool rs_ok = corrected.has_value();

    // 4.4.1: an inverted sync opens a group of eight. Counting carries the
    // group across a packet whose sync byte was lost.
    if (coded_[0] == kTsSyncInverted) {
        group_index_ = 0;
    } else if (group_index_ >= 0) {
        group_index_ = (group_index_ + 1) % 8;
    }
    if (group_index_ < 0) {
        return;  // no group start seen yet, so the PRBS phase is unknown
    }
    if (group_index_ == 0) {
        prbs_.reset();
    } else {
        (void)prbs_.next_byte();
    }

    DvbsPacket packet;
    packet.bytes[0] = kTsSync;
    for (std::size_t i = 1; i < kTsPacketBytes; ++i) {
        packet.bytes[i] = static_cast<std::uint8_t>(coded_[i] ^ prbs_.next_byte());
    }
    packet.rs_ok = rs_ok;
    packet.rs_corrected = rs_ok ? static_cast<std::uint8_t>(*corrected) : 0;
    if (!rs_ok) {
        packet.bytes[1] |= 0x80U;  // transport_error_indicator, H.222.0 2.4.3.3
        ++stats_.rs_failed;
    } else {
        stats_.rs_corrected_bytes += *corrected;
    }
    ++stats_.packets;
    out_.push_back(packet);
}

// ---------------------------------------------------------------------------
// Transport stream
// ---------------------------------------------------------------------------

std::optional<TsHeader> ts_parse_header(std::span<const std::uint8_t> p) {
    // H.222.0 2.4.3.2, table 2-2.
    if (p.size() < kTsPacketBytes || p[0] != kTsSync) {
        return std::nullopt;
    }
    TsHeader h;
    h.transport_error = (p[1] & 0x80U) != 0;
    h.payload_unit_start = (p[1] & 0x40U) != 0;
    h.pid = static_cast<std::uint16_t>(((p[1] & 0x1FU) << 8U) | p[2]);
    h.scrambling = static_cast<std::uint8_t>((p[3] >> 6U) & 3U);
    h.adaptation_field_control = static_cast<std::uint8_t>((p[3] >> 4U) & 3U);
    h.continuity_counter = static_cast<std::uint8_t>(p[3] & 0x0FU);
    std::size_t offset = 4;
    if ((h.adaptation_field_control & 2U) != 0) {
        offset = 5 + static_cast<std::size_t>(p[4]);  // adaptation_field_length
        if (offset > kTsPacketBytes) {
            return std::nullopt;
        }
    }
    h.payload_offset = (h.adaptation_field_control & 1U) != 0 ? offset : kTsPacketBytes;
    return h;
}

std::uint32_t ts_crc32(std::span<const std::uint8_t> bytes) {
    static constexpr Crc crc(kCrc32Mpeg);
    return static_cast<std::uint32_t>(crc.compute(bytes));
}

void TsPsiParser::reset() { *this = TsPsiParser{}; }

bool TsPsiParser::wanted(std::uint16_t pid) const {
    // PID 0 carries the PAT (H.222.0 table 2-3); PID 0x0011 the SDT
    // (EN 300 468 5.2.3).
    if (pid == 0x0000 || pid == 0x0011) {
        return true;
    }
    if (pat_) {
        for (const auto& prog : pat_->programs) {
            if (prog.program_number != 0 && prog.pid == pid) {
                return true;
            }
        }
    }
    return false;
}

void TsPsiParser::push(std::span<const std::uint8_t> packet) {
    const auto h = ts_parse_header(packet);
    if (!h) {
        return;
    }
    ++pid_counts_[h->pid];
    if (h->transport_error || h->scrambling != 0 || !wanted(h->pid)) {
        return;
    }
    if (h->payload_offset >= kTsPacketBytes) {
        return;
    }
    auto payload = packet.subspan(h->payload_offset, kTsPacketBytes - h->payload_offset);
    auto& a = assembly_[h->pid];
    if (h->payload_unit_start) {
        // H.222.0 2.4.4.2: the pointer_field counts the bytes that finish
        // the previous section before the first new one starts.
        const std::size_t pointer = payload[0];
        payload = payload.subspan(1);
        if (pointer > payload.size()) {
            a = {};
            return;
        }
        if (a.active) {
            feed(a, payload.first(pointer));
        }
        a.data.clear();
        a.active = true;
        feed(a, payload.subspan(pointer));
    } else if (a.active) {
        feed(a, payload);
    }
}

void TsPsiParser::feed(Assembly& a, std::span<const std::uint8_t> bytes) {
    a.data.insert(a.data.end(), bytes.begin(), bytes.end());
    while (a.active && !a.data.empty()) {
        if (a.data[0] == 0xFF) {
            // Stuffing to the end of the packet, H.222.0 2.4.4.
            a.data.clear();
            a.active = false;
            return;
        }
        if (a.data.size() < 3) {
            return;
        }
        const std::size_t length = 3 + (((a.data[1] & 0x0FU) << 8U) | a.data[2]);
        if (length > 4096) {
            a = {};
            return;
        }
        if (a.data.size() < length) {
            return;
        }
        on_section(std::span<const std::uint8_t>(a.data).first(length));
        a.data.erase(a.data.begin(), a.data.begin() + static_cast<std::ptrdiff_t>(length));
    }
}

namespace {

std::uint16_t be16(std::span<const std::uint8_t> s, std::size_t i) {
    return static_cast<std::uint16_t>((s[i] << 8U) | s[i + 1]);
}
std::uint16_t pid13(std::span<const std::uint8_t> s, std::size_t i) {
    return static_cast<std::uint16_t>(((s[i] & 0x1FU) << 8U) | s[i + 1]);
}
std::uint16_t len12(std::span<const std::uint8_t> s, std::size_t i) {
    return static_cast<std::uint16_t>(((s[i] & 0x0FU) << 8U) | s[i + 1]);
}

}  // namespace

void TsPsiParser::on_section(std::span<const std::uint8_t> s) {
    // Every table here uses the long form: 8 header bytes, body, CRC_32.
    if (s.size() < 12 || (s[1] & 0x80U) == 0) {
        return;
    }
    const std::size_t end = s.size() - 4;
    const std::uint32_t carried = (std::uint32_t{s[end]} << 24U) | (std::uint32_t{s[end + 1]} << 16U) |
                                  (std::uint32_t{s[end + 2]} << 8U) | s[end + 3];
    if (ts_crc32(s.first(end)) != carried) {
        ++crc_errors_;
        return;
    }
    ++sections_;
    const std::uint8_t table_id = s[0];
    const std::uint16_t extension = be16(s, 3);
    const auto version = static_cast<std::uint8_t>((s[5] >> 1U) & 0x1FU);
    const bool current = (s[5] & 1U) != 0;
    const std::uint8_t section_number = s[6];
    if (!current) {
        return;
    }

    if (table_id == 0x00) {
        // H.222.0 2.4.4.3, table 2-30.
        if (section_number == 0 || !pat_ || pat_->version != version) {
            pat_ = TsPat{extension, version, {}};
        }
        for (std::size_t i = 8; i + 4 <= end; i += 4) {
            pat_->programs.push_back({be16(s, i), pid13(s, i + 2)});
        }
    } else if (table_id == 0x02) {
        // H.222.0 2.4.4.8, table 2-33.
        TsPmt pmt;
        pmt.program_number = extension;
        pmt.version = version;
        pmt.pcr_pid = pid13(s, 8);
        std::size_t i = 12 + len12(s, 10);
        while (i + 5 <= end) {
            pmt.streams.push_back({s[i], pid13(s, i + 1)});
            i += 5 + len12(s, i + 3);
        }
        pmts_[pmt.program_number] = std::move(pmt);
    } else if (table_id == 0x42) {
        // EN 300 468 5.2.3, table 5: the SDT of the actual transport stream.
        if (s.size() < 15) {
            return;
        }
        if (section_number == 0 || !sdt_ || sdt_->version != version) {
            sdt_ = TsSdt{extension, be16(s, 8), version, {}};
        }
        std::size_t i = 11;
        while (i + 5 <= end) {
            TsService svc;
            svc.service_id = be16(s, i);
            svc.running_status = static_cast<std::uint8_t>(s[i + 3] >> 5U);
            svc.free_ca_mode = ((s[i + 3] >> 4U) & 1U) != 0;
            const std::size_t dl = len12(s, i + 3);
            std::size_t d = i + 5;
            const std::size_t dend = std::min(end, d + dl);
            while (d + 2 <= dend) {
                const std::uint8_t tag = s[d];
                const std::size_t len = s[d + 1];
                const std::size_t body = d + 2;
                if (body + len > dend) {
                    break;
                }
                // EN 300 468 6.2.33, table 88: the service_descriptor.
                if (tag == 0x48 && len >= 3) {
                    svc.service_type = s[body];
                    const std::size_t pl = s[body + 1];
                    if (2 + pl < len) {
                        svc.provider.assign(s.begin() + static_cast<std::ptrdiff_t>(body + 2),
                                            s.begin() + static_cast<std::ptrdiff_t>(body + 2 + pl));
                        const std::size_t nl = s[body + 2 + pl];
                        if (3 + pl + nl <= len) {
                            const auto from = body + 3 + pl;
                            svc.name.assign(s.begin() + static_cast<std::ptrdiff_t>(from),
                                            s.begin() + static_cast<std::ptrdiff_t>(from + nl));
                        }
                    }
                }
                d = body + len;
            }
            sdt_->services.push_back(std::move(svc));
            i += 5 + dl;
        }
    }
}

}  // namespace revenant::decode
