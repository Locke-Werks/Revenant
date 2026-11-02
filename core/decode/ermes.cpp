// ERMES receiver. See ermes.h for the specification and what is decoded.

#include "core/decode/ermes.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>

namespace revenant::decode {

namespace {

constexpr std::uint32_t kInfoMask = (1U << kErmesInfoBits) - 1U;

std::uint32_t remainder(std::uint32_t word) {
    // Division of a 30-bit polynomial by the degree-12 generator, clause 7.
    for (int bit = 29; bit >= 12; --bit) {
        if ((word >> static_cast<unsigned>(bit)) & 1U) {
            word ^= kErmesGenerator << static_cast<unsigned>(bit - 12);
        }
    }
    return word & 0xFFFU;
}

struct SyndromeTable {
    // 0 means no pattern of weight one or two has this syndrome.
    std::array<std::uint32_t, 4096> pattern{};
    SyndromeTable() {
        for (unsigned i = 0; i < 30; ++i) {
            pattern[remainder(1U << i)] = 1U << i;
            for (unsigned j = i + 1; j < 30; ++j) {
                pattern[remainder((1U << i) | (1U << j))] = (1U << i) | (1U << j);
            }
        }
    }
};

const SyndromeTable& syndromes() {
    static const SyndromeTable table;
    return table;
}

// 133-4 9.3.1, level back to dibit.
std::uint32_t level_to_dibit(float level) {
    if (level > 2.0F) {
        return 0b10U;
    }
    if (level > 0.0F) {
        return 0b11U;
    }
    if (level > -2.0F) {
        return 0b01U;
    }
    return 0b00U;
}

// The 30 symbol levels of preamble then sync word, 6.2.
std::array<float, 30> sync_pattern() {
    std::array<float, 30> out{};
    for (std::size_t w = 0; w < 2; ++w) {
        const std::uint32_t word = w == 0 ? kErmesPreamble : kErmesSyncWord;
        for (std::size_t s = 0; s < 15; ++s) {
            const std::uint32_t dibit = (word >> (28U - 2U * static_cast<unsigned>(s))) & 3U;
            out[w * 15 + s] = static_cast<float>(kErmesDibitToLevel[dibit]);
        }
    }
    return out;
}

std::uint32_t bits_value(std::span<const std::uint8_t> bits, std::size_t from, std::size_t count) {
    std::uint32_t v = 0;
    for (std::size_t i = 0; i < count; ++i) {
        v = (v << 1U) | (from + i < bits.size() ? bits[from + i] & 1U : 0U);
    }
    return v;
}

// 6.5.1.2: AIF length in bits for an AIT and AIN.
std::size_t aif_bits(bool aii, std::uint8_t ait, std::uint8_t ain) {
    if (!aii) {
        return 0;
    }
    switch (ait) {
        case 0b001:  // 6.5.1.2.1: category and 16-bit length.
            return 18;
        case 0b010:  // 6.5.1.2.2: F plus the field.
            switch (ain) {
                case 0b0001: return 2 + 31;
                case 0b0010: return 2 + 19;
                case 0b0100: return 2 + 13;
                case 0b1000:
                case 0b0111: return 2 + 8;
                default: return 0;
            }
        case 0b100:  // 6.5.1.2.3.
            return ain == 0b0010 ? 5 : 0;
        default:  // 111 has a zero-length AIF, 6.5.1.2.4; the rest are reserved.
            return 0;
    }
}

}  // namespace

std::uint32_t ermes_encode(std::uint32_t info) {
    const std::uint32_t shifted = (info & kInfoMask) << 12U;
    return shifted | remainder(shifted);
}

bool ermes_is_structure_word(std::uint32_t word, std::uint32_t pattern) {
    // The terminator and delimiter of 6.4 and 6.5.4 are not codewords: both
    // sit in the same coset, whose lightest member has weight four or more,
    // so two bit errors still leave them nearer their pattern than any
    // codeword with one error. Checked in tests/decode/test_ermes.cpp.
    return std::popcount((word ^ pattern) & ((1U << 30U) - 1U)) <= 2;
}

ErmesCodeword ermes_decode(std::uint32_t word) {
    word &= (1U << 30U) - 1U;
    const std::uint32_t syndrome = remainder(word);
    if (syndrome == 0) {
        return {word >> 12U, 0, true};
    }
    const std::uint32_t pattern = syndromes().pattern[syndrome];
    if (pattern == 0) {
        return {word >> 12U, 0, false};
    }
    return {(word ^ pattern) >> 12U, std::popcount(pattern), true};
}

std::array<std::uint8_t, 270> ermes_interleave(std::span<const std::uint32_t, 9> words) {
    // Clause 8: column by column, MSB column first.
    std::array<std::uint8_t, 270> out{};
    std::size_t n = 0;
    for (int bit = 29; bit >= 0; --bit) {
        for (std::size_t row = 0; row < 9; ++row) {
            out[n++] = static_cast<std::uint8_t>((words[row] >> static_cast<unsigned>(bit)) & 1U);
        }
    }
    return out;
}

std::array<std::uint32_t, 9> ermes_deinterleave(std::span<const std::uint8_t, 270> bits) {
    std::array<std::uint32_t, 9> out{};
    std::size_t n = 0;
    for (int bit = 29; bit >= 0; --bit) {
        for (std::size_t row = 0; row < 9; ++row) {
            out[row] |= static_cast<std::uint32_t>(bits[n++] & 1U) << static_cast<unsigned>(bit);
        }
    }
    return out;
}

std::size_t ermes_message_partition_start(std::size_t initial_addresses) {
    // 5.4: J = 9 - MOD(4 + I, 9) terminators follow I addresses.
    const std::size_t j = 9 - ((4 + initial_addresses) % 9);
    return kErmesHeaderCodewords + initial_addresses + j;
}

char ermes_numeric_char(std::uint8_t nibble) {
    // 133-2 Table B.1.
    static constexpr std::array<char, 16> table = {'0', '1', '2', '3', '4', '5', '6', '7',
                                                   '8', '9', '/', ' ', 'U', '-', '.', '%'};
    return table[nibble & 0xFU];
}

char ermes_alpha_char(std::uint8_t code) {
    // 133-2 Table B.2; see the deviation in ermes.h.
    code &= 0x7FU;
    if (code == 0x0A) {
        return '\n';
    }
    if (code == 0x0D) {
        return '\r';
    }
    if (code < 0x20 || code == 0x7F) {
        return '?';
    }
    return static_cast<char>(code);
}

void ermes_fill_payload(ErmesMessage& message, std::span<const std::uint8_t> bits) {
    message.text.clear();
    message.data.clear();
    message.message_bits = 0;
    switch (message.category) {
        case ErmesCategory::Tone:
            break;
        case ErmesCategory::Numeric: {
            // 6.5.3: no terminator; spaces fill the rest.
            std::size_t count = bits.size() / 4;
            std::size_t last = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const auto nibble = static_cast<std::uint8_t>(bits_value(bits, i * 4, 4));
                message.text.push_back(ermes_numeric_char(nibble));
                if (nibble != kErmesNumericSpace) {
                    last = i + 1;
                }
            }
            message.text.resize(last);
            message.message_bits = last * 4;
            break;
        }
        case ErmesCategory::Alphanumeric: {
            const std::size_t count = bits.size() / 7;
            for (std::size_t i = 0; i < count; ++i) {
                const auto code = static_cast<std::uint8_t>(bits_value(bits, i * 7, 7));
                if (code == kErmesEom) {
                    break;
                }
                message.text.push_back(ermes_alpha_char(code));
            }
            message.message_bits = message.text.size() * 7;
            break;
        }
        case ErmesCategory::Transparent: {
            // 6.5.3: a single one follows the data, then zeros.
            std::size_t end = bits.size();
            while (end > 0 && bits[end - 1] == 0) {
                --end;
            }
            if (end > 0) {
                --end;
            }
            message.data.assign(bits.begin(), bits.begin() + static_cast<std::ptrdiff_t>(end));
            message.message_bits = end;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<ErmesDecoder> ErmesDecoder::create(const ErmesConfig& config) {
    if (config.rate < 16000) {
        return fail(std::format("ERMES needs at least 16000 S/s, got {}", config.rate));
    }
    ErmesDecoder d;
    d.config_ = config;
    d.sps_ = static_cast<double>(config.rate) / kErmesSymbolRate;
    auto disc = FmDiscriminator::create(config.rate);
    if (!disc) {
        return std::unexpected(disc.error());
    }
    d.discriminator_ = *disc;

    // Receive filter: a Gaussian-shaped low pass, 3 dB at 2.6 kHz. Not from
    // the document, which specifies only the transmitter's premodulation
    // filter (9.3.3); chosen so the four levels separate at the symbol
    // centres after that filter, and measured in tests/decode/test_ermes.cpp.
    const std::size_t taps = static_cast<std::size_t>(std::lround(3.0 * d.sps_)) | 1U;
    auto response = [](double hz, const void*) -> double {
        const double x = hz / 2600.0;
        return std::exp(-0.5 * std::log(2.0) * x * x);
    };
    auto designed = design_from_response(config.rate, taps, response, nullptr);
    if (!designed) {
        return std::unexpected(designed.error());
    }
    double sum = 0.0;
    for (float t : *designed) {
        sum += t;
    }
    for (float& t : *designed) {
        t = static_cast<float>(t / sum);
    }
    auto fir = RealFir::create(*designed);
    if (!fir) {
        return std::unexpected(fir.error());
    }
    d.filter_ = std::move(*fir);
    d.filter_delay_ = (taps - 1) / 2;
    return d;
}

void ErmesDecoder::reset() {
    discriminator_.reset();
    filter_.reset();
    buffer_.clear();
    buffer_start_ = 0;
    search_from_ = 0;
    state_ = State::Search;
    have_previous_batch_ = false;
    parse_ = Parse::Idle;
    open_.reset();
    extra_bits_.clear();
    data_bits_.clear();
    stats_ = {};
}

void ErmesDecoder::process(ConstComplexSpan samples, ErmesOutput& out) {
    std::vector<float> audio(samples.size());
    if (!discriminator_.process(samples, audio)) {
        return;
    }
    process_audio(audio, out);
}

void ErmesDecoder::process_audio(ConstRealSpan audio, ErmesOutput& out) {
    std::vector<float> filtered(audio.size());
    if (!filter_.process(audio, filtered)) {
        return;
    }
    buffer_.insert(buffer_.end(), filtered.begin(), filtered.end());
    run(out);
    trim();
}

float ErmesDecoder::sample_at(double position) const {
    const double local = position - static_cast<double>(buffer_start_);
    const auto i = static_cast<std::size_t>(std::floor(local));
    const double frac = local - std::floor(local);
    if (i + 1 >= buffer_.size()) {
        return buffer_.empty() ? 0.0F : buffer_.back();
    }
    return static_cast<float>(buffer_[i] * (1.0 - frac) + buffer_[i + 1] * frac);
}

double ErmesDecoder::symbol_position(std::size_t symbol) const {
    return batch_origin_ + static_cast<double>(symbol) * sps_;
}

bool ErmesDecoder::have_symbols(std::size_t count) const {
    const double last = symbol_position(count) + 2.0;
    return last < static_cast<double>(buffer_start_ + buffer_.size());
}

float ErmesDecoder::read_level(std::size_t symbol) const {
    return static_cast<float>((sample_at(symbol_position(symbol)) - offset_) / gain_);
}

std::uint32_t ErmesDecoder::read_word(std::size_t symbol) const {
    std::uint32_t w = 0;
    for (std::size_t s = 0; s < kErmesCodewordSymbols; ++s) {
        w = (w << 2U) | level_to_dibit(read_level(symbol + s));
    }
    return w;
}

bool ErmesDecoder::try_sync(double& best_score, bool& inverted, double& best_position) {
    static const std::array<float, 30> pattern = sync_pattern();
    static const double pattern_mean = [] {
        double m = 0.0;
        for (float p : pattern) {
            m += p;
        }
        return m / 30.0;
    }();
    const double span = 30.0 * sps_ + 2.0;
    const double end = static_cast<double>(buffer_start_ + buffer_.size());

    auto score_at = [&](double t) {
        double xs[30];
        double mean = 0.0;
        for (std::size_t k = 0; k < 30; ++k) {
            xs[k] = sample_at(t + static_cast<double>(k) * sps_);
            mean += xs[k];
        }
        mean /= 30.0;
        double xy = 0.0;
        double xx = 0.0;
        double yy = 0.0;
        for (std::size_t k = 0; k < 30; ++k) {
            const double x = xs[k] - mean;
            const double y = pattern[k] - pattern_mean;
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        return xx > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
    };

    double t = std::max(static_cast<double>(search_from_), static_cast<double>(buffer_start_));
    while (t + span + sps_ < end) {
        const double s = score_at(t);
        if (std::abs(s) >= config_.sync_threshold) {
            // Peak over the next symbol period.
            double best = s;
            double best_t = t;
            for (double u = t + 1.0; u <= t + sps_; u += 1.0) {
                const double su = score_at(u);
                if (std::abs(su) > std::abs(best)) {
                    best = su;
                    best_t = u;
                }
            }
            // Parabolic refinement on the magnitude.
            const double a = std::abs(score_at(best_t - 1.0));
            const double b = std::abs(best);
            const double c = std::abs(score_at(best_t + 1.0));
            const double denom = a - 2.0 * b + c;
            double delta = denom < 0.0 ? 0.5 * (a - c) / denom : 0.0;
            delta = std::clamp(delta, -0.5, 0.5);
            best_score = std::abs(best);
            inverted = best < 0.0;
            best_position = best_t + delta;
            search_from_ = static_cast<SampleIndex>(best_t) + 1;
            return true;
        }
        t += 1.0;
    }
    search_from_ = static_cast<SampleIndex>(std::max(0.0, t));
    return false;
}

void ErmesDecoder::begin_batch(ErmesOutput&) {
    // Least-squares gain and offset over the 30 known symbols.
    static const std::array<float, 30> pattern = sync_pattern();
    double sp = 0.0;
    double sx = 0.0;
    double spp = 0.0;
    double spx = 0.0;
    for (std::size_t k = 0; k < 30; ++k) {
        const double p = pattern[k];
        const double x = sample_at(symbol_position(k));
        sp += p;
        sx += x;
        spp += p * p;
        spx += p * x;
    }
    const double n = 30.0;
    const double det = n * spp - sp * sp;
    gain_ = (n * spx - sp * sx) / det;
    offset_ = (sx - gain_ * sp) / n;
    if (std::abs(gain_) < 1e-12) {
        gain_ = 1e-12;
    }
}

void ErmesDecoder::finish_message(ErmesOutput& out, bool truncated) {
    if (!open_) {
        return;
    }
    open_->truncated = truncated;
    ermes_fill_payload(*open_, data_bits_);
    out.messages.push_back(std::move(*open_));
    open_.reset();
    data_bits_.clear();
    extra_bits_.clear();
}

void ErmesDecoder::feed_message_word(const ErmesCodeword& cw, bool delimiter, ErmesOutput& out) {
    auto append = [](std::vector<std::uint8_t>& bits, std::uint32_t info) {
        for (int b = 17; b >= 0; --b) {
            bits.push_back(static_cast<std::uint8_t>((info >> static_cast<unsigned>(b)) & 1U));
        }
    };
    auto note = [&](ErmesMessage& m) {
        m.corrected_bits += cw.corrected;
        m.uncorrectable += cw.valid ? 0 : 1;
    };

    switch (parse_) {
        case Parse::Idle:
            if (delimiter) {
                parse_ = Parse::AfterDelimiter;
            }
            return;
        case Parse::AfterDelimiter:
            if (delimiter) {
                return;
            }
            header1_ = cw.info;
            open_.emplace();
            open_->system = batch_.system;
            open_->first_sample = batch_.first_sample;
            note(*open_);
            parse_ = Parse::Header2;
            return;
        case Parse::Header2: {
            ErmesMessage& m = *open_;
            note(m);
            // 6.5.1: 22 + 5 + 1 + 1 + 7 bits.
            const std::uint64_t h = (static_cast<std::uint64_t>(header1_) << 18U) | cw.info;
            m.local_address = static_cast<std::uint32_t>(h >> 14U) & 0x3FFFFFU;
            m.initial_address = m.local_address >> 4U;
            m.batch_bits = static_cast<std::uint8_t>(m.local_address & 0xFU);
            m.message_number = static_cast<std::uint8_t>((h >> 9U) & 0x1FU);
            m.external = ((h >> 8U) & 1U) != 0;
            m.aii = ((h >> 7U) & 1U) != 0;
            m.vif = static_cast<std::uint8_t>(h & 0x7FU);
            if (!m.aii) {
                // 6.5.1.1: RSVD 1, category 2, UMI 1, alert 3.
                m.category = static_cast<ErmesCategory>((m.vif >> 4U) & 3U);
                m.urgent = ((m.vif >> 3U) & 1U) != 0;
                m.alert = static_cast<std::uint8_t>(m.vif & 7U);
            } else {
                // 6.5.1.2: AIT 3, AIN 4.
                m.ait = static_cast<std::uint8_t>((m.vif >> 4U) & 7U);
                m.ain = static_cast<std::uint8_t>(m.vif & 0xFU);
                if (m.ait == 0b001) {
                    m.urgent = (m.ain & 8U) != 0;
                    m.alert = static_cast<std::uint8_t>(m.ain & 7U);
                }
            }
            const std::size_t bits = (m.external ? 13U : 0U) + aif_bits(m.aii, m.ait, m.ain);
            extra_words_ = (bits + kErmesInfoBits - 1) / kErmesInfoBits;
            extra_bits_.clear();
            data_bits_.clear();
            parse_ = extra_words_ > 0 ? Parse::Extra : Parse::Data;
            return;
        }
        case Parse::Extra: {
            if (delimiter) {
                // A message with no data, ended early.
                finish_message(out, false);
                parse_ = Parse::AfterDelimiter;
                return;
            }
            ErmesMessage& m = *open_;
            note(m);
            append(extra_bits_, cw.info);
            if (extra_bits_.size() / kErmesInfoBits < extra_words_) {
                return;
            }
            std::size_t at = 0;
            if (m.external) {
                m.operator_identity = static_cast<std::uint16_t>(bits_value(extra_bits_, 0, 13));
                at = 13;
            }
            const std::size_t n = aif_bits(m.aii, m.ait, m.ain);
            m.aif.assign(extra_bits_.begin() + static_cast<std::ptrdiff_t>(at),
                         extra_bits_.begin() + static_cast<std::ptrdiff_t>(at + n));
            if (m.aii && m.ait == 0b001 && n == 18) {
                // 6.5.1.2.1: category, then the 16-bit link counter of 11.4.
                m.category = static_cast<ErmesCategory>(bits_value(m.aif, 0, 2));
                m.link_counter = static_cast<std::uint16_t>(bits_value(m.aif, 2, 16));
            }
            parse_ = Parse::Data;
            return;
        }
        case Parse::Data:
            if (delimiter) {
                finish_message(out, false);
                parse_ = Parse::AfterDelimiter;
                return;
            }
            note(*open_);
            append(data_bits_, cw.info);
            return;
    }
}

void ErmesDecoder::run(ErmesOutput& out) {
    for (;;) {
        switch (state_) {
            case State::Search: {
                double score = 0.0;
                bool inverted = false;
                double position = 0.0;
                if (!try_sync(score, inverted, position)) {
                    return;
                }
                batch_origin_ = position;
                inverted_ = inverted;
                batch_ = {};
                batch_.sync_score = score;
                batch_.inverted = inverted;
                const double origin = position - static_cast<double>(filter_delay_);
                batch_.first_sample = static_cast<SampleIndex>(std::max(0.0, std::round(origin)));
                state_ = State::Header;
                break;
            }
            case State::Header: {
                if (!have_symbols(kErmesHeaderCodewords * kErmesCodewordSymbols)) {
                    return;
                }
                begin_batch(out);
                const ErmesCodeword si1 = ermes_decode(read_word(30));
                const ErmesCodeword si2 = ermes_decode(read_word(45));
                const ErmesCodeword ssi = ermes_decode(read_word(60));
                if (!si1.valid || !si2.valid) {
                    ++stats_.sync_rejected;
                    state_ = State::Search;
                    break;
                }
                // 6.3.1: 7 + 3 + 6 + 1 + 1 + 5 + 6 + 3 + 4 bits.
                const std::uint64_t si = (static_cast<std::uint64_t>(si1.info) << 18U) | si2.info;
                ErmesSystemInfo& s = batch_.system;
                s.country = static_cast<std::uint8_t>((si >> 29U) & 0x7FU);
                s.op = static_cast<std::uint8_t>((si >> 26U) & 7U);
                s.paging_area = static_cast<std::uint8_t>((si >> 20U) & 0x3FU);
                s.eti = ((si >> 19U) & 1U) != 0;
                s.bai = ((si >> 18U) & 1U) != 0;
                s.fsi = static_cast<std::uint8_t>((si >> 13U) & 0x1FU);
                s.cycle = static_cast<std::uint8_t>((si >> 7U) & 0x3FU);
                s.ssn = static_cast<std::uint8_t>((si >> 4U) & 7U);
                s.batch = static_cast<std::uint8_t>(si & 0xFU);
                if (s.cycle > 59 || s.ssn > 4) {
                    ++stats_.sync_rejected;
                    state_ = State::Search;
                    break;
                }
                if (ssi.valid) {
                    ErmesSupplementary sup;
                    sup.ssit = static_cast<std::uint8_t>(ssi.info >> 14U);
                    sup.ssif = static_cast<std::uint16_t>(ssi.info & 0x3FFFU);
                    if (sup.ssit == 0) {
                        // 6.3.2.1: zone 3, hour 5, date 5, RSVD 1.
                        sup.zone = static_cast<std::uint8_t>(sup.ssif >> 11U);
                        sup.hour = static_cast<std::uint8_t>((sup.ssif >> 6U) & 0x1FU);
                        sup.date = static_cast<std::uint8_t>((sup.ssif >> 1U) & 0x1FU);
                    } else if (sup.ssit == 1) {
                        // 6.3.2.2: day 3, month 4, year 7.
                        sup.day_of_week = static_cast<std::uint8_t>(sup.ssif >> 11U);
                        sup.month = static_cast<std::uint8_t>((sup.ssif >> 7U) & 0xFU);
                        sup.year = static_cast<std::uint16_t>(1990U + (sup.ssif & 0x7FU));
                    }
                    batch_.supplementary = sup;
                }
                batch_.corrected_bits = si1.corrected + si2.corrected + ssi.corrected;
                batch_.uncorrectable = ssi.valid ? 0 : 1;
                batch_length_ =
                    s.batch == 15 ? kErmesLastBatchCodewords : kErmesBatchCodewords;
                word_index_ = kErmesHeaderCodewords;
                ++stats_.batches;
                stats_.codewords += 3;
                stats_.corrected_bits += static_cast<std::uint64_t>(batch_.corrected_bits);
                stats_.uncorrectable += static_cast<std::uint64_t>(batch_.uncorrectable);
                if (parse_ == Parse::Data && open_) {
                    open_->continued = true;
                }
                state_ = State::Address;
                break;
            }
            case State::Address: {
                if (!have_symbols((word_index_ + 1) * kErmesCodewordSymbols)) {
                    return;
                }
                const std::uint32_t raw = read_word(word_index_ * kErmesCodewordSymbols);
                ++stats_.codewords;
                ++word_index_;
                if (ermes_is_structure_word(raw, kErmesApt)) {
                    message_start_ = ermes_message_partition_start(batch_.initial_addresses.size());
                    word_index_ = message_start_;
                    state_ = State::Codeblocks;
                    break;
                }
                const ErmesCodeword cw = ermes_decode(raw);
                batch_.corrected_bits += cw.corrected;
                stats_.corrected_bits += static_cast<std::uint64_t>(cw.corrected);
                if (!cw.valid) {
                    ++batch_.uncorrectable;
                    ++stats_.uncorrectable;
                }
                batch_.initial_addresses.push_back(cw.info);
                // 5.4: at most 139 addresses.
                if (batch_.initial_addresses.size() > 139) {
                    batch_.codewords = word_index_;
                    out.batches.push_back(batch_);
                    state_ = State::Search;
                }
                break;
            }
            case State::Codeblocks: {
                if (word_index_ + kErmesCodeblockCodewords > batch_length_) {
                    batch_.codewords = word_index_;
                    out.batches.push_back(batch_);
                    // The next batch's preamble is due straight after this one.
                    const double next = symbol_position(batch_length_ * kErmesCodewordSymbols);
                    search_from_ = static_cast<SampleIndex>(std::max(0.0, next - sps_));
                    state_ = State::Search;
                    break;
                }
                const std::size_t first = word_index_ * kErmesCodewordSymbols;
                if (!have_symbols(first + kErmesCodeblockCodewords * kErmesCodewordSymbols)) {
                    return;
                }
                std::array<std::uint8_t, 270> bits{};
                for (std::size_t s = 0; s < 135; ++s) {
                    const std::uint32_t dibit = level_to_dibit(read_level(first + s));
                    bits[2 * s] = static_cast<std::uint8_t>(dibit >> 1U);
                    bits[2 * s + 1] = static_cast<std::uint8_t>(dibit & 1U);
                }
                const auto words = ermes_deinterleave(bits);
                std::array<ErmesCodeword, 9> decoded{};
                std::array<bool, 9> delimiters{};
                int failures = 0;
                for (std::size_t r = 0; r < 9; ++r) {
                    delimiters[r] = ermes_is_structure_word(words[r], kErmesDelimiter);
                    decoded[r] = delimiters[r] ? ErmesCodeword{0, 0, true} : ermes_decode(words[r]);
                    failures += decoded[r].valid ? 0 : 1;
                }
                if (failures >= 5) {
                    // Transmitter stopped after the last codeblock, 5.5.
                    batch_.codewords = word_index_;
                    out.batches.push_back(batch_);
                    search_from_ = static_cast<SampleIndex>(symbol_position(first));
                    state_ = State::Search;
                    break;
                }
                for (std::size_t r = 0; r < 9; ++r) {
                    const ErmesCodeword& cw = decoded[r];
                    ++stats_.codewords;
                    batch_.corrected_bits += cw.corrected;
                    stats_.corrected_bits += static_cast<std::uint64_t>(cw.corrected);
                    if (!cw.valid) {
                        ++batch_.uncorrectable;
                        ++stats_.uncorrectable;
                    }
                    feed_message_word(cw, delimiters[r], out);
                }
                word_index_ += kErmesCodeblockCodewords;
                break;
            }
        }
    }
}

void ErmesDecoder::flush(ErmesOutput& out) {
    if (state_ == State::Address || state_ == State::Codeblocks) {
        batch_.codewords = word_index_;
        out.batches.push_back(batch_);
    }
    if (open_ && parse_ == Parse::Data) {
        finish_message(out, true);
    }
    open_.reset();
    parse_ = Parse::Idle;
    state_ = State::Search;
}

void ErmesDecoder::trim() {
    // Keep what the current state may still read: from the batch origin when
    // inside a batch, from the search point otherwise.
    double keep_from = static_cast<double>(search_from_);
    if (state_ != State::Search) {
        keep_from = batch_origin_;
    }
    keep_from -= 4.0;
    if (keep_from <= static_cast<double>(buffer_start_)) {
        return;
    }
    const auto drop = std::min(static_cast<std::size_t>(keep_from - static_cast<double>(buffer_start_)),
                               buffer_.size());
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
    buffer_start_ += drop;
}

}  // namespace revenant::decode
