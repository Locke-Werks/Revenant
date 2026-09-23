// The physical layer the digital voice modes share: matched filtering, symbol
// timing recovery, and correlation against a known symbol pattern.
//
// WHY THIS FILE EXISTS SEPARATELY FROM THE THREE MODES
//
// P25 Phase 1, D-STAR and TETRA specify three different modulations and three
// different framings, and underneath all three the receiver does the same four
// things: filter to the symbol bandwidth, find where the symbol instants are,
// slice, and correlate for the pattern the standard says marks a frame. Only
// the filter and the slicer differ, and they differ by a handful of constants
// each. Writing that machinery once means a timing bug is found once, and it
// means the three mode files contain the standards and nothing else, which is
// what makes them checkable against the standards.
//
// WHAT IS NOT HERE
//
// No constant in this file comes from any of the three standards. Everything
// here is textbook receiver structure, and the numbers a caller supplies are
// the ones with citations, in the mode file that supplies them. That split is
// deliberate: a reader auditing provenance can skip this file entirely, and a
// reader debugging a lock failure does not have to read three standards.
//
// ONE CALL, OR A STREAM
//
// fm_discriminate, filter_real and filter_complex take each call as a whole
// signal: the discriminator starts against 1+0i and the filters from zeros.
// That is right for a capture handed over in one piece, which is how the
// transmitters in core/dsp/synth use them. A decoder the engine feeds one
// block at a time wants FmDiscriminator and RealFir instead, which carry the
// previous sample and the filter history across calls, and SymbolSync, which
// anchors its windows to the stream. Through those three a stream gives the
// same symbols however it is split, bit for bit. P25 learned this the hard
// way on 2026-09-22: in 1024-sample blocks, calling the one-call forms per
// block, with each block's mean taken as the carrier offset on top, cost it
// one of the six headers it decoded from the capture whole, and
// put 38 NID bits and 28 Golay words of correction on the rest where the
// whole capture needed none, all of it gone once the state was carried.
// tests/decode/test_p25p1_blocking.cpp has the breakdown. D-STAR and TETRA
// still call the one-call forms per block.
//
// A carrier offset through a frequency discriminator is a constant added to
// every symbol. centred_correlation_at finds a sync word regardless of it and
// fit_levels measures it, and the deviation, from the sync word's own known
// symbols, so a burst receiver needs no running estimate of either and has
// nothing to settle.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// The project's rule is that every GPU kernel has a scalar twin and the two
// agree to zero ULP. These are host functions with no kernel behind them, so
// that rule does not reach them, exactly as core/decode/rds_bits.h says of the
// RDS physical layer. What stands in its place is the round trip against the
// synthetic transmitters in core/dsp/synth/dv_mod.h.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ComplexSpan;
using dsp::Complex32;
using dsp::ConstComplexSpan;
using dsp::ConstRealSpan;
using dsp::RealSpan;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Frequency discrimination
// ---------------------------------------------------------------------------

// Instantaneous frequency of a complex baseband signal, in hertz, one output
// per input sample.
//
// out[n] = arg(in[n] * conj(in[n-1])) * rate / (2*pi), with out[0] taken
// against an assumed previous sample of 1+0i so that the first output is not a
// discontinuity a timing loop has to ride out.
//
// This is what a constant-envelope receiver's discriminator produces, and it
// is the input both the C4FM and the GMSK paths want: TIA-102.BAAA-A clause
// 9.6 specifies the P25 receiver as a frequency detector followed by a filter
// and a clock recovery device, and the JARL standard Ap1.5 states D-STAR's
// bit-to-deviation polarity, which is a statement about the discriminator
// output.
//
// Returns an error when the spans are different lengths or the rate is not
// positive.
[[nodiscard]] Status fm_discriminate(ConstComplexSpan in, RealSpan out, SampleRate rate);

// The same discriminator for a stream that arrives in pieces.
//
// fm_discriminate takes every call as the start of a signal, so a decoder
// calling it once per block puts a false first sample at every block
// boundary: the argument of the block's first sample against 1+0i, which for
// a carrier at an arbitrary phase is anywhere in +/- rate/2. At 48000 S/s that
// is up to 24 kHz against the 600 Hz a P25 symbol unit deviates. This one
// carries the previous sample across calls, so the first output of a call is
// the one the whole stream would have produced there, and its output for a
// stream is the same however the stream was split.
class FmDiscriminator {
   public:
    // Default constructible so a decoder can hold one by value and assign into
    // it from its own create(). A default-constructed one has a zero scale and
    // outputs zeros.
    FmDiscriminator() = default;

    [[nodiscard]] static Expected<FmDiscriminator> create(SampleRate rate);

    // Spans must be the same length.
    [[nodiscard]] Status process(ConstComplexSpan in, RealSpan out);

    // Forgets the previous sample. Between independent captures only.
    void reset();

   private:
    double scale_ = 0.0;
    Complex32 previous_{1.0F, 0.0F};
};

// ---------------------------------------------------------------------------
// Filter design
// ---------------------------------------------------------------------------

// A linear-phase FIR whose magnitude response is sampled from a caller-supplied
// function of frequency in hertz.
//
// The response is sampled on a dense grid, inverse transformed to an impulse
// response, truncated to `taps` and windowed with a Hamming window. `taps` must
// be odd so the result is symmetric about a whole sample and the group delay is
// an integer number of samples, which is what lets a caller line a filtered
// stream back up with the sample index it came from by subtracting (taps-1)/2.
//
// The truncation is why the window is here. Both P25 filters are specified as
// magnitude responses that reach exactly zero at a band edge
// (TIA-102.BAAA-A clauses 9.3 and 9.4), and a rectangular truncation of a
// response like that rings at the edge rather than rolling off.
using ResponseFn = double (*)(double hertz, const void* context);

[[nodiscard]] Expected<std::vector<float>> design_from_response(
    SampleRate rate, std::size_t taps, ResponseFn response, const void* context);

// A root raised cosine pulse, sampled at `rate`, for a symbol rate of
// `symbol_rate` and roll-off `alpha`. Closed form rather than frequency
// sampled, because the RRC has one and the removable singularities are easier
// to get right once than to chase through an inverse transform.
//
// The singularities are at t = 0 and t = +/- T/(4*alpha); both are handled by
// their limits.
[[nodiscard]] Expected<std::vector<float>> design_rrc(SampleRate rate, double symbol_rate,
                                                      double alpha, std::size_t taps);

// A Gaussian pulse of bandwidth-time product `bt`, sampled at `rate`, for a
// symbol rate of `symbol_rate`. This is the premodulation filter of GMSK: the
// impulse response is a Gaussian whose 3 dB bandwidth is bt*symbol_rate,
// convolved with a rectangle one symbol wide, and normalised so the response
// integrates to one.
[[nodiscard]] Expected<std::vector<float>> design_gaussian(SampleRate rate, double symbol_rate,
                                                           double bt, std::size_t taps);

// out[n] = sum_k taps[k] * in[n-k], with in treated as zero before its start.
// Spans must be the same length.
[[nodiscard]] Status filter_real(ConstRealSpan in, ConstRealSpan taps, RealSpan out);
[[nodiscard]] Status filter_complex(ConstComplexSpan in, ConstRealSpan taps, ComplexSpan out);

// filter_real for a stream that arrives in pieces.
//
// filter_real treats the input as zero before the start of each call, which
// is right for a whole capture and wrong for a block of one: every call then
// begins with the filter ramping up from silence, and for the first taps-1
// outputs only part of the impulse response sees signal. For the P25 receive
// filter, 121 taps at 48000 S/s, that is 12 symbols at every block boundary.
// This one keeps the last taps-1 inputs across calls, so its output for a
// stream is the same however the stream was split, and sample for sample the
// same as filter_real over the stream in one call: the sum runs over the taps
// in the same order, and the zeros before the stream's start contribute
// nothing to it.
class RealFir {
   public:
    RealFir() = default;

    [[nodiscard]] static Expected<RealFir> create(std::vector<float> taps);

    // Spans must be the same length.
    [[nodiscard]] Status process(ConstRealSpan in, RealSpan out);

    // Forgets the history, as though the next call were the start of the
    // stream. Between independent captures only.
    void reset();

   private:
    std::vector<float> taps_;

    // The last taps-1 inputs, oldest first, then the current call's input
    // behind them while it is being filtered.
    std::vector<float> history_;
};

// filter_complex for a stream that arrives in pieces, for the reason RealFir
// gives. TETRA's matched filter is the case: 65 taps at 72000 S/s is 16
// symbols ramping up from silence at every block boundary when the history is
// dropped. Sample for sample the same as filter_complex over the stream in
// one call, by the same argument as RealFir.
class ComplexFir {
   public:
    ComplexFir() = default;

    [[nodiscard]] static Expected<ComplexFir> create(std::vector<float> taps);

    // Spans must be the same length.
    [[nodiscard]] Status process(ConstComplexSpan in, ComplexSpan out);

    // Forgets the history. Between independent captures only.
    void reset();

   private:
    std::vector<float> taps_;
    std::vector<Complex32> history_;
};

// ---------------------------------------------------------------------------
// Symbol timing recovery
// ---------------------------------------------------------------------------

struct SymbolSyncConfig {
    SampleRate rate = 0;
    double symbol_rate = 0.0;

    // Symbols per timing estimate. The estimator averages the symbol-rate
    // spectral line over this many symbols, so a longer window is a quieter
    // estimate and a slower response to a changing clock.
    //
    // 32 is the default because it is long enough that the estimate is steady
    // at the signal to noise where these modes still decode, and short enough
    // that a TETRA burst of 255 symbols gets several independent estimates
    // rather than one. A caller with a shorter burst than this gets no
    // symbols at all, which is why the burst modes here are all longer.
    std::size_t window_symbols = 32;
};

// One recovered symbol, at the instant the loop decided was the symbol centre.
struct RecoveredSymbol {
    Complex32 value{};

    // Fractional sample position in the input stream, so a caller can map a
    // symbol back to where in the capture it came from.
    double position = 0.0;
};

// Feedforward symbol timing recovery, by the square-law estimator, over a
// cubic Farrow interpolator.
//
// WHY FEEDFORWARD AND NOT A TRACKING LOOP
//
// A Gardner loop was written here first and did not work, for a reason worth
// recording because it is not obvious and it looks like several other faults.
// Gardner's error term is the midpoint sample times the difference of the two
// symbols either side of it. On a two-level constellation that is a clean
// S-curve. On C4FM's four levels the same expression carries a data-dependent
// term proportional to the difference of the squares of the two symbols,
// which is zero on average and enormous sample by sample, so the loop spends
// its whole budget riding the data and never converges on the timing. What
// that looks like from the outside is a receiver that finds the frame sync,
// because the sync word uses only the outer two levels, and then decodes the
// payload as noise.
//
// The square-law estimator has no such term. The squared magnitude of a
// linearly modulated signal carries a spectral line at the symbol rate whose
// phase is the timing offset, for any constellation, so one complex
// correlation against that line over a window of symbols gives the offset
// directly with no loop, no acquisition and no gain to tune. It is the same
// estimator for the two real FM paths and for TETRA's complex one, which is
// what lets this class stay mode-agnostic.
//
// WHAT IT COSTS
//
// An estimate per window rather than per symbol, so a clock error is tracked
// at the window rate. At the default window that is about a hundredth of the
// symbol rate, which is far faster than any crystal moves.
//
// BLOCK INVARIANCE
//
// Windows are anchored to absolute sample positions from the start of the
// stream, so the symbols this returns do not depend on how the caller chose
// to block the input. That is the same property core/dsp/synth/modulators.h
// requires of every generator and for the same reason: a result that depends
// on the blocking cannot be a reference for anything.
class SymbolSync {
   public:
    // Default constructible so a decoder can hold one by value and assign
    // into it from its own create(). A default-constructed one has a zero
    // symbol period and emits nothing, which is inert rather than wrong: the
    // only way to reach one is to hold a SymbolSync that no create() ever
    // filled in, and every decoder in this tree fills it in before its own
    // create() returns.
    SymbolSync() = default;

    [[nodiscard]] static Expected<SymbolSync> create(const SymbolSyncConfig& config);

    // Consumes a block and appends every symbol whose instant fell inside a
    // window that is now complete. State carries across calls, so a caller
    // may feed the stream in any blocking and get the same symbols out.
    void process(ConstComplexSpan in, std::vector<RecoveredSymbol>& out);

    // Discards the recovery state without discarding the configuration. Used
    // between independent captures, never mid-stream.
    void reset();

   private:
    double samples_per_symbol_ = 0.0;
    std::size_t window_symbols_ = 0;
    std::size_t window_samples_ = 0;

    // Samples not yet covered by a completed window, plus the margin the
    // interpolator needs behind the first symbol of the next one.
    std::vector<Complex32> buffer_;
    std::uint64_t buffer_start_ = 0;

    // Absolute sample position of the first sample of the next window, and of
    // the next symbol to emit. The second is fractional; the first is not.
    std::uint64_t next_window_ = 0;
    double next_symbol_ = 0.0;
    bool have_phase_ = false;
};

// ---------------------------------------------------------------------------
// Sync pattern correlation
// ---------------------------------------------------------------------------

// The result of sliding a known pattern along a run of soft symbol values.
struct SyncHit {
    // Index into the soft symbol run at which the pattern starts.
    std::size_t offset = 0;

    // Normalised correlation, 1.0 for a noiseless exact match and -1.0 for an
    // exact match of the inverted pattern. Reported rather than thresholded
    // here so a mode can set its own threshold beside the clause that justifies
    // it.
    double score = 0.0;

    // True when the best score was negative, which for the two FM modes means
    // the discriminator polarity is inverted relative to the standard. Every
    // real receiver meets this, because whether a positive deviation comes out
    // of the discriminator positive depends on the sideband the tuner landed
    // on, and nothing in a standard can fix it.
    bool inverted = false;
};

// Finds the best alignment of `pattern` within `symbols`, by normalised
// cross-correlation. `pattern` holds the expected soft value of each symbol,
// in the same units as `symbols`.
//
// Returns an empty optional when `symbols` is shorter than `pattern`.
[[nodiscard]] Expected<SyncHit> correlate_pattern(ConstRealSpan symbols, ConstRealSpan pattern);

// The same, restricted to one alignment. Cheap enough to call per symbol when a
// caller is tracking an already-acquired frame and only wants to confirm the
// pattern is still where it expects.
[[nodiscard]] double correlation_at(ConstRealSpan symbols, ConstRealSpan pattern,
                                    std::size_t offset);

// correlation_at with the mean of each side removed first, which is the
// Pearson correlation coefficient of the two.
//
// A frequency discriminator turns a carrier offset into a constant added to
// every symbol, and a constant is exactly what this ignores, so a sync word
// scores the same at any offset. correlation_at does not. Worked for P25's
// 24-symbol frame sync, eleven +3 and thirteen -3, noiseless: an offset of
// one symbol unit takes its score from 1 to 0.946, and an offset of two to
// 0.818 or 0.846 depending on its sign, which drops a clean sync word under
// a 0.9 threshold. The price is one degree of freedom: a pattern shorter than
// two symbols cannot be scored, and a run with no variation at all, a
// squelched channel or a steady tone, scores zero.
[[nodiscard]] double centred_correlation_at(ConstRealSpan symbols, ConstRealSpan pattern,
                                            std::size_t offset);

// The straight line that takes a known pattern onto the symbols received for
// it: symbols[offset + i] ~ gain * pattern[i] + level, by least squares.
//
// Through a frequency discriminator, `level` is the carrier offset in symbol
// units and `gain` is the transmitter's deviation against the nominal, so
// (symbol - level) / gain puts the symbols after the pattern back on the
// levels a standard states. Both are measured from symbols the receiver
// already knows, which is what lets a burst be sliced correctly from its
// first symbol rather than after an estimator has settled.
struct LevelFit {
    double gain = 1.0;
    double level = 0.0;
};

// Returns an error when the range runs past the end of `symbols`, or when the
// pattern has no variation to fit a gain against.
[[nodiscard]] Expected<LevelFit> fit_levels(ConstRealSpan symbols, ConstRealSpan pattern,
                                            std::size_t offset);

// ---------------------------------------------------------------------------
// Bit and symbol packing
// ---------------------------------------------------------------------------

// Reads `count` bits from `bits` starting at `first`, most significant first,
// into the low bits of the result. Returns 0 when the range runs past the end,
// which callers guard against before calling rather than checking after.
[[nodiscard]] std::uint64_t pack_bits_msb(std::span<const std::uint8_t> bits, std::size_t first,
                                          std::size_t count);

// Bit error rate between two equal-length bit runs. Returns a negative value
// when the lengths differ, which is a programming error rather than a
// measurement.
[[nodiscard]] double bit_error_rate(std::span<const std::uint8_t> a,
                                    std::span<const std::uint8_t> b);

}  // namespace revenant::decode
