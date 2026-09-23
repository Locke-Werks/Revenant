#include "core/decode/sitor_b.h"

#include <bit>

namespace revenant::decode {

namespace {

// M.625-4 Table 1, one row per combination number, 1 to 32: the ITA2 column
// and the transmitted 7-unit column, as the table prints them. The ITA2
// column is carried as well as the 7-unit one so the test can hold it
// against ITU-T S.1 Table 1, which is the same alphabet from another
// document.
struct Row {
    const char* ita2;
    const char* signal;
};

constexpr Row kTable1[32] = {
    {"ZZAAA", "BBBYYYB"}, {"ZAAZZ", "YBYYBBB"}, {"AZZZA", "BYBBBYY"}, {"ZAAZA", "BBYYBYB"},
    {"ZAAAA", "YBBYBYB"}, {"ZAZZA", "BBYBBYY"}, {"AZAZZ", "BYBYBBY"}, {"AAZAZ", "BYYBYBB"},
    {"AZZAA", "BYBBYYB"}, {"ZZAZA", "BBBYBYY"}, {"ZZZZA", "YBBBBYY"}, {"AZAAZ", "BYBYYBB"},
    {"AAZZZ", "BYYBBBY"}, {"AAZZA", "BYYBBYB"}, {"AAAZZ", "BYYYBBB"}, {"AZZAZ", "BYBBYBY"},
    {"ZZZAZ", "YBBBYBY"}, {"AZAZA", "BYBYBYB"}, {"ZAZAA", "BBYBYYB"}, {"AAAAZ", "YYBYBBB"},
    {"ZZZAA", "YBBBYYB"}, {"AZZZZ", "YYBBBBY"}, {"ZZAAZ", "BBBYYBY"}, {"ZAZZZ", "YBYBBBY"},
    {"ZAZAZ", "BBYBYBY"}, {"ZAAAZ", "BBYYYBB"}, {"AAAZA", "YYYBBBB"}, {"AZAAA", "YYBBYBB"},
    {"ZZZZZ", "YBYBBYB"}, {"ZZAZZ", "YBBYBBY"}, {"AAZAA", "YYBBBYB"}, {"AAAAA", "YBYBYBB"},
};

// Table 1 note 3: bit position 1 first, B = 0, Y = 1.
constexpr std::uint8_t by_to_signal(const char* units) {
    std::uint8_t v = 0;
    for (std::size_t i = 0; i < kSitorSignalUnits; ++i) {
        if (units[i] == 'Y') {
            v = static_cast<std::uint8_t>(v | (1U << i));
        }
    }
    return v;
}

// Table 1 note 1 and ITU-T S.1 clause 3.2: A is start polarity and 0, Z is
// stop polarity and 1, code element 1 first.
constexpr std::uint8_t az_to_combination(const char* units) {
    std::uint8_t v = 0;
    for (std::size_t i = 0; i < 5; ++i) {
        if (units[i] == 'Z') {
            v = static_cast<std::uint8_t>(v | (1U << i));
        }
    }
    return v;
}

struct Lookup {
    // Indexed by 7-unit signal: combination + 1, or 0 for none.
    std::array<std::uint8_t, 128> combination_of{};
    // Indexed by ITA2 combination.
    std::array<std::uint8_t, 32> signal_of{};
};

constexpr Lookup make_lookup() {
    Lookup l{};
    for (const Row& r : kTable1) {
        const std::uint8_t s = by_to_signal(r.signal);
        const std::uint8_t c = az_to_combination(r.ita2);
        l.combination_of[s] = static_cast<std::uint8_t>(c + 1);
        l.signal_of[c] = s;
    }
    return l;
}

constexpr Lookup kLookup = make_lookup();

static_assert(by_to_signal("BBBBYYY") == kSitorPhasing1);
static_assert(by_to_signal("YBBYYBB") == kSitorPhasing2);
static_assert(by_to_signal("BBYYBBY") == kSitorIdleBeta);

constexpr std::uint8_t kCarriageReturn = az_to_combination("AAAZA");  // No. 27
constexpr std::uint8_t kLineFeed = az_to_combination("AZAAA");        // No. 28

bool is_phasing(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return a == kSitorPhasing2 && b == kSitorPhasing1 && c == kSitorPhasing2 && d == kSitorPhasing1;
}

}  // namespace

std::uint8_t sitor_signal(const char* by_units) {
    return by_to_signal(by_units);
}

SitorSignal sitor_classify(std::uint8_t signal) {
    SitorSignal s;
    signal &= 0x7FU;
    // Clause 1.1: the constant ratio, three Y in seven.
    if (std::popcount(signal) != kSitorOnesPerSignal) {
        return s;
    }
    if (signal == kSitorPhasing1) {
        s.kind = SitorSignal::Kind::Phasing1OrAlpha;
    } else if (signal == kSitorPhasing2) {
        s.kind = SitorSignal::Kind::Phasing2;
    } else if (signal == kSitorIdleBeta) {
        s.kind = SitorSignal::Kind::IdleBeta;
    } else if (kLookup.combination_of[signal] != 0) {
        s.kind = SitorSignal::Kind::Traffic;
        s.combination = static_cast<std::uint8_t>(kLookup.combination_of[signal] - 1);
    }
    return s;
}

std::uint8_t sitor_encode(std::uint8_t combination) {
    return kLookup.signal_of[combination & 0x1FU];
}

// ---------------------------------------------------------------------------
// SitorBDecoder
// ---------------------------------------------------------------------------

Expected<SitorBDecoder> SitorBDecoder::create(const SitorConfig& config) {
    if (config.shift_hz <= 0 || config.shift_hz % 2 != 0 ||
        config.centre_hz <= config.shift_hz / 2) {
        return fail("a SITOR shift must be positive, even, and leave both tones above zero");
    }
    if (config.mutilation_window == 0 ||
        !(config.mutilation_limit > 0.0 && config.mutilation_limit <= 1.0)) {
        return fail("the SITOR mutilation window must hold something and the limit lie in (0, 1]");
    }
    // Table 1 note 2: B is the higher emitted frequency and Y the lower. The
    // discriminator's positive side is Y, which is binary 1.
    const Hertz lower = config.centre_hz - config.shift_hz / 2;
    const Hertz higher = config.centre_hz + config.shift_hz / 2;
    ToneDiscriminatorConfig tones;
    tones.rate = config.rate;
    tones.mark_hz = config.upper_sideband ? lower : higher;
    tones.space_hz = config.upper_sideband ? higher : lower;
    tones.symbol_rate = kSitorBaud;
    auto discriminator = ToneDiscriminator::create(tones);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "SITOR-B"));
    }
    BitClockConfig clock;
    clock.rate = config.rate;
    clock.symbol_rate = kSitorBaud;
    auto bit_clock = BitClock::create(clock);
    if (!bit_clock) {
        return std::unexpected(with_context(bit_clock.error(), "SITOR-B"));
    }

    SitorBDecoder d;
    d.config_ = config;
    d.discriminator_ = std::move(*discriminator);
    d.clock_ = std::move(*bit_clock);
    d.reset();
    return d;
}

void SitorBDecoder::reset() {
    discriminator_.reset();
    clock_.reset();
    stand_by();
    stats_ = {};
}

void SitorBDecoder::stand_by() {
    shift_ = 0;
    bit_count_ = 0;
    phased_ = false;
    inverted_ = false;
    printing_ = false;
    figures_ = false;
    signal_ = 0;
    unit_ = 0;
    slot_ = 0;
    dx_.clear();
    alpha_run_ = 0;
    closing_ = 0;
    recent_.clear();
    recent_lost_ = 0;
}

void SitorBDecoder::process(ConstRealSpan audio, std::vector<SitorCharacter>& out) {
    soft_.clear();
    bits_.clear();
    discriminator_.process(audio, soft_);
    clock_.process(soft_, bits_);
    const auto delay = static_cast<SampleIndex>(discriminator_.group_delay());
    for (const SoftBit& b : bits_) {
        on_bit(b.value >= 0.0F ? 1U : 0U, b.position > delay ? b.position - delay : 0, out);
    }
}

void SitorBDecoder::on_bit(std::uint8_t bit, SampleIndex sample, std::vector<SitorCharacter>& out) {
    constexpr std::size_t kSearchBits = 4 * kSitorSignalUnits;
    if (!phased_) {
        shift_ = ((shift_ << 1U) | bit) & ((1U << kSearchBits) - 1U);
        ++bit_count_;
        if (bit_count_ < kSearchBits) {
            return;
        }
        // The last four signals, oldest first, each with bit position 1 in
        // bit 0.
        std::uint8_t s[4] = {};
        for (std::size_t j = 0; j < 4; ++j) {
            for (std::size_t u = 0; u < kSitorSignalUnits; ++u) {
                const auto at = static_cast<unsigned>(kSearchBits - 1 - kSitorSignalUnits * j - u);
                s[j] = static_cast<std::uint8_t>(s[j] | (((shift_ >> at) & 1U) << u));
            }
        }
        // Clause 4.4.3: phasing signal 2 then 1, or 1 then 2, with two more
        // in step; phasing signal 2 marks the DX position. Complemented, it
        // is the same signal from the other sideband.
        for (const bool invert : {false, true}) {
            std::uint8_t t[4];
            for (std::size_t j = 0; j < 4; ++j) {
                t[j] = invert ? static_cast<std::uint8_t>(s[j] ^ 0x7FU) : s[j];
            }
            const bool dx_first = is_phasing(t[0], t[1], t[2], t[3]);
            const bool rx_first = is_phasing(t[1], t[0], t[3], t[2]);
            if (!dx_first && !rx_first) {
                continue;
            }
            stand_by();
            phased_ = true;
            inverted_ = invert;
            ++stats_.phasings;
            // The four signals just read end on an RX slot when phasing
            // signal 2 came first, so the next is DX, and on a DX slot
            // otherwise.
            slot_ = dx_first ? 0 : 1;
            return;
        }
        return;
    }

    if (inverted_) {
        bit ^= 1U;
    }
    if (unit_ == 0) {
        signal_sample_ = sample;
        signal_ = 0;
    }
    signal_ = static_cast<std::uint8_t>(signal_ | (bit << unit_));
    if (++unit_ == kSitorSignalUnits) {
        unit_ = 0;
        on_signal(signal_, signal_sample_, out);
    }
}

void SitorBDecoder::on_signal(std::uint8_t signal, SampleIndex sample,
                              std::vector<SitorCharacter>& out) {
    const bool dx = (slot_ % 2) == 0;
    ++slot_;

    // Clause 4.6.7.2: stand-by "not less than 210 ms after" the end of
    // transmission, which is three signals. Waiting them out is what lets
    // the RX copy of the last traffic signal arrive.
    if (closing_ > 0 && --closing_ == 0) {
        ++stats_.ends_of_transmission;
        stand_by();
        return;
    }

    if (dx) {
        // Clause 4.6.7.2: two consecutive idle signals alpha in the DX
        // position end the transmission.
        if (sitor_classify(signal).kind == SitorSignal::Kind::Phasing1OrAlpha) {
            if (++alpha_run_ == 2 && closing_ == 0) {
                closing_ = 3;
            }
        } else {
            alpha_run_ = 0;
        }
        dx_.push_back({signal, sample});
        return;
    }

    // Clause 4.2: this RX signal repeats the DX signal five slots back, and
    // the DX signals of the two slots between are still waiting. Before
    // three are waiting, this RX slot repeats a signal sent before phasing.
    if (dx_.size() < 3) {
        return;
    }
    const Pending first = dx_.front();
    dx_.pop_front();

    const SitorSignal d = sitor_classify(first.signal);
    const SitorSignal r = sitor_classify(signal);
    // Clause 4.4.2: while phasing, the DX position carries phasing signal 2
    // and the RX position phasing signal 1. They are not copies of each
    // other, so a DX phasing signal paired with whatever arrives in its RX
    // slot is phasing, not a character whose copies disagree.
    if (d.kind == SitorSignal::Kind::Phasing2) {
        return;
    }
    const bool d_ok = d.kind != SitorSignal::Kind::Mutilated;
    const bool r_ok = r.kind != SitorSignal::Kind::Mutilated;

    // Clause 4.3: use the unmutilated copy; two unmutilated copies that
    // differ are both mutilated.
    SitorSignal chosen;
    bool from_rx = false;
    if (d_ok && r_ok) {
        if (first.signal == signal) {
            chosen = d;
        }
    } else if (d_ok) {
        chosen = d;
    } else if (r_ok) {
        chosen = r;
        from_rx = true;
    }
    const bool lost = chosen.kind == SitorSignal::Kind::Mutilated;
    stats_.dx_mutilated += d_ok ? 0U : 1U;
    stats_.rx_mutilated += r_ok ? 0U : 1U;
    stats_.both_mutilated += lost ? 1U : 0U;

    // Clause 4.6.6.
    recent_.push_back(lost);
    recent_lost_ += lost ? 1U : 0U;
    if (recent_.size() > config_.mutilation_window) {
        recent_lost_ -= recent_.front() ? 1U : 0U;
        recent_.pop_front();
    }
    if (recent_.size() == config_.mutilation_window &&
        static_cast<double>(recent_lost_) >=
            config_.mutilation_limit * static_cast<double>(config_.mutilation_window)) {
        ++stats_.losses_of_phase;
        stand_by();
        return;
    }

    print(chosen, lost, first.sample, from_rx, false, out);
}

void SitorBDecoder::flush(std::vector<SitorCharacter>& out) {
    if (phased_) {
        for (const Pending& waiting : dx_) {
            const SitorSignal d = sitor_classify(waiting.signal);
            // Clause 4.4.2, as in on_signal: phasing signal 2 in a DX slot is
            // phasing and not a character.
            if (d.kind == SitorSignal::Kind::Phasing2) {
                continue;
            }
            // Clause 4.3 with one copy: it is the character if it checks.
            const bool lost = d.kind == SitorSignal::Kind::Mutilated;
            stats_.dx_mutilated += lost ? 1U : 0U;
            print(d, lost, waiting.sample, false, true, out);
        }
    }
    stand_by();
}

void SitorBDecoder::print(const SitorSignal& chosen, bool lost, SampleIndex sample, bool from_rx,
                          bool single_copy, std::vector<SitorCharacter>& out) {
    if (!lost && chosen.kind != SitorSignal::Kind::Traffic) {
        // Service signals are not printed.
        return;
    }

    if (!printing_) {
        // Clause 4.6.4.
        const bool line_end =
            !lost && (chosen.combination == kCarriageReturn || chosen.combination == kLineFeed);
        if (config_.wait_for_line_end && !line_end) {
            return;
        }
        printing_ = true;
    }

    SitorCharacter c;
    c.position = sample;
    c.from_rx = from_rx;
    c.single_copy = single_copy;
    c.phasing = stats_.phasings;
    if (lost) {
        // Clause 4.6.5.
        c.mutilated = true;
        c.glyph = config_.error_glyph;
    } else {
        c.combination = chosen.combination;
        if (c.combination == kIta2LetterShift) {
            figures_ = false;
        } else if (c.combination == kIta2FigureShift) {
            figures_ = true;
        } else {
            c.glyph = figures_ ? ita2_figure(c.combination) : ita2_letter(c.combination);
        }
    }
    c.figures = figures_;
    ++stats_.characters;
    out.push_back(c);
}

}  // namespace revenant::decode
