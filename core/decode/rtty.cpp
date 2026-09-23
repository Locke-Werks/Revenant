#include "core/decode/rtty.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace revenant::decode {

namespace {

// ITU-T S.1 Table 1, one row per combination number, 1 to 32. The unit
// string is the table's own coding column, code element 1 first, A and Z as
// the table writes them. Carried as the document's strings rather than as
// integers so that tests/decode/test_rtty.cpp and a reader can both check a
// row against the table without converting anything.
//
// Rows 6, 7, 8, 29 and 30 are printed in the table as stacked columns rather
// than as strings; they are read here column by column, one unit per column.
struct Table1Row {
    const char* units;
    char32_t letter;
    char32_t figure;
};

// Clause 4.2's "arbitrary sign ... such as, for instance, a square".
constexpr char32_t kUndefinedFigure = char32_t{0x25A1};
// Clause 4.1 and 4.3. Project mapping, see rtty.h.
constexpr char32_t kWhoAreYou = U'\x05';
constexpr char32_t kAudibleSignal = U'\x07';

constexpr Table1Row kTable1[kIta2Combinations] = {
    {"ZZAAA", U'A', U'-'},  {"ZAAZZ", U'B', U'?'},  {"AZZZA", U'C', U':'},
    {"ZAAZA", U'D', kWhoAreYou},
    {"ZAAAA", U'E', U'3'},  {"ZAZZA", U'F', kUndefinedFigure},
    {"AZAZZ", U'G', kUndefinedFigure},
    {"AAZAZ", U'H', kUndefinedFigure},
    {"AZZAA", U'I', U'8'},  {"ZZAZA", U'J', kAudibleSignal},
    {"ZZZZA", U'K', U'('},  {"AZAAZ", U'L', U')'},  {"AAZZZ", U'M', U'.'},
    {"AAZZA", U'N', U','},  {"AAAZZ", U'O', U'9'},  {"AZZAZ", U'P', U'0'},
    {"ZZZAZ", U'Q', U'1'},  {"AZAZA", U'R', U'4'},  {"ZAZAA", U'S', U'\''},
    {"AAAAZ", U'T', U'5'},  {"ZZZAA", U'U', U'7'},  {"AZZZZ", U'V', U'='},
    {"ZZAAZ", U'W', U'2'},  {"ZAZZZ", U'X', U'/'},  {"ZAZAZ", U'Y', U'6'},
    {"ZAAAZ", U'Z', U'+'},
    {"AAAZA", U'\r', U'\r'},  // No. 27, carriage return
    {"AZAAA", U'\n', U'\n'},  // No. 28, line feed
    {"ZZZZZ", 0, 0},          // No. 29, letter shift
    {"ZZAZZ", 0, 0},          // No. 30, figure shift
    {"AAZAA", U' ', U' '},    // No. 31, space
    {"AAAAA", 0, 0},          // No. 32, all-space or null
};

constexpr std::uint8_t units_to_combination(const char* units) {
    std::uint8_t value = 0;
    for (std::size_t i = 0; i < kIta2Units; ++i) {
        if (units[i] == 'Z') {
            value = static_cast<std::uint8_t>(value | (1U << i));
        }
    }
    return value;
}

struct Inverse {
    std::array<std::uint8_t, kIta2Combinations> row_of{};
};

constexpr Inverse make_inverse() {
    Inverse inverse{};
    for (std::size_t row = 0; row < kIta2Combinations; ++row) {
        inverse.row_of[units_to_combination(kTable1[row].units)] =
            static_cast<std::uint8_t>(row);
    }
    return inverse;
}

constexpr Inverse kInverse = make_inverse();

static_assert(units_to_combination("ZZZZZ") == kIta2LetterShift);
static_assert(units_to_combination("ZZAZZ") == kIta2FigureShift);
static_assert(units_to_combination("AAZAA") == kIta2Space);
static_assert(units_to_combination("AAAAA") == kIta2Null);

const Table1Row& row_for(std::uint8_t combination) {
    return kTable1[kInverse.row_of[combination & 0x1FU]];
}

void append_utf8(std::string& out, char32_t c) {
    const auto v = static_cast<std::uint32_t>(c);
    if (v < 0x80U) {
        out.push_back(static_cast<char>(v));
    } else if (v < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (v >> 6U)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (v >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((v >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (v & 0x3FU)));
    }
}

}  // namespace

char32_t ita2_letter(std::uint8_t combination) { return row_for(combination).letter; }

char32_t ita2_figure(std::uint8_t combination) { return row_for(combination).figure; }

int ita2_combination_number(std::uint8_t combination) {
    return static_cast<int>(kInverse.row_of[combination & 0x1FU]) + 1;
}

std::optional<Ita2Code> ita2_encode(char32_t character) {
    if (character >= U'a' && character <= U'z') {
        character = character - U'a' + U'A';
    }
    // The dash and apostrophe S.1 prints, accepted as well as their ASCII
    // forms, since both mean the same combination.
    if (character == char32_t{0x2013} || character == char32_t{0x2212}) {
        character = U'-';
    }
    if (character == char32_t{0x2019}) {
        character = U'\'';
    }
    if (character == 0 || character == kUndefinedFigure) {
        return std::nullopt;
    }
    for (std::size_t row = 0; row < kIta2Combinations; ++row) {
        const Table1Row& r = kTable1[row];
        const std::uint8_t combination = units_to_combination(r.units);
        if (r.letter == character && r.figure == character) {
            return Ita2Code{combination, Ita2Code::Case::Either};
        }
        if (r.letter == character) {
            return Ita2Code{combination, Ita2Code::Case::Letters};
        }
        if (r.figure == character) {
            return Ita2Code{combination, Ita2Code::Case::Figures};
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// RttyDecoder
// ---------------------------------------------------------------------------

Expected<RttyDecoder> RttyDecoder::create(const RttyConfig& config) {
    if (config.shift_hz <= 0) {
        return fail("an RTTY shift must be positive");
    }
    if (!config.space_above_mark && config.shift_hz >= config.mark_hz) {
        return fail("a space tone below mark must stay above zero hertz");
    }

    ToneDiscriminatorConfig tones;
    tones.rate = config.rate;
    tones.mark_hz = config.mark_hz;
    tones.space_hz = config.space_above_mark ? config.mark_hz + config.shift_hz
                                             : config.mark_hz - config.shift_hz;
    tones.symbol_rate = config.baud;
    auto discriminator = ToneDiscriminator::create(tones);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "RTTY"));
    }

    RttyDecoder d;
    d.config_ = config;
    d.discriminator_ = std::move(*discriminator);
    d.samples_per_unit_ = static_cast<double>(config.rate) / config.baud;
    d.reset();
    return d;
}

void RttyDecoder::reset() {
    discriminator_.reset();
    history_.clear();
    history_start_ = 0;
    scan_ = 1;
    have_edge_ = false;
    edge_ = 0.0;
    figures_ = false;
    framing_errors_ = 0;
    false_starts_ = 0;
}

float RttyDecoder::soft_at(double index) const {
    const double local = index - static_cast<double>(history_start_);
    const auto below = static_cast<std::size_t>(std::floor(local));
    const double t = local - std::floor(local);
    const float a = history_[below];
    const float b = (below + 1 < history_.size()) ? history_[below + 1] : a;
    return static_cast<float>(static_cast<double>(a) + t * static_cast<double>(b - a));
}

void RttyDecoder::process(ConstRealSpan audio, std::vector<RttyCharacter>& out) {
    discriminator_.process(audio, history_);

    for (;;) {
        const std::uint64_t end = history_start_ + history_.size();
        if (!have_edge_) {
            // S.1 clause 3.2: the start element is condition A, which is
            // space, and the line rests at stop polarity, which is mark. A
            // character begins where mark gives way to space.
            while (scan_ < end) {
                const float before = history_[scan_ - 1 - history_start_];
                const float after = history_[scan_ - history_start_];
                ++scan_;
                if (before >= 0.0F && after < 0.0F) {
                    const double t = static_cast<double>(before) /
                                     (static_cast<double>(before) - static_cast<double>(after));
                    edge_ = static_cast<double>(scan_ - 2) + t;
                    have_edge_ = true;
                    break;
                }
            }
            if (!have_edge_) {
                break;
            }
        }
        if (!frame(out)) {
            break;
        }
    }

    // Keep what the next search and the next framing can reach back to, and
    // trim in large steps so the erase is not paid per sample.
    const double reach = (kRttyTimingSearchUnits + 1.0) * samples_per_unit_;
    std::uint64_t keep_from = scan_ - 1;
    if (have_edge_) {
        const double earliest = edge_ - reach;
        keep_from = earliest > 0.0 ? std::min<std::uint64_t>(keep_from, static_cast<std::uint64_t>(earliest))
                                   : 0;
    }
    if (keep_from > history_start_ &&
        keep_from - history_start_ > static_cast<std::uint64_t>(16.0 * samples_per_unit_)) {
        const auto drop = static_cast<std::ptrdiff_t>(keep_from - history_start_);
        history_.erase(history_.begin(), history_.begin() + drop);
        history_start_ = keep_from;
    }
}

bool RttyDecoder::frame(std::vector<RttyCharacter>& out) {
    // Readings sit at the centres of the start unit, the five code units and
    // the first unit of the stop element: 0.5, 1.5 ... 6.5 units after the
    // edge. The last one must be inside the history at the latest timing the
    // search can pick.
    constexpr int kReadings = 7;
    const double spu = samples_per_unit_;
    const double latest = edge_ + (6.5 + kRttyTimingSearchUnits) * spu;
    const std::uint64_t end = history_start_ + history_.size();
    if (latest + 1.0 >= static_cast<double>(end)) {
        return false;
    }

    // An edge so close to the start of the stream that the search would
    // read before the first sample is searched forward only.
    const double min_offset = std::max(-kRttyTimingSearchUnits * spu,
                                       static_cast<double>(history_start_) - (edge_ + 0.5 * spu));

    constexpr int kSteps = 32;
    double best_offset = 0.0;
    double best_metric = -std::numeric_limits<double>::infinity();
    for (int step = -kSteps; step <= kSteps; ++step) {
        const double offset = kRttyTimingSearchUnits * spu * static_cast<double>(step) / kSteps;
        if (offset < min_offset) {
            continue;
        }
        double metric = 0.0;
        for (int k = 0; k < kReadings; ++k) {
            const double v = static_cast<double>(
                soft_at(edge_ + offset + (static_cast<double>(k) + 0.5) * spu));
            if (k == 0) {
                metric -= v;
            } else if (k == kReadings - 1) {
                metric += v;
            } else {
                metric += std::abs(v);
            }
        }
        if (metric > best_metric) {
            best_metric = metric;
            best_offset = offset;
        }
    }

    const double start = edge_ + best_offset;
    have_edge_ = false;

    float margin = std::numeric_limits<float>::max();
    std::uint8_t combination = 0;
    float readings[kReadings] = {};
    for (int k = 0; k < kReadings; ++k) {
        readings[k] = soft_at(start + (static_cast<double>(k) + 0.5) * spu);
        margin = std::min(margin, std::abs(readings[k]));
    }

    if (readings[0] >= 0.0F) {
        // Not space at the start centre: a glitch, not a character. Look for
        // the next edge just past this one.
        ++false_starts_;
        scan_ = static_cast<std::uint64_t>(std::floor(edge_)) + 2;
        return true;
    }

    // From here on the next start element can only follow the stop reading.
    scan_ = static_cast<std::uint64_t>(std::ceil(start + 6.5 * spu));

    if (readings[kReadings - 1] < 0.0F) {
        ++framing_errors_;
        return true;
    }

    // Table 1 note: code element 1 first. S.1 clause 3.2: Z, which is mark,
    // is binary 1.
    for (std::size_t unit = 0; unit < kIta2Units; ++unit) {
        if (readings[unit + 1] >= 0.0F) {
            combination = static_cast<std::uint8_t>(combination | (1U << unit));
        }
    }

    RttyCharacter c;
    const double edge_in_audio = start - static_cast<double>(discriminator_.group_delay());
    c.position = edge_in_audio > 0.0 ? static_cast<SampleIndex>(std::llround(edge_in_audio)) : 0;
    c.combination = combination;
    c.margin = margin;
    if (combination == kIta2LetterShift) {
        figures_ = false;
    } else if (combination == kIta2FigureShift) {
        figures_ = true;
    } else {
        c.glyph = figures_ ? ita2_figure(combination) : ita2_letter(combination);
        if (config_.unshift_on_space && combination == kIta2Space) {
            figures_ = false;
        }
    }
    c.figures = figures_;
    out.push_back(c);
    return true;
}

std::string rtty_text(std::span<const RttyCharacter> characters) {
    std::string text;
    for (const RttyCharacter& c : characters) {
        if (c.glyph != 0) {
            append_utf8(text, c.glyph);
        }
    }
    return text;
}

}  // namespace revenant::decode
