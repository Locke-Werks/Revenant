// PSK31 and PSK63: receiver audio to text.
//
// SPECIFICATION
//
// Peter Martinez G3PLX, "PSK31: A New Radio-Teletype Mode", QEX July/August
// 1999 pp 3-9, the RadCom December 1998 and January 1999 article reprinted
// with its February 1999 updates. Cited below as "QEX" with a page number.
// The alphabet is core/decode/varicode.h, from the same article's Table 1.
//
// PSK63 is not in that article. It is the same signal at twice the symbol
// rate: Wavecom's decoder documentation ("PSK-31, PSK-63, PSK-125, PSK-250",
// wavecom.ch, fetched 2026-09-22) lists the family's symbol rates as 31.25,
// 62.5, 125 and 250 Bd with the same Varicode alphabet and DBPSK or DQPSK
// modulation, and that is the whole of what this file assumes about it.
//
// WHAT THE ARTICLE SPECIFIES AND WHERE
//
//   - 31.25 bits per second, page 5: "I have chosen 31.25, because it can be
//     easily derived from the 8-kHz sample-rate".
//   - Binary: 0 is a polarity reversal and 1 a steady carrier, page 6.
//   - Idle is continuous zeros, so continuous reversals; the end of a
//     transmission is a tail of unmodulated carrier, page 6.
//   - QPSK: one of four 90 degree phase shifts per symbol, from a run of five
//     data bits through the table in the sidebar "The Convolutional Code",
//     page 9, where "1" means advance by 90 degrees, "2" a polarity reversal
//     and "3" retard by 90, and an advancing phase is higher in frequency.
//   - Symbol timing from the amplitude modulation the reversals put on the
//     carrier, page 6: "it always contains a pure-tone component at the baud
//     rate".
//   - Viterbi decoding with a decision delay of 20 bits, page 8: "I chose a
//     decoder delay of four times the time spread, or 20 bits."
//
// WHAT IT DOES NOT SPECIFY, AND WHAT THIS FILE CHOSE
//
// The pulse shape is described rather than given: continuous reversals
// "filtered to the minimum bandwidth" are "equivalent to a double-sideband
// suppressed-carrier emission, that is, to two tones either side of a
// suppressed carrier", page 5, and arrl.org/psk31-spec says the output "is
// cosine-filtered". Two tones at plus and minus half the symbol rate are a
// carrier whose envelope is |cos(pi*t/T)|, so a reversal is a half cosine from
// one polarity to the other across one symbol period and a steady symbol is a
// steady carrier. core/dsp/synth/psk31_mod.h generates exactly that and this
// receiver's matched filter is that pulse. The QPSK transitions, which the
// article does not draw, use the same raised-cosine crossfade between
// successive symbol phasors, which reduces to the binary case when the two
// are opposite.
//
// The receive filter, the AFC and the squelch-free start are engineering
// choices, labelled where they are made.
//
// THE CONVOLUTIONAL CODE IS AN ORDINARY ONE, AND THE TABLE SAYS WHICH
//
// Page 9 prints 32 phase shifts and says "the logic behind this table is
// beyond the scope of this article". The logic is recoverable from the table:
// every entry is reproduced by a rate 1/2, constraint length 5 feedforward code
// with generators
//
//     G_I(D) = D + D^2 + D^3        G_Q(D) = 1 + D^3 + D^4
//
// where D^0 is the newest bit, provided the two output bits are read as the
// signs of the in-phase and quadrature parts of the phase shift advanced by a
// further 45 degrees, 1 meaning positive. A search of all 1024 generator pairs
// finds six that reproduce the table, all of them pairs drawn from
// {D+D^2+D^3, 1+D+D^2+D^4, 1+D^3+D^4}, and up to swapping the two axes this
// is the only one of the six whose outputs are the in-phase and quadrature
// signs. That is what lets core/decode/dv_codes.h's soft-decision Viterbi
// decoder serve unchanged, with one soft value per axis.
// tests/decode/test_psk31.cpp regenerates all 32 printed entries from these
// generators, and the article's own worked example besides.
//
// CLEAN ROOM
//
// No PSK31 implementation was read. Every constant cites the article or the
// Wavecom page above.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in
// tests/decode/test_psk31.cpp against core/dsp/synth/psk31_mod.h stands in
// its place, as for every decoder in core/decode.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/tone_frontend.h"
#include "core/decode/varicode.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

enum class Psk31Mode {
    Bpsk31,
    Bpsk63,
    Qpsk31,
};

// QEX page 5.
inline constexpr double kPsk31SymbolRate = 31.25;

// The Wavecom documentation cited above: the family's second rate.
inline constexpr double kPsk63SymbolRate = 62.5;

[[nodiscard]] double psk31_symbol_rate(Psk31Mode mode);

// The four phase shifts of QEX page 9, in quarter turns: 0 none, 1 advance by
// 90 degrees, 2 polarity reversal, 3 retard by 90 degrees.
inline constexpr std::uint8_t kPskShiftNone = 0;
inline constexpr std::uint8_t kPskShiftAdvance = 1;
inline constexpr std::uint8_t kPskShiftReverse = 2;
inline constexpr std::uint8_t kPskShiftRetard = 3;

// QEX page 9, sidebar "The Convolutional Code": the phase shift for each run
// of five Varicode bits, indexed by the run read as a binary number with the
// left, earliest, bit most significant. So entry 0b00001 is the run 00001,
// whose newest bit is a 1.
//
// The sidebar prints the row for 10101 as "1010 3", one digit short; its
// column position and the arrl.org/psk31-spec copy, which gives 10101 as
// -90 degrees, both put it here as 3.
inline constexpr std::array<std::uint8_t, 32> kQpsk31PhaseTable = {
    2, 1, 3, 0, 3, 0, 2, 1,  // 00000 .. 00111
    0, 3, 1, 2, 1, 2, 0, 3,  // 01000 .. 01111
    1, 2, 0, 3, 0, 3, 1, 2,  // 10000 .. 10111
    3, 0, 2, 1, 2, 1, 3, 0,  // 11000 .. 11111
};

// The generator form of that table, as derived in the header comment, with the
// coefficient of D^0 in the least significant bit as core/decode/dv_codes.h
// writes every generator. Output 0 is the in-phase sign and output 1 the
// quadrature sign of the shift advanced by 45 degrees, 1 meaning positive.
inline constexpr std::uint32_t kQpsk31Generators[2] = {0b01110U, 0b11001U};
inline constexpr std::uint32_t kQpsk31Memory = 4;

// QEX page 8.
inline constexpr std::size_t kQpsk31DecisionDelayBits = 20;

// The phase shift for one encoder register state, from the generators, so the
// transmitter and the test can both check it against kQpsk31PhaseTable.
[[nodiscard]] std::uint8_t qpsk31_shift_from_generators(std::uint32_t run_of_five);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct Psk31Config {
    // The receiver's audio rate.
    SampleRate rate = 48000;

    // Where in the audio passband the operator put the signal. The tone does
    // not have to be exactly here: see capture_hz.
    Hertz centre_hz = 1000;

    Psk31Mode mode = Psk31Mode::Bpsk31;

    // AFC capture range, one-sided, in hertz. NOT A SPECIFIED VALUE. Clicking
    // a signal on a waterfall puts it within a few hertz; a transceiver's
    // dial and a sound card's clock add some more. 40 Hz covers a click that
    // is out by more than a whole PSK31 signal width either way. The front
    // end's passband and the acquisition search both follow from it.
    double capture_hz = 40.0;

    // Symbols of signal the coarse frequency estimate is made over before
    // anything is decoded. NOT A SPECIFIED VALUE. 64 symbols is two seconds of
    // PSK31, about as long as a transmission's idle preamble, and long enough
    // that the squared signal's spectral line stands clear of the noise at the
    // signal to noise ratios this decoder still reads.
    std::size_t acquisition_symbols = 64;

    // QPSK depends on the sideband and binary does not, QEX page 8. Set when
    // the audio came from a lower sideband demodulator, which mirrors the
    // spectrum and so turns every phase advance into a retard.
    bool lower_sideband = false;

    std::size_t decision_delay_bits = kQpsk31DecisionDelayBits;
};

// One decoded character, placed in the audio stream.
struct Psk31Character {
    std::uint8_t ascii = 0;
    bool recognised = false;

    // Audio sample index at which the character's first bit was decided.
    SampleIndex first_sample = 0;
};

class Psk31 {
   public:
    [[nodiscard]] static Expected<Psk31> create(const Psk31Config& config);

    // Consumes audio and appends every character completed by it. State
    // carries across calls, and the result does not depend on the blocking.
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<Psk31Character>& out);

    // True once the coarse frequency estimate has been made.
    [[nodiscard]] bool acquired() const { return acquired_; }

    // The tone's measured offset from centre_hz, coarse plus fine, in hertz.
    [[nodiscard]] double frequency_offset_hz() const;

    // How far the carrier's spectral line stood above the mean of the search
    // when it was accepted: core/decode/tone_frontend.h's line_to_mean.
    [[nodiscard]] double acquisition_strength() const { return acquisition_strength_; }

    // The strongest line among the windows turned down before acquisition,
    // which on a quiet channel is the noise's own best effort and is what the
    // acquisition threshold has to clear.
    [[nodiscard]] double strongest_rejected() const { return strongest_rejected_; }

    // Every data bit decided during the last call to process, in order, with
    // the audio sample index of each. Exposed for bit error rate measurement.
    [[nodiscard]] std::span<const std::uint8_t> last_bits() const { return last_bits_; }
    [[nodiscard]] std::span<const SampleIndex> last_bit_samples() const {
        return last_bit_samples_;
    }

    void reset();

   private:
    Psk31() = default;

    void run_symbols(std::span<const Complex32> baseband, std::vector<Psk31Character>& out);
    void decide(Complex32 difference, SampleIndex sample, std::vector<Psk31Character>& out);
    void emit_bit(std::uint8_t bit, SampleIndex sample, std::vector<Psk31Character>& out);
    void run_viterbi(bool flush, std::vector<Psk31Character>& out);

    Psk31Config config_{};
    double symbol_rate_ = 0.0;
    ToneFrontEnd front_{};
    std::vector<float> matched_;
    SymbolSync sync_{};

    // Decimated samples held until the acquisition window is full.
    std::vector<Complex32> acquisition_;
    bool acquired_ = false;
    double coarse_offset_hz_ = 0.0;
    double acquisition_strength_ = 0.0;
    double strongest_rejected_ = 0.0;

    // Decimated index of the next sample into the frequency correction, and
    // the matched filter's history.
    SampleIndex corrected_index_ = 0;
    SampleIndex sync_origin_ = 0;
    std::vector<Complex32> matched_history_;
    std::vector<Complex32> corrected_;
    std::vector<RecoveredSymbol> recovered_;

    Complex32 previous_symbol_{};
    bool have_previous_ = false;

    // Fine AFC: a leaky average of the differential phasor raised to the
    // modulation order, whose angle is the residual rotation per symbol.
    std::complex<double> residual_{0.0, 0.0};
    double level_ = 0.0;

    // QPSK: soft values awaiting the Viterbi decoder, and the last four bits
    // already committed, which fix the encoder state at the window start.
    std::vector<float> pending_soft_;
    std::vector<SampleIndex> pending_samples_;
    std::array<std::uint8_t, 4> committed_tail_{};

    VaricodeDecoder varicode_{};
    std::uint64_t bit_count_ = 0;
    std::vector<VaricodeCharacter> characters_;
    std::vector<SampleIndex> bit_samples_ring_;

    std::vector<std::uint8_t> last_bits_;
    std::vector<SampleIndex> last_bit_samples_;
    std::vector<Complex32> decimated_;
};

}  // namespace revenant::decode
