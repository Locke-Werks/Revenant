// RDS physical layer: recovered 57 kHz subcarrier to a differentially decoded
// bitstream.
//
// SPECIFICATION
//
// Implements EN 50067:1998, the CENELEC Radio Data System specification,
// clauses 1.1 through 1.7 (the physical layer) in full. No deviation.
//
// EN 50067:1998 is cited rather than IEC 62106 because EN 50067 is the text
// that was actually read: it is the direct ancestor of the IEC series, its
// full text is freely available, and docs/clean-room.md requires a citation
// to name the document somebody opened rather than the one that superseded
// it. Two consequences a reader should carry:
//
//   - The IEC clause numbering differs. EN 50067 clause 3.1.5.1 is IEC 62106
//     clause 6.1.5.1. The clause numbers below are EN 50067's.
//   - Nothing here is region dependent. NRSC-4 (April 1998) clauses 1.1
//     through 1.7, the United States RBDS text, are word for word identical
//     to EN 50067's. One demodulator serves both regions, and the RDS/RBDS
//     split does not begin until the group layer.
//
// CLEAN ROOM
//
// No implementation of RDS was read while this was written. Every constant
// below names the clause, equation or table it came from. Where a number is
// an engineering choice rather than a specified value, the comment says so.
//
// WHAT THIS TAKES AS INPUT, AND WHY
//
// A REAL FM composite multiplex (MPX) signal, as a span of float, at a stated
// integer sample rate. Not complex baseband, and not a signal already mixed
// down to the subcarrier.
//
// RDS lives at 57 kHz on the composite, which exists only AFTER the FM
// discriminator. FM is not a linear modulation, so a linear filter and mixer
// applied to the receiver's complex baseband cannot separate the subcarrier
// from the rest of the station: a receiver placed at the station frequency
// plus 57 kHz picks up the upper skirt of the same FM carrier, not the data.
// The composite is the only place the subcarrier exists as a distinct signal.
//
// In this engine the composite is what a WFM receiver produces when its
// audio_rate is set high enough that the audio decimation filter does not run
// across the subcarrier. core/dsp/vrx_reference.cpp designs no filter at all
// when the decimation resolves to 1 (plan_vrx sets audio_taps to 1), and
// otherwise designs a Kaiser lowpass whose PASSBAND edge is 0.4 * audio_rate
// and whose stopband starts at 0.5 * audio_rate. The composite reaches
// 59375 Hz, so the whole of it is inside that passband only at an audio_rate
// of 148438 or above. RdsBitsConfig::rate defaults to 171000, which clears
// it. That path costs no new kernel, no new stage and no new buffer.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it said the composite survived intact at any audio_rate
// at or above kMinimumRateHz, which is 125000. It does not, and 125000 was
// never the number for this: 0.45 * 125000 is 56250, below the subcarrier
// itself, so at that rate the audio filter removes the data outright before
// the decoder ever sees it. kMinimumRateHz is the DECODER's floor, the one
// create() enforces, and it governs a composite arriving from a file or from
// core/dsp/synth/rds_mod.h with no receiver in the path at all. It says
// nothing about the receiver, which is why the two numbers differ and why
// naming one of them twice was the mistake.
//
// The kMinimumRateHz paragraph below carried 132000 for the receiver bound
// and that was wrong too, in the smaller way: 0.45 * audio_rate is the
// filter's CUTOFF, its 6 dB point, not its passband edge. An audio_rate of
// 132000 puts 59375 Hz at the cutoff, with the upper half of the data band
// already 6 dB down and sloping. Both numbers have been corrected to 148438,
// which is 59375 / 0.4, and both are recorded here rather than swapped
// quietly because a reader who sized a receiver off either one got a
// composite with its data sideband eaten and a decoder that will not lock,
// which does not look like a rate problem from the outside.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// The project's rule is that every GPU kernel has a scalar twin and the two
// agree to zero ULP. This decoder is host code with no kernel behind it, so
// that rule does not reach it and a reader should not assume it does. What
// stands in its place is the synthetic transmitter in core/dsp/synth/rds_mod.h
// and the round trip in tests/decode/test_rds_bits.cpp: the modulator is
// written from the same clauses, and a disagreement between the pair means one
// of the two misread the document.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::Hertz;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// EN 50067:1998 clause 1.1. Locked to the third harmonic of the 19 kHz pilot
// while the transmission is stereo, and 57 kHz +/- 6 Hz while it is mono. The
// pilot tolerance is +/- 2 Hz, which is where the +/- 6 Hz comes from.
inline constexpr Hertz kSubcarrierHz = 57000;
inline constexpr Hertz kPilotHz = 19000;
inline constexpr Hertz kSubcarrierToleranceHz = 6;

// EN 50067:1998 clause 1.5. The bit rate is the subcarrier divided by 48, so
// 57000 / 48 = 1187.5 exactly and the subcarrier, the pilot and the bit clock
// are all coherent. Tolerance is +/- 0.125 bit/s.
//
// This is the one rate in the file that is not an integer, and it is not a
// frequency: docs/conventions.md fixes Hertz as int64_t for frequencies
// because mixing stages accumulate error in them. A bit rate is divided once,
// at construction, and never mixed, so a double here costs nothing and an
// integer here would be a lie about a value the standard states as a fraction.
inline constexpr int kSubcarrierBitRateDivisor = 48;
inline constexpr double kBitRateHz = 1187.5;
inline constexpr double kBitPeriodSeconds = 1.0 / kBitRateHz;

// EN 50067:1998 clause 1.7, equation (3):
//
//     HT(f) = cos(pi * f * td / 4)   for 0 <= f <= 2/td
//     HT(f) = 0                      for f > 2/td
//
// with td = 1/1187.5 s. The cutoff is 2/td = 2375 Hz exactly.
//
// WHY THIS IS NOT 2400 HZ
//
// 2400 Hz is widely quoted as the RDS shaping cutoff, and the brief this work
// was commissioned from quoted it too. It is wrong, and the way it is wrong is
// worth recording rather than silently correcting: 2400 is the last tick on
// the frequency axis of EN 50067 figures 3 and 4, not a value in the text. The
// text gives the band edge only as the symbolic 2/td, and pdftotext renders
// the pi in equation (3) as a percent sign, so a reader working from extracted
// text sees a broken formula next to a graph whose axis ends at 2400 and takes
// the number off the graph. The fraction layout of equation (3) was confirmed
// from the PDF's own drawing primitives.
//
// The difference matters twice. It is the band edge the receiver's matched
// filter is designed to, and cos(pi * f * td / 4) is exactly zero at 2375 Hz,
// which is the property that makes truncating the response there something
// other than a brick wall. Designed to 2400 the response does not reach zero
// and the truncation rings.
inline constexpr Hertz kShapingCutoffHz = 2375;

// ---------------------------------------------------------------------------
// The clause 1.7 shaping filter, shared between transmitter and receiver
// ---------------------------------------------------------------------------

// The continuous impulse response of HT(f), in seconds.
//
// EN 50067:1998 clause 1.7 splits the data-spectrum shaping equally between
// transmitter and receiver "to give optimum performance in the presence of
// random noise", so the overall channel response HO(f) = HT(f)^2 is a 100
// percent cosine roll-off. One specification, two users, so one definition
// here rather than one in the modulator and one in the demodulator. Two copies
// of a filter that are supposed to be identical is how a round trip starts
// passing for the wrong reason.
//
// Closed form, derived from equation (3) by inverse Fourier transform:
//
//     h(t) = 8 * cos(4*pi*t/td) / (pi * td * (1 - 64*(t/td)^2))
//
// with a removable singularity at t = +/- td/8 where both factors vanish and
// the limit is 2/td. h(0) = 8/(pi*td), which is the integral of HT(f) over all
// f, as it must be. The tails fall as 1/t^2, so a truncation at four bit
// periods is already 60 dB down.
//
// The derivation is this project's arithmetic, not a transcription: the
// standard prints HT(f) and nothing else.
[[nodiscard]] double shaping_impulse(double seconds, double bit_period_seconds);

// ---------------------------------------------------------------------------
// Input rate
// ---------------------------------------------------------------------------

// The composite reaches 57000 + 2375 = 59375 Hz, so Nyquist alone demands more
// than 118750 samples per second. 125000 is the floor enforced here: it leaves
// the negative-frequency image of the subcarrier, which lands at
// rate - 114000 Hz once the mixer has run, at 11 kHz, comfortably outside the
// 2375 Hz data passband.
//
// The engine's own floor is higher for an unrelated reason. A WFM receiver's
// audio decimation filter runs its transition from 0.4 to 0.5 of the audio
// rate (plan_vrx in core/dsp/vrx_reference.cpp), so the composite's 59375 Hz
// edge is inside the passband only from an audio_rate of 148438 up, and below
// that the receiver eats the upper data sideband before the composite ever
// reaches this decoder. That is a property of the receiver, not of this code,
// which is why the two numbers differ. See the retraction in the input
// paragraph at the top of this file: this bound was written as 132000, which
// is where 59375 Hz reaches the filter's 6 dB CUTOFF rather than its passband
// edge, and a composite delivered at that rate arrives with half its data
// band sloping away.
//
// create() enforces 125000 and not 148438, deliberately. A composite handed
// over by core/dsp/synth/rds_mod.h, or read out of a file, went through no
// audio filter and is intact at 125000; refusing it would be this decoder
// declining a signal it can decode because of a stage that was not in the
// path.
inline constexpr SampleRate kMinimumRateHz = 125000;

// ---------------------------------------------------------------------------
// Output seam
// ---------------------------------------------------------------------------

// Bits leave one at a time, oldest first, each one a differentially decoded
// data bit in transmission order. That is the whole contract with the group
// layer: no block framing, no syndrome, no offset words, nothing above clause
// 1.6 crosses this line.
//
// EN 50067:1998 clause 2.2 has every information word, checkword, binary
// number and address transmitted most significant bit first, so the group
// layer assembles words by shifting each arriving bit in at the bottom.
//
// THE SEAM CARRIES NO DISCONTINUITY MARKER, AND THE READER HAS TO KNOW IT
//
// The decoder drops bits mid-stream. It stops emitting whenever the lock
// falls away and starts again wherever it relocks, and it does that at every
// fade, at every reacquisition, and for the whole of the scan and lock
// window that follow one. Nothing in this signature says so: two bits
// arriving back to back through this sink may be two consecutive bits off
// the air or may have a second of dead carrier between them, and there is no
// way to tell them apart from here.
//
// That is survivable and is not an oversight, because the group layer does
// not trust bit adjacency in the first place. EN 50067 clause 2.3 frames on
// the offset words, and core/decode/rds_groups.h hunts for a syndrome match
// and confirms it block by block before it declares sync. A gap costs it the
// re-hunt it would have paid anyway on a bad patch, and an unmarked gap
// costs the same as a marked one.
//
// What it is NOT survivable for is anything that reads the bit stream as a
// continuous recording: counting elapsed bits as elapsed time, timing a
// clock-time group against the bits either side of it, or measuring a bit
// error rate against a reference sequence. A caller doing any of those wants
// RdsBitsStatus::reacquisitions sampled beside the bits, which does move on
// every drop, or it wants the sink overload and its own timestamping. Stated
// here rather than fixed with a marker on the seam, because adding one would
// change the contract for a group layer that does not need it.
using BitSink = std::function<void(bool)>;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct RdsBitsConfig {
    // Sample rate of the real composite handed to process(). Integer per
    // docs/conventions.md.
    SampleRate rate = 171000;

    // Track the 19 kHz pilot and take the subcarrier as its third harmonic,
    // per EN 50067:1998 clause 1.1.
    //
    // This is an aid, not a requirement. The pilot pins the subcarrier
    // frequency exactly, so the carrier loop below starts with nothing to pull
    // in. With no pilot present, or with this off, the subcarrier NCO runs at
    // exactly 57000 Hz and the carrier loop absorbs the error. Clause 1.1
    // permits a mono transmission to carry RDS with no pilot at all, so a
    // decoder that needs one is wrong about the standard.
    bool track_pilot = true;

    // Is the 19 kHz pilot a tone or is it noise?
    //
    // The pilot arm is filtered twice, once at a wide bandwidth and once ten
    // times narrower, and the two magnitudes are compared. A coherent tone
    // sits at DC after the mixer and survives both filters equally, so the
    // ratio is close to one. Noise scales as the square root of the
    // bandwidth, so the ratio is close to sqrt(0.1) = 0.32. The threshold
    // sits between the two and needs no knowledge of the absolute level,
    // which is the point: the obvious test, pilot magnitude against the
    // composite RMS, has to be set below the quietest real pilot and above
    // the loudest noise, and at a 300 Hz measurement bandwidth those two
    // numbers are not in that order.
    //
    // Getting this wrong is not cosmetic. A false pilot lets a loop driven by
    // noise random-walk, and the walk is tripled on the way to the
    // subcarrier, so the carrier loop below ends up chasing this loop instead
    // of the transmitter.
    double pilot_tone_ratio_threshold = 0.6;

    // And a floor, as a fraction of the composite RMS, so that a silent input
    // does not pass the ratio test on the strength of two equally tiny
    // numbers.
    double pilot_minimum_level = 0.01;

    // Carrier recovery loop bandwidth, in hertz, once the decoder is locked.
    // The loop is a second order BPSK phase loop.
    //
    // 20 Hz is 1.7 percent of the bit rate, narrow enough that loop phase
    // jitter is not the thing limiting sensitivity, and wide enough to hold
    // clause 1.1's +/- 6 Hz subcarrier tolerance with room for a receiver
    // local oscillator error on top.
    double carrier_loop_bandwidth_hz = 20.0;

    // And the bandwidth it acquires at, before lock.
    //
    // A second order loop's pull-in TIME grows as the square of the frequency
    // error over the cube of its natural frequency, and a BPSK phase detector
    // doubles the error it sees because it works on twice the phase. At 20 Hz
    // a 30 Hz offset takes around two seconds to pull in, which is longer
    // than a decoder gets before somebody decides the station has no RDS. At
    // 150 Hz the same offset is gone in milliseconds, and the loop narrows to
    // carrier_loop_bandwidth_hz the moment the biphase consistency says it
    // has something. Widening again on loss of lock is automatic and is the
    // behaviour wanted: a signal that just faded is one that needs
    // reacquiring.
    double carrier_acquisition_bandwidth_hz = 150.0;

    // Symbol timing loop bandwidth, in hertz. 6 Hz against a 1187.5 bit/s
    // clock is 0.005 of the bit rate, which tracks a sample clock error of
    // hundreds of parts per million with no steady-state phase error, the
    // integrator absorbing the rate difference.
    double timing_loop_bandwidth_hz = 6.0;

    // Lock thresholds on the biphase sign consistency: the fraction of bits
    // whose two half-bit samples came back with opposite signs.
    //
    // EN 50067:1998 clause 1.7 makes every biphase symbol an odd impulse pair,
    // so the two halves of a bit ALWAYS have opposite signs. That is a
    // property of the code, not of the data, which is what makes it the right
    // thing to measure: noise gives 0.5 and a locked decoder gives close to 1,
    // whatever the payload happens to be.
    //
    // The thresholds are set from where the consistency actually sits rather
    // than from a round number. Each half-bit sample is in error with
    // probability about Q(sqrt(Eb/N0)), and the two halves agree when both
    // are right or both are wrong, so at 3 dB of Eb/N0 the consistency is
    // around 0.84 and at 6 dB around 0.96. Locking at 0.72 therefore reaches
    // about 1 dB of Eb/N0, which is well past where the block layer's own
    // error correction gives up, and it is still four sigma above the 0.5
    // that noise produces over the window below.
    double lock_threshold = 0.72;
    double unlock_threshold = 0.60;

    // Carrier coherence below which the decoder will not attempt timing
    // acquisition at all. Coherence is |E[z^2]| / E[|z|^2] on the derotated
    // baseband, which is SNR/(1+SNR) in the post-filter bandwidth: 0.25 is
    // about -4.8 dB there, well below where any bit is recoverable.
    double carrier_lock_threshold = 0.25;

    // Bits of biphase consistency history behind the lock decision. 256 bits
    // is 216 ms, about two and a half groups at the 87.6 ms clause 2.1 gives
    // for one, so the decision rests on more than one group's worth of
    // evidence without taking a second to notice a loss.
    //
    // The window length is also what keeps the unlock threshold honest. The
    // standard error of a consistency estimate over N bits is
    // sqrt(0.25/N), so 256 bits puts 0.60 four sigma above the 0.5 that
    // noise produces and a false lock out of reach; 128 bits would put it at
    // 2.3 sigma, which is about one false lock per hundred windows.
    //
    // That argument is about a window measured against the timing phase the
    // decoder is using now, and it says nothing at all about a window carried
    // across a reacquisition. Until 2026-09-20 the carrier-coherence gate
    // restarted the timing scan without clearing the window, so a decoder
    // coming out of a fade committed to a phase and then read 255 pre-fade
    // hits plus one new one as 0.99 consistency: four sigma of margin bought
    // nothing, because the sample was not of the thing being decided.
    // RdsBitSync::begin_reacquisition is what keeps the two together, and
    // both ways out of a lock go through it.
    std::size_t lock_window_bits = 256;

    // Bits accumulated by the timing acquisition scan before it commits to a
    // phase. The scan is the only thing that resolves the half-bit ambiguity
    // (see the note on acquisition in rds_bits.cpp), and it wants enough bits
    // for the two-to-one margin between the right answer and the half-bit-off
    // one to stand clear of the data.
    std::size_t acquisition_bits = 96;
};

// ---------------------------------------------------------------------------
// Reported state
// ---------------------------------------------------------------------------

enum class RdsLock : std::uint8_t {
    // Nothing usable. Either the carrier loop has not cohered or timing
    // acquisition has not run. No bits are emitted in this state.
    Unlocked,

    // Timing is tracking and consistency is being measured, but it has not
    // reached lock_threshold. No bits are emitted in this state either.
    Acquiring,

    // Emitting.
    Locked,
};

[[nodiscard]] const char* lock_name(RdsLock state);

struct RdsBitsStatus {
    RdsLock lock = RdsLock::Unlocked;

    // Biphase sign consistency mapped onto [0, 1]: 2 * (consistency - 0.5),
    // clamped. Zero is indistinguishable from noise, one is a clean eye. This
    // is the quality figure to surface, because it degrades smoothly and it
    // means the same thing at every SNR.
    double quality = 0.0;

    // The raw consistency the quality is derived from, in [0.5, 1] in practice.
    double biphase_consistency = 0.5;

    // |E[z^2]| / E[|z|^2] on the derotated data baseband. Equals
    // SNR/(1 + SNR) measured in the post-filter bandwidth, so it is readable
    // as a signal quality number in its own right.
    double carrier_coherence = 0.0;

    // Residual subcarrier frequency error the carrier loop is holding, in
    // hertz. Non-integer on purpose: this is a measurement, not a tuning
    // request, and rounding a measurement to whole hertz throws away the only
    // evidence that would show a drifting transmitter.
    double carrier_offset_hz = 0.0;

    // Recovered bit rate, from the timing loop's period estimate. Nominal is
    // 1187.5; a difference is the transmitter's clock against this receiver's.
    double bit_rate_hz = 0.0;

    bool pilot_locked = false;

    // Smoothed pilot phasor amplitude as a fraction of the composite RMS.
    double pilot_level = 0.0;

    // Narrow-arm over wide-arm magnitude on the pilot mixer. Close to one for
    // a tone, close to sqrt(0.1) for noise. This is what pilot_locked is
    // decided on, so it is reported beside it rather than left implicit.
    double pilot_tone_ratio = 0.0;

    std::uint64_t samples_consumed = 0;
    std::uint64_t bits_emitted = 0;

    // Times the decoder gave up and re-ran timing acquisition. A climbing
    // count with bits still flowing is a marginal signal; a climbing count
    // with no bits is the wrong station.
    std::uint64_t reacquisitions = 0;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

// Streaming, and blocking independent: feeding N samples in one call and in
// any partition of that call produces the same bits in the same order.
// Nothing here reads a clock or holds a lock.
//
// ALLOCATION
//
// process(mpx, sink) allocates nothing. Every filter history, every loop and
// the scan accumulator are sized by create() and never resized, and a
// recovered bit goes straight out through the sink. That is the overload a
// real-time path wants and the one the property is stated for.
//
// process(mpx) does allocate, on the emitted bits. It appends to queue_,
// which grows the ordinary way, and drain() hands the storage over to the
// caller by swapping it out, so the next call starts from zero capacity and
// grows again. That is deliberate: the queue exists so a caller can pull bits
// on its own schedule, and a bounded ring would have to decide what to drop.
//
// WHAT THIS PARAGRAPH USED TO SAY
//
// Until 2026-09-20 it said "nothing here reads a clock, allocates after
// construction, or holds a lock", flat, for the whole class. The queueing
// overload broke it on every bit, and it is the overload the tests and the
// simplest caller use, so the claim was false along the path most people
// take. Recorded rather than quietly narrowed because somebody reading that
// sentence would have put this decoder on an audio callback with drain()
// behind it and taken an allocation per bit at 1187.5 bits a second in a
// place where an allocation is not allowed.
//
// THE CHAIN, and what in the standard puts each stage there
//
//   1. Pilot loop at the input rate, 19 kHz, clause 1.1. Its phase is tripled
//      to give the subcarrier reference. Frozen at nominal when the pilot is
//      absent.
//
//   2. Complex mix by that reference, then a decimating Kaiser lowpass down to
//      a working rate near 19 kHz. Complex rather than real because clause 1.2
//      lets the subcarrier be locked either IN PHASE or IN QUADRATURE with the
//      third harmonic of the pilot, both are permitted, so a decoder cannot
//      assume one and has to carry both axes until it can measure which.
//
//   3. The clause 1.7 matched filter HT(f). Clause 1.4 makes the modulation
//      suppressed-carrier AM of the subcarrier by the shaped biphase signal,
//      so the matched filter is the transmitter's own shaping filter: the
//      standard split the shaping in half for exactly this.
//
//   4. A second order BPSK carrier loop. This is what resolves clause 1.2's
//      in-phase-or-quadrature ambiguity, and it leaves the 180 degree one that
//      every suppressed-carrier recovery leaves. Clause 1.6 exists to absorb
//      that, and does.
//
//   5. Timing acquisition then a mid-bit zero-crossing tracking loop.
//
//   6. The biphase decision, clause 1.7: first half minus second half.
//
//   7. Differential decode, clause 1.6 Table 2: new output = previous input
//      XOR new input.
//
// The note to figure 2 of EN 50067 warns that a receiver whose symbol decoder
// is an integrate-and-dump must NOT put a full HT(f) in front of it, because
// the integrate-and-dump contributes shaping of its own and the pair
// over-filters. This decoder does not integrate and dump. It samples the
// filtered waveform at the two half-bit instants and differences them, which
// is the impulse-pair correlator matched to clause 1.7's e(t) and contributes
// no shaping, so HT(f) in front of it is right and the overall response is the
// 100 percent cosine roll-off the clause asks for.
class RdsBitSync {
public:
    [[nodiscard]] static Expected<RdsBitSync> create(const RdsBitsConfig& config);

    // Consumes the span and pushes every recovered bit to the sink, in order.
    // The sink is called only while the decoder is locked: an unlocked decoder
    // emits nothing rather than handing noise to the group layer, which would
    // otherwise spend its error budget on it.
    void process(ConstRealSpan mpx, const BitSink& sink);

    // The same, queueing instead. drain() takes the queue.
    void process(ConstRealSpan mpx);

    [[nodiscard]] std::vector<std::uint8_t> drain();
    [[nodiscard]] std::size_t pending_bits() const { return queue_.size(); }

    // Back to the state create() returned. Filter histories, loops, queue and
    // counters all cleared.
    void reset();

    [[nodiscard]] const RdsBitsStatus& status() const { return status_; }
    [[nodiscard]] const RdsBitsConfig& config() const { return config_; }

    // Working rate after decimation, and the decimation factor that produced
    // it. Exposed because a test that wants to reason about the loops in
    // samples needs them, and because a caller sizing a buffer does.
    [[nodiscard]] double working_rate_hz() const { return working_rate_; }
    [[nodiscard]] int decimation() const { return decimation_; }

private:
    RdsBitSync() = default;

    // One input sample through the pilot loop, the mixer and the decimator.
    void push_input(float sample, const BitSink* sink);

    // One decimated complex sample through shaping, carrier recovery and
    // timing.
    void push_working(double real_part, double imag_part, const BitSink* sink);

    // Timing acquisition scan and tracking. Called once the working-rate
    // history holds enough samples to reach the next bit's late point.
    void run_timing(const BitSink* sink);

    // Throw away everything downstream of the carrier loop and start the
    // timing scan again.
    //
    // Both ways out of a lock come through here, and that is the point. They
    // used to clear different sets of state: the deliberate one cleared the
    // consistency window with the scan, and the carrier-coherence gate
    // cleared only the scan. A decoder that faded out and back then ran its
    // scan against a window still holding a full 256 pre-fade hits, and the
    // first bit after the scan read as 0.99 consistency and cleared
    // lock_threshold before one bit of the recovered signal had been looked
    // at. The window is the evidence the lock decision rests on, so it has to
    // be evidence about the signal being decoded now.
    //
    // Idempotent, because the coherence gate calls it once per bit period for
    // as long as the carrier stays incoherent. status_.reacquisitions is
    // counted by the caller rather than here for that reason: one fade is one
    // reacquisition, not one per bit of it.
    void begin_reacquisition();

    void emit(bool bit, const BitSink* sink);

    // Cubic interpolation of the post-carrier-loop real signal at a fractional
    // working-sample position.
    [[nodiscard]] double interpolate(double position) const;

    // The biphase decision statistic at a fractional bit instant: the sample
    // there minus the sample half a bit later. Clause 1.7's impulse pair,
    // correlated.
    [[nodiscard]] double biphase_statistic(double position) const;

    RdsBitsConfig config_{};
    RdsBitsStatus status_{};

    double working_rate_ = 0.0;
    int decimation_ = 1;
    double samples_per_bit_ = 0.0;

    // Pilot loop, at the input rate.
    double pilot_phase_ = 0.0;      // turns, reduced into [0, 1)
    double pilot_increment_ = 0.0;  // turns per input sample, nominal
    double pilot_freq_ = 0.0;       // loop integrator, turns per input sample
    double pilot_kp_ = 0.0;
    double pilot_ki_ = 0.0;

    // Two cascaded one-poles per arm. One pole does not put away the image at
    // twice the pilot frequency that mixing a real input leaves behind, and
    // the phase detector reads that image as error.
    double pilot_wide_real_[2]{};
    double pilot_wide_imag_[2]{};
    double pilot_narrow_real_[2]{};
    double pilot_narrow_imag_[2]{};
    double pilot_wide_alpha_ = 0.0;
    double pilot_narrow_alpha_ = 0.0;
    double pilot_wide_magnitude_ = 0.0;
    double pilot_narrow_magnitude_ = 0.0;
    double pilot_magnitude_alpha_ = 0.0;
    double composite_power_ = 0.0;
    double composite_alpha_ = 0.0;

    // Decimating anti-alias lowpass, complex, at the input rate.
    std::vector<double> decim_taps_{};
    std::vector<double> decim_real_{};
    std::vector<double> decim_imag_{};
    std::size_t decim_write_ = 0;
    int decim_count_ = 0;

    // Clause 1.7 shaping filter, complex, at the working rate.
    std::vector<double> shape_taps_{};
    std::vector<double> shape_real_{};
    std::vector<double> shape_imag_{};
    std::size_t shape_write_ = 0;

    // Carrier loop, at the working rate.
    double carrier_phase_ = 0.0;  // radians
    double carrier_freq_ = 0.0;   // radians per working sample
    double carrier_kp_acquire_ = 0.0;
    double carrier_ki_acquire_ = 0.0;
    double carrier_kp_track_ = 0.0;
    double carrier_ki_track_ = 0.0;
    double coherence_real_ = 0.0;
    double coherence_imag_ = 0.0;
    double coherence_power_ = 0.0;
    double coherence_alpha_ = 0.0;

    // Derotated real signal, addressed by absolute working-sample index. The
    // ring has to reach back far enough for the early gate of the previous bit
    // and forward through the late gate of the current one, plus the cubic
    // interpolator's own span.
    std::vector<double> history_{};
    std::size_t history_mask_ = 0;
    std::uint64_t history_count_ = 0;

    // Timing loop, in working samples.
    double bit_position_ = 0.0;
    double bit_period_ = 0.0;
    double timing_kp_ = 0.0;
    double timing_ki_ = 0.0;

    // Timing acquisition: |D| accumulated against each candidate phase.
    std::vector<double> scan_accumulator_{};
    std::size_t scan_bits_ = 0;
    bool scanning_ = true;

    // Biphase sign consistency over a sliding window of bits.
    std::vector<std::uint8_t> consistency_window_{};
    std::size_t consistency_write_ = 0;
    std::size_t consistency_filled_ = 0;
    std::size_t consistency_hits_ = 0;

    // Bits below the unlock threshold seen since the last bit at or over the
    // LOCK threshold. A window's worth of them gives up and re-scans rather
    // than hoping.
    //
    // NOT IN A ROW, which is what this said and what the member's own name
    // used to say. A bit between the two thresholds neither adds to this nor
    // clears it, so what is counted need never be consecutive. See the
    // reset in RdsBitSync::run_timing for why that is on purpose.
    std::size_t unlock_bits_ = 0;

    // Clause 1.6 differential decoder state: the previous received bit.
    bool have_previous_ = false;
    bool previous_bit_ = false;

    std::vector<std::uint8_t> queue_{};
};

}  // namespace revenant::decode
