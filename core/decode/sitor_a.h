// SITOR-A (AMTOR mode A), the ARQ mode of maritime narrow-band direct
// printing, monitored passively: receiver audio carrying both ends of a link
// to the information blocks, the control signals and the text that was
// accepted.
//
// SPECIFICATION
//
// Recommendation ITU-R M.625-4 (03/2012), "Direct-printing telegraph
// equipment employing automatic identification in the maritime mobile
// service", read from the ITU's own publication service
// (itu.int/rec/R-REC-M.625) on 2026-11-02. Clauses used: 1.1 to 1.3 (the
// 7-unit constant ratio code, 100 Bd, F1B or J2B at 170 Hz shift about a
// 1 700 Hz audio centre), Table 1 with its notes 2 and 3 (traffic signals,
// B the higher frequency, bit position 1 first, B = 0 and Y = 1), Table 2's
// mode A column (control signals 1 to 5, idle signals alpha and beta,
// signal repetition), 3.2.3 (the 450 ms cycle), 3.3.1 and 3.3.2 (a block of
// three signals in 210 ms), 3.4.1 (one 70 ms control signal per block),
// 3.5.3 (call blocks carry signal repetition in a fixed place), 3.7.4 to
// 3.7.10 (the alternation of control signals 1 and 2 and what a repetition
// looks like), 3.7.11 (change-over, control signal 3 and the idle-alpha
// block) and 3.7.14 (the end-of-communication block of three idle alphas).
// M.476-5 Annex 1 is the earlier text of the same system and agrees on every
// value used here; M.625-4 is cited because it is the one in force.
//
// HOW A MONITOR READS IT
//
// A monitor is neither station. It hears a 210 ms burst of three signals
// and, in the 240 ms pause after it, a 70 ms burst of one, every 450 ms; the
// two come from different transmitters over different paths, so each is
// sliced on its own timing. Acquisition is two blocks of three valid signals
// one cycle apart; after that each block is looked for at its expected
// place, refined by a fraction of a bit, and the control signal anywhere in
// the gap after it.
//
// Clauses 3.7.5 and 3.7.6 say what the IRS asks for: a control signal that
// differs from the one before it accepts the block between them, and the
// same control signal again asks for the block to be sent again. That is
// exactly what a monitor needs to print each accepted block once. Where the
// monitor lost one of the two control signals it falls back to printing a
// block unless it is identical to the block before it, and says so in the
// block's `accepted_by` field.
//
// DEVIATIONS
//
// None from the Recommendation, which specifies the stations rather than a
// monitor. Engineering choices, not specified: the thresholds on how far
// above noise a block must stand to be acquired, and the four cycles in a
// row with no block after which the monitor drops the link and searches
// again, which is also how it follows a change-over, since the block then
// comes from the other station at a different place in the cycle.
//
// WHERE THIS STOPS
//
// Call blocks are reported with their raw signals, but the identification
// signals of Table 3a and the check-sum derivation of clause 2.5 are not
// interpreted. Selective calling of clause 3.5.3 needs nothing more for a
// monitor to follow the traffic that comes after it.
//
// CLEAN ROOM
//
// No SITOR, AMTOR or NAVTEX implementation was read.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/decode/fsk.h"
#include "core/decode/sitor_b.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// Clause 3.2.3, 3.3.2 and 3.4.1, in 100 Bd units.
inline constexpr std::size_t kSitorACycleUnits = 45;
inline constexpr std::size_t kSitorABlockSignals = 3;
inline constexpr std::size_t kSitorABlockUnits = kSitorABlockSignals * kSitorSignalUnits;  // 21

// Table 2, mode A column, held as sitor_b.h holds a signal: bit position 1 in
// bit 0, Y = 1.
inline constexpr std::uint8_t kSitorACs1 = 0b0011010;   // BYBYYBB
inline constexpr std::uint8_t kSitorACs2 = 0b0010101;   // YBYBYBB
inline constexpr std::uint8_t kSitorACs3 = 0b0100110;   // BYYBBYB
inline constexpr std::uint8_t kSitorACs4 = 0b1001010;   // BYBYBBY
inline constexpr std::uint8_t kSitorACs5 = 0b0010110;   // BYYBYBB
inline constexpr std::uint8_t kSitorAIdleBeta = 0b1001100;   // BBYYBBY
inline constexpr std::uint8_t kSitorAIdleAlpha = 0b1110000;  // BBBBYYY
inline constexpr std::uint8_t kSitorARq = 0b0011001;         // YBBYYBB

// A single signal heard in the pause, as Table 2's mode A column names it.
enum class SitorAControl : std::uint8_t { None, Cs1, Cs2, Cs3, Cs4, Cs5, Rq, Other };

[[nodiscard]] SitorAControl sitor_a_control(std::uint8_t signal);

struct SitorABlock {
    enum class Kind : std::uint8_t {
        // Three signals, at least one of them traffic.
        Information,
        // Clause 3.7.10: three signal repetitions.
        Repetition,
        // Clause 3.5.3: signal repetition in place 2 or 3, a call block.
        Call,
        // Clause 3.7.11.4 and 3.7.14.1: three idle alphas.
        IdleAlpha,
        // Anything else of valid signals, idle betas filling a block.
        Idle,
        // At least one signal broke the constant ratio.
        Mutilated,
    };
    Kind kind = Kind::Mutilated;
    // Raw 7-unit signals, bit position 1 in bit 0.
    std::array<std::uint8_t, kSitorABlockSignals> signals{};
    // Audio sample index of the block's first unit.
    SampleIndex position = 0;
    // The control signal heard in the pause before this block and after it.
    SitorAControl control_before = SitorAControl::None;
    SitorAControl control_after = SitorAControl::None;
    // Whether this block's traffic was printed, and on what evidence.
    enum class Accepted : std::uint8_t { No, ByControlSignals, ByContent };
    Accepted accepted_by = Accepted::No;
};

struct SitorAConfig {
    SampleRate rate = 48000;
    Hertz centre_hz = kSitorCentreHz;
    Hertz shift_hz = kSitorShiftHz;
    // Table 1 note 2 on an upper sideband receiver. Acquisition tries the
    // other polarity as well, so this only orders the search.
    bool upper_sideband = true;
    char32_t error_glyph = char32_t{0xFFFD};
};

struct SitorAStats {
    std::uint64_t acquisitions = 0;
    std::uint64_t blocks = 0;
    std::uint64_t mutilated_blocks = 0;
    std::uint64_t control_signals = 0;
    std::uint64_t repeats_suppressed = 0;
    std::uint64_t losses = 0;
};

struct SitorAOutput {
    std::vector<SitorABlock> blocks;
    // The accepted traffic, decided per sitor_b.h's SitorCharacter.
    std::vector<SitorCharacter> characters;
};

class SitorAMonitor {
public:
    [[nodiscard]] static Expected<SitorAMonitor> create(const SitorAConfig& config);

    // Consumes audio. A block is appended once the pause after it has been
    // searched for its control signal, so a block is complete when it comes
    // out. State carries across calls.
    void process(ConstRealSpan audio, SitorAOutput& out);

    [[nodiscard]] const SitorAStats& stats() const { return stats_; }
    [[nodiscard]] bool locked() const { return locked_; }
    void reset();

private:
    SitorAMonitor() = default;

    // Mean tone-energy difference, Y less B, and mean tone-energy sum over
    // the central half of the unit starting at `at`, a fractional stream
    // sample index.
    [[nodiscard]] double unit_difference(double at) const;
    [[nodiscard]] double unit_sum(double at) const;
    struct Sliced {
        std::uint8_t signal = 0;
        // Mean magnitude of the unit differences.
        double strength = 0.0;
        bool valid = false;
    };
    [[nodiscard]] Sliced slice(double at, bool invert) const;
    [[nodiscard]] double block_strength(double at, bool invert, bool* valid) const;
    // The noise floor as the cycle before `at` shows it: a low percentile of
    // its units' tone-energy sums, which lands in the pauses neither station
    // fills.
    [[nodiscard]] double floor_before(double at) const;
    // The strongest valid single signal between two stream positions.
    [[nodiscard]] SitorAControl find_control(double first, double last, double floor) const;

    void search();
    void track();
    void emit(SitorABlock block, SitorAOutput& out);

    SitorAConfig config_{};
    double samples_per_unit_ = 0.0;
    double samples_per_cycle_ = 0.0;
    std::size_t window_ = 0;
    double y_step_ = 0.0;
    double b_step_ = 0.0;

    // Sliding one-unit correlators at the two tones.
    std::vector<std::complex<double>> y_ring_;
    std::vector<std::complex<double>> b_ring_;
    std::complex<double> y_sum_{};
    std::complex<double> b_sum_{};
    std::size_t ring_pos_ = 0;
    double phase_ = 0.0;
    double b_phase_ = 0.0;

    // Prefix sums from stream sample prefix_base_, of the difference and the
    // sum of the two tones' energies.
    std::vector<double> difference_;
    std::vector<double> sum_;
    std::uint64_t prefix_base_ = 0;
    std::uint64_t total_ = 0;

    double search_from_ = 0.0;
    bool locked_ = false;
    bool invert_ = false;
    double next_block_ = 0.0;
    int missed_ = 0;
    SitorAControl last_control_ = SitorAControl::None;
    std::optional<std::array<std::uint8_t, kSitorABlockSignals>> last_printed_;
    bool figures_ = false;
    std::vector<SitorABlock> ready_;

    SitorAStats stats_{};
};

}  // namespace revenant::decode
