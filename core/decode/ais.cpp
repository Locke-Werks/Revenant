#include "core/decode/ais.h"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/decode/ax25.h"

namespace revenant::decode {

namespace {

std::string raw_text(std::span<const std::uint8_t> octets, std::size_t first_bit,
                     std::size_t characters) {
    std::string text;
    text.reserve(characters);
    for (std::size_t i = 0; i < characters; ++i) {
        text.push_back(ais_sixbit_char(ais_field(octets, first_bit + 6 * i, 6)));
    }
    return text;
}

void trim_padding(std::string& text) {
    while (!text.empty() && (text.back() == '@' || text.back() == ' ')) {
        text.pop_back();
    }
}

// Table 48's 28-bit longitude and 27-bit latitude, in 1/10000 minute.
constexpr unsigned kLongitudeBits = 28;
constexpr unsigned kLatitudeBits = 27;

// Figure 41: A and B nine bits, C and D six, A the most significant.
AisDimensions dimensions_at(std::span<const std::uint8_t> octets, std::size_t at) {
    AisDimensions d;
    d.to_bow = ais_field(octets, at, 9);
    d.to_stern = ais_field(octets, at + 9, 9);
    d.to_port = ais_field(octets, at + 18, 6);
    d.to_starboard = ais_field(octets, at + 24, 6);
    return d;
}

std::optional<std::uint16_t> unless(std::uint32_t value, std::uint32_t not_available) {
    if (value == not_available) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(value);
}

// The position, accuracy flag and the SOG, COG and heading as Tables 48, 70
// and 71 lay them out, from the SOG field on. Messages 1 to 3 put SOG at bit
// 50, Messages 18 and 19 at 46; the fields after it are in the same order and
// widths in all five: SOG 10, PA 1, longitude 28, latitude 27, COG 12,
// heading 9, time stamp 6.
std::size_t read_motion(AisMessage& m, std::span<const std::uint8_t> octets, std::size_t at) {
    m.sog_tenths = unless(ais_field(octets, at, 10), kAisSogNotAvailable);
    at += 10;
    m.position_accuracy = ais_field(octets, at, 1) != 0U;
    at += 1;
    const std::int32_t lon = ais_signed_field(octets, at, kLongitudeBits);
    at += kLongitudeBits;
    const std::int32_t lat = ais_signed_field(octets, at, kLatitudeBits);
    at += kLatitudeBits;
    m.position = ais_position(lon, lat);
    m.cog_tenths = unless(ais_field(octets, at, 12), kAisCogNotAvailable);
    at += 12;
    m.heading = unless(ais_field(octets, at, 9), kAisHeadingNotAvailable);
    at += 9;
    m.timestamp = static_cast<std::uint8_t>(ais_field(octets, at, 6));
    at += 6;
    return at;
}

// Table 48, Messages 1, 2 and 3: 168 bits.
bool parse_position_report(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 168) {
        return false;
    }
    m.navigational_status = static_cast<std::uint8_t>(ais_field(octets, 38, 4));
    m.rate_of_turn = static_cast<std::int8_t>(ais_signed_field(octets, 42, 8));
    const std::size_t after = read_motion(m, octets, 50);
    // Time stamp ends at 143; special manoeuvre 2, spare 3, RAIM 1, then the
    // communication state of Table 49.
    m.special_manoeuvre = static_cast<std::uint8_t>(ais_field(octets, after, 2));
    m.raim = ais_field(octets, 148, 1) != 0U;
    m.communication_state = ais_field(octets, 149, 19);
    return true;
}

// Table 51, Messages 4 and 11: 168 bits.
bool parse_base_station(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 168) {
        return false;
    }
    AisMessage::Utc utc;
    utc.year = static_cast<std::uint16_t>(ais_field(octets, 38, 14));
    utc.month = static_cast<std::uint8_t>(ais_field(octets, 52, 4));
    utc.day = static_cast<std::uint8_t>(ais_field(octets, 56, 5));
    utc.hour = static_cast<std::uint8_t>(ais_field(octets, 61, 5));
    utc.minute = static_cast<std::uint8_t>(ais_field(octets, 66, 6));
    utc.second = static_cast<std::uint8_t>(ais_field(octets, 72, 6));
    m.utc = utc;
    m.position_accuracy = ais_field(octets, 78, 1) != 0U;
    m.position = ais_position(ais_signed_field(octets, 79, kLongitudeBits),
                              ais_signed_field(octets, 107, kLatitudeBits));
    m.epfd = static_cast<std::uint8_t>(ais_field(octets, 134, 4));
    // 138 is the long-range transmission control bit, then 9 spare.
    m.raim = ais_field(octets, 148, 1) != 0U;
    m.communication_state = ais_field(octets, 149, 19);
    return true;
}

// Table 52, Message 5: 424 bits.
bool parse_static_voyage(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 424) {
        return false;
    }
    m.ais_version = static_cast<std::uint8_t>(ais_field(octets, 38, 2));
    m.imo_number = ais_field(octets, 40, 30);
    m.callsign = ais_text(octets, 70, 7);
    m.name = ais_text(octets, 112, 20);
    m.ship_type = static_cast<std::uint8_t>(ais_field(octets, 232, 8));
    m.dimensions = dimensions_at(octets, 240);
    m.epfd = static_cast<std::uint8_t>(ais_field(octets, 270, 4));
    // Table 52's ETA: bits 19-16 month, 15-11 day, 10-6 hour, 5-0 minute.
    AisMessage::Eta eta;
    eta.month = static_cast<std::uint8_t>(ais_field(octets, 274, 4));
    eta.day = static_cast<std::uint8_t>(ais_field(octets, 278, 5));
    eta.hour = static_cast<std::uint8_t>(ais_field(octets, 283, 5));
    eta.minute = static_cast<std::uint8_t>(ais_field(octets, 288, 6));
    m.eta = eta;
    m.draught_tenths = static_cast<std::uint8_t>(ais_field(octets, 294, 8));
    m.destination = ais_text(octets, 302, 20);
    m.dte_not_available = ais_field(octets, 422, 1) != 0U;
    return true;
}

// Table 70, Message 18: 168 bits.
bool parse_class_b_position(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 168) {
        return false;
    }
    // Eight spare bits at 38, then the motion fields.
    read_motion(m, octets, 46);
    // Time stamp ends at 139, then 2 spare.
    AisMessage::ClassBFlags flags;
    flags.carrier_sense = ais_field(octets, 141, 1) != 0U;
    flags.display = ais_field(octets, 142, 1) != 0U;
    flags.dsc = ais_field(octets, 143, 1) != 0U;
    flags.whole_band = ais_field(octets, 144, 1) != 0U;
    flags.message_22 = ais_field(octets, 145, 1) != 0U;
    m.class_b = flags;
    m.assigned_mode = ais_field(octets, 146, 1) != 0U;
    m.raim = ais_field(octets, 147, 1) != 0U;
    // 148 is the communication state selector flag, then 19 bits of state.
    m.communication_state = ais_field(octets, 149, 19);
    return true;
}

// Table 71, Message 19: 312 bits.
bool parse_class_b_extended(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 312) {
        return false;
    }
    read_motion(m, octets, 46);
    // Time stamp ends at 139, then 4 spare.
    m.name = ais_text(octets, 143, 20);
    m.ship_type = static_cast<std::uint8_t>(ais_field(octets, 263, 8));
    m.dimensions = dimensions_at(octets, 271);
    m.epfd = static_cast<std::uint8_t>(ais_field(octets, 301, 4));
    m.raim = ais_field(octets, 305, 1) != 0U;
    m.dte_not_available = ais_field(octets, 306, 1) != 0U;
    m.assigned_mode = ais_field(octets, 307, 1) != 0U;
    return true;
}

// Table 73, Message 21: 272 to 360 bits.
bool parse_aton(AisMessage& m, std::span<const std::uint8_t> octets) {
    const std::size_t bits = octets.size() * 8;
    if (bits < 272) {
        return false;
    }
    m.aton_type = static_cast<std::uint8_t>(ais_field(octets, 38, 5));
    std::string name = ais_text(octets, 43, 20);
    m.position_accuracy = ais_field(octets, 163, 1) != 0U;
    m.position = ais_position(ais_signed_field(octets, 164, kLongitudeBits),
                              ais_signed_field(octets, 192, kLatitudeBits));
    m.dimensions = dimensions_at(octets, 219);
    m.epfd = static_cast<std::uint8_t>(ais_field(octets, 249, 4));
    m.timestamp = static_cast<std::uint8_t>(ais_field(octets, 253, 6));
    m.off_position = ais_field(octets, 259, 1) != 0U;
    m.aton_status = static_cast<std::uint8_t>(ais_field(octets, 260, 8));
    m.raim = ais_field(octets, 268, 1) != 0U;
    m.virtual_aton = ais_field(octets, 269, 1) != 0U;
    m.assigned_mode = ais_field(octets, 270, 1) != 0U;
    // 271 is spare. Then Table 73's "Name of Aid-to-Navigation Extension",
    // 0 to 14 six-bit characters with no "@" padding, followed by 0, 2, 4 or
    // 6 spare bits to the octet boundary. The characters are however many
    // whole six-bit groups follow, capped at 14; when the spare run is six
    // bits it reads as one "@", which ais_text drops, since the extension
    // itself never sends one.
    const std::size_t extension = std::min<std::size_t>((bits - 272) / 6, 14);
    if (extension > 0) {
        // The extension continues the name, which is then all twenty
        // characters of its field, a trailing space included: read both
        // whole and trim the join.
        name = raw_text(octets, 43, 20) + raw_text(octets, 272, extension);
        trim_padding(name);
    }
    m.name = std::move(name);
    return true;
}

// Tables 78 and 79, Message 24: Part A 160 bits, Part B 168.
bool parse_static_report(AisMessage& m, std::span<const std::uint8_t> octets) {
    if (octets.size() * 8 < 40) {
        return false;
    }
    const auto part = static_cast<std::uint8_t>(ais_field(octets, 38, 2));
    if (part == 0) {
        if (octets.size() * 8 < 160) {
            return false;
        }
        m.part_number = part;
        m.name = ais_text(octets, 40, 20);
        return true;
    }
    if (part == 1) {
        if (octets.size() * 8 < 168) {
            return false;
        }
        m.part_number = part;
        m.ship_type = static_cast<std::uint8_t>(ais_field(octets, 40, 8));
        // Table 79A: manufacturer's ID in bits 41 to 24 of the 42, then the
        // unit model code in 23 to 20 and the serial number in 19 to 0.
        m.vendor = ais_text(octets, 48, 3);
        m.unit_model = static_cast<std::uint8_t>(ais_field(octets, 66, 4));
        m.unit_serial = ais_field(octets, 70, 20);
        m.callsign = ais_text(octets, 90, 7);
        m.dimensions = dimensions_at(octets, 132);
        m.epfd = static_cast<std::uint8_t>(ais_field(octets, 162, 4));
        return true;
    }
    // Parts 2 and 3 are not defined.
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// The link layer
// ---------------------------------------------------------------------------

std::uint16_t ais_fcs(std::span<const std::uint8_t> data) {
    // See the header: the HDLC FCS, which ax25_fcs computes as ISO 3309 and
    // Finnegan and Benson Figure 5 state it.
    return ax25_fcs(data);
}

std::uint32_t ais_field(std::span<const std::uint8_t> octets, std::size_t first_bit,
                        unsigned width) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < width; ++i) {
        const std::size_t k = first_bit + i;
        const std::size_t octet = k / 8;
        const unsigned bit = (octet < octets.size())
                                 ? ((octets[octet] >> (7U - static_cast<unsigned>(k % 8))) & 1U)
                                 : 0U;
        v = (v << 1U) | bit;
    }
    return v;
}

std::int32_t ais_signed_field(std::span<const std::uint8_t> octets, std::size_t first_bit,
                              unsigned width) {
    // Annex 8 clause 3: "Negative numbers are expressed using 2's complement."
    const std::uint32_t raw = ais_field(octets, first_bit, width);
    const std::uint32_t sign = 1U << (width - 1U);
    if ((raw & sign) == 0U) {
        return static_cast<std::int32_t>(raw);
    }
    return static_cast<std::int32_t>(static_cast<std::int64_t>(raw) -
                                     (static_cast<std::int64_t>(1) << width));
}

char ais_sixbit_char(unsigned value) {
    // Table 47.
    const unsigned v = value & 0x3FU;
    return static_cast<char>(v < 32U ? v + 64U : v);
}

std::string ais_text(std::span<const std::uint8_t> octets, std::size_t first_bit,
                     std::size_t characters) {
    std::string text = raw_text(octets, first_bit, characters);
    trim_padding(text);
    return text;
}

std::optional<AisPosition> ais_position(std::int32_t longitude_units,
                                        std::int32_t latitude_units) {
    if (longitude_units == kAisLongitudeNotAvailable ||
        latitude_units == kAisLatitudeNotAvailable) {
        return std::nullopt;
    }
    AisPosition p;
    p.longitude = static_cast<double>(longitude_units) / kAisPositionUnitsPerDegree;
    p.latitude = static_cast<double>(latitude_units) / kAisPositionUnitsPerDegree;
    if (std::abs(p.longitude) > 180.0 || std::abs(p.latitude) > 90.0) {
        return std::nullopt;
    }
    return p;
}

AisMessage ais_parse(std::span<const std::uint8_t> octets) {
    AisMessage m;
    m.octets.assign(octets.begin(), octets.end());
    m.message_id = static_cast<std::uint8_t>(ais_field(octets, 0, kAisMessageIdBits));
    m.repeat = static_cast<std::uint8_t>(ais_field(octets, 6, 2));
    m.mmsi = ais_field(octets, 8, 30);

    switch (m.message_id) {
        case 1:
        case 2:
        case 3: m.parsed = parse_position_report(m, octets); break;
        case 4:
        case 11: m.parsed = parse_base_station(m, octets); break;
        case 5: m.parsed = parse_static_voyage(m, octets); break;
        case 18: m.parsed = parse_class_b_position(m, octets); break;
        case 19: m.parsed = parse_class_b_extended(m, octets); break;
        case 21: m.parsed = parse_aton(m, octets); break;
        case 24: m.parsed = parse_static_report(m, octets); break;
        default: m.parsed = false; break;
    }
    if (!m.parsed) {
        // Whatever a partial parse filled in is not a claim about the message.
        AisMessage bare;
        bare.message_id = m.message_id;
        bare.repeat = m.repeat;
        bare.mmsi = m.mmsi;
        bare.octets = std::move(m.octets);
        return bare;
    }
    return m;
}

// ---------------------------------------------------------------------------
// AisDecoder
// ---------------------------------------------------------------------------

Expected<AisDecoder> AisDecoder::create(const AisConfig& config) {
    if (config.rate < kAisMinimumRate) {
        return fail(std::format("the AIS decoder needs at least {} samples a second, three to "
                                "each of M.1371-5's 9600 bits, and was given {}",
                                kAisMinimumRate, config.rate));
    }
    if (!(config.sync_threshold > 0.0 && config.sync_threshold < 1.0)) {
        return fail("the AIS sync threshold is a correlation and must lie in (0, 1)");
    }
    // One bit of samples, rounded, each weighted alike.
    const auto taps_count = static_cast<std::size_t>(
        std::max(1.0, std::round(static_cast<double>(config.rate) / kAisBitRate)));
    auto filter = RealFir::create(
        std::vector<float>(taps_count, 1.0F / static_cast<float>(taps_count)));
    if (!filter) {
        return std::unexpected(with_context(filter.error(), "AIS"));
    }

    AisDecoder d;
    d.config_ = config;
    d.filter_ = std::move(*filter);
    d.filter_delay_ = (taps_count - 1) / 2;
    d.rate_ = static_cast<std::uint64_t>(config.rate);
    d.instant_denominator_ = static_cast<std::uint64_t>(kAisBitRate) * kAisPhases;

    // Clause 3.2.2.3's "010101..." then clause 3.2.2.4's 01111110, each bit
    // as clause 2.6's NRZI makes it a level: a zero changes the level.
    float level = 1.0F;
    for (std::size_t k = 0; k < kSyncBits; ++k) {
        const unsigned bit = (k < kAisTrainingBits)
                                 ? static_cast<unsigned>(k % 2)
                                 : ((kAisFlag >> (k - kAisTrainingBits)) & 1U);
        if (bit == 0U) {
            level = -level;
        }
        d.pattern_[k] = level;
    }
    double mean = 0.0;
    for (const float v : d.pattern_) {
        mean += v;
    }
    mean /= static_cast<double>(kSyncBits);
    double spread = 0.0;
    for (const float v : d.pattern_) {
        spread += (v - mean) * (v - mean);
    }
    d.pattern_mean_ = mean;
    d.pattern_spread_ = spread;
    d.reset();
    return d;
}

void AisDecoder::reset() {
    filter_.reset();
    previous_soft_ = 0.0F;
    soft_count_ = 0;
    next_instant_ = 0;
    for (Phase& p : phases_) {
        p = Phase{};
    }
    recent_.clear();
    stats_ = {};
}

void AisDecoder::process(ConstRealSpan audio, std::vector<AisMessage>& out) {
    const auto delay = static_cast<std::uint64_t>(filter_delay_);

    // RealFir carries its history across calls, so the filtered stream is
    // the same however the audio was cut.
    soft_.resize(audio.size());
    if (!filter_.process(audio, soft_)) {
        return;
    }
    for (const float y : soft_) {
        const std::uint64_t n = soft_count_++;
        for (;;) {
            // Instant m sits at m * rate / (9600 * phases) filtered samples,
            // and is read between the samples either side of it; every
            // instant before this sample's has been read already.
            const std::uint64_t numerator = next_instant_ * rate_;
            const std::uint64_t index = numerator / instant_denominator_;
            if (index + 1 > n) {
                break;
            }
            const double fraction = static_cast<double>(numerator % instant_denominator_) /
                                    static_cast<double>(instant_denominator_);
            const auto a = static_cast<double>(previous_soft_);
            const auto b = static_cast<double>(y);
            const auto value = static_cast<float>(a + (b - a) * fraction);

            const auto phase_index = static_cast<std::uint8_t>(next_instant_ % kAisPhases);
            const SampleIndex sample = index > delay ? index - delay : 0;
            on_reading(phases_[phase_index], phase_index, value, sample, out);
            ++next_instant_;
        }
        previous_soft_ = y;
    }

    // Forget reported frames no phase can still be finishing: every phase
    // closes the same frame within a bit of the first.
    const double samples_per_bit = static_cast<double>(rate_) / kAisBitRate;
    const auto horizon = static_cast<SampleIndex>(std::ceil(4.0 * samples_per_bit));
    const SampleIndex now = soft_count_ > delay ? soft_count_ - delay : 0;
    std::erase_if(recent_, [&](const Reported& r) { return r.last_sample + horizon < now; });
}

void AisDecoder::on_reading(Phase& p, std::uint8_t phase_index, float value, SampleIndex sample,
                            std::vector<AisMessage>& out) {
    // The bursts already being read take this bit first.
    std::erase_if(p.attempts,
                  [&](Attempt& a) { return !on_bit(a, phase_index, value, sample, out); });

    p.ring[p.head] = value;
    p.head = (p.head + 1) % kSyncBits;
    if (p.filled < kSyncBits) {
        ++p.filled;
        return;
    }

    // Least squares of the last kSyncBits readings against the pattern:
    // reading = amplitude * level + offset. The correlation says whether
    // this is a burst's start at all; the offset is its slicing level.
    double sum = 0.0;
    for (const float v : p.ring) {
        sum += v;
    }
    const double mean = sum / static_cast<double>(kSyncBits);
    double covariance = 0.0;
    double spread = 0.0;
    for (std::size_t k = 0; k < kSyncBits; ++k) {
        const double v = static_cast<double>(p.ring[(p.head + k) % kSyncBits]) - mean;
        covariance += v * (static_cast<double>(pattern_[k]) - pattern_mean_);
        spread += v * v;
    }
    if (!(spread > 0.0)) {
        return;
    }
    const double correlation = covariance / std::sqrt(spread * pattern_spread_);
    // NRZI carries nothing in the polarity, so the pattern may arrive either
    // way up.
    if (std::abs(correlation) < config_.sync_threshold) {
        return;
    }
    if (p.attempts.size() >= kAttemptsPerPhase) {
        return;
    }
    const double amplitude = covariance / pattern_spread_;
    Attempt a;
    a.threshold = mean - amplitude * pattern_mean_;
    // The level of the flag's last bit, which the first data bit is read
    // against.
    a.previous_level = static_cast<double>(value) >= a.threshold;
    ++stats_.syncs;
    p.attempts.push_back(std::move(a));
}

bool AisDecoder::on_bit(Attempt& a, std::uint8_t phase_index, float value, SampleIndex sample,
                        std::vector<AisMessage>& out) {
    // Clause 2.6: NRZI, a change of level for a zero.
    const bool level = static_cast<double>(value) >= a.threshold;
    const std::uint8_t bit = (level == a.previous_level) ? 1U : 0U;
    a.previous_level = level;

    if (bit != 0U) {
        ++a.ones;
        if (a.ones > kAisStuffAfterOnes + 1) {
            // Seven ones: neither a flag nor anything clause 3.2.2.1 lets
            // data contain.
            return false;
        }
        a.bits.push_back(1);
        if (!a.have_first) {
            a.first_sample = sample;
            a.have_first = true;
        }
        // Clause 3.2.2.11's five slots and no end flag: lost.
        return a.bits.size() <= kAisMaximumFrameBits;
    }

    const int run = a.ones;
    a.ones = 0;
    if (run == kAisStuffAfterOnes + 1) {
        // Clause 3.2.2.7: the end flag, 01111110. Its leading zero and six
        // ones went into the data; take them back out. A flag straight after
        // the start flag closes nothing and the burst reads on.
        a.bits.resize(a.run_start > 0 ? a.run_start - 1 : 0);
        if (a.bits.empty()) {
            a.run_start = 0;
            a.have_first = false;
            return true;
        }
        on_frame(a, phase_index, sample, out);
        return false;
    }
    if (run == kAisStuffAfterOnes) {
        // Clause 3.2.2.1: "the first zero after five (5) consecutive ones
        // (1's) should be removed."
        a.run_start = a.bits.size();
        return true;
    }
    a.bits.push_back(0);
    if (!a.have_first) {
        a.first_sample = sample;
        a.have_first = true;
    }
    a.run_start = a.bits.size();
    return a.bits.size() <= kAisMaximumFrameBits;
}

void AisDecoder::on_frame(const Attempt& a, std::uint8_t phase_index, SampleIndex last_sample,
                          std::vector<AisMessage>& out) {
    // Whole octets, clause 3.3.7's byte boundary, and at least a message and
    // its FCS.
    if (a.bits.size() % 8 != 0 || a.bits.size() / 8 < kAisMinimumMessageOctets + 2) {
        return;
    }
    ++stats_.candidates;
    std::vector<std::uint8_t> octets(a.bits.size() / 8, 0);
    for (std::size_t i = 0; i < a.bits.size(); ++i) {
        // Clause 3.3.7: each octet least significant bit first.
        octets[i / 8] = static_cast<std::uint8_t>(octets[i / 8] | (a.bits[i] << (i % 8)));
    }
    const std::size_t n = octets.size();
    const auto data = std::span<const std::uint8_t>(octets).first(n - 2);
    const auto received =
        static_cast<std::uint16_t>(octets[n - 2] | (static_cast<unsigned>(octets[n - 1]) << 8U));
    const std::uint16_t fcs = ais_fcs(data);
    if (fcs != received) {
        ++stats_.fcs_failures;
        if (static_cast<std::uint16_t>(fcs ^ 0xFFFFU) == received) {
            ++stats_.fcs_uncomplemented;
        }
        return;
    }

    // Another phase already reported this frame: the same octets closing
    // within a few bits of each other.
    const double samples_per_bit = static_cast<double>(rate_) / kAisBitRate;
    const auto near = static_cast<SampleIndex>(std::ceil(2.0 * samples_per_bit));
    for (const Reported& r : recent_) {
        const SampleIndex apart =
            r.last_sample > last_sample ? r.last_sample - last_sample : last_sample - r.last_sample;
        if (apart <= near && std::ranges::equal(r.octets, data)) {
            ++stats_.duplicates;
            return;
        }
    }
    recent_.push_back(Reported{std::vector<std::uint8_t>(data.begin(), data.end()), last_sample});

    AisMessage message = ais_parse(data);
    message.first_sample = a.first_sample;
    message.last_sample = last_sample;
    message.phase = phase_index;
    ++stats_.messages;
    out.push_back(std::move(message));
}

}  // namespace revenant::decode
