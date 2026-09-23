// CPU twins of the three noise mitigation kernels, and the host-side design
// that turns a receiver's request into what they read.
//
//   core/shaders/noise_blank.comp     impulse blanker, complex baseband at the
//                                     channel rate, before the fine filter
//   core/shaders/noise_line.comp      manual notch and automatic notch, on
//                                     the demodulated audio
//   core/shaders/noise_spectral.comp  spectral noise reduction, on the
//                                     demodulated audio
//
// Each is off by default on every receiver, and a receiver with all three off
// records no dispatch for any of them. docs/noise.md says what each one does
// to a signal and carries the measured figures.
//
// Bit-identical to the kernels, not close to them, on the same terms as
// core/dsp/vrx_reference.h: the twins transcribe the kernels' operation order,
// the same ascending loops, the same separate multiplies and adds, the same
// Newton reciprocal, so a kernel that drifts by one rounding fails
// tests/reference/test_noise.cpp rather than being called close enough.
//
// TWO OF THE THREE CARRY STATE, AND SAY SO. The fine stage and the
// demodulators are pure functions of the absolute sample index, which is what
// lets a stream be re-entered anywhere. An adaptive filter and a noise floor
// tracker cannot be: what they do to sample n depends on everything before
// it. So the line and spectral kernels take a state buffer in and hand it back
// updated, and each twin is a pure function of (state, input) to (state,
// output). Bit-exactness holds for the same sequence of dispatches from the
// same state, which is what the conformance cases assert, including across a
// dispatch boundary. The blanker is stateless: its only carried quantity is a
// ring of detection flags that is itself a pure function of the channel ring.
//
// PROVENANCE, per docs/clean-room.md. Written from published algorithms. No
// GPL implementation was read, fetched or consulted, and nothing here came
// from any other radio program's source. The documents are named at each
// section below. None of them was opened for this work: the recursions are
// the ones those papers are universally cited for, and every constant that
// is not in them is this project's own, stated with the measurement that
// chose it. The house rule is that a citation names a document somebody
// read, so these name the document and say plainly that nobody here read it
// during this work, as core/engine/vrx.h does for the de-emphasis curves.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/vrx.h"
#include "core/error.h"

namespace revenant::dsp {

struct VrxPlan;

// ---------------------------------------------------------------------------
// The impulse blanker
// ---------------------------------------------------------------------------
//
// Designed from the detection-by-threshold principle of S. V. Vaseghi,
// "Advanced Digital Signal Processing and Noise Reduction", Wiley, the
// chapter "Impulsive Noise: Modelling, Detection and Removal": an impulse is
// a sample whose energy stands far above a local estimate of the background,
// and removing it is replacing the samples it covers. Cited by chapter title
// because the numbering moves between editions. The chapter's detectors
// prewhiten with an inverse filter first; this one does not, because at the
// channel rate the background is already close to white across the channel.
//
// WHERE IT RUNS. On the receiver's coarse channel, at the channel rate,
// BEFORE core/shaders/vrx_fine.comp narrows it. That is the whole reason it
// is a separate stage rather than something applied to the audio: an
// impulse a few source samples long is a few channel samples long here, and
// after a 2.7 kHz channel filter the same impulse is a millisecond-long
// ringing burst of the filter's own impulse response, which no threshold can
// cut out without cutting the programme with it.
//
// WHAT IT DOES, two passes over the same channel samples:
//
//   detect  flag[j] = 1 when |x[j]|^2 * W > T * sum(|x[m]|^2), the sum over
//           the W samples ending G samples before j. The window is the
//           running magnitude estimate; the guard G keeps the leading skirt
//           of the impulse being tested out of its own reference. A zero sum
//           never flags, so a ring still at its clear value does not blank
//           the first real sample it sees.
//
//   apply   blanked[i] = 0 when any flag in [i - hang, i + lead] is set, and
//           x[i] otherwise.
//
// It BLANKS, it does not interpolate. A zeroed run of a few channel samples
// is a notch in time that the fine filter then smooths, and the energy it
// removes from the programme is the programme's energy over that run, which
// at the default figures is under a tenth of a millisecond per impulse.
//
// THE FIGURES, stated here and in docs/noise.md:
//
//   threshold  VrxParams::nb_threshold_db, default 12 dB. Complex Gaussian
//              noise exceeds its mean power by 12 dB with probability
//              exp(-15.85), about 1.3e-7, so noise alone almost never blanks.
//   window     0.5 ms of channel samples, clamped to [16, 256].
//   guard      the lead plus two samples.
//   hang, lead two channel samples each, 14 us at 144 kS/s. An impulse
//              reaches the channel through the channelizer's prototype
//              filter and arrives smeared over 2 * taps_per_branch channel
//              samples, and the obvious design blanks all of that. It was
//              tried and measured worse: the threshold already flags every
//              sample of the smear that stands clear of the background, and
//              a gap a fraction of a millisecond long puts its distortion
//              straight into the voice's band, so each extra sample of hang
//              cuts programme and buys nothing. core/dsp/noise_reference.cpp
//              has the sweep.

// Specialization constants of core/shaders/noise_blank.comp at ids 1 to 5.
struct BlankerConfig {
    // kBlankDetect or kBlankApply. One kernel file, two pipelines, the same
    // way core/shaders/vrx_demod.comp folds to one mode.
    std::uint32_t pass = 0;
    std::uint32_t window = 64;
    std::uint32_t guard = 4;
    std::uint32_t hang = 2;
    std::uint32_t lead = 2;

    friend constexpr bool operator==(const BlankerConfig&, const BlankerConfig&) = default;
};

inline constexpr std::uint32_t kBlankDetect = 0;
inline constexpr std::uint32_t kBlankApply = 1;
inline constexpr std::uint32_t kMaxBlankWindow = 256;
inline constexpr std::uint32_t kMinBlankWindow = 16;

// How far below and above the samples a blanker dispatch produces it reads,
// in channel samples. The detect pass for i - hang reads down to
// i - hang - guard - window; the apply pass for i reads flags up to i + lead.
[[nodiscard]] constexpr std::uint32_t blanker_reach_below(const BlankerConfig& config) {
    return config.hang + config.guard + config.window;
}

// Seven packed words, the push constant block of both passes.
//
// TWO RINGS, TWO MASKS. The channel ring is read where the channelizer wrote
// it, at chan_base + (index & chan_mask), exactly as core/shaders/vrx_fine.comp
// reads it. The flag and blanked rings are this receiver's own and are much
// shorter: they hold what the frames in flight are reading, a few thousand
// samples, where the channel ring holds the engine's whole ring_seconds. So a
// sample at absolute index i sits at (i & chan_mask) in one and (i & out_mask)
// in the other, and the host hands over both offsets of the dispatch's first
// sample. The fine stage then reads the blanked ring with chan_base zero and
// out_mask as its channel mask.
struct BlankerParams {
    std::uint32_t chan_base = 0;
    std::uint32_t chan_mask = 0;
    std::uint32_t chan_first = 0;
    std::uint32_t out_mask = 0;
    std::uint32_t out_first = 0;
    std::uint32_t count = 0;

    // T in the inequality above, as a power ratio: 10^(dB/10) rounded to
    // float once on the host.
    float threshold = 15.848932F;
};

static_assert(sizeof(BlankerParams) == 7 * sizeof(std::uint32_t),
              "BlankerParams must be seven packed 32-bit words to alias the kernel's push "
              "constant block");

// The figures above for one channel rate, and the threshold as a power ratio.
[[nodiscard]] BlankerConfig design_blanker(SampleRate channel_rate);
[[nodiscard]] float blanker_threshold(double threshold_db);

// Twin of the detect pass. channel_ring is the whole channel-major ring;
// flags holds out_mask + 1 values and receives count of them, at
// (out_first + i) & out_mask, and nothing else.
[[nodiscard]] Status reference_blank_detect(const BlankerConfig& config,
                                            const BlankerParams& params,
                                            ConstComplexSpan channel_ring,
                                            RealSpan flags);

// Twin of the apply pass. blanked holds out_mask + 1 samples and receives
// count of them, at (out_first + i) & out_mask, and nothing else.
[[nodiscard]] Status reference_blank_apply(const BlankerConfig& config,
                                           const BlankerParams& params,
                                           ConstComplexSpan channel_ring,
                                           ConstRealSpan flags,
                                           ComplexSpan blanked);

[[nodiscard]] Status validate(const BlankerConfig& config, const BlankerParams& params);

// ---------------------------------------------------------------------------
// The line kernel: the manual notch and the automatic one
// ---------------------------------------------------------------------------
//
// One kernel for both because both are a recursion over the audio sample by
// sample, and a recursion is one thread's work however it is dressed. Running
// them in one dispatch costs one pass over the samples instead of two.
//
// THE MANUAL NOTCH is a second-order section with a zero pair and a pole pair
// on the same angle, read off the pole-zero geometry of Oppenheim and
// Schafer, "Discrete-Time Signal Processing", chapter 5, frequency response
// of rational system functions. Poles at radius rp = 1 - pi * width / Fa set
// the width, since a pole pair that close to the circle has a -3 dB
// bandwidth of 2(1 - rp) radians per sample. Zeros at rz = 1 - (1 - rp) *
// 10^(-depth/20) set the depth, since at the notch frequency the response is
// (1 - rz) / (1 - rp) to first order. The section is then scaled to unit gain
// at whichever of DC and the Nyquist frequency is further from the notch, so
// it cuts where it is aimed and nowhere else. tests/reference/test_noise.cpp
// measures the depth and the width it actually gets against what was asked
// for.
//
// THE AUTOMATIC NOTCH is an adaptive line enhancer: B. Widrow et al.,
// "Adaptive Noise Cancelling: Principles and Applications", Proc. IEEE 63(12),
// December 1975, the configuration that cancels periodic interference with no
// external reference. A predictor sees the audio delayed by D samples and
// learns whatever it can predict across that delay; the notch's output is the
// prediction ERROR, so what it learned is what gets subtracted. The update is
// normalized LMS with leakage, from S.
// Haykin, "Adaptive Filter Theory", the normalized LMS chapter and the leaky
// LMS algorithm:
//
//   y  = sum_k w[k] * r[k],     r[k] = v[n - D - k*S]
//   e  = v[n] - y               the output
//   w  = leak * w + (mu / (eps + sum_k r[k]^2)) * e * r
//
// WHAT IT COSTS A VOICE. A voiced syllable is a harmonic series that holds
// for a tenth of a second or more, which is predictable across any delay, so
// the predictor learns some of it too. The small mu is what limits that: a
// heterodyne holds for seconds and a syllable does not, and a step that
// learns a tone over the order of a second takes only part of a vowel before
// it moves. Only part: on the synthetic voice of tests/engine/
// test_engine_noise.cpp with nothing to cancel, the automatic notch leaves an
// output SNR of 14.9 dB, so it is a control to switch on when there is a
// whistle and off when there is not. core/dsp/noise_reference.cpp has the sweep behind the
// step, and docs/noise.md the figures through the engine. The leakage keeps
// a filter with nothing left to cancel from wandering.
//
// THE STRIDE S IS THIS FILE'S, and it is why 128 taps are enough. At 48 kS/s a
// predictor's frequency resolution is about Fa / taps, and 128 adjacent taps
// resolve 375 Hz: the notch would take a wide bite out of whatever voice sits
// near the tone. The audio of a linear mode carries nothing above its
// passband's reach, so the predictor can read every S-th sample instead,
// with S chosen so Fa / S is still 2.5 times that reach, and the same 128 taps
// span S times as long. A USB receiver at 2.7 kHz gets S = 7 and resolves
// about 54 Hz. The images the stride creates at multiples of Fa / S fall where
// the passband put no audio, which is exactly why it is only offered on the
// linear modes.
//
// NOT ON CW. A CW operator is listening to a tone, which is the one thing an
// adaptive line enhancer exists to remove, so on CW the automatic notch is
// refused by name rather than left to cancel the signal. Not on the FM modes
// either, whose audio is not band-limited by the passband the way the stride
// needs.
//
// THE SUMS ARE SPLIT OVER LANES, which is part of the arithmetic and not a
// scheduling detail. The kernel runs kLanes invocations; lane l owns taps l,
// l + kLanes, l + 2*kLanes and so on, sums its own products in ascending tap
// order, and every lane then adds the kLanes partial sums in ascending lane
// order. The twin does exactly that. kLanes is its own specialization
// constant rather than the workgroup size, so the pipeline's local size is
// set to it and the answer cannot depend on a scheduling choice.

// Specialization constants of core/shaders/noise_line.comp at ids 1 to 3.
struct LineConfig {
    std::uint32_t lanes = 32;
    std::uint32_t taps = 128;

    // The history ring, a power of two holding delay + (taps - 1) * stride
    // samples of the notch's output below the current one.
    std::uint32_t history = 2048;

    friend constexpr bool operator==(const LineConfig&, const LineConfig&) = default;
};

inline constexpr std::uint32_t kLineNotch = 1U << 0;
inline constexpr std::uint32_t kLineAle = 1U << 1;
inline constexpr std::uint32_t kMaxLineStride = 8;

// The state buffer, in floats:
//   [0, 4)                  the notch's x[n-1], x[n-2], y[n-1], y[n-2]
//   [4]                     the history write position, an exact integer
//   [8, 8 + taps)           the predictor's weights
//   [8 + taps, + history)   the history ring
inline constexpr std::size_t kLineStatePosition = 4;
inline constexpr std::size_t kLineStateWeights = 8;
[[nodiscard]] constexpr std::size_t line_state_size(const LineConfig& config) {
    return kLineStateWeights + config.taps + config.history;
}

// Twelve packed words, the kernel's push constant block.
struct LineParams {
    std::uint32_t count = 0;

    // kLineNotch and kLineAle, either or both.
    std::uint32_t flags = 0;

    std::uint32_t stride = 1;

    // D, in audio samples, at least 1.
    std::uint32_t delay = 1;

    // The notch section: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2.
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;

    float mu = 0.0F;
    float leak = 1.0F;
    float eps = 1.0e-6F;
};

static_assert(sizeof(LineParams) == 12 * sizeof(std::uint32_t),
              "LineParams must be twelve packed 32-bit words to alias the kernel's push "
              "constant block");

// Twin of core/shaders/noise_line.comp. audio is processed in place, count
// samples of it; state is line_state_size(config) floats, read and written.
[[nodiscard]] Status reference_line(const LineConfig& config, const LineParams& params,
                                    RealSpan audio, RealSpan state);

[[nodiscard]] Status validate(const LineConfig& config, const LineParams& params);

// The notch section's coefficients for a notch at audio_hz, before the
// double-to-float rounding the kernel sees. Exposed so a test can measure the
// response the design promises rather than trusting the arithmetic.
struct NotchDesign {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

[[nodiscard]] Expected<NotchDesign> design_notch(double audio_hz, double depth_db,
                                                 double width_hz, SampleRate audio_rate);

// ---------------------------------------------------------------------------
// Spectral noise reduction
// ---------------------------------------------------------------------------
//
// Short-time spectral attenuation with a noise floor tracker, three published
// pieces put together:
//
//   the analysis and synthesis: overlap-add at 50 percent with a square-root
//   Hann window on both sides, whose squares sum to one at that overlap, so a
//   unit gain reconstructs the input exactly apart from float rounding. J. B.
//   Allen, "Short Term Spectral Analysis, Synthesis, and Modification by
//   Discrete Fourier Transform", IEEE Trans. ASSP 25(3), June 1977.
//
//   the gain: power spectral subtraction, G = 1 - alpha * N / S, with an
//   over-subtraction factor alpha and a spectral floor the gain may not fall
//   below. S. F. Boll, "Suppression of Acoustic Noise in Speech Using
//   Spectral Subtraction", IEEE Trans. ASSP 27(2), April 1979, for the
//   subtraction; M. Berouti, R. Schwartz and J. Makhoul, "Enhancement of
//   Speech Corrupted by Acoustic Noise", ICASSP 1979, for the over-
//   subtraction and the floor, which are what trade residual noise against
//   the warbling artefact subtraction is known for. S here is a smoothed
//   power rather than one frame's, for the same reason.
//
//   the noise estimate: continuous minimum tracking. G. Doblinger,
//   "Computationally Efficient Speech Enhancement by Spectral Minima Tracking
//   in Subbands", EUROSPEECH 1995. Per bin, with S the smoothed power:
//
//     if Pmin < S:  Pmin = gamma * Pmin + ((1 - gamma) / (1 - beta)) * (S - beta * S_prev)
//     else:         Pmin = S
//
//   It follows the floor down at once and up slowly, so it needs no voice
//   activity detector: speech lifts S for a syllable and the minimum barely
//   moves. A minimum sits below the mean of the power it tracks, so the
//   estimate is scaled up by a bias the conformance suite measured on white
//   noise; see kSpectralBias in core/dsp/noise_reference.cpp.
//
// ONE STRENGTH CONTROL, VrxParams::nr_strength in [0, 1], moves alpha and the
// floor together: alpha = 1 + s and the floor at -(6 + 12s) dB. docs/noise.md
// has what that does at 0, 0.5 and 1.
//
// A Wiener gain, S / N - 1 over S / N, was tried as the alternative the brief
// allowed and is the same function: with the SNR estimated from the smoothed
// power it reduces algebraically to 1 - N / S, which is the subtraction above
// at alpha = 1. A decision-directed estimate of the SNR in its place was
// measured on a double-precision model of this stage fed the synthetic voice
// of tests/engine/test_engine_noise.cpp as audio, with band-limited noise at
// 6 and 15 dB: 5.4 and 12.5 dB of output SNR against 6.4 and 15.0 for the
// subtraction at strength 0, from 4.6 and 13.6 untouched. The subtraction
// stayed.
//
// THE DFT IS DIRECT, NOT FAST. A frame of at most 512 samples is 131072
// multiply-adds each way, which a workgroup finishes in microseconds at audio
// frame rates, and a direct sum in ascending index order is one operation
// sequence a twin can transcribe. The trigonometric table is designed on the
// host in double and rounded once, so neither side computes a sine.
//
// LATENCY. One frame: an output sample is the input from N samples earlier,
// 10.7 ms at 48 kS/s. The stage's audio arrives that much later with
// reduction on than with it off, and the first frame after it is switched on
// is silence while the pipeline fills.

// Specialization constant of core/shaders/noise_spectral.comp at id 1: the
// frame length N, a power of two in [kMinSpectralFrame, kMaxSpectralFrame].
// The hop is N/2 and there are N/2 + 1 bins.
struct SpectralConfig {
    std::uint32_t frame = 512;

    friend constexpr bool operator==(const SpectralConfig&, const SpectralConfig&) = default;
};

// 512 is the ceiling because the kernel keeps its whole working set in
// workgroup memory, ten kilobytes at 512, and Vulkan promises only sixteen.
inline constexpr std::uint32_t kMinSpectralFrame = 64;
inline constexpr std::uint32_t kMaxSpectralFrame = 512;

// Ten packed words, the kernel's push constant block.
struct SpectralParams {
    std::uint32_t count = 0;

    // The power smoothing a and 1 - a, both rounded on the host, so neither
    // side forms the difference.
    float smoothing = 0.7F;
    float smoothing_complement = 0.3F;

    // Doblinger's gamma and beta, and (1 - gamma) / (1 - beta).
    float gamma = 0.998F;
    float beta = 0.96F;
    float rise = 0.05F;

    float bias = 1.0F;
    float alpha = 2.0F;
    float floor_gain = 0.2F;
    float eps = 1.0e-12F;
};

static_assert(sizeof(SpectralParams) == 10 * sizeof(std::uint32_t),
              "SpectralParams must be ten packed 32-bit words to alias the kernel's push "
              "constant block");

// The state buffer, in floats, for a frame of N and hop H = N/2, K = H + 1:
//   [0]                 samples into the current hop, an exact integer
//   [1]                 frames processed, saturating at 2
//   [2, 2 + N)          the last N inputs, older hop first
//   [.., + H)           the overlap-add tail
//   [.., + H)           the output queue for the current hop
//   [.., + K)           the smoothed power per bin
//   [.., + K)           the tracked minimum per bin
[[nodiscard]] constexpr std::size_t spectral_state_size(const SpectralConfig& config) {
    const std::size_t n = config.frame;
    return 2U + n + n / 2U + n / 2U + 2U * (n / 2U + 1U);
}

// The frame for an audio rate: the largest power of two at or below Fa / 90,
// about 11 ms, clamped to [64, 512]. 512 at 48 kS/s, which puts a bin every
// 94 Hz, fine enough to leave a voice's harmonics between the bins where the
// noise is.
[[nodiscard]] SpectralConfig spectral_config_for(SampleRate audio_rate);

// Four tables of N floats, end to end: cos(2 pi k / N), sin(2 pi k / N), the
// analysis window, and the synthesis window with the inverse transform's 1/N
// folded in.
[[nodiscard]] std::vector<float> design_spectral_tables(const SpectralConfig& config);

// The push constants for a strength in [0, 1], count left at zero.
[[nodiscard]] SpectralParams spectral_params(double strength);

// Twin of core/shaders/noise_spectral.comp. audio is processed in place;
// state is spectral_state_size(config) floats, read and written.
[[nodiscard]] Status reference_spectral(const SpectralConfig& config,
                                        const SpectralParams& params, ConstRealSpan tables,
                                        RealSpan audio, RealSpan state);

[[nodiscard]] Status validate(const SpectralConfig& config, const SpectralParams& params);

// ---------------------------------------------------------------------------
// From a receiver's request to what the three kernels run
// ---------------------------------------------------------------------------

// Whether each stage is OFFERED on a mode, which is a property of the mode
// and not of anything the operator set. A request for a stage that is not
// offered is refused by validate_noise_request with a sentence naming the
// mode, rather than accepted and quietly doing nothing.
//
// The blanker is offered on every mode that produces audio. The audio stages
// run on mono audio only, which excludes a stereo WFM receiver: its two
// channels are L and R, and a notch or a noise floor per channel would pull
// the stereo image around. The manual notch needs a mode whose audio
// frequency is a function of where a signal sits in the passband, which is
// the five linear modes; the automatic notch is those less CW.
[[nodiscard]] bool blanker_offered(engine::Demod mode);
[[nodiscard]] bool notch_offered(engine::Demod mode);
[[nodiscard]] bool auto_notch_offered(engine::Demod mode);
[[nodiscard]] bool noise_reduction_offered(engine::Demod mode);

// The audio frequency a notch placed at passband_hz lands on: passband_hz for
// USB, minus it for LSB, its magnitude for AM and DSB, and its sum with the
// pitch for CW, whose fine stage mixes the carrier to the pitch. Empty for a
// mode with no such mapping.
[[nodiscard]] std::optional<double> notch_audio_hz(engine::Demod mode, Hertz passband_hz,
                                                   Hertz cw_pitch);

// Refuses a request whose noise fields are out of range or ask for a stage
// the mode does not offer. Called by engine::place, so add_vrx and
// set_vrx_params both answer the caller rather than a stage refusing on the
// recording thread where nobody hears it.
[[nodiscard]] Status validate_noise_request(const engine::VrxParams& params);

// What a receiver's stage records, resolved from its request and its plan.
struct NoisePlan {
    bool blank = false;
    float blank_threshold = 0.0F;

    // Present when either notch runs, with count left at zero.
    bool line = false;
    LineParams line_params{};

    bool spectral = false;
    SpectralParams spectral_params{};
};

[[nodiscard]] Expected<NoisePlan> plan_noise(const engine::VrxParams& params,
                                             const VrxPlan& plan, const LineConfig& line);

}  // namespace revenant::dsp
