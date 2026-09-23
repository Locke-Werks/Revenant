// CPU twins of core/shaders/vrx_fine.comp and core/shaders/vrx_demod.comp,
// plus the host-side design that feeds both.
//
// Bit-identical to the kernels, not close to them. The twins transcribe the
// kernels' operation order rather than their mathematical result: the same
// ascending tap index, the same lerp-then-multiply-accumulate shape, the same
// Newton iterations in the same sequence, the same integer recurrences
// including their 32-bit wraps. Reordering a 32-tap complex accumulation
// moves the result by thousands of units in the last place, so "computes the
// same sum" is not the same claim and would not catch a kernel that had
// drifted.
//
// They live in core/ rather than tests/ because a reference in the test tree
// is a reference nobody ships, reviews or keeps current. See
// docs/conventions.md, "Reference implementations".
//
// This file carries two jobs and says so rather than pretending otherwise.
// The twins are one. The other is the design that produces what both sides
// read: the polyphase tap table, the NCO circle, the audio filter with any
// de-emphasis curve folded into it, the AM DC-removal window, FM stereo's
// pilot bandpass and rotation table, and the per-mode gain. That code is not
// a twin and has no operation order to match, exactly as
// core/dsp/pfb_design.cpp is not a twin of the channelizer kernels. It is
// here because the design and the twin have to agree about every index, and
// splitting them across two files is how they stop agreeing.
//
// Written from published mathematics and published channel plans only, per
// docs/clean-room.md. Crochiere and Rabiner, "Multirate Digital Signal
// Processing", chapters 3 and 6, for the polyphase decomposition and
// fractional rate conversion; Oppenheim and Schafer, "Discrete-Time Signal
// Processing", section 7.5.3 for the Kaiser window and its order estimate and
// chapter 2 for the modulation theorem; Abramowitz and Stegun eq. 9.6.12 for
// the modified Bessel function; Carlson, "Communication Systems", chapters 4
// and 5 for the detectors. The FM broadcast figures, the pre-emphasis time
// constants and the pilot-tone stereo system are channel plans this project
// states rather than documents it read, and each says so where it is
// declared, per docs/clean-room.md. No GPL implementation was read, fetched
// or consulted.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/dsp/pfb.h"
#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"

namespace revenant::dsp {

// ---------------------------------------------------------------------------
// Deterministic transcendentals
// ---------------------------------------------------------------------------
//
// Vulkan requires OpFAdd, OpFSub and OpFMul to be correctly rounded and then
// allows OpFDiv 2.5 units in the last place, Sqrt 3 ULP and Atan 4096 ULP. A
// demodulator built on the built-ins therefore produces different bits on
// every vendor and no CPU twin can referee it, which would leave the
// project's central rule unenforced exactly where the arithmetic is hardest
// to eyeball.
//
// These four are built from the correctly-rounded operations and from integer
// bit manipulation, which is exact. They are not faster than the built-ins and
// not more accurate. They are SPECIFIED, which is the only property that
// matters here. core/shaders/vrx_demod.comp carries the same sequences in the
// same order, and these are the definition of what the kernel is allowed to
// do.

// 1/sqrt(value) for value > 0, by the exponent-halving seed plus four Newton
// iterations. Valid for roughly 2^-100 to 2^100; outside that an intermediate
// overflows or lands in the denormals this project flushes to zero.
[[nodiscard]] float det_rsqrt(float value);

// sqrt(value), as value * det_rsqrt(value). Returns +0 for zero, for a
// negative argument and for a NaN.
[[nodiscard]] float det_sqrt(float value);

// 1/value for value > 0, by the exponent-negating seed plus four Newton
// iterations. Callers reduce to a positive argument first.
[[nodiscard]] float det_recip(float value);

// atan(t) for t in [0, 1], an odd polynomial in t*t evaluated by descending
// Horner. Worst error over the interval, evaluated in float32, is 9.6e-08
// radian, which is the float evaluation floor rather than the fit's.
[[nodiscard]] float det_atan_unit(float t);

// atan2(y, x) in (-pi, pi]. Octant-reduced first, so the polynomial only sees
// [0, 1] and the single division is min over max. Differs from the IEEE
// function on exactly one class of input: atan2(-0.0, x) for negative x
// returns +pi here.
[[nodiscard]] float det_atan2(float y, float x);

// ---------------------------------------------------------------------------
// Exact phase, in fixed-point turns
// ---------------------------------------------------------------------------
//
// The NCO never accumulates. `phase += increment` in float32 rounds the
// increment once and then adds that rounding n times, so the error grows
// linearly with n and reads downstream as a slow frequency error, which looks
// like a mistuned transmitter and gets blamed on one. Measured against the
// exact phase, a float32 accumulator at 2500 Hz on a 48 kHz stream is 0.26
// radian out after four million samples.
//
// Instead the phase at an absolute index j is
//
//     phi(j) = (j * D) mod 2^64,   D = round(2^64 * f / rate)
//
// as an unsigned 0.64 fixed-point turn, so the uint64 wrap IS the reduction
// modulo one turn. The increment D is formed once by integer long division,
// which is where the exact rational frequency is spent: f is a/b hertz and
// the rate is an integer, so 2^64*a over b*rate is a ratio of integers and
// the rounding happens exactly once, at that division.
//
// Two properties follow.
//
// The phase does not drift. Rounding D once moves the oscillator's frequency
// by at most rate*2^-65, which is 1.3e-15 Hz at 48 kHz, and relative to that
// frequency the phase is exact at every index forever.
//
// phi(j) depends on j and on nothing else, so a stream rendered in one
// dispatch and the same stream rendered in blocks of seven come out
// bit-identical. An earlier version carried the exactly-reduced phase at each
// block's first output and accumulated a 0.32 increment from there, which is
// MORE accurate and is not good enough: the increment's rounding accumulated
// inside the block and a third of the outputs changed when the block size
// did, because the table index straddles a boundary at exactly the phases a
// rational frequency keeps landing on. Retroactive decode and
// faster-than-realtime replay both re-enter the stream at an arbitrary index,
// so an output that depends on where the blocks fell is not usable for
// either.

// Largest hz_denominator * rate this arithmetic accepts. The long division
// below shifts the remainder left by sixteen bits four times over, so the
// denominator has to leave room inside a signed 64-bit integer. 2^31 is
// reached only by a 2048-channel grid on a channel rate above one megahertz,
// which is a grid that would not hold a receiver anyway.
inline constexpr std::int64_t kMaxPhaseDenominator = std::int64_t{1} << 31;

// round(2^64 * numerator / denominator) modulo 2^64, exactly, for
// 0 <= numerator < denominator <= kMaxPhaseDenominator. Long division in four
// base-65536 digits, because numerator << 64 does not fit anything this
// toolchain has.
[[nodiscard]] std::uint64_t turn_fixed64(std::int64_t numerator, std::int64_t denominator);

// The per-sample phase increment for a frequency of hz_numerator/hz_denominator
// hertz at the given rate, as a 0.64 turn. Negative frequencies wrap, which is
// correct: exp(-j*2*pi*(-f)) and exp(-j*2*pi*(1-f)) are the same phasor.
[[nodiscard]] Expected<std::uint64_t> nco_delta(std::int64_t hz_numerator,
                                                std::int64_t hz_denominator,
                                                SampleRate rate);

// The phase at an absolute index. The uint64 product wraps modulo 2^64, which
// is the reduction modulo one turn, so this is exact at any index and depends
// on nothing else.
[[nodiscard]] constexpr std::uint64_t nco_phase(std::uint64_t delta, SampleIndex index) {
    return delta * index;
}

// exp(-j*2*pi*i/K) for K = 1 << log2_size, built from the first octant and
// reflected so the four quadrature entries are exactly (1,0), (0,-1), (-1,0)
// and (0,1). Computing W^(K/2) from cos(pi) and sin(pi) instead leaves a
// residue in the imaginary part, which turns a free sign flip into a real
// complex multiply and costs bit-exactness for nothing; core/dsp/pfb.h's
// build_twiddles makes the same argument at length.
inline constexpr std::uint32_t kMaxNcoLog2 = 24;
[[nodiscard]] Expected<std::vector<Complex32>> build_nco_table(std::uint32_t log2_size);

// ---------------------------------------------------------------------------
// The fine stage
// ---------------------------------------------------------------------------

// The three specialization constants of core/shaders/vrx_fine.comp, at ids 1,
// 2 and 3. Id 0 is always the workgroup size.
struct VrxFineConfig {
    // T, taps per polyphase branch.
    std::uint32_t taps = 32;

    // P, branches in the polyphase table. The tap buffer holds P*T + 1
    // entries: linear interpolation between branch p and branch p+1 reaches
    // index P*T at the last branch of the last tap, and the host writes zero
    // there so the kernel needs no special case.
    std::uint32_t phases = 256;

    // log2 of the shared NCO table's length.
    std::uint32_t nco_log2 = 16;

    // Memberwise, because these three ARE the pipeline's specialization
    // constants: two configs that compare equal specialize to the same
    // pipeline and bind the same buffer sizes. See VrxShape.
    friend constexpr bool operator==(const VrxFineConfig&, const VrxFineConfig&) = default;
};

inline constexpr std::uint32_t kMaxFineTaps = 256;
inline constexpr std::uint32_t kMaxFinePhases = 4096;

// Complex values in the tap buffer a fine pipeline binds: P*T + 1, the extra
// entry being the zero the kernel's interpolation reads at the last branch of
// the last tap.
//
// A function of the config and nothing else, which is why VrxShape does not
// carry the length separately. It used to be compared separately, in
// DemodStage::retune, alongside the config that determines it.
//
// THIS FUNCTION IS THE LICENCE FOR THAT AND IT HAS TO BE CALLED TO BE ONE.
// It was declared, documented as the reason the length is not a shape
// member, and then used by nothing, so the shape rested on arithmetic three
// other places wrote out by hand. It is now the one expression: design_-
// fine_taps allocates from it, reference_vrx_fine validates against it, and
// core/engine/vrx_stage.cpp checks a plan's table against it both when it
// sizes the device buffers and on every retune that the shape comparison
// lets through. A retune copies a fixed byte count into a buffer built for
// the previous plan, so a table whose length did not match its config would
// be copied short or read past its end with nothing saying so.
[[nodiscard]] constexpr std::size_t fine_tap_table_size(const VrxFineConfig& config) {
    return static_cast<std::size_t>(config.phases) * static_cast<std::size_t>(config.taps) + 1U;
}

// The fifteen values core/shaders/vrx_fine.comp takes as push constants, in
// the order it declares them. Fifteen tightly packed 32-bit words is the
// scalar push-constant layout of the shader's Params block, so this struct can
// be handed to a dispatch as bytes with no repacking. The static assert below
// is what keeps that true.
//
// WHAT THIS PARAGRAPH USED TO SAY: thirteen, twice. The count is what the
// paragraph is for, and the static_assert below has said fifteen since the
// members that made it fifteen were added, so the file disagreed with itself
// with the compiler enforcing the half nobody read.
struct VrxFineParams {
    // channel_index * out_ring_blocks: where this receiver's channel starts
    // in the channel-major ring core/shaders/pfb_fft.comp writes.
    std::uint32_t chan_base = 0;

    // Per-channel ring capacity in blocks minus one, a power of two minus one.
    std::uint32_t chan_mask = 0;

    // Ring offset of channel sample n_0, the integer part of this block's
    // first output instant. A ring offset and not a SampleIndex: SampleIndex
    // is uint64 and GLSL has no 64-bit integer here, so the host owns the
    // absolute index. core/shaders/pfb_branch.comp makes the same trade.
    std::uint32_t in_offset = 0;

    std::uint32_t out_offset = 0;
    std::uint32_t out_mask = 0;
    std::uint32_t count = 0;

    // The resampler recurrence: step_whole = Fc / Fd, step_rem = Fc mod Fd,
    // out_rate = Fd, frac0 = (j_0 * Fc) mod Fd.
    std::uint32_t step_whole = 0;
    std::uint32_t step_rem = 0;
    std::uint32_t out_rate = 0;
    std::uint32_t frac0 = 0;

    // The output mixer, as 0.64 turns in two 32-bit words each.
    std::uint32_t nco_phase_high = 0;
    std::uint32_t nco_phase_low = 0;
    std::uint32_t nco_delta_high = 0;
    std::uint32_t nco_delta_low = 0;

    // 1.0f / out_rate, precomputed because the kernel must not divide: Vulkan
    // permits OpFDiv 2.5 ULP and requires OpFMul to be correctly rounded, so a
    // multiply by a shared float can be refereed and a divide cannot.
    float inv_out_rate = 0.0F;
};

static_assert(sizeof(VrxFineParams) == 15 * sizeof(std::uint32_t),
              "VrxFineParams must be fifteen packed 32-bit words to alias the kernel's "
              "push constant block");

// Twin of core/shaders/vrx_fine.comp.
//
// channel_ring is the whole channel-major ring; the twin indexes it at
// chan_base + ((base - k) & chan_mask), exactly as the kernel does, so it
// exercises the same wrap. taps is the P*T + 1 complex table. nco is the
// 1 << nco_log2 entry circle. fine_ring receives the output at
// (out_offset + i) & out_mask and nothing else is touched, so a caller can
// zero both buffers and compare all of them.
[[nodiscard]] Status reference_vrx_fine(const VrxFineConfig& config,
                                        const VrxFineParams& params,
                                        ConstComplexSpan channel_ring,
                                        ConstComplexSpan taps,
                                        ConstComplexSpan nco,
                                        ComplexSpan fine_ring);

// Rejects a configuration the kernel cannot run, or can run only with
// arithmetic that overflows 32 bits on the device and not on the host. Called
// by reference_vrx_fine and by the planner, so both reject the same thing.
[[nodiscard]] Status validate(const VrxFineConfig& config, const VrxFineParams& params);

// ---------------------------------------------------------------------------
// The demodulators
// ---------------------------------------------------------------------------

// The specialization constant core/shaders/vrx_demod.comp selects on is the
// value of engine::Demod, so there is no mapping table between the enum and
// the pipeline and nothing to get out of step.
inline constexpr std::uint32_t kDemodRaw = 0;
inline constexpr std::uint32_t kDemodAm = 1;
inline constexpr std::uint32_t kDemodNfm = 2;
inline constexpr std::uint32_t kDemodWfm = 3;
inline constexpr std::uint32_t kDemodUsb = 4;
inline constexpr std::uint32_t kDemodLsb = 5;
inline constexpr std::uint32_t kDemodDsb = 6;
inline constexpr std::uint32_t kDemodCw = 7;

// The digital voice modes. Since 2026-09-22 these DO reach both kernels: the
// fine stage mixes, filters and resamples them like any other receiver, and
// core/shaders/vrx_demod.comp hands the result out unchanged through the same
// passthrough branch the raw tap's constant selects. So these three values are
// specialization constants now, and the kernel's own copies of them are what
// route the three to that branch.
//
// WHAT THIS PARAGRAPH USED TO SAY: "These never reach the kernel:
// engine::is_complex_tap routes them down the raw tap's path, which is a
// buffer copy and no specialization constant at all." That was true while
// core/engine/vrx_stage.cpp declined every complex tap. It declines only
// Demod::Raw now.
inline constexpr std::uint32_t kDemodP25p1 = 8;
inline constexpr std::uint32_t kDemodDstar = 9;
inline constexpr std::uint32_t kDemodTetra = 10;

// DMR, appended on 2026-09-23 and routed to the same passthrough.
inline constexpr std::uint32_t kDemodDmr = 11;

static_assert(static_cast<std::uint32_t>(engine::Demod::Raw) == kDemodRaw);
static_assert(static_cast<std::uint32_t>(engine::Demod::Am) == kDemodAm);
static_assert(static_cast<std::uint32_t>(engine::Demod::Nfm) == kDemodNfm);
static_assert(static_cast<std::uint32_t>(engine::Demod::Wfm) == kDemodWfm);
static_assert(static_cast<std::uint32_t>(engine::Demod::Usb) == kDemodUsb);
static_assert(static_cast<std::uint32_t>(engine::Demod::Lsb) == kDemodLsb);
static_assert(static_cast<std::uint32_t>(engine::Demod::Dsb) == kDemodDsb);
static_assert(static_cast<std::uint32_t>(engine::Demod::Cw) == kDemodCw);
static_assert(static_cast<std::uint32_t>(engine::Demod::P25p1) == kDemodP25p1);
static_assert(static_cast<std::uint32_t>(engine::Demod::Dstar) == kDemodDstar);
static_assert(static_cast<std::uint32_t>(engine::Demod::Tetra) == kDemodTetra);
static_assert(static_cast<std::uint32_t>(engine::Demod::Dmr) == kDemodDmr);

// What those eight catch and what they do not, because the difference has
// already been got wrong once in this tree.
//
// They catch a REORDER: move Dsb above Usb and the assertion that names it
// fails. They cannot catch an ADDITION. A ninth enumerator after Cw leaves
// all eight true, and C++ offers no way to count an enum's enumerators, so
// there is no assertion to write here that would not itself be a hand-kept
// number going stale beside the one it is guarding.
//
// The guard for an addition is a switch instead, and it lives where the
// answer for a new mode has to be decided rather than here: fm_deviation,
// minimum_demod_rate and vrx_demod_gain in core/dsp/vrx_reference.cpp each
// switch over engine::Demod with every enumerator spelled out and no default
// label, which is the one shape /w14062 diagnoses. Adding a mode is a build
// error in all three. Keep them that way; a default label in any of them
// puts the silent answer back.

// Whether a 32-bit mode word names an enumerator of engine::Demod at all.
//
// THE RANGE GUARD THIS REPLACES WAS ONE MODE LIST SHORT FOR A DAY. validate,
// demod_rate_for, plan_vrx and the shorthand minimum_demod_rate each refused
// any mode above kDemodCw, written as `mode > kDemodCw`, after the three
// digital voice modes had been appended at 8, 9 and 10. Nothing could plan a
// P25, D-STAR or TETRA receiver through this file, and Graph::set_vrx_params,
// which asks vrx_shape_for, refused every retune of one. A comparison against
// the last enumerator is a hand-kept number and it went stale the day the
// enum grew; this is a switch with no default label, so the next mode appended
// is a /w14062 diagnostic here instead of a refusal somewhere downstream.
//
// The word is checked against the enum's 8-bit range before the cast, because
// a scoped enum with a fixed underlying type converts an out-of-range integer
// by truncation, and 257 would otherwise read as Demod::Am.
[[nodiscard]] constexpr bool is_known_mode(std::uint32_t mode) {
    if (mode > 0xFFU) {
        return false;
    }
    switch (static_cast<engine::Demod>(mode)) {
        case engine::Demod::Raw:
        case engine::Demod::Am:
        case engine::Demod::Nfm:
        case engine::Demod::Wfm:
        case engine::Demod::Usb:
        case engine::Demod::Lsb:
        case engine::Demod::Dsb:
        case engine::Demod::Cw:
        case engine::Demod::P25p1:
        case engine::Demod::Dstar:
        case engine::Demod::Tetra:
        case engine::Demod::Dmr: return true;
    }
    return false;
}

// Whether a mode hands out complex baseband rather than detected audio:
// engine::is_complex_tap, asked of the 32-bit word the planner and the kernel
// hold. False for a word that is no mode at all.
//
// This is the predicate every "is this the raw tap" test in this file used to
// be. The raw tap was the only complex output until the three digital voice
// modes got a fine stage, and a check written as `mode == kDemodRaw` would
// hand a P25 receiver an audio decimation filter and a mono detector.
[[nodiscard]] constexpr bool is_complex_output(std::uint32_t mode) {
    return is_known_mode(mode) && engine::is_complex_tap(static_cast<engine::Demod>(mode));
}

// The rate a digital voice receiver delivers its complex baseband at, which is
// also the step its demodulation rate is rounded up in. Zero for every mode
// that is not one of the three, whose demodulation rate is rounded up in
// steps of the audio rate instead.
//
// A whole number of samples per symbol for each, and not a rate chosen here:
// each is the decoder's own default, the rate its filter lengths were sized
// at and its round trip in tests/decode was measured at. A receiver handing
// a decoder any other rate is asking it to run outside what was measured.
//
//   P25p1   48000 S/s, 10 per symbol. TIA-102.BAAA-A clause 9.2, 4800
//           symbols per second. decode::P25Config::rate.
//   Dstar   48000 S/s, 10 per bit. JARL Ver 7.0 clause 4.1.2 b, 96 bits
//           every 20 ms, so 4800 bits per second of GMSK.
//           decode::DStarConfig::rate.
//   Tetra   72000 S/s, 4 per symbol. EN 300 392-2 clause 5.3, 36 kbit/s of
//           pi/4-DQPSK at two bits per symbol, so 18000 symbols per second.
//           decode::TetraConfig::rate.
//   Dmr     48000 S/s, 10 per symbol. ETSI TS 102 361-1 clause 10.2.1, 4800
//           symbols per second. decode::DmrConfig::rate.
//
// The figures are stated here rather than read from core/decode, for the
// reason core/engine/vrx.h gives for kCompositeAudioRateHz: nothing in the
// engine's DSP depends on core/decode and four constants are not worth the
// edge. tests/reference/test_vrx.cpp asserts all four against the decoders'
// own defaults and symbol rates, so the copies cannot drift apart silently.
//
// The audio rate is not the step for these modes, and that is deliberate
// rather than an omission. A receiver asking for no audio rate is handed the
// engine's default, 48000 on the shipped configuration, and a TETRA receiver
// rounded up in steps of that would run at 48000, which is 2.67 samples per
// symbol.
[[nodiscard]] SampleRate complex_tap_rate_step(std::uint32_t mode);

// The longest audio FIR the kernel may be asked to run, and the longest the
// DECIMATION DESIGN is allowed to spend on its own.
//
// WHAT kMaxAudioTaps USED TO BE AND WHY IT MOVED. Until 2026-09-20 it was
// 1024 and it was both numbers at once, because the audio filter had only
// one job. It now has two: the de-emphasis curve is folded into the same
// table, since the kernel has no state to run a one-pole with, and the
// folded length is the decimation filter's plus (deemphasis_taps - 1) times
// the decimation. A WFM receiver at 48 kHz spends about 420 taps on the
// decimation and about 410 on the curve. Leaving the cap at 1024 would have
// made every second WFM plan a clamp.
//
// The design budget stays at exactly the old number, so no filter this tree
// designed before today comes out different: design_vrx clamps the
// decimation length to kMaxDecimationTaps and nothing else reads it.
inline constexpr std::uint32_t kMaxAudioTaps = 4096;
inline constexpr std::uint32_t kMaxDecimationTaps = 1024;
inline constexpr std::uint32_t kMaxDcTaps = 4096;

// The longest de-emphasis expansion, before folding. 75 microseconds at
// 192 kHz of audio wants 240; this is four times that, and it is a cap on
// the truncation rather than on the curve, so hitting it costs accuracy at
// the very bottom of the audio band and nothing else. design_deemphasis_taps
// reports what it spent and what the truncation floor came out at.
inline constexpr std::uint32_t kMaxDeemphasisTaps = 1024;

// The longest complex pilot bandpass. 19 kHz with a 3 kHz transition at
// 336 kS/s wants about 490 taps; this is four times that, so a receiver
// demodulating at over a megahertz still gets the filter it asked for.
inline constexpr std::uint32_t kMaxPilotTaps = 2048;

// FM stereo's own three frequencies and the filter that separates them.
//
// The pilot is at 19 kHz and the difference channel is double-sideband
// suppressed carrier on its second harmonic at 38 kHz. Those two are the
// pilot-tone stereo system, which is stated here rather than transcribed:
// NOT OPENED FOR THIS WORK, on the same terms as core/dsp/synth/wfm_mod.h's
// pre-emphasis constants, and core/dsp/synth/wfm_mod.cpp is the transmitter
// this receiver was scored against.
//
// The bandpass has to pass the pilot and reject two things close to it: the
// sum channel, which stops at 15 kHz, and the difference channel's lower
// sideband, which starts at 23 kHz. A 1 kHz half-width with a 3 kHz
// transition on each side lands exactly between them and is symmetric, which
// is not a coincidence: the plan put the pilot in the middle of that gap.
inline constexpr Hertz kStereoPilotHz = 19'000;
inline constexpr double kStereoPilotHalfWidthHz = 1'000.0;
inline constexpr double kStereoPilotTransitionHz = 3'000.0;

// 70 dB rather than the 80 the other filters get. What leaks through is the
// sum channel at 15 kHz, which at full modulation sits 21 dB above a
// 9 percent pilot, so 70 dB of rejection leaves it 49 dB under the pilot and
// the phase error it causes bounds separation near 49 dB. Buying 80 costs
// 140 more taps for 10 dB nobody can hear past the clock-offset limit
// described on the kernel's stereo branch.
inline constexpr double kStereoPilotAttenuationDb = 70.0;

// The six specialization constants of core/shaders/vrx_demod.comp, at ids 1
// to 6.
struct VrxDemodConfig {
    std::uint32_t mode = kDemodNfm;

    // Fd / Fa. One for every mode whose demodulation rate already is the
    // audio rate, which is all of them except wideband FM.
    std::uint32_t decimation = 1;

    // Length of the audio decimation filter, applied to the detector output.
    // One, with a unit weight, when decimation is one.
    std::uint32_t audio_taps = 1;

    // Length of the AM DC-removal window. Only the AM branch reads it, but it
    // is always at least one so the weights buffer is never empty.
    std::uint32_t dc_taps = 1;

    // Floats per output FRAME, which is one for a mono detector and two for
    // an interleaved pair.
    //
    // Two means different things in the modes that use it and the
    // difference is in the mode, not here: the four complex taps write I then
    // Q, a stereo WFM receiver writes L then R. A consumer that only needs to
    // size a buffer reads this and does not have to know which.
    //
    // WHAT THIS FIELD REPLACES. Until 2026-09-20 the raw tap's second
    // component was a special case spelled out at four sites, each of them
    // some form of "two if the mode is raw and one otherwise", and stereo
    // would have made that five places to change and five places to forget.
    // core/engine/vrx_stage.cpp sized its readback from one of them.
    std::uint32_t channels = 1;

    // Length of the complex pilot bandpass, and zero unless this is a
    // stereo receiver. Not folded into audio_taps like the de-emphasis
    // curve, because it runs on the detector output before the audio filter
    // rather than after it, and because its output is complex.
    std::uint32_t pilot_taps = 0;

    // Memberwise, for the same reason VrxFineConfig's is. See VrxShape.
    friend constexpr bool operator==(const VrxDemodConfig&, const VrxDemodConfig&) = default;
};

// How many fine samples below its newest input one output reads: the audio
// filter's own reach, or the pilot filter's from the centre of that window,
// whichever is deeper, plus the detector's history under both.
//
// ONE FUNCTION, BECAUSE TWO COPIES IS HOW THIS GOES WRONG. validate() below
// uses it to refuse a dispatch the ring cannot hold, and
// core/engine/vrx_stage.cpp uses it to size the ring in the first place.
// Those two answering differently is a kernel reading a slot a later
// dispatch has already written, which is silent and sounds like a click.
[[nodiscard]] constexpr std::uint32_t demod_fine_history(const VrxDemodConfig& config) {
    const std::uint32_t detector = (config.mode == kDemodAm) ? config.dc_taps - 1U
                                   : (config.mode == kDemodNfm || config.mode == kDemodWfm)
                                       ? 1U
                                       : 0U;
    const std::uint32_t audio_reach = config.audio_taps - 1U;
    const std::uint32_t pilot_reach =
        (config.pilot_taps == 0) ? 0U : (config.audio_taps / 2U) + config.pilot_taps - 1U;
    return (audio_reach > pilot_reach ? audio_reach : pilot_reach) + detector;
}

// The four values core/shaders/vrx_demod.comp takes as push constants, in the
// order it declares them.
struct VrxDemodParams {
    std::uint32_t in_mask = 0;

    // Ring offset of the fine sample that audio output 0 detects at. The
    // detector and the decimation filter read below it, and the host
    // guarantees that history is still live.
    std::uint32_t in_offset = 0;

    std::uint32_t count = 0;

    // The mode's audio scale, computed by vrx_demod_gain so the convention
    // lives in one place rather than in eight branches of the kernel.
    float gain = 1.0F;
};

static_assert(sizeof(VrxDemodParams) == 4 * sizeof(std::uint32_t),
              "VrxDemodParams must be four packed 32-bit words to alias the kernel's push "
              "constant block");

// Twin of core/shaders/vrx_demod.comp.
//
// weights is the one buffer the kernel binds and holds four tables end to
// end: the audio filter, config.audio_taps of them, with any de-emphasis
// already folded in; the AM DC-removal window, config.dc_taps; the complex
// pilot bandpass, 2*config.pilot_taps interleaved real then imaginary; and
// the stereo rotation table, 2*config.audio_taps interleaved. The last two
// are empty unless config.pilot_taps is non-zero.
//
// audio receives count * config.channels values.
[[nodiscard]] Status reference_vrx_demod(const VrxDemodConfig& config,
                                         const VrxDemodParams& params,
                                         ConstComplexSpan fine_ring,
                                         ConstRealSpan weights,
                                         RealSpan audio);

[[nodiscard]] Status validate(const VrxDemodConfig& config, const VrxDemodParams& params);

// ---------------------------------------------------------------------------
// Design
// ---------------------------------------------------------------------------

// A receiver's passband, as two signed edges in hertz from VrxParams::center.
//
// Two numbers and not a width, because a width cannot say where the band
// sits. USB is the carrier plus 300 to plus 2700 hertz and has no energy at
// the carrier at all; a CW operator listening at a 700 Hz sidetone wants a
// window offset by the sidetone. Both were expressible before this only as a
// mode the planner special-cased, which is why USB and LSB were the two
// modes with their own arithmetic in plan_vrx and nothing else could be
// asked for.
//
// The frame is VrxParams::center, for every mode, with no exceptions. The
// filter passes [center + low, center + high]. What the fine stage MIXES to
// DC is a separate question and is center for seven modes and
// center - cw_pitch for CW, so the pitch is a translation and never an edge.
// Picking the tuned frequency as the origin is what makes the USB reading
// above literally true rather than needing a half-bandwidth correction.
struct Passband {
    Hertz low = 0;
    Hertz high = 0;

    [[nodiscard]] constexpr Hertz width() const { return high - low; }

    // The furthest either edge reaches from a given origin, which is the
    // half-span the stream has to be able to represent. Takes the origin
    // rather than assuming zero because the constraint is against the fine
    // stage's fold, and the fold is about the mix centre.
    [[nodiscard]] constexpr Hertz reach_from(Hertz origin) const {
        const Hertz below = origin - low;
        const Hertz above = high - origin;
        return (below > above) ? below : above;
    }

    friend constexpr bool operator==(Passband, Passband) = default;
};

// What a mode is tuned to when nobody said. Ordinary channel widths, shaped
// as edges.
//
// These moved here from tools/cli/main.cpp, where they were the CLI's
// private table and therefore not what the UI or any other client would use.
// A default that lives in one client is a default the next client invents
// differently.
//
// USB and LSB are the two entries that are not a width reshaped. 300 to 2700
// hertz is this project's own SSB convention, already written down in
// core/dsp/synth/modulators.h where the modulator generates exactly that
// audio band, so a receiver whose default did not match it would be filtering
// off signal the tree's own generator produces.
[[nodiscard]] Passband default_passband(engine::Demod mode);

// The passband a request asks for, before it is fitted to the channel.
//
// One of three answers, in order. The explicit pair when VrxParams carries
// one. Otherwise a positive VrxParams::bandwidth expanded through the mode's
// shorthand rule, which reproduces the geometry this planner had before
// edges existed: symmetric about the centre for raw, AM, NFM, WFM, DSB and
// CW, [0, B] for USB and [-B, 0] for LSB. Otherwise the mode's default
// above.
//
// A bandwidth of zero is "not stated" and a negative one is a mistake, and
// the two are answered differently on purpose. Zero reaching the default is
// what lets a client change a receiver's mode without carrying its own copy
// of the table: it sends zeros and reads the granted edges back off
// VrxPlacement. The Qt client is out of process and links no part of the
// DSP, so a table it could call directly would have had to be a second copy.
//
// Refuses low >= high, with both numbers in the message: an empty or
// inverted passband is a request nobody can fill and the two numbers are
// what says which way round the caller had them.
[[nodiscard]] Expected<Passband> resolve_passband(const engine::VrxParams& params);

// The passband fitted to what one grid channel can carry, each edge on its
// own.
//
// Both edges are bounded by half of max_channel_bandwidth below, which is
// how far from the receiver a symmetric band was allowed to reach before
// this change. Applying the same limit to each edge separately is what is
// new: a request too wide at the top keeps its lower edge where the operator
// put it, where one width could only ever take the same amount off both
// ends. A clamped receiver can therefore now come back off-centre as well as
// narrow, which is why the granted pair travels on VrxPlacement and a
// display draws the request and the grant in two shades.
//
// The limit is deliberately NOT the distance from the receiver to the
// channel's Nyquist, which is larger on the slack side by |residual|. That
// headroom is load-bearing: plan_vrx bounds the fine filter's transition by
// the gap between the fold and the passband edge, and an edge granted all
// the way to the Nyquist leaves none, so a receiver too wide for its channel
// would be refused outright where today it is given the widest filter that
// fits. The cost is up to 2*|residual| of band a clamped receiver could in
// principle have had on one side.
//
// THE POST-CONDITION: both edges come back inside [-limit, +limit], so low
// is never above high. A caller therefore reads a width of zero, never a
// negative one, and "nothing fits" and "nothing was asked for" are the same
// shape rather than two. A band lying WHOLLY past one limit collapses onto
// that limit, and a placement one channel can carry nothing for at all
// (max_channel_bandwidth zero) collapses to an empty band at the receiver's
// centre.
//
// WHAT THIS FUNCTION USED TO DO IN THOSE TWO CASES, BECAUSE BOTH WERE
// SILENT. It clamped the outside of each edge only, so [limit + 1000,
// limit + 2000] came back as [limit + 1000, limit], inverted; and it
// returned the request untouched when max_channel_bandwidth was zero, which
// is how a receiver a channel cannot carry came to be planned and reported
// by place() as unclamped. Neither produced an error anywhere near where it
// happened.
[[nodiscard]] Passband clamp_to_channel(const engine::VrxPlacement& placement, Passband band);

// Widest bandwidth one grid channel can deliver to a receiver placed here.
//
// The hard limit is the channel stream's own Nyquist. The fine stage reads
// nothing but this one channel, so a receiver's passband has to fit inside
// [-Fc/2, +Fc/2] measured from the CHANNEL's centre, and the receiver sits
// |residual| away from that centre. Hence Fc - 2*|residual|, which at the
// project's 2x-oversampled grid (D = M/2, so Fc is twice the channel spacing
// and |residual| is at most half a spacing) is never below one full channel
// spacing.
//
// This is a fit test and not a flatness test, and the difference is worth
// stating. The prototype in core/dsp/pfb_design.cpp puts its passband edge at
// 0.25*rate/M and its stopband edge at 0.75*rate/M, so a receiver whose band
// reaches out towards the channel edge sees the prototype rolling off, down
// to -6 dB where two adjacent channels cross. Nothing here corrects that
// droop; VrxPlacement has no field to report it in, and at M1 the answer for
// a receiver that wants a flat wide channel is a coarser grid.
//
// STILL THE LIMIT, BUT NO LONGER THE FIT. clamp_to_channel above is what
// place() and plan_vrx call, and half of this figure is the bound it puts on
// each edge, so this remains the answer to "how wide can a receiver here
// be" and is no longer the answer to "what did this receiver get". A
// symmetric request is granted exactly this width, to the hertz, which is
// what it was granted before.
[[nodiscard]] Hertz max_channel_bandwidth(const engine::VrxPlacement& placement);

// Peak deviation a mode's channel plan implies for a requested bandwidth.
//
// Not a free parameter and not a magic number: VrxParams carries a bandwidth
// and no deviation, so the deviation has to come from the plan the mode
// belongs to. Land mobile NFM is 5 kHz in a 25 kHz channel, so deviation is
// bandwidth/5. FM broadcast is 75 kHz in 200 kHz, so deviation is
// 3*bandwidth/8. Held as ratios rather than as constants so a narrower
// request scales instead of silently demodulating at the wrong level.
// Returns zero for a mode that has no deviation.
[[nodiscard]] Hertz fm_deviation(std::uint32_t mode, Hertz bandwidth);

// Smallest demodulation rate a mode can be demodulated at, for a passband
// whose edges are given in the frame the FINE STAGE MIXES TO DC.
//
// Three constraints, and the caller does the one piece of frame arithmetic
// there is: band here is the passband translated into the mix frame, which
// is the passband unchanged for seven modes and the passband plus the CW
// pitch for CW, because CW mixes a pitch below the tuned frequency.
//
//   width/2 * 3       The fine filter needs a transition band to live in at
//                     all. Its passband edge is half a width from its own
//                     centre and it reaches stopband half a transition
//                     further out, so a transition of width/2 puts the
//                     stopband at 0.75 of a width and Fd has to reach twice
//                     that. Independent of where the band sits, because it
//                     is measured from the filter's own centre.
//   reach * 2         Whatever the detector does afterwards, the fine stream
//                     has to be able to represent the band, and the furthest
//                     edge from the mix centre is what sets that.
//   width * 2, AM     The envelope of a band of width W carries content out
//                     to W wherever that band sits, because an envelope is
//                     built from differences.
//
// WHAT THIS FUNCTION USED TO SAY, AND WHY TWO OF ITS CASES ARE GONE. Until
// this change it took one symmetric bandwidth and carried a case for USB and
// LSB (2B, "a product detector on a one-sided passband of width B produces
// audio to B") and a case for CW (2*pitch + B, "the passband sits at the
// pitch, so its upper edge is pitch + B/2"). Both were the reach term
// written out for the one passband shape each of those modes was allowed to
// have: a USB band of [0, B] reaches B from the mix centre, and a CW band of
// [-B/2, +B/2] translated by the pitch reaches pitch + B/2. With the band
// stated rather than inferred they are the same expression, so they are one
// line instead of three. They are recorded here rather than deleted quietly
// because a reader arriving from core/shaders/vrx_fine.comp's mode table
// will be looking for them.
//
// HOW CLOSE "THE SAME EXPRESSION" ACTUALLY IS. THE FIRST ANSWER WAS
// "UNCHANGED TO THE HERTZ", WHICH WAS WRONG. THE CORRECTION TO IT WAS
// "EXACT FOR AN EVEN BANDWIDTH, 1 TO 2 HZ LOWER FOR AN ODD ONE, ON EVERY
// MODE", AND THAT IS WRONG TOO: USB AND LSB ARE EXACT AT EVERY BANDWIDTH,
// AND THE LANE'S OWN TEST SAID SO WHILE THIS PARAGRAPH SAID OTHERWISE.
//
// The gap is not a property of the parity on its own. It is a property of
// which shorthand expansion the mode uses, and resolve_passband gives the
// eight modes two of them. USB and LSB expand to [0, B] and [-B, 0], which
// state the full width, so the band is B wide whatever B is. The other six
// expand to [-B/2, +B/2], which takes a half-width twice rather than
// subtracting one from the other, so an odd B gives a band of B - 1. That
// missing hertz was never in the filter: the planner has always passed
// bandwidth/2 to design_fine_taps as a half-width, so an odd request has
// always been built one hertz narrow and only the rate it was rounded up to
// carried the difference.
//
// Measured mode by mode against the function this replaced. Both columns
// are the SHORTHAND overload below, which is the signature the old one had.
// h is B/2 rounded down and the pitch P is zero or more:
//
//   B >= 2            now                before                even   odd
//   ------------------------------------------------------------------------
//   Raw Nfm Wfm Dsb   3h                 (3B + 1)/2            same   2 Hz low
//   Am                4h                 2B                    same   2 Hz low
//   Usb Lsb           2B                 2B                    same   same
//   Cw                max(3h, 2P + 2h)   max((3B+1)/2, 2P+B)   same   1 or 2 low
//
// CW is the one row with two odd-bandwidth answers because it is the one
// mode where two terms compete for the maximum. The shape floor loses 2 Hz
// to the narrower band and the reach term loses 1, so an odd request whose
// floor wins is 2 Hz lower and one whose reach wins is 1 Hz lower. It is
// never equal.
//
// B = 1 is its own row and is not 1 to 2 Hz off anything. The six symmetric
// modes expand it to [0, 0], an empty band, so the empty-band rule further
// down answers 0 rather than a rate. Against the old figures that is 2 Hz
// low for five of them and max(2, 2P + 1) Hz low for CW, which at the
// default 700 Hz pitch is 1401 Hz and at a pitch of ZERO is 2, the same as
// the other five, because there the shape floor wins the maximum instead of
// the pitch term. An earlier draft of this sentence wrote the CW gap as
// 2P + 1 flat, which is right for every pitch the mode is useful at and
// wrong at the one the table also sweeps. Nothing is lost by it: the
// planner refuses a
// one-hertz symmetric request as well, because channel_carries is handed a
// band with no width. USB and LSB expand B = 1 to a real one-hertz band and
// answer 2, the same as before.
//
// tests/reference/test_vrx.cpp asserts the table, every mode against every
// bandwidth and pitch in it. It is pinned rather than left as arithmetic
// because the figures are reachable from a client that sends an odd width
// and would otherwise look like drift.
//
// The exhaustive switch over engine::Demod stays, and stays without a
// default label, even though only AM now differs. It is the guard that makes
// a ninth demodulator a build error rather than a mode that silently sits at
// the shared floor. See the long note beside the kDemod constants.
//
// Zero for an empty or inverted band. For a `mode` that is not an enumerator
// of engine::Demod, the widest of the constraints THIS FUNCTION CAN SEE,
// rather than the shared floor: the floor is the rate below which no mode
// can be filtered, not one at which an unidentified detector is safe, and
// giving it to a folding detector aliases the top of the audio band with
// nothing reporting it. plan_vrx rejects such a mode before it reaches here.
//
// WHAT "CAN SEE" EXCLUDES, BECAUSE THIS USED TO SAY "THE WIDEST OF THE
// CONSTRAINTS" FULL STOP AND THAT WAS ONE CONSTRAINT SHORT. The CW pitch
// never reaches this function. It is applied by the caller, which
// translates the band into the mix frame, and that translation is made for
// engine::Demod::Cw and no other value, so an unidentified mode is handed
// an untranslated band and the pitch is nowhere in the answer. A caller
// that wants an unidentified mode covered against a pitch translates the
// band itself before calling. Adding a pitch parameter here to close it was
// considered and refused: it would put a second frame convention in a
// function whose whole contract is that the caller owns the frame.
[[nodiscard]] Hertz minimum_demod_rate(std::uint32_t mode, Passband band_in_mix_frame);

// The symmetric shorthand, kept so callers holding one width still have an
// answer and so the generalisation above can be checked against what it
// replaced. Exactly minimum_demod_rate(mode, band) for the band the mode's
// shorthand rule expands `bandwidth` to, translated by the pitch for CW.
//
// A pitch below zero is read as zero, which is what plan_vrx and
// demod_rate_for both do with VrxParams::cw_pitch. It used to be passed
// through signed, and that made this overload answer a different number
// from the engine for the one input the engine clamps: a pitch of -3000 on
// a 1 kHz CW request translates the band to [-3500, -2500], which reaches
// 3500 Hz from the mix centre and so asks for 7000 S/s, where the engine
// mixes at no pitch at all and runs at 1500. Neither figure was wrong
// about its own band. They were about different bands, which is worse in a
// function whose stated job is to answer what the engine will do.
[[nodiscard]] Hertz minimum_demod_rate(std::uint32_t mode, Hertz bandwidth, Hertz cw_pitch);

// The demodulation rate a request will actually run at: the minimum above,
// rounded up to a whole multiple of the audio rate.
//
// Split out of plan_vrx so the rate a caller can get cheaply and the rate
// the planner builds a filter for are one piece of arithmetic rather than
// two copies of it.
//
// The four digital voice modes round up in steps of complex_tap_rate_step
// rather than of the audio rate, so for them `audio_rate` is checked and
// otherwise not read: a P25 receiver lands on 48000 and a TETRA one on 72000
// whatever audio rate the engine was configured with. plan_vrx calls this; so does vrx_shape_for, which is
// what the graph and the stage ask.
//
// WHAT THIS PARAGRAPH USED TO SAY, AND WHY IT WAS NEVER TRUE. It called
// this "the one number a caller needs before it can tell a retune that is a
// push constant from one that is a remove and an add", and said
// Graph::set_vrx_params asks it on every retune. It does not, and it must
// not: the demodulation rate is one of five things a rebuild depends on,
// and answering that question from the rate alone is exactly the two-entry
// list VrxShape exists to replace. set_vrx_params asks vrx_shape_for.
//
// It remains the right question for a client that wants the rate ITSELF
// rather than a verdict: the passband display's pane spans the
// demodulation rate, so a surface that wants to know how wide the pane will
// be before the engine answers asks this.
//
// Shares plan_vrx's resolution and fit, so the two cannot answer differently.
[[nodiscard]] Expected<SampleRate> demod_rate_for(const engine::VrxParams& params,
                                                  const engine::VrxPlacement& placement,
                                                  SampleRate audio_rate);

// The mode's audio scale. The convention: a unit-amplitude signal fully
// modulating its own mode produces audio that swings to exactly +/-1.0. See
// the header comment of core/shaders/vrx_demod.comp for what that means mode
// by mode.
[[nodiscard]] float vrx_demod_gain(std::uint32_t mode, SampleRate demod_rate,
                                   Hertz deviation);

// The polyphase tap table: a real Kaiser-windowed sinc lowpass of half-width
// half_width_hz, sampled at phases samples per input sample, normalised so
// every branch has exactly unit gain at the filter's centre, then modulated
// to that centre. Returns phases*taps + 1 complex values, the last of them
// zero.
//
// The per-branch normalisation is what removes the phase-dependent gain
// ripple that would otherwise appear as a tone at the resampler's beat
// frequency. It is exact at the centre and only there: a tone at the filter's
// centre comes out of any branch at unit magnitude, because the branch's
// modulation factor and the tone's own advance cancel term by term.
[[nodiscard]] Expected<std::vector<Complex32>> design_fine_taps(const VrxFineConfig& config,
                                                                SampleRate channel_rate,
                                                                Hertz half_width_hz,
                                                                std::int64_t centre_numerator,
                                                                std::int64_t centre_denominator,
                                                                double attenuation_db);

// The audio decimation filter: a real Kaiser-windowed sinc lowpass at the
// demodulation rate, normalised to unit gain at DC. Length is forced odd so
// the group delay is an integer.
//
// cutoff_hz of zero takes 0.45 of the audio rate, which is what this always
// did and is right for every mode whose audio band is simply "as much as the
// rate carries". WFM is the exception and is why the parameter exists: its
// audio band stops at 15 kHz by the channel plan and the 19 kHz pilot sits
// between there and the fold, so a receiver that filtered to 0.45 of 48 kHz
// would pass the pilot into the audio at -21 dB and, worse, into the stereo
// difference channel where it lands on top of the programme. Defaulted
// rather than required because tests/decode/test_rds_bits.cpp calls this
// with four arguments to build the composite path by hand.
[[nodiscard]] Expected<std::vector<float>> design_audio_taps(std::uint32_t taps,
                                                             SampleRate demod_rate,
                                                             SampleRate audio_rate,
                                                             double attenuation_db,
                                                             double cutoff_hz = 0.0);

// The de-emphasis curve, as a truncated FIR at the AUDIO rate.
//
// The curve is the standard one-pole, H(s) = 1/(1 + s*tau), which in
// discrete time is y[n] = (1-a)x[n] + a*y[n-1] with a = exp(-1/(tau*Fa)).
// That is a recursion, and core/shaders/vrx_demod.comp has nowhere to put
// one: every invocation computes its own output from the ring alone, with no
// state carried between blocks, which is what makes the audio a pure
// function of the absolute sample index and lets a block be recomputed.
//
// So the recursion is written out. Its impulse response is (1-a)*a^k, which
// decays geometrically, and this returns it truncated at the point a^k falls
// below float32's own resolution and renormalised to sum to exactly one so
// the DC gain is exact. That is the same shape core/shaders/vrx_demod.comp's
// AM DC-removal window already uses and for the same reason.
//
// It is NOT an approximation anybody has to reason about at runtime: the
// difference from the true one-pole is bounded by the truncation floor,
// which is below the difference between two adjacent floats, and
// DeemphasisDesign reports the figure rather than asserting it.
struct DeemphasisDesign {
    // (1-a)*a^k renormalised, k ascending. Exactly {1.0f} for a flat curve,
    // which folds to the identity.
    std::vector<float> taps;

    // a, the pole. Zero for a flat curve.
    double pole = 0.0;

    // a^N, the fraction of the true impulse response that was cut off, as
    // positive decibels below the direct term. Infinity for a flat curve.
    double truncation_db = 0.0;

    // True when kMaxDeemphasisTaps stopped the expansion before the floor
    // did, so truncation_db is worse than the design wanted. The caller says
    // so rather than the operator hearing it.
    bool truncated_early = false;

    // Group delay at DC, in samples of the audio rate: sum(k*h[k]).
    double group_delay_audio_samples = 0.0;
};

[[nodiscard]] Expected<DeemphasisDesign> design_deemphasis_taps(engine::Deemphasis curve,
                                                                SampleRate audio_rate);

// The decimation filter and the de-emphasis curve as ONE filter at the
// demodulation rate.
//
// The kernel applies one FIR to the detector output and then takes every
// Rth result. The de-emphasis belongs after that decimation, at the audio
// rate. Those are the same filter: a rate change after H(z^R) is H(z) after
// the rate change, the noble identity, and convolution commutes, so
// decimation_taps convolved with the curve zero-stuffed by R, applied before
// the decimation, is exactly the curve applied after it. One table, one
// loop, no second pass and nothing carried.
//
// THE ORDER THAT MATTERS IS NOT THIS ONE. De-emphasis goes on L and R and
// never on the composite, which is a statement about where in the STEREO
// matrix it sits, not about where in the filter chain. Both the sum and the
// difference path get this same folded table, and the matrix that follows is
// linear, so folding here is applying the curve to L and R.
// core/dsp/synth/wfm_mod.h documents the same order on the transmit side.
//
// Returns decimation_taps unchanged when the curve is flat.
[[nodiscard]] Expected<std::vector<float>> fold_deemphasis(ConstRealSpan decimation_taps,
                                                           ConstRealSpan deemphasis_taps,
                                                           std::uint32_t decimation);

// How long a pilot bandpass this demodulation rate needs. Cheap: one order
// estimate, no filter designed, so vrx_shape_for can ask it on every retune.
[[nodiscard]] std::uint32_t stereo_pilot_taps(SampleRate demod_rate);

// The two tables FM stereo adds to the weights buffer.
//
// The pilot bandpass is a real Kaiser-windowed sinc lowpass of half-width
// kStereoPilotHalfWidthHz, normalised to unit gain at DC, then modulated to
// +kStereoPilotHz. Applied to a real signal it is the analytic pilot, and
// for a tone at exactly kStereoPilotHz the modulation cancels the window's
// own delay term by term, so the output carries the pilot's phase at the
// instant asked about rather than at the instant half a window ago. That
// property is what removes every group-delay correction from the kernel and
// it is the same one design_fine_taps relies on.
//
// The rotation table carries the reference from the one instant it was
// measured to every tap of the audio filter: entry t is 2*cos and 2*sin of
// 2*omega*(t - audio_taps/2), where omega is the pilot's advance per sample.
// The factor of two is the difference channel's own, folded in here so the
// kernel's inner loop is one multiply-add.
struct StereoDesign {
    // 2*pilot_taps values, interleaved real then imaginary.
    std::vector<float> pilot;

    // 2*audio_taps values, interleaved cosine then sine.
    std::vector<float> rotation;

    // What the pilot filter's length bought, reported rather than assumed,
    // on the same terms as VrxPlan::fine_stopband_db.
    double pilot_transition_hz = 0.0;
    double pilot_stopband_db = 0.0;
};

[[nodiscard]] Expected<StereoDesign> design_stereo_tables(SampleRate demod_rate,
                                                          std::uint32_t audio_taps,
                                                          std::uint32_t pilot_taps);

// The AM DC-removal window: a Hann window normalised to sum to one, so that
// subtracting it from the direct term nulls DC exactly. Hann rather than a
// boxcar because a boxcar's -13 dB first sidelobe leaves up to 1.9 dB of
// ripple in the audio passband just above the corner and Hann's -31 dB leaves
// about 0.25 dB, at identical cost.
[[nodiscard]] std::vector<float> design_dc_weights(std::uint32_t taps);

// Everything a receiver needs, derived once from its request and its
// placement.
//
// Pure: it reads the grid, the rate, the request and the placement and
// nothing else. It allocates the three tables and hands them over rather than
// caching them, because a cache keyed on anything would make two receivers
// with the same parameters share a buffer and make retuning one of them a
// heisenbug in the other.
struct VrxPlan {
    engine::VrxPlacement placement;
    std::uint32_t mode = kDemodNfm;

    SampleRate channel_rate = 0;
    SampleRate audio_rate = 0;
    SampleRate demod_rate = 0;

    // What actually comes out, which is the audio rate for every mode except
    // the four complex taps. The raw tap is not a demodulator: it hands out
    // complex baseband at the receiver's bandwidth, so decimating it to 48 kHz
    // would throw away most of what it exists to expose, and its output rate
    // is the demodulation rate. The four digital voice modes are the same
    // shape, at the rate complex_tap_rate_step names for each. This is the
    // rate that belongs in an AudioChunk, and it is the rate the indices
    // passed to demod_block are counted in.
    SampleRate output_rate = 0;

    // The passband after resolution and after fitting to what one channel
    // can deliver, as signed hertz from params.center. This is the request
    // in its granted form and the thing every derived quantity below is
    // computed from.
    Passband passband{};

    // passband.width(), kept as its own field because a great deal of this
    // file's arithmetic and every one of its callers used to be written
    // against one width and still reads better that way.
    //
    // It is the GRANTED width and not the requested one, which is what it
    // has always been. What changed is that a width no longer determines the
    // filter: two receivers with the same bandwidth and different edges are
    // different filters, so anything that needs to know where the band sits
    // reads `passband` rather than this.
    Hertz bandwidth = 0;

    // Either edge was pulled in to fit the channel. Not "the width did not
    // fit", which is what it meant while there was only a width: a clamp can
    // now move one edge and leave the other, so a caller that wants to know
    // WHICH compares `passband` against resolve_passband of the request, or
    // reads VrxPlacement::granted_low and granted_high, which carry the same
    // pair out to a client that never sees a plan.
    bool bandwidth_clamped = false;

    // Where the fine filter is centred and what the fine stage mixes to DC,
    // both exact rationals in hertz, both in the channel's own frame. They
    // differ for USB, LSB and CW; see core/shaders/vrx_fine.comp.
    std::int64_t filter_numerator = 0;
    std::int64_t filter_denominator = 1;
    std::int64_t mix_numerator = 0;
    std::int64_t mix_denominator = 1;

    VrxFineConfig fine{};
    std::vector<Complex32> fine_taps;
    std::uint64_t fine_nco_delta = 0;

    VrxDemodConfig demod{};
    std::vector<float> demod_weights;
    float demod_gain = 1.0F;

    // Peak deviation the FM gain was derived from. Zero outside FM.
    Hertz deviation = 0;

    // The de-emphasis curve this receiver is running, RESOLVED. Never
    // engine::Deemphasis::Default: the plan is the answer and Default is the
    // question. engine::resolve_deemphasis decides, once, and both this and
    // engine::VrxStatus::applied_deemphasis call it, so a status and a plan
    // cannot disagree.
    engine::Deemphasis deemphasis = engine::Deemphasis::None;

    // What the curve cost and how well it came out. Zero taps' worth and an
    // infinite floor when the curve is flat.
    std::uint32_t deemphasis_taps = 1;
    double deemphasis_truncation_db = 0.0;
    bool deemphasis_truncated_early = false;

    // The decimation filter's own length, BEFORE the curve was folded into
    // it, and the two edges it was designed between.
    //
    // demod.audio_taps is the folded length, which is what the kernel runs
    // and what its ring history has to cover. These three are what
    // audio_stopband_db was computed from, so a reader comparing a stopband
    // figure against a tap count reads these and not that.
    //
    // The edges are 0.4 and 0.5 of the audio rate for every mode except a
    // WFM receiver delivering programme audio, whose band stops at 15 kHz
    // and whose stopband has to start before the 19 kHz pilot. Both are
    // zero when the demodulation rate is already the audio rate and there is
    // no filter at all.
    std::uint32_t audio_decimation_taps = 1;
    double audio_pass_hz = 0.0;
    double audio_stop_hz = 0.0;

    // Whether the stereo decoder is built, RESOLVED. True implies
    // demod.channels of 2 and a non-zero demod.pilot_taps, which
    // dsp::validate refuses to let drift apart.
    //
    // It says the receiver is DECODING stereo and not that the station is
    // transmitting it. The pilot decides that per sample, in the kernel, and
    // a consumer reads the answer off two channels that come back
    // bit-identical when the gate is shut.
    bool stereo = false;

    // What the pilot bandpass's length bought, on the same terms as
    // fine_transition_hz and fine_stopband_db. Zero when there is no pilot
    // filter.
    double pilot_transition_hz = 0.0;
    double pilot_stopband_db = 0.0;

    // What the design achieved, reported rather than assumed.
    //
    // fine_transition_hz is the transition width the chosen tap count buys,
    // not the width that was asked for: Kaiser sets the stopband depth from
    // the window's shape parameter and the transition from the length, so a
    // receiver whose filter hit the tap cap gets a wider transition rather
    // than a shallower stopband, and this is where it says so. A narrow
    // receiver on a fast channel is the case that hits the cap, because a
    // single-stage filter needs a tap per unit of rate over transition; the
    // answer for one that needs more is a second filter at the demodulation
    // rate, which M1 does not have.
    //
    // fine_stopband_db is the depth reached by the point the resampler folds,
    // so it is the anti-alias figure and not the window's target when the two
    // differ. Both come from Kaiser's order estimate, which is empirical and
    // optimistic by one to two decibels; core/dsp/pfb_design.cpp measured
    // exactly that against the filters it produces.
    double fine_stopband_db = 0.0;
    double fine_transition_hz = 0.0;
    double audio_stopband_db = 0.0;

    // Delay from the channel stream to the audio, in samples of each stage's
    // own rate. The fine figure is the interpolated prototype's centre; the
    // audio figure is the decimation filter's. The AM DC-removal window adds
    // essentially nothing, because in its passband the subtracted term is
    // small and the direct term carries no delay at all.
    double fine_group_delay_channel_samples = 0.0;
    double audio_group_delay_demod_samples = 0.0;
};

[[nodiscard]] Expected<VrxPlan> plan_vrx(const GridParams& grid, SampleRate rate,
                                         const engine::VrxParams& params,
                                         const engine::VrxPlacement& placement);

// ---------------------------------------------------------------------------
// The pipeline's shape
// ---------------------------------------------------------------------------

// Everything about a plan that a running receiver cannot be changed to
// without being rebuilt.
//
// A retune either is or is not a rebuild, and this type is the whole of that
// question. Two plans with equal shapes differ only in tuning: a re-modulated
// tap table copied into the buffer the kernel already reads, a new mixer
// increment, a channel index and a detector gain, all of which a stage
// applies under a command buffer already in flight. Two plans with different
// shapes need a different pipeline, a different ring or a different buffer
// size, and destroying either while an earlier frame still names it is the
// one thing the graph promises never to do.
//
// ONE PREDICATE, BECAUSE TWO LISTS IS HOW THIS WENT WRONG. Graph::set_vrx_-
// params and DemodStage::retune both have to answer it, and they used to
// answer it with two hand-kept lists of fields. The graph's list was two
// entries long, the stage's six, and the difference was reachable: a
// set_vrx_params carrying nothing but a new audio rate can leave the
// demodulation rate and the tap count exactly where they were while moving
// the output rate and the decimation, so the graph accepted it, stored it,
// echoed it back, and the stage refused it on the recording thread where
// there was no caller left to tell. Anything a rebuild depends on belongs in
// this struct and nowhere else.
//
// The fine tap TABLE's length is not a member. It is P*T + 1 and therefore a
// function of `fine`, which fine_tap_table_size states; comparing it as well
// would be a second list again. core/engine/vrx_stage.cpp checks a plan's
// table against that function rather than taking the sentence on trust,
// because it is the one place where the sentence being false would be felt
// as a bad copy instead of a refusal.
struct VrxShape {
    VrxFineConfig fine{};
    VrxDemodConfig demod{};

    SampleRate channel_rate = 0;
    SampleRate demod_rate = 0;
    SampleRate output_rate = 0;

    friend bool operator==(const VrxShape&, const VrxShape&) = default;
};

[[nodiscard]] VrxShape shape_of(const VrxPlan& plan);

// The shape a request would produce, WITHOUT designing the three filter
// tables plan_vrx designs.
//
// Same resolution, same fit and same arithmetic as plan_vrx, which calls
// this and then fills the tables in. What it skips is the only expensive
// part: a Kaiser-windowed sinc per polyphase branch, the audio decimation
// filter and the DC-removal window, which together are the reason a planner
// is too heavy to call on the control plane. Everything here is integer
// arithmetic and two order estimates.
//
// This is what a caller asks when the question is "can this change be
// applied in place", which is a question asked once per retune and, on a
// passband drag, once per gesture.
[[nodiscard]] Expected<VrxShape> vrx_shape_for(const GridParams& grid, SampleRate rate,
                                               const engine::VrxParams& params,
                                               const engine::VrxPlacement& placement);

// The refusal both callers give, in one wording, because a client recognises
// it by its words.
//
// ui/models/receiver_link.cpp matches on "remove and an add" to turn the
// refusal into a rebuild that keeps the pane's identity: the wire carries no
// error code, so the phrase is the contract. Two copies of it in two
// translation units is one copy away from a client that stops recognising
// half of them.
[[nodiscard]] std::string describe_shape_change(const VrxShape& from, const VrxShape& to);

// What this receiver is doing to the audio, in one sentence.
//
// THE REASON THIS EXISTS. An operator tuned a real broadcast station on
// 2026-09-20 and heard audio that was harsh at the top of the band, and
// nothing anywhere in the program could have told him why: there was no
// de-emphasis, no field said so, and 361 passing tests agreed. A curve that
// is applied silently is only half the fix, because the next time the
// question is "is it on?" the answer has to be somewhere other than the
// sound. It names the curve, the audio band the filter was built for,
// whether the stereo decoder is running, and what a clamp took away when
// one did.
//
// NOTHING PRINTS IT YET, WHICH IS SAID HERE RATHER THAN LEFT TO BE
// DISCOVERED. tests/reference/test_vrx.cpp asserts on the words and is the
// only caller. The three surfaces an operator actually reads are
// core/rpc/revenant.capnp's VrxStatus, tools/cli's receiver listing and
// ui/'s receiver rack, and all three belong to other lanes this round, so
// the sentence exists and is correct and is not yet in front of anybody. A
// caller holding an engine::VrxStatus can answer the two questions it leads
// with more cheaply: VrxStatus::applied_deemphasis and
// VrxStatus::decoding_stereo are pure functions of fields already echoed
// back, and need no plan.
[[nodiscard]] std::string describe_audio_chain(const VrxPlan& plan);

// ---------------------------------------------------------------------------
// Per-block parameters
// ---------------------------------------------------------------------------
//
// The exact arithmetic that turns an absolute index into the kernels' 32-bit
// push constants. It lives here rather than in the dispatcher because getting
// it wrong is silent: a truncated index works for three and a half minutes at
// 20 MS/s and then does not, and a phase derived by accumulation works
// forever and is slowly wrong.

struct VrxFineBlock {
    VrxFineParams params{};

    // Absolute channel-sample index of n_0, and the oldest and newest channel
    // samples this dispatch reads. The caller holds the ring claim across
    // [oldest_input, newest_input] and retires below oldest_input.
    SampleIndex first_input = 0;
    SampleIndex oldest_input = 0;
    SampleIndex newest_input = 0;
};

[[nodiscard]] Expected<VrxFineBlock> fine_block(const VrxPlan& plan,
                                                std::uint32_t channel_base,
                                                std::uint32_t channel_mask,
                                                std::uint32_t fine_mask,
                                                SampleIndex first_output,
                                                std::uint32_t count);

struct VrxDemodBlock {
    VrxDemodParams params{};

    // Absolute demodulation-rate index audio sample 0 detects at, and the
    // oldest fine sample the dispatch reads.
    SampleIndex first_fine = 0;
    SampleIndex oldest_fine = 0;
};

[[nodiscard]] Expected<VrxDemodBlock> demod_block(const VrxPlan& plan,
                                                  std::uint32_t fine_mask,
                                                  SampleIndex first_audio,
                                                  std::uint32_t count);

// ---------------------------------------------------------------------------
// The display tap
// ---------------------------------------------------------------------------
//
// What the passband pane transforms. It is core/shaders/vrx_fine.comp again,
// specialized a second time with a different tap table, reading the same
// channel and writing a ring of its own.
//
// WHY IT EXISTS. The pane used to transform the fine ring, which is written
// AFTER the channel filter. The pane is wider than the filter, so the noise
// floor carried the filter's own magnitude response: full level inside the
// passband, stopband level outside, and a skirt that moved whenever the
// operator dragged an edge. Measured on white noise at -40 dBFS with a
// 6 kHz NFM filter, the floor read -84.2 dB inside the passband and fell to
// -170 dB at the edges of the pane, and a tone 2 kHz outside the passband
// read 83.6 dB below an identical tone inside it. docs/ui-spectrum.md has the
// before and after figures.
//
// This tap reads the channel BEFORE any receiver filter: the same mix the
// fine stage applies, so the axis is the fine stream's to the hertz, then a
// fixed anti-alias lowpass and an integer decimation, nothing else. The
// filter the operator is dragging is drawn over the result by the client.
//
// THE RATE RULE.
//
// The pane keeps the central half of the transform, which is what
// core/shaders/spectrum.comp keeps natively, so it spans half the display
// rate: [dc - Fdisp/4, dc + Fdisp/4]. The anti-alias filter is flat to that
// edge and in its stopband by 3*Fdisp/4, which is the nearest frequency that
// folds back inside the pane, so nothing aliases into what is shown.
//
// Fdisp is Fc/R for an integer R, so the resampler is a pure decimator: its
// fractional phase is zero at every output, one polyphase branch is the whole
// table, and the recurrence is exact integer arithmetic with no remainder.
// R is the largest rung of the channel rate's ladder (display_ladder) that
// keeps the pane at least four passband reaches wide, where the reach is how
// far the passband's further edge sits from the pane's centre. A symmetric
// band B wide therefore gets a pane of at least 2B, a one-sided band such as
// USB's [0, B] at least 4B, and neither more than twice that, because rungs
// are at least a factor of two apart.
//
// R is also capped by the tap budget. The table is kDisplayTaps long whatever
// R is, so that a change of R is a new table and new push constants rather
// than a new pipeline, and 256 taps at the channel rate reach
// kDisplayAttenuationDb across a transition of Fdisp/2 only while R stays at
// or below display_max_decimation(). At the canonical 625 kS/s channel that
// is R = 25; the ladder's top rung there is 20, so the narrowest pane is
// 15.6 kHz.
//
// WHY THE PANE HOLDS STILL DURING A DRAG. R changes only when the reach
// crosses a rung, and adjacent rungs are at least a factor of two apart, so
// an edge dragged across most of a pane moves nothing and the pane changes
// span only when the passband has roughly doubled or halved. A retune that
// keeps R keeps the display stream running; one that moves R restarts it,
// and the graph counts the frames skipped while the new window fills.
//
// Written from the same published mathematics as the fine stage: Crochiere
// and Rabiner, "Multirate Digital Signal Processing", chapter 2 for
// decimation by an integer and the alias bound above, and Oppenheim and
// Schafer section 7.5.3 for the Kaiser design.

// Taps in the display filter's one branch. kMaxFineTaps, which is what
// dsp::validate lets the kernel run.
inline constexpr std::uint32_t kDisplayTaps = kMaxFineTaps;

// The anti-alias stopband the display filter has to reach before the fold,
// and the most it is designed for when the tap budget would allow more. The
// ceiling is the fine stage's own spur floor: the 16-bit NCO table puts
// spurs near -96 dBc, so designing past 100 dB buys nothing visible.
inline constexpr double kDisplayAttenuationDb = 80.0;
inline constexpr double kDisplayAttenuationCeilingDb = 100.0;

// The rungs R may take for a channel rate, ascending from 1. Each rung after
// the first is the smallest divisor of the channel rate that is at least
// twice the rung below it, and none exceeds display_max_decimation(), so
// every rung divides the rate exactly and rungs are at least a factor of two
// apart. A rate with few small divisors has few rungs, down to 1 alone. At
// 625 kS/s and at 75 kS/s alike the ladder is 1, 2, 4, 8, 20.
[[nodiscard]] std::vector<std::uint32_t> display_ladder(SampleRate channel_rate);

// The largest R at which kDisplayTaps reach kDisplayAttenuationDb across a
// transition of Fdisp/2, by Kaiser's order estimate. Independent of the rate
// itself because the transition is a fixed fraction of it: 25.
[[nodiscard]] std::uint32_t display_max_decimation();

// The decimation the rule above picks for a passband whose further edge sits
// reach_hz from the display centre.
[[nodiscard]] std::uint32_t display_decimation(SampleRate channel_rate, Hertz reach_hz);

struct VrxDisplayPlan {
    SampleRate channel_rate = 0;
    SampleRate display_rate = 0;
    std::uint32_t decimation = 1;

    // What the rate was chosen from, in the frame the fine stage mixes to DC.
    Hertz reach_hz = 0;

    // taps = kDisplayTaps, phases = 1, and the receiver's own NCO table.
    VrxFineConfig fine{};
    std::vector<Complex32> taps;

    // The fine stage's mix frequency at the display rate, from the plan's
    // exact rational, so the display's DC is the fine stream's DC.
    std::uint64_t nco_delta = 0;

    // The design, in hertz from the display's DC. Flat to pass_hz, which is
    // the pane's own edge; in stopband by stop_hz, which is the nearest
    // frequency that folds into the pane; attenuation_db is what the Kaiser
    // design was asked for between them.
    double pass_hz = 0.0;
    double stop_hz = 0.0;
    double attenuation_db = 0.0;

    // Delay from the channel stream to the display stream, in channel
    // samples: the one branch's centre, (kDisplayTaps - 1) / 2.
    double group_delay_channel_samples = 0.0;
};

// The display tap for a receiver's plan. Pure, like plan_vrx, and cheap
// enough to call on every retune: one Kaiser-windowed sinc of kDisplayTaps.
[[nodiscard]] Expected<VrxDisplayPlan> plan_vrx_display(const VrxPlan& plan);

// Push constants for display outputs [first_output, first_output + count),
// by the same exact arithmetic fine_block uses for the fine stream.
[[nodiscard]] Expected<VrxFineBlock> display_block(const VrxDisplayPlan& plan,
                                                   std::uint32_t channel_base,
                                                   std::uint32_t channel_mask,
                                                   std::uint32_t display_mask,
                                                   SampleIndex first_output,
                                                   std::uint32_t count);

}  // namespace revenant::dsp
