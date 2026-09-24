#include "core/decode/dsc.h"

#include <algorithm>
#include <bit>
#include <format>

namespace revenant::decode {

namespace {

// Figure 1 b), with clause 1.2.1: after the dot pattern the characters go
// out DX, RX, DX, RX. Six DX slots of phasing, then the information
// characters in the DX slots from slot 12; each is repeated in the RX slot
// five slots later, "the transmission of four other characters" between the
// two copies, so the RX slots carry eight phasing characters and then the
// same information two pairs behind. The figure draws the two rows and does
// not say in words which of a pair goes first; DX first is the order that
// puts four characters between the copies, since RX first with the same two
// pairs of lag would put two.
constexpr std::size_t kFirstInformationDxSlot = 2 * kDscPhasingDxCount;
constexpr std::size_t kRxLagSlots = 5;
constexpr std::size_t kFirstInformationRxSlot = kFirstInformationDxSlot + kRxLagSlots;
constexpr std::size_t kPhasingSlots = 2 * kDscPhasingRx.size();

// Bits kept behind the newest while searching: every phasing slot, and a
// little more.
constexpr std::size_t kSearchHistoryBits = (kPhasingSlots + 4) * kDscCharacterBits;

// Characters lost in both copies in a row before a call is given up as a
// carrier gone rather than a burst of noise.
constexpr int kLostInARow = 3;

bool is_eos(std::uint8_t symbol) {
    return symbol == kDscEosAcknowledgeRq || symbol == kDscEosAcknowledgeBq || symbol == kDscEos;
}

// The expected symbol of a phasing slot, or nothing for a slot phasing does
// not fix: slots 12 and 14 are the two format specifiers.
std::optional<std::uint8_t> phasing_symbol(std::size_t slot) {
    if (slot % 2 == 0) {
        if (slot / 2 < kDscPhasingDxCount) {
            return kDscPhasingDx;
        }
        return std::nullopt;
    }
    const std::size_t j = slot / 2;
    if (j < kDscPhasingRx.size()) {
        return kDscPhasingRx[j];
    }
    return std::nullopt;
}

// Table A1-2: each symbol 00 to 99 is two decimal digits, the first sent
// first. Fails on a command symbol where digits belong.
Expected<std::string> digits_of(std::span<const std::uint8_t> symbols) {
    std::string out;
    out.reserve(symbols.size() * 2);
    for (const std::uint8_t s : symbols) {
        if (s > 99) {
            return fail(std::format("symbol {} where Table A1-2 puts two digits", s));
        }
        out.push_back(static_cast<char>('0' + s / 10));
        out.push_back(static_cast<char>('0' + s % 10));
    }
    return out;
}

std::uint64_t number_of(std::string_view digits) {
    std::uint64_t v = 0;
    for (const char c : digits) {
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return v;
}

// Clause 5.2 and its Note 1: ten digits, the tenth 0 unless M.1080 applies,
// and the identity is the first nine.
Expected<std::uint64_t> identity_of(std::span<const std::uint8_t> five) {
    auto digits = digits_of(five);
    if (!digits) {
        return std::unexpected(digits.error());
    }
    return number_of(std::string_view(*digits).substr(0, 9));
}

// Clause 8.1.2.
Expected<std::optional<DscPosition>> position_of(std::span<const std::uint8_t> five) {
    auto digits = digits_of(five);
    if (!digits) {
        return std::unexpected(digits.error());
    }
    const std::string& d = *digits;
    if (d == "9999999999") {
        return std::optional<DscPosition>{};
    }
    const int quadrant = d[0] - '0';
    if (quadrant > 3) {
        return fail(std::format("quadrant digit {}, where clause 8.1.2 allows 0 to 3", quadrant));
    }
    const double latitude = static_cast<double>(number_of(d.substr(1, 2))) +
                            static_cast<double>(number_of(d.substr(3, 2))) / 60.0;
    const double longitude = static_cast<double>(number_of(d.substr(5, 3))) +
                             static_cast<double>(number_of(d.substr(8, 2))) / 60.0;
    DscPosition p;
    // NE 0, NW 1, SE 2, SW 3.
    p.latitude = (quadrant >= 2) ? -latitude : latitude;
    p.longitude = (quadrant == 1 || quadrant == 3) ? -longitude : longitude;
    return std::optional<DscPosition>{p};
}

// Clause 8.1.3.
Expected<std::optional<std::uint16_t>> utc_of(std::span<const std::uint8_t> two) {
    auto digits = digits_of(two);
    if (!digits) {
        return std::unexpected(digits.error());
    }
    if (*digits == "8888") {
        return std::optional<std::uint16_t>{};
    }
    return std::optional<std::uint16_t>{static_cast<std::uint16_t>(number_of(*digits))};
}

// Table A1-5, a three-character element, character 3 sent first.
DscFrequency frequency_of(std::span<const std::uint8_t> three) {
    DscFrequency f;
    std::copy(three.begin(), three.end(), f.symbols.begin());
    auto digits = digits_of(three);
    if (!digits) {
        return f;
    }
    const std::string& d = *digits;  // HM TM M H T U
    switch (d[0]) {
        case '0':
        case '1':
        case '2':
            f.kind = DscFrequency::Kind::Frequency;
            f.value = number_of(d) * 100;
            break;
        case '3':
            f.kind = DscFrequency::Kind::HfChannel;
            f.value = number_of(std::string_view(d).substr(1));
            break;
        case '9':
            f.kind = DscFrequency::Kind::VhfChannel;
            f.value = number_of(std::string_view(d).substr(2));
            break;
        default: break;
    }
    return f;
}

// Clauses 8.1 and 8.2: nature, coordinates, time and subsequent
// communications, 9 characters.
Status read_distress(DscCall& call, std::span<const std::uint8_t> s) {
    call.nature_of_distress = s[0];
    auto position = position_of(s.subspan(1, 5));
    if (!position) {
        return std::unexpected(position.error());
    }
    call.distress_position = *position;
    auto utc = utc_of(s.subspan(6, 2));
    if (!utc) {
        return std::unexpected(utc.error());
    }
    call.utc_hhmm = *utc;
    call.subsequent_communications = s[8];
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// The code
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kDscCharacterBits> dsc_encode_symbol(std::uint8_t symbol) {
    std::array<std::uint8_t, kDscCharacterBits> bits{};
    unsigned b_count = 0;
    for (unsigned i = 0; i < kDscInformationBits; ++i) {
        // Clause 1.1.1: information bits least significant first.
        bits[i] = static_cast<std::uint8_t>((symbol >> i) & 1U);
        b_count += (bits[i] == 0U) ? 1U : 0U;
    }
    for (unsigned i = 0; i < 3; ++i) {
        // Check bits most significant first.
        bits[kDscInformationBits + i] = static_cast<std::uint8_t>((b_count >> (2U - i)) & 1U);
    }
    return bits;
}

std::optional<std::uint8_t> dsc_decode_symbol(std::span<const std::uint8_t, kDscCharacterBits> bits) {
    unsigned symbol = 0;
    unsigned b_count = 0;
    for (unsigned i = 0; i < kDscInformationBits; ++i) {
        symbol |= (bits[i] & 1U) << i;
        b_count += ((bits[i] & 1U) == 0U) ? 1U : 0U;
    }
    const unsigned check = ((bits[7] & 1U) << 2U) | ((bits[8] & 1U) << 1U) | (bits[9] & 1U);
    if (check != b_count) {
        return std::nullopt;
    }
    return static_cast<std::uint8_t>(symbol);
}

std::uint8_t dsc_ecc(std::span<const std::uint8_t> information) {
    unsigned parity = 0;
    for (const std::uint8_t s : information) {
        parity ^= s & 0x7FU;
    }
    return static_cast<std::uint8_t>(parity);
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

Expected<DscCall> dsc_parse(std::span<const std::uint8_t> information) {
    if (information.size() < 3) {
        return fail("a DSC call shorter than two format specifiers and an end of sequence");
    }
    DscCall call;
    call.format = information[0];
    call.eos = information.back();
    call.symbols.assign(information.begin(), information.end());
    const std::span<const std::uint8_t> body =
        information.subspan(2, information.size() - 3);  // after the specifiers, before EOS
    std::size_t at = 0;
    const auto take = [&](std::size_t n) -> Expected<std::span<const std::uint8_t>> {
        if (at + n > body.size()) {
            return fail(std::format("a format {} call ends before its Table A1-4 fields do",
                                    call.format));
        }
        const auto out = body.subspan(at, n);
        at += n;
        return out;
    };

    const auto read_self_id = [&]() -> Status {
        auto five = take(5);
        if (!five) {
            return std::unexpected(five.error());
        }
        auto id = identity_of(*five);
        if (!id) {
            return std::unexpected(id.error());
        }
        call.self_id = *id;
        return {};
    };

    // Clause 8.2 and Tables A1-4.2 to A1-4.4: a telecommand, the distress ID
    // and the distress information. Clause 8.4: the ID sent as symbol 126
    // five times is unknown.
    const auto read_relay = [&]() -> Status {
        auto one = take(1);
        if (!one) {
            return std::unexpected(one.error());
        }
        call.telecommand1 = (*one)[0];
        auto five = take(5);
        if (!five) {
            return std::unexpected(five.error());
        }
        if (std::ranges::all_of(*five, [](std::uint8_t s) { return s == kDscNoInformation; })) {
            call.distress_id.reset();
        } else {
            auto id = identity_of(*five);
            if (!id) {
                return std::unexpected(id.error());
            }
            call.distress_id = *id;
        }
        auto nine = take(9);
        if (!nine) {
            return std::unexpected(nine.error());
        }
        return read_distress(call, *nine);
    };

    // Clause 8.3 and Tables A1-4.5 to A1-4.10: two telecommands, then
    // Message 2 and perhaps 3, kept whole and read as channel or frequency
    // elements where they are.
    const auto read_routine = [&]() -> Status {
        auto two = take(2);
        if (!two) {
            return std::unexpected(two.error());
        }
        call.telecommand1 = (*two)[0];
        call.telecommand2 = (*two)[1];
        const auto rest = body.subspan(at);
        call.message_symbols.assign(rest.begin(), rest.end());
        // Clause 8.3.2: up to two three-character elements. A position is
        // told apart by its leading 55, which Table A1-5 gives no element,
        // and a network number by its leading 105 or 106 (clause 8.3.3).
        for (std::size_t e = 0; e + 3 <= rest.size() && e < 6; e += 3) {
            const auto element = rest.subspan(e, 3);
            if (element[0] == 55 || element[0] > 99) {
                if (!std::ranges::all_of(element,
                                         [](std::uint8_t s) { return s == kDscNoInformation; })) {
                    break;
                }
            }
            call.frequencies.push_back(frequency_of(element));
        }
        at = body.size();
        return {};
    };

    Status parsed;
    switch (call.format) {
        case kDscFormatDistress: {
            // Table A1-4.1: self-ID, nature, coordinates, time, subsequent
            // communications.
            parsed = read_self_id();
            if (parsed) {
                auto nine = take(9);
                parsed = nine ? read_distress(call, *nine) : Status(std::unexpected(nine.error()));
            }
            break;
        }
        case kDscFormatAllShips: {
            // Tables A1-4.2 to A1-4.5: no address, clause 5.1.
            auto category = take(1);
            if (!category) {
                parsed = std::unexpected(category.error());
                break;
            }
            call.category = (*category)[0];
            parsed = read_self_id();
            if (parsed) {
                parsed = (call.category == kDscCategoryDistress) ? read_relay() : read_routine();
            }
            break;
        }
        case kDscFormatIndividual:
        case kDscFormatGroup:
        case kDscFormatGeographicArea:
        case kDscFormatAutomatic: {
            auto address = take(5);
            if (!address) {
                parsed = std::unexpected(address.error());
                break;
            }
            if (call.format == kDscFormatGeographicArea) {
                // Clause 5.3's ten digits, kept as digits.
                auto digits = digits_of(*address);
                if (!digits) {
                    parsed = std::unexpected(digits.error());
                    break;
                }
                call.area_digits = *digits;
            } else {
                auto id = identity_of(*address);
                if (!id) {
                    parsed = std::unexpected(id.error());
                    break;
                }
                call.address = *id;
            }
            auto category = take(1);
            if (!category) {
                parsed = std::unexpected(category.error());
                break;
            }
            call.category = (*category)[0];
            parsed = read_self_id();
            if (parsed) {
                parsed = (call.category == kDscCategoryDistress) ? read_relay() : read_routine();
            }
            break;
        }
        default:
            // Table A1-3 note 1: "Unassigned symbols should be rejected."
            parsed = fail(std::format("format specifier {} is not one clause 4.1 lists",
                                      call.format));
            break;
    }
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (at != body.size()) {
        return fail(std::format("a format {} call carries {} characters its format does not",
                                call.format, body.size() - at));
    }
    return call;
}

// ---------------------------------------------------------------------------
// DscDecoder
// ---------------------------------------------------------------------------

Expected<DscDecoder> DscDecoder::create(const DscConfig& config) {
    ToneDiscriminatorConfig tones;
    tones.rate = config.rate;
    // Positive soft values are mark, which here is Y, binary 1.
    tones.mark_hz = config.y_hz;
    tones.space_hz = config.b_hz;
    tones.symbol_rate = config.bit_rate;
    auto discriminator = ToneDiscriminator::create(tones);
    if (!discriminator) {
        return std::unexpected(with_context(discriminator.error(), "DSC"));
    }
    BitClockConfig clock;
    clock.rate = config.rate;
    clock.symbol_rate = config.bit_rate;
    auto bit_clock = BitClock::create(clock);
    if (!bit_clock) {
        return std::unexpected(with_context(bit_clock.error(), "DSC"));
    }
    DscDecoder d;
    d.config_ = config;
    d.discriminator_ = std::move(*discriminator);
    d.clock_ = std::move(*bit_clock);
    d.reset();
    return d;
}

void DscDecoder::reset() {
    discriminator_.reset();
    clock_.reset();
    history_.clear();
    history_samples_.clear();
    history_base_ = 0;
    bit_count_ = 0;
    locked_ = false;
    slot0_bit_ = 0;
    next_slot_ = 0;
    characters_.clear();
    eos_at_.reset();
    stats_ = {};
}

void DscDecoder::process(ConstRealSpan audio, std::vector<DscCall>& out) {
    soft_.clear();
    bits_.clear();
    discriminator_.process(audio, soft_);
    clock_.process(soft_, bits_);
    const auto delay = static_cast<SampleIndex>(discriminator_.group_delay());
    for (const SoftBit& b : bits_) {
        // Clause 1.4 and Table A1-1: the lower tone, mark here, is Y, 1.
        const std::uint8_t bit = (b.value >= 0.0F) ? 1U : 0U;
        on_bit(bit, b.position > delay ? b.position - delay : 0, out);
    }
}

std::optional<std::uint8_t> DscDecoder::slot_symbol(std::size_t slot) const {
    const std::uint64_t first = slot0_bit_ + slot * kDscCharacterBits;
    if (first < history_base_ || first + kDscCharacterBits > history_base_ + history_.size()) {
        return std::nullopt;
    }
    const std::size_t at = static_cast<std::size_t>(first - history_base_);
    return dsc_decode_symbol(
        std::span<const std::uint8_t, kDscCharacterBits>(history_.data() + at, kDscCharacterBits));
}

void DscDecoder::on_bit(std::uint8_t bit, SampleIndex sample, std::vector<DscCall>& out) {
    history_.push_back(bit);
    history_samples_.push_back(sample);
    ++bit_count_;

    if (!locked_) {
        if (try_phasing()) {
            ++stats_.phasings;
            locked_ = true;
            characters_.clear();
            eos_at_.reset();
        }
    }
    while (locked_ && slot0_bit_ + (next_slot_ + 1) * kDscCharacterBits <= bit_count_) {
        on_slot(out);
    }

    // Keep what a search or the call in hand can still read.
    std::uint64_t keep_from = bit_count_ > kSearchHistoryBits ? bit_count_ - kSearchHistoryBits : 0;
    if (locked_) {
        keep_from = std::min(keep_from, slot0_bit_);
    }
    if (keep_from > history_base_ + 4096) {
        const auto drop = static_cast<std::ptrdiff_t>(keep_from - history_base_);
        history_.erase(history_.begin(), history_.begin() + drop);
        history_samples_.erase(history_samples_.begin(), history_samples_.begin() + drop);
        history_base_ = keep_from;
    }
}

bool DscDecoder::try_phasing() {
    // Clause 3.3: phasing is achieved on two DX and one RX, two RX and one DX,
    // or three RX, each in its own position, "in either consecutive or
    // non-consecutive positions". Try every phasing slot as the one the
    // newest bit completes, and count what the slots before it hold.
    if (bit_count_ < kDscCharacterBits) {
        return false;
    }
    for (std::size_t s = 0; s < kPhasingSlots; ++s) {
        if (bit_count_ < (s + 1) * kDscCharacterBits) {
            break;
        }
        const std::uint64_t slot0 = bit_count_ - (s + 1) * kDscCharacterBits;
        if (slot0 < history_base_) {
            break;
        }
        int dx = 0;
        int rx = 0;
        for (std::size_t t = 0; t <= s; ++t) {
            const auto expected = phasing_symbol(t);
            if (!expected) {
                continue;
            }
            const std::size_t at =
                static_cast<std::size_t>(slot0 + t * kDscCharacterBits - history_base_);
            const auto got = dsc_decode_symbol(std::span<const std::uint8_t, kDscCharacterBits>(
                history_.data() + at, kDscCharacterBits));
            if (got && *got == *expected) {
                (t % 2 == 0 ? dx : rx) += 1;
            }
        }
        if ((dx >= 2 && rx >= 1) || (rx >= 2 && dx >= 1) || rx >= 3) {
            slot0_bit_ = slot0;
            // The slots up to s are phasing, or format specifiers already in
            // the history; on_slot reads from slot 12 either way.
            next_slot_ = std::min(s + 1, kFirstInformationDxSlot);
            return true;
        }
    }
    return false;
}

void DscDecoder::on_slot(std::vector<DscCall>& out) {
    const std::size_t slot = next_slot_++;
    if (slot < kFirstInformationDxSlot) {
        return;
    }
    const auto symbol = slot_symbol(slot);
    const auto ensure = [&](std::size_t i) {
        if (characters_.size() <= i) {
            characters_.resize(i + 1);
        }
    };
    if ((slot - kFirstInformationDxSlot) % 2 == 0) {
        const std::size_t i = (slot - kFirstInformationDxSlot) / 2;
        ensure(i);
        characters_[i].dx = symbol;
        return;
    }
    if (slot < kFirstInformationRxSlot) {
        return;  // RX phasing slots 13 and 15.
    }
    const std::size_t i = (slot - kFirstInformationRxSlot) / 2;
    ensure(i);
    characters_[i].rx = symbol;

    if (eos_at_) {
        if (i == *eos_at_ + 1) {
            finish(out);
        }
        return;
    }

    // Character i is complete. A call gives up on a carrier gone, or on a
    // sequence with no end.
    int lost_in_a_row = 0;
    for (std::size_t k = characters_.size(); k > 0; --k) {
        const Resolved& c = characters_[k - 1];
        if (k - 1 > i) {
            continue;
        }
        if (c.dx || c.rx) {
            break;
        }
        ++lost_in_a_row;
    }
    if (lost_in_a_row >= kLostInARow || i + 1 >= kDscMaximumCharacters) {
        ++stats_.lost;
        locked_ = false;
        return;
    }
    const Resolved& c = characters_[i];
    const std::optional<std::uint8_t> value = c.dx ? c.dx : c.rx;
    // The first end of sequence ends the information: clause 9's three
    // symbols are assigned nowhere else a call carries them (Table A1-3's
    // note 6 on their telecommand rows), and digits are 0 to 99.
    if (i >= 2 && value && is_eos(*value)) {
        eos_at_ = i;
    }
}

void DscDecoder::finish(std::vector<DscCall>& out) {
    locked_ = false;
    const std::size_t end = *eos_at_;
    const std::size_t ecc_index = end + 1;

    std::vector<std::uint8_t> primary;
    std::vector<std::size_t> disagree;
    std::vector<std::uint8_t> alternative;
    int from_rx = 0;
    for (std::size_t k = 0; k <= ecc_index; ++k) {
        const Resolved& c = characters_[k];
        if (!c.dx && !c.rx) {
            ++stats_.lost;
            return;
        }
        if (c.dx) {
            primary.push_back(*c.dx);
            if (c.rx && *c.rx != *c.dx) {
                disagree.push_back(k);
                alternative.push_back(*c.rx);
            }
        } else {
            primary.push_back(*c.rx);
            ++from_rx;
        }
    }

    // Clause 4.2: a distress alert and an all ships call need the format
    // specifier twice. Other calls need it once, and a disagreement between
    // the two is settled below with the rest.
    const std::uint8_t format0 = primary[0];
    const std::uint8_t format1 = primary[1];
    if (format0 != format1 &&
        (format0 == kDscFormatDistress || format0 == kDscFormatAllShips ||
         format1 == kDscFormatDistress || format1 == kDscFormatAllShips)) {
        ++stats_.format_mismatches;
        return;
    }

    // Clause 10.2, with the one format specifier and one end of sequence it
    // asks for: everything from the second specifier to the end of sequence.
    const auto ecc_holds = [&](const std::vector<std::uint8_t>& symbols) {
        const std::span<const std::uint8_t> information(symbols.data() + 1, end);
        return dsc_ecc(information) == (symbols[ecc_index] & 0x7FU);
    };

    // Clause 10.4 asks for the most out of the signal "including use of the
    // error-check character": where DX and RX both checked and disagreed,
    // try the RX copies until the parity holds. At most four such
    // characters, sixteen combinations; more is a call too damaged to trust.
    std::vector<std::uint8_t> chosen = primary;
    bool ok = ecc_holds(chosen);
    if (!ok && !disagree.empty() && disagree.size() <= 4) {
        const unsigned combinations = 1U << disagree.size();
        for (unsigned mask = 1; mask < combinations && !ok; ++mask) {
            std::vector<std::uint8_t> trial = primary;
            for (std::size_t d = 0; d < disagree.size(); ++d) {
                if ((mask >> d) & 1U) {
                    trial[disagree[d]] = alternative[d];
                }
            }
            if (ecc_holds(trial)) {
                chosen = std::move(trial);
                ok = true;
            }
        }
    }
    if (!ok) {
        // Clause 10.3's allowance, "may be ignored if this was due to an
        // error ... correctable by use of the time diversity code", is about
        // acknowledging, which this does not do; a receiver that reports a
        // call the parity refuses is reporting a guess.
        ++stats_.ecc_failures;
        return;
    }
    if (chosen[0] != chosen[1]) {
        // A single format specifier is enough for these formats (clause
        // 4.2); take whichever made the parity hold, which is chosen[1],
        // the one the check covers.
        chosen[0] = chosen[1];
    }

    const std::span<const std::uint8_t> information(chosen.data(), end + 1);
    auto call = dsc_parse(information);
    if (!call) {
        ++stats_.malformed;
        return;
    }
    call->ecc = chosen[ecc_index];
    call->from_rx = from_rx;
    call->disagreements = static_cast<int>(disagree.size());
    const std::uint64_t first_bit = slot0_bit_;
    const std::uint64_t last_bit =
        slot0_bit_ + (kFirstInformationRxSlot + 2 * ecc_index + 1) * kDscCharacterBits - 1;
    const auto sample_of = [&](std::uint64_t bit) -> SampleIndex {
        if (bit < history_base_ || bit >= history_base_ + history_samples_.size()) {
            return 0;
        }
        return history_samples_[static_cast<std::size_t>(bit - history_base_)];
    };
    call->first_sample = sample_of(first_bit);
    call->last_sample = sample_of(last_bit);
    ++stats_.calls;
    out.push_back(std::move(*call));
}

}  // namespace revenant::decode
