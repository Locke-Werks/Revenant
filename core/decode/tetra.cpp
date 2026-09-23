#include "core/decode/tetra.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "core/decode/dv_codes.h"

namespace revenant::decode {
namespace {

constexpr double kPi = std::numbers::pi;

const ConvolutionalCode& tetra_mother_code() {
    static const ConvolutionalCode code{kTetraMotherMemory, kTetraMotherGenerators};
    return code;
}

// Reads `count` bits most significant first from a bit-per-byte span.
std::uint32_t read_field(std::span<const std::uint8_t> bits, std::size_t& cursor,
                         std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t bit = (cursor < bits.size()) ? (bits[cursor] & 1U) : 0U;
        value = (value << 1U) | bit;
        ++cursor;
    }
    return value;
}

void write_field(std::span<std::uint8_t> bits, std::size_t& cursor, std::uint32_t value,
                 std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (cursor < bits.size()) {
            bits[cursor] = static_cast<std::uint8_t>((value >> (count - 1 - i)) & 1U);
        }
        ++cursor;
    }
}

}  // namespace

double tetra_phase_transition(std::uint8_t first, std::uint8_t second) {
    // Clause 5.4, table 5.1:
    //   B(2k-1) B(2k)   D(k)
    //      1      1    -3*pi/4
    //      0      1    +3*pi/4
    //      0      0    +pi/4
    //      1      0    -pi/4
    const bool a = (first & 1U) != 0U;
    const bool b = (second & 1U) != 0U;
    if (b) {
        return a ? -3.0 * kPi / 4.0 : 3.0 * kPi / 4.0;
    }
    return a ? -kPi / 4.0 : kPi / 4.0;
}

// ---------------------------------------------------------------------------
// The SYNC PDU
// ---------------------------------------------------------------------------

std::array<std::uint8_t, kTetraBschInformationBits> tetra_sync_pdu_bits(const TetraSyncPdu& pdu) {
    std::array<std::uint8_t, kTetraBschInformationBits> bits{};
    std::size_t cursor = 0;
    // Table 21.76, in the order the table lists the elements.
    write_field(bits, cursor, pdu.system_code, 4);
    write_field(bits, cursor, pdu.colour_code, 6);
    write_field(bits, cursor, pdu.timeslot, 2);
    write_field(bits, cursor, pdu.frame_number, 5);
    write_field(bits, cursor, pdu.multiframe_number, 6);
    write_field(bits, cursor, pdu.sharing_mode, 2);
    write_field(bits, cursor, pdu.reserved_frames, 3);
    write_field(bits, cursor, pdu.uplane_dtx_allowed ? 1U : 0U, 1);
    write_field(bits, cursor, pdu.frame18_extension ? 1U : 0U, 1);
    write_field(bits, cursor, 0U, 1);  // Reserved, default value 0
    // Table 18.17, the 29 bit D-MLE-SYNC carried as the TM-SDU.
    write_field(bits, cursor, pdu.mobile_country_code, 10);
    write_field(bits, cursor, pdu.mobile_network_code, 14);
    write_field(bits, cursor, pdu.neighbour_cell_broadcast, 2);
    write_field(bits, cursor, pdu.cell_load, 2);
    write_field(bits, cursor, pdu.late_entry_supported ? 1U : 0U, 1);
    return bits;
}

TetraSyncPdu tetra_parse_sync_pdu(std::span<const std::uint8_t> bits) {
    TetraSyncPdu pdu;
    std::size_t cursor = 0;
    pdu.system_code = static_cast<std::uint8_t>(read_field(bits, cursor, 4));
    pdu.colour_code = static_cast<std::uint8_t>(read_field(bits, cursor, 6));
    pdu.timeslot = static_cast<std::uint8_t>(read_field(bits, cursor, 2));
    pdu.frame_number = static_cast<std::uint8_t>(read_field(bits, cursor, 5));
    pdu.multiframe_number = static_cast<std::uint8_t>(read_field(bits, cursor, 6));
    pdu.sharing_mode = static_cast<std::uint8_t>(read_field(bits, cursor, 2));
    pdu.reserved_frames = static_cast<std::uint8_t>(read_field(bits, cursor, 3));
    pdu.uplane_dtx_allowed = read_field(bits, cursor, 1) != 0U;
    pdu.frame18_extension = read_field(bits, cursor, 1) != 0U;
    (void)read_field(bits, cursor, 1);  // Reserved
    pdu.mobile_country_code = static_cast<std::uint16_t>(read_field(bits, cursor, 10));
    pdu.mobile_network_code = static_cast<std::uint16_t>(read_field(bits, cursor, 14));
    pdu.neighbour_cell_broadcast = static_cast<std::uint8_t>(read_field(bits, cursor, 2));
    pdu.cell_load = static_cast<std::uint8_t>(read_field(bits, cursor, 2));
    pdu.late_entry_supported = read_field(bits, cursor, 1) != 0U;
    return pdu;
}

// ---------------------------------------------------------------------------
// The BSCH codec
// ---------------------------------------------------------------------------

Expected<std::vector<std::uint8_t>> tetra_bsch_encode(std::span<const std::uint8_t> information) {
    if (information.size() != kTetraBschInformationBits) {
        return fail(std::format(
            "the TETRA BSCH carries {} type-1 bits per clause 8.3.1.2; got {}",
            kTetraBschInformationBits, information.size()));
    }

    // Clause 8.3.1.2: a (76,60) block code, which is the clause 8.2.3.3
    // (K1+16, K1) code with K1 = 60.
    std::vector<std::uint8_t> type2;
    type2.reserve(kTetraBschType2Bits);
    type2.insert(type2.end(), information.begin(), information.end());
    const std::vector<std::uint8_t> parity = tetra_block_code_parity(information);
    type2.insert(type2.end(), parity.begin(), parity.end());
    // "Four tail bits, b2(77) through b2(80), all set equal to zero."
    type2.insert(type2.end(), 4, static_cast<std::uint8_t>(0));

    auto mother = convolutional_encode(tetra_mother_code(), type2);
    if (!mother) {
        return std::unexpected(with_context(mother.error(), "TETRA BSCH mother code"));
    }

    const std::vector<std::uint32_t> puncture = tetra_rate_two_thirds_map(kTetraBschType3Bits);
    std::vector<std::uint8_t> type3(kTetraBschType3Bits, 0);
    for (std::size_t j = 0; j < kTetraBschType3Bits; ++j) {
        const std::uint32_t index = puncture[j];
        if (index >= mother->size()) {
            return fail(std::format(
                "the clause 8.2.3.1.3 puncturing wants mother bit {} of {}", index,
                mother->size()));
        }
        type3[j] = (*mother)[index];
    }

    auto interleave = tetra_block_interleave_map(kTetraBschType3Bits, kTetraBschInterleaveStep);
    if (!interleave) {
        return std::unexpected(with_context(interleave.error(), "TETRA BSCH interleaving"));
    }
    std::vector<std::uint8_t> type4(kTetraBschType3Bits, 0);
    for (std::size_t position = 0; position < kTetraBschType3Bits; ++position) {
        type4[position] = type3[(*interleave)[position]];
    }

    // Clause 8.2.5.2: for the scrambling of BSCH all thirty bits of the
    // extended colour code are zero.
    const std::vector<std::uint8_t> zero_colour_code(30, 0);
    auto sequence = tetra_scrambling_sequence(zero_colour_code, kTetraBschType3Bits);
    if (!sequence) {
        return std::unexpected(with_context(sequence.error(), "TETRA BSCH scrambling"));
    }
    for (std::size_t i = 0; i < kTetraBschType3Bits; ++i) {
        type4[i] = static_cast<std::uint8_t>((type4[i] ^ (*sequence)[i]) & 1U);
    }
    return type4;
}

Expected<TetraBschDecode> tetra_bsch_decode(std::span<const float> soft) {
    if (soft.size() != kTetraBschType3Bits) {
        return fail(std::format("the TETRA synchronisation block is {} bits; got {}",
                                kTetraBschType3Bits, soft.size()));
    }

    const std::vector<std::uint8_t> zero_colour_code(30, 0);
    auto sequence = tetra_scrambling_sequence(zero_colour_code, kTetraBschType3Bits);
    if (!sequence) {
        return std::unexpected(with_context(sequence.error(), "TETRA BSCH descrambling"));
    }

    std::vector<float> descrambled(kTetraBschType3Bits, 0.0F);
    for (std::size_t i = 0; i < kTetraBschType3Bits; ++i) {
        descrambled[i] = (*sequence)[i] ? -soft[i] : soft[i];
    }

    auto interleave = tetra_block_interleave_map(kTetraBschType3Bits, kTetraBschInterleaveStep);
    if (!interleave) {
        return std::unexpected(with_context(interleave.error(), "TETRA BSCH deinterleaving"));
    }
    std::vector<float> type3(kTetraBschType3Bits, 0.0F);
    for (std::size_t position = 0; position < kTetraBschType3Bits; ++position) {
        type3[(*interleave)[position]] = descrambled[position];
    }

    // Depuncture: the mother code emits four bits per input bit and the rate
    // 2/3 code keeps three of every eight. The positions the puncturing
    // discarded carry no information, so they enter the Viterbi decoder as
    // zero, which is the soft value meaning "no opinion".
    const std::vector<std::uint32_t> puncture = tetra_rate_two_thirds_map(kTetraBschType3Bits);
    const std::size_t mother_bits = kTetraBschType2Bits * tetra_mother_code().generators.size();
    std::vector<float> mother(mother_bits, 0.0F);
    for (std::size_t j = 0; j < kTetraBschType3Bits; ++j) {
        const std::uint32_t index = puncture[j];
        if (index < mother_bits) {
            mother[index] = type3[j];
        }
    }

    auto decoded = viterbi_decode(tetra_mother_code(), mother, true);
    if (!decoded) {
        return std::unexpected(with_context(decoded.error(), "TETRA BSCH Viterbi decode"));
    }
    if (decoded->size() < kTetraBschInformationBits + 16) {
        return fail(std::format("the TETRA BSCH decode produced {} bits, short of {}",
                                decoded->size(), kTetraBschInformationBits + 16));
    }

    TetraBschDecode out;
    std::copy_n(decoded->begin(), kTetraBschInformationBits, out.information.begin());
    out.block_code_verified =
        tetra_block_code_verify(std::span(*decoded).subspan(0, kTetraBschInformationBits + 16));
    return out;
}

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

Expected<Tetra> Tetra::create(const TetraConfig& config) {
    if (config.rate <= 0) {
        return fail(std::format("Tetra needs a positive sample rate; got {}", config.rate));
    }
    const double samples_per_symbol = static_cast<double>(config.rate) / kTetraSymbolRate;
    if (samples_per_symbol < 2.0) {
        return fail(std::format(
            "TETRA pi/4-DQPSK is 18000 symbols per second (EN 300 392-2 clause 5.3), so a "
            "rate of {} Hz gives {:.3f} samples per symbol and the timing recovery has no "
            "midpoint to look at. 36000 Hz is the floor and 72000 is the usual choice",
            config.rate, samples_per_symbol));
    }
    if (config.sync_threshold <= 0.0 || config.sync_threshold > 1.0) {
        return fail(
            std::format("Tetra needs a sync threshold in (0, 1]; got {}", config.sync_threshold));
    }

    auto taps = design_rrc(config.rate, kTetraSymbolRate, kTetraRollOff, config.filter_taps);
    if (!taps) {
        return std::unexpected(with_context(taps.error(),
                                            "designing the clause 5.5 root raised cosine filter"));
    }

    SymbolSyncConfig sync_config;
    sync_config.rate = config.rate;
    sync_config.symbol_rate = kTetraSymbolRate;
    auto sync = SymbolSync::create(sync_config);
    if (!sync) {
        return std::unexpected(with_context(sync.error(), "creating the TETRA symbol timing"));
    }

    auto filter = ComplexFir::create(std::move(*taps));
    if (!filter) {
        return std::unexpected(with_context(filter.error(), "building the TETRA matched filter"));
    }

    Tetra decoder;
    decoder.config_ = config;
    decoder.filter_ = std::move(*filter);
    decoder.sync_ = std::move(*sync);
    return decoder;
}

void Tetra::reset() {
    filter_.reset();
    sync_.reset();
    soft_bits_.clear();
    consumed_ = 0;
    trimmed_ = 0;
    previous_symbol_ = Complex32{1.0F, 0.0F};
    have_previous_ = false;
}

Status Tetra::process(ConstComplexSpan samples, std::vector<TetraBurst>& out) {
    if (samples.empty()) {
        return {};
    }

    // The filter carries its history across calls, so the symbols a stream
    // produces do not depend on how it was blocked. Until 2026-09-23 it
    // restarted from zeros at every call, which in 1024-sample calls lost 24
    // of 36 bursts that the whole capture decoded;
    // tests/decode/test_tetra_blocking.cpp has the figures.
    filtered_.resize(samples.size());
    if (auto status = filter_.process(samples, filtered_); !status) {
        return std::unexpected(with_context(status.error(), "TETRA matched filtering"));
    }

    recovered_.clear();
    sync_.process(filtered_, recovered_);

    for (const RecoveredSymbol& symbol : recovered_) {
        if (!have_previous_) {
            previous_symbol_ = symbol.value;
            have_previous_ = true;
            continue;
        }

        // Clause 5.4, equation 5.1: S(k) = S(k-1) * exp(j*D(k)), so the
        // transition is the argument of the product with the conjugate.
        const Complex32 product = symbol.value * std::conj(previous_symbol_);
        previous_symbol_ = symbol.value;
        const double phase = std::atan2(static_cast<double>(product.imag()),
                                        static_cast<double>(product.real()));

        // Table 5.1 inverted. B(2k-1) is one for the two negative transitions,
        // and B(2k) is one for the two whose magnitude is 3*pi/4. The soft
        // values follow core/decode/dv_codes.h's convention, where positive
        // leans towards zero.
        soft_bits_.push_back(static_cast<float>(phase));
        soft_bits_.push_back(static_cast<float>(kPi / 2.0 - std::abs(phase)));
    }

    // The training sequence as soft values in the same convention.
    std::array<float, kTetraSyncTrainingSequence.size()> pattern{};
    for (std::size_t i = 0; i < kTetraSyncTrainingSequence.size(); ++i) {
        pattern[i] = kTetraSyncTrainingSequence[i] ? -1.0F : 1.0F;
    }

    // The training sequence sits at bit offset kTetraSyncBurstTraining.offset
    // inside the burst, so a hit there puts the burst's first bit that far
    // back.
    const std::size_t lead = kTetraSyncBurstTraining.offset;
    std::size_t position = consumed_;
    while (position + kTetraSyncTrainingSequence.size() <= soft_bits_.size()) {
        if (position < lead) {
            ++position;
            continue;
        }
        const std::size_t burst_start = position - lead;
        if (burst_start + kTetraBurstBits > soft_bits_.size()) {
            break;
        }

        const double score = correlation_at(soft_bits_, pattern, position);
        if (score < config_.sync_threshold) {
            ++position;
            continue;
        }

        TetraBurst burst;
        burst.first_symbol = (trimmed_ + burst_start) / 2;
        burst.sync_score = score;
        for (std::size_t i = 0; i < kTetraBurstBits; ++i) {
            burst.bits[i] = static_cast<std::uint8_t>(soft_bits_[burst_start + i] < 0.0F ? 1 : 0);
        }

        std::vector<float> block1(kTetraBschType3Bits, 0.0F);
        for (std::size_t i = 0; i < kTetraBschType3Bits; ++i) {
            block1[i] = soft_bits_[burst_start + kTetraSyncBurstBlock1.offset + i];
        }
        if (auto decoded = tetra_bsch_decode(block1)) {
            burst.block_code_verified = decoded->block_code_verified;
            if (decoded->block_code_verified) {
                burst.sync = tetra_parse_sync_pdu(decoded->information);
            }
        }

        out.push_back(std::move(burst));
        position = burst_start + kTetraBurstBits;
    }

    consumed_ = position;

    const std::size_t keep = kTetraBurstBits * 2;
    if (consumed_ > keep) {
        const std::size_t drop = consumed_ - keep;
        soft_bits_.erase(soft_bits_.begin(),
                         soft_bits_.begin() + static_cast<std::ptrdiff_t>(drop));
        consumed_ -= drop;
        trimmed_ += drop;
    }
    return {};
}

}  // namespace revenant::decode
