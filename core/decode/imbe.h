// P25 Phase 1 IMBE vocoder, decode side: 144 channel bits in, 160 samples of
// 8 kHz PCM out, every 20 ms.
//
// SPECIFICATION
//
// Implements ANSI/TIA-102.BABA-2003, "Project 25 Vocoder Description",
// approved 23 December 2003, which reprints Digital Voice Systems' "Project 25
// Vocoder Description" version 1.3 of 1 November 1995. Sections 6 through 9
// and 11 are the decode side and all of them are implemented here. The encode
// side, sections 5 and 10, is not: nothing in this project transmits P25.
//
// Every clause number below is TIA-102.BABA's own. Equation numbers are the
// numbers printed in the document, so (46) here is equation (46) there.
//
// Where the document contradicts itself, or is silent on something the
// decoder cannot avoid deciding, the code follows one branch and says which at
// the place the choice is made. Eight such places are known and each carries a
// comment naming the alternatives. They are collected in the "WHERE THE
// DOCUMENT DISAGREES WITH ITSELF OR SAYS NOTHING" section below so that a
// reader checking the implementation against the standard does not have to
// find them one at a time.
//
// CLEAN ROOM
//
// No IMBE implementation was read while this was written, and none has ever
// been read by the session that wrote it. The standard was fetched as a PDF
// from the public archive at archive.org/details/TIA-102_Series_Documents,
// item file "TIA-102.BABA_Project_25_IMBE_Vocoder_Description.pdf", and every
// constant here came out of that file. The six annex tables were extracted
// from the PDF's own text layer by script rather than retyped, and each one
// was cross-checked against a constraint the standard states somewhere else
// before it was allowed into this file. Those cross-checks are reproduced as
// tests in tests/decode/test_imbe.cpp, so the transcription is checkable
// without the PDF in hand.
//
// docs/clean-room.md permits this outright: a published specification is the
// preferred source and the only one that needs no exception.
//
// WHY IMBE AND NOT AMBE
//
// AMBE and AMBE+2 have no published specification. There is no document to
// implement against, so there is no clean-room route to them at any price, and
// no patent expiry changes that. IMBE is the opposite case on both counts: the
// algorithm is specified in full, down to the quantizer tables, and the
// document is public. That is the whole reason this decoder exists and the
// other two do not.
//
// WHAT THE OPERATOR IS TOLD
//
// A decoder that is not producing speech must not look like a channel with
// nobody on it. Every call to decode() fills last_frame(), which says whether
// the frame was decoded, repeated or muted, which rule fired, how many bits
// each of the seven error control codes corrected, and what the running error
// rate is. describe_last_frame() renders the same thing as one line for a log.
//
// Muting in particular is loud in the report and quiet in the audio: the
// standard has the decoder emit comfort noise at about 1/6500 of full scale
// (section 7.8), which is silence to a listener. A caller that watches only
// the samples cannot tell that from a dead channel. That is the defect this
// report exists to prevent.
//
// WHERE THE DOCUMENT DISAGREES WITH ITSELF OR SAYS NOTHING
//
// Three contradictions and five silences. The contradictions are the ones that
// matter: a reader who implements the printed equation gets a decoder that
// does not work and no indication of why.
//
//  1. Equations (64) and (72) give the index relation between the quantizer
//     value b_m and the DCT coefficient C(i,k) as m = 6 + k + sum(J_n) over
//     n < i. That does not reproduce Annex G: it disagrees on 1080 of the 1272
//     entries, counted. The ordering stated in prose in sections 6.3.2 and
//     6.4.2, b8 onward walking blocks 1..6 and k = 2..J_i inside each block,
//     does reproduce Annex G exactly, for every L from 9 to 56. The prose and
//     the annex win; the summed term is sum(J_n - 1). See hoc_index() in the
//     implementation.
//
//  2. Section 7.7 makes a frame invalid when eps_0 >= 2 and eps_T >= 10 + 40
//     eps_R, equations (97) and (98), and section 7.8 mutes when eps_R > .0875.
//     Annex K Flow Chart 9 makes a frame invalid when eps_T >= 11 and
//     eps_0 >= 2, mutes when eps_R >= .085, and adds three cases the prose does
//     not have: b0 in 208..215 and b0 in 220..255 are invalid, b0 in 216..219
//     mutes outright, and the fourth consecutive invalid frame mutes instead of
//     repeating. The flow chart is followed, because it is a superset of the
//     prose and is the only one of the two that says what to do on a long
//     outage. Both thresholds are named at the test.
//
//  3. Annex K Flow Chart 11 labels the step that turns the code vectors
//     nu(4..6) into the bit vectors u(4..6) "[15,11] Hamming Encode". It is a
//     decode; the arrow in the same box runs nu -> u. Read as printed the
//     decoder could not work at all.
//
//  4. Annex A initialises nine decoder state variables and omits tau_M(-1),
//     the amplitude smoothing threshold of equation (115). It is initialised
//     here to 20480, the value equation (115) itself produces for a frame with
//     no errors, which is the only value that makes an error-free first frame
//     behave the same as an error-free hundredth frame.
//
//  5. Equations (99) through (104) list what a frame repeat carries forward
//     and do not list v_bar, the V/UV decisions after the adaptive smoothing of
//     equation (113). Synthesis reads v_bar and not v_tilde, and a repeat skips
//     the smoothing that would have produced v_bar, so v_bar is carried forward
//     too. Annex A initialises v_bar(-1) separately from v_tilde(-1), which is
//     the document agreeing that the two are distinct state.
//
//  6. Equation (139) requires psi_l to be updated every frame for l up to 56
//     "regardless of the value of L", and equation (140) then derives phi_l
//     from it only for l up to max[L(-1), L(0)]. A later frame can have a
//     larger L than either, and synthesis would then read a phi_l(-1) that was
//     never written. phi_l is computed for all 56 by the same rule, which is
//     the only extension that leaves the defined range unchanged.
//
//  7. Section 7.8 says a muted frame emits "random noise which is uniformly
//     distributed over the interval [-5, 5]" and does not say what generates
//     it. It comes from equation (117), the generator the unvoiced path
//     already uses, so a mute needs no second seed and stays reproducible.
//     This project's conventions require a stated seed for anything random,
//     and the standard supplies one: u(-105) = 3147.
//
//  8. Section 7.8 has a mute bypass speech synthesis, and does not say what
//     the weighted overlap add of equation (126) should carry into the next
//     frame when nothing was synthesized. The tail is set to zero, so the
//     first frame after a mute fades its unvoiced half in over the window
//     rather than joining onto whatever was last there.
//
// THE VOCODER SEAM
//
// core/decode/vocoder.h belongs to another branch and is not touched here, so
// ImbeDecoder does not derive from Vocoder and does not name VocoderFrame.
// What it does is carry that interface's shapes, so the adapter is a wrapper
// and not a rewrite:
//
//   decode(std::span<const std::uint8_t>, std::span<float>) -> Status
//   reset() -> void
//   name() const noexcept -> std::string_view
//
// are the seam's signatures exactly. VocoderFrame's four fields are the four
// constants below: kChannelFrameBits or kBitVectorBits, kPcmFrames, and
// kSampleRateHz.
//
// One of those four needs a decision rather than a translation. The seam's
// comment puts bit_count at 88 for IMBE, "before FEC", which is the eight
// prioritized bit vectors of section 7.1 and nothing else. A P25 Phase 1
// voice frame on the air is 144 bits, and the forward error control inside
// those 144 is where the decoder learns that a frame is unusable: without it
// there is no eps_T, so sections 7.6 through 7.8 cannot run and a corrupted
// frame is synthesized as though it were speech. decode() therefore takes
// either length and says which it got, so whichever number lane C lands on,
// the caller that has the whole frame can hand it over and get the error
// handling with it.

// NOT BIT EXACT AGAINST A GPU TWIN
//
// The project's rule that every kernel agrees with its scalar twin to zero ULP
// does not reach this file, because there is no kernel: a vocoder frame is 160
// samples and the work is serial across frames. What stands in its place is
// determinism, which is tested: the same channel bits produce the same samples
// to the last bit, whatever order the caller feeds frames in and whatever it
// does between them.
//
// The standard prints no test vector and no golden buffer. It prints one
// worked example, section 10, and that example is an encoder example: it runs
// spectral amplitudes forward into quantizer values, which is the direction
// this file does not implement. So there is nothing here to assert against a
// number the document supplies, and the tests assert against invariants the
// document states instead.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/error.h"

namespace revenant::decode {

// What happened to the frame that was just handed in.
enum class ImbeFrameState : std::uint8_t {
    // Decoded. The samples came from this frame's own model parameters.
    Decoded,
    // The frame was rejected and the previous frame's parameters were
    // synthesized again. Section 7.7.
    Repeated,
    // The frame was rejected and the output is comfort noise. Section 7.8.
    Muted,
};

// Which rule in Annex K Flow Chart 9 decided. Named rather than boolean
// because "repeated" alone does not tell an operator whether the radio is out
// of range or the transmitter is sending a reserved pitch code.
enum class ImbeFrameCause : std::uint8_t {
    // No rule fired.
    None,
    // eps_T >= 11 and eps_0 >= 2: the error control codes corrected about as
    // many bits as they can, which is what a wrongly demodulated frame looks
    // like. Flow Chart 9 and section 7.4.
    ErrorBurst,
    // b0 in 208..215 or 220..255, outside the 0..207 the encoder uses for a
    // pitch estimate. Section 6.1 and Flow Chart 9.
    ReservedPitchIndex,
    // b0 in 216..219, which Flow Chart 9 routes straight to a mute.
    MuteRequestPitchIndex,
    // eps_R >= .085, the running error rate. Flow Chart 9.
    ErrorRate,
    // The fourth invalid frame in a row, which mutes rather than repeating a
    // fourth time. Flow Chart 9.
    FourthConsecutiveInvalid,
};

// Everything the decoder learned about one frame. Filled on every call to
// decode(), including the ones that refuse.
struct ImbeFrameReport {
    ImbeFrameState state = ImbeFrameState::Decoded;
    ImbeFrameCause cause = ImbeFrameCause::None;

    // eps_i, the bits corrected by each of the four [23,12] Golay codes and
    // three [15,11] Hamming codes, indexed by code vector. Section 7.6. Zero
    // throughout when the caller hands in bit vectors rather than a channel
    // frame, because there is then no code to correct anything.
    std::array<std::uint8_t, 7> corrected = {};

    // eps_T, equation (95).
    std::uint32_t errors_total = 0;

    // eps_R(0), equation (96). Runs across frames and survives a repeat.
    double error_rate = 0.0;

    // How many invalid frames have arrived in a row, this one included.
    std::uint32_t consecutive_invalid = 0;

    // b0 as received, before it is checked against 0..207. Section 6.1.
    std::uint32_t pitch_index = 0;

    // The model parameters actually synthesized, which on a repeat are the
    // previous frame's. L, K and the fundamental of equations (46) to (48).
    std::uint32_t harmonics = 0;
    std::uint32_t voiced_bands = 0;
    double fundamental_hz = 0.0;

    // How many of the L spectral amplitudes were voiced after the adaptive
    // smoothing of equation (113). L and this number together are the shape of
    // the sound: all voiced is a tone, none voiced is a hiss.
    std::uint32_t voiced_harmonics = 0;
};

// Four pieces of the bit manipulation layer, exposed because otherwise there
// is nothing here a test can assert against. The standard prints no test
// vector and no golden buffer for the decode direction, so what it does print
// about these four is all the external truth available, and each one has a
// property stated in the document that a mis-transcription breaks:
//
//   - Annex H is a bijection onto the 144 code vector bits, and section 7.5
//     states that two bits of the same error control code are never closer
//     than three symbols.
//   - Figure 22 draws the priority scan in full for L = 16.
//   - Sections 7.3 to 7.5 in the transmit direction are the exact inverse of
//     what decode() undoes, so a frame built with imbe_pack_frame and handed
//     straight back must come out as the bit vectors that went in.
//   - Annex C is an even window and Annex I is a trapezoid, and both are
//     printed value by value.
//
// None of this is the IMBE encoder. Nothing here quantizes a spectral
// amplitude or estimates a pitch; sections 5 and 10 are not implemented.

// Annex H. Index is the position in the 144 bit frame, two bits per symbol,
// Bit 1 of a symbol before Bit 0. The value is the code vector index times 32
// plus the bit number within that vector, where bit N-1 is the MSB.
[[nodiscard]] std::span<const std::uint16_t> imbe_frame_map() noexcept;

// Figure 22, the priority scan of b3 through b(L+1) for one value of L. Fills
// cells in scan order with the quantizer value index times 16 plus the bit
// number within that value, and returns how many it wrote. The caller's span
// must hold at least 70, which is the longest scan any L produces.
[[nodiscard]] Expected<std::size_t> imbe_priority_scan(
    std::uint32_t harmonics, std::span<std::uint16_t> cells);

// Sections 7.3, 7.4 and 7.5 in the transmit direction: error control coding,
// bit modulation and the intra-frame interleave, applied to the eight bit
// vectors u0..u7. vectors is eight entries wide, widths 12, 12, 12, 12, 11,
// 11, 11 and 7 with bit N-1 the MSB, and frame receives 144 bytes of 0 or 1.
[[nodiscard]] Status imbe_pack_frame(std::span<const std::uint32_t> vectors,
                                     std::span<std::uint8_t> frame);

// Annex I, the speech synthesis window, and Annex C, the pitch refinement
// window. Both are zero outside the range the annex prints.
[[nodiscard]] double imbe_synthesis_window(int n) noexcept;
[[nodiscard]] double imbe_pitch_refinement_window(int n) noexcept;

class ImbeDecoder {
public:
    // One 20 ms channel frame at 7200 bit/s, 88 bits of model parameters plus
    // 56 of forward error control. Section 7.3.
    static constexpr std::size_t kChannelFrameBits = 144;

    // The same frame with the error control stripped: the eight prioritized
    // bit vectors u0..u7, 12+12+12+12+11+11+11+7. Section 7.1. decode()
    // accepts this too, for a caller whose FEC lives somewhere else.
    static constexpr std::size_t kBitVectorBits = 88;

    static constexpr std::size_t kPcmFrames = 160;
    static constexpr std::uint32_t kSampleRateHz = 8000;

    // Section 11.1: the synthetic speech signal "is suitable for digital to
    // analog conversion with a sixteen bit converter", so the standard's
    // s_tilde(n) lives on a +-32768 scale. decode() divides by this, because
    // every other audio surface in this engine is float32 in +-1. The mute
    // level of section 7.8, +-5, therefore arrives as about 1.5e-4.
    static constexpr float kFullScale = 32768.0F;

    ImbeDecoder();

    // bits is one per byte, 0 or 1, in transmission order, and must be
    // kChannelFrameBits or kBitVectorBits long. out is the caller's and must
    // hold at least kPcmFrames.
    //
    // A refusal names the number it got, the number it wanted and what to do,
    // and leaves out untouched. It does not consume decoder state, so a caller
    // that fixes the call and retries gets the same answer it would have got
    // first time.
    [[nodiscard]] Status decode(std::span<const std::uint8_t> bits,
                                std::span<float> out);

    // Back to the Annex A initial state. Everything: the model parameters, the
    // phases, the noise sequence, the error rate and the overlap-add tail.
    void reset();

    [[nodiscard]] std::string_view name() const noexcept { return "IMBE"; }

    [[nodiscard]] const ImbeFrameReport& last_frame() const noexcept {
        return report_;
    }

    // One line for a log: state, cause, corrected bits, error rate, L, K, how
    // many harmonics were voiced and the fundamental in hertz.
    [[nodiscard]] std::string describe_last_frame() const;

    // gamma_w of equation (121), the unvoiced scaling coefficient. It is a
    // function of two fixed windows and nothing else, so it is a constant of
    // the standard rather than of a frame. Exposed because a test can then
    // check the two annex windows were transcribed correctly by checking one
    // number.
    [[nodiscard]] static double unvoiced_scaling_coefficient() noexcept;

private:
    static constexpr std::size_t kMaxHarmonics = 56;
    static constexpr std::size_t kDftSize = 256;
    static constexpr std::size_t kNoiseWindowSpan = 209;  // n = -104..104

    struct Parameters {
        std::uint32_t harmonics = 0;   // L_tilde
        std::uint32_t bands = 0;       // K_tilde
        double fundamental = 0.0;      // omega_tilde_0, radians per sample
        // Unenhanced amplitudes, equation (77). The next frame predicts from
        // these and not from the enhanced ones, section 8.
        std::array<double, kMaxHarmonics + 2> unenhanced = {};
        // Enhanced and smoothed amplitudes, equations (110) and (116). These
        // are what synthesis reads.
        std::array<double, kMaxHarmonics + 2> enhanced = {};
        // V/UV per spectral amplitude, equation (51), and after the smoothing
        // of equation (113).
        std::array<std::uint8_t, kMaxHarmonics + 2> voiced = {};
        std::array<std::uint8_t, kMaxHarmonics + 2> voiced_smoothed = {};
    };

    [[nodiscard]] Status unpack(std::span<const std::uint8_t> bits);
    void decode_parameters();
    void enhance_and_smooth();
    void synthesize(std::span<float> out);
    void synthesize_mute(std::span<float> out);
    void advance_noise();
    void rotate();

    // u0..u7, each as an unsigned integer with bit N-1 the MSB. Section 7.1.
    std::array<std::uint32_t, 8> u_ = {};
    // b0..b(L+2), the quantizer values. Section 6.
    std::array<std::uint32_t, kMaxHarmonics + 3> b_ = {};

    Parameters current_;
    Parameters previous_;

    // Section 8, equation (111).
    double local_energy_ = 0.0;
    // Section 9, equation (115).
    double amplitude_threshold_ = 0.0;
    // Section 7.6, equation (96).
    double error_rate_ = 0.0;

    // Equations (139) and (140). Tracked for all 56 possible harmonics every
    // frame regardless of L, as section 11.3 requires.
    std::array<double, kMaxHarmonics + 1> psi_ = {};
    std::array<double, kMaxHarmonics + 1> phi_ = {};
    std::array<double, kMaxHarmonics + 1> phi_previous_ = {};

    // u(n) for the current frame, n = -104..104 at index n + 104. Section 11.2.
    std::array<double, kNoiseWindowSpan> noise_ = {};
    double noise_state_ = 0.0;
    bool noise_primed_ = false;

    // u_tilde_w(n, -1), n = -128..127 at index n + 128. Equation (126).
    std::array<double, kDftSize> unvoiced_tail_ = {};

    std::uint32_t consecutive_invalid_ = 0;
    ImbeFrameReport report_;
};

}  // namespace revenant::decode
