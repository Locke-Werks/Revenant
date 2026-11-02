// LPC-10, the 2400 bit/s linear predictive vocoder: 54-bit frames in, 180
// samples of 8 kHz PCM out, every 22.5 ms. An encoder for tests and
// modulators, and a frame synchroniser for a raw bit stream.
//
// SPECIFICATION
//
// Implements FED-STD-1015, "Telecommunications: Analog to Digital Conversion
// of Voice by 2,400 Bit/Second Linear Predictive Coding", 28 November 1984,
// as redesignated FIPS PUB 137 and carried with FIPS PUB 137 Change Notice 1
// of 17 December 1996 (which only makes LPC-10 non-mandatory and changes no
// technical clause). Read from the ten-page scan filed as Exhibit 1022 in
// IPR2017-01077, fetched 2026-11-02 from
// ptabdata.blob.core.windows.net/files/2017/IPR2017-01077/
// v23_Exhibit%201022%20-%20FS1015-LPC10.pdf, which is the copy Wikipedia's
// FIPS 137 article links. The scan's text layer is unreliable, so every table
// here was read from the page images, not from extracted text.
//
// Clauses used: 3.2.2 (8 kHz, 12-bit A/D), 3.2.3 (preemphasis 1 - 0.9375
// z^-1), 3.3.1 (2400 bit/s, 54-bit frames, 22.5 ms), 3.3.2 (bit allocation),
// 3.3.3 (bit assignment table), 3.3.4 (alternating sync bit), 3.4.1 and 3.4.2
// (pitch and voicing coding and decoding tables), 3.5 (RMS table), 3.6.1 to
// 3.6.9 (the ten reflection coefficient tables), 3.7 (the error control
// code), 3.8.1 (the 40-value voiced excitation and its overlap rule), 3.8.2
// (4th order unvoiced synthesis), 3.8.3 (voicing transitions), 3.8.4
// (de-emphasis 1 / (1 - 0.75 z^-1)).
//
// LPC-10e is the name the enhanced analysis of the mid-1980s goes by. The
// enhancements are on the transmit side (voicing classifier, pitch tracking,
// smoothing) and are not in FED-STD-1015; the bit stream they produce is the
// one this document defines, so the decoder here receives either.
//
// WHAT THE DOCUMENT LEAVES TO THE IMPLEMENTER, AND WHAT WAS CHOSEN
//
//  1. Analysis. The document specifies the preemphasis and the quantizers
//     and nothing else about the transmitter: no window, no pitch tracker,
//     no voicing rule. The encoder below is this project's own design and is
//     described where it is written. Any encoder that fills the tables
//     correctly is conforming.
//  2. Interpolation. 3.1.2 mentions "interpolation and other enhancements"
//     without defining them. RMS, the reflection coefficients and the pitch
//     period are interpolated linearly across the frame, applied once per
//     pitch epoch (voiced) or once per 45-sample block (unvoiced).
//  3. Gain. 3.5 transmits the RMS of the preemphasized speech. The decoder
//     scales each excitation epoch so that the all-pole filter's output has
//     that RMS, using the filter's power gain 1 / prod(1 - k_i^2).
//  4. Transitions. 3.8.3 says "typically" the adjacent frames supply the
//     parameters. Without a frame of lookahead, the voiced half of a
//     transition frame takes the last voiced pitch, and it is placed first
//     when the previous frame was voiced and second otherwise.
//  5. Invalid pitch codes, the (I) entries of 3.4.2, and uncorrectable error
//     control words (3.7). The document says nothing about what follows. The
//     decoder repeats the previous frame's parameters and reports it.
//  6. RC(1) and RC(2) index -16 (code 10000) is not in the 3.6.1 table. It
//     decodes as index -15.
//  7. Levels. 3.5 scales RMS to 0..511 and 3.2.2 asks for 12 bits, so the
//     float scale here is 2048 per unit: +-1.0 is the 12-bit converter's full
//     range on both sides.
//  8. 3.8.4's de-emphasis is not the inverse of 3.2.3's preemphasis (0.75
//     against 0.9375). The document says the difference is deliberate
//     filtering, and both are implemented exactly as printed.
//
// CLEAN ROOM
//
// No LPC-10 implementation was read: not the NSA/DoD reference C, not
// spandsp, not any port of either. Everything below is from the document
// above and from textbook linear prediction (Levinson-Durbin, the step-up
// recursion, lattice stability), which the document assumes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// 3.3.1: 54 bits every 22.5 ms at 2400 bit/s; 3.2.2: 8 kHz sampling.
inline constexpr std::size_t kLpc10FrameBits = 54;
inline constexpr std::size_t kLpc10FrameSamples = 180;
inline constexpr std::uint32_t kLpc10SampleRateHz = 8000;
inline constexpr std::size_t kLpc10Order = 10;

// 3.2.2 / 3.5: the RMS scale is a 12-bit converter's; +-1.0 float is +-2048.
inline constexpr float kLpc10FullScale = 2048.0F;

enum class Lpc10Voicing : std::uint8_t {
    Unvoiced,    // 3.4.1: pitch field all ZERO
    Transition,  // 3.4.1: pitch field all ONE
    Voiced,
};

// One frame of quantized parameters, as the table indexes the document
// transmits. rc[i] is the signed index of RC(i+1) (3.6, "index" column);
// rc[4..9] are ignored when the frame is not voiced.
struct Lpc10Frame {
    Lpc10Voicing voicing = Lpc10Voicing::Unvoiced;
    int pitch_period = 0;  // samples, 20..156, voiced frames only
    int rms_code = 0;      // 0..31, 3.5
    std::array<int, kLpc10Order> rc{};
};

// What came out of one 54-bit frame besides the parameters.
struct Lpc10Unpacked {
    Lpc10Frame frame;
    std::uint8_t sync = 0;           // bit 54
    bool pitch_invalid = false;      // a (I) code of 3.4.2
    int ec_corrected = 0;            // 3.7 words with one bit put right
    int ec_uncorrectable = 0;        // 3.7 words with two or more in error
};

// ---- Tables, exposed so the tests can cross-check the transcription -------

// 3.4.1: the 60 voiced pitch periods and their 7-bit codes, longest first.
struct Lpc10PitchCode {
    int period;
    std::uint8_t code;
};
[[nodiscard]] std::span<const Lpc10PitchCode> lpc10_pitch_encode_table() noexcept;

// 3.4.2: the decoding table. >0 is a pitch period, and the three markers
// below are the (U), (T) and (I) entries.
inline constexpr int kLpc10PitchU = 0;
inline constexpr int kLpc10PitchT = -1;
inline constexpr int kLpc10PitchI = -2;
[[nodiscard]] std::span<const int> lpc10_pitch_decode_table() noexcept;

// 3.5: RMS. code -> 9-bit value, and the lower edge of each code's range.
[[nodiscard]] int lpc10_rms_decode(int code) noexcept;
[[nodiscard]] int lpc10_rms_encode(double rms) noexcept;
[[nodiscard]] std::span<const std::int16_t> lpc10_rms_from_table() noexcept;

// 3.6: RC(k), k = 1..10. Index range per coefficient is
// [lpc10_rc_min_index(k), lpc10_rc_max_index(k)].
[[nodiscard]] int lpc10_rc_bits(int k) noexcept;
[[nodiscard]] int lpc10_rc_min_index(int k) noexcept;
[[nodiscard]] int lpc10_rc_max_index(int k) noexcept;
[[nodiscard]] double lpc10_rc_decode(int k, int index) noexcept;
[[nodiscard]] double lpc10_rc_from(int k, int index) noexcept;  // lower edge
[[nodiscard]] int lpc10_rc_encode(int k, double value) noexcept;

// 3.7: the four-bit error control word for a four-bit data nibble.
[[nodiscard]] std::uint8_t lpc10_ec_encode(std::uint8_t nibble) noexcept;

// 3.8.1: the 40 recommended excitation values.
[[nodiscard]] std::span<const std::int16_t> lpc10_excitation() noexcept;

// ---- Framing ----------------------------------------------------------------

// Bits are one per byte, 0 or 1, in transmission order (bit 1 first, 3.3.3).
[[nodiscard]] Status lpc10_pack(const Lpc10Frame& frame, std::uint8_t sync,
                                std::span<std::uint8_t> bits);
[[nodiscard]] Expected<Lpc10Unpacked> lpc10_unpack(std::span<const std::uint8_t> bits);

// ---- Decoder ----------------------------------------------------------------

struct Lpc10FrameReport {
    Lpc10Voicing voicing = Lpc10Voicing::Unvoiced;
    int pitch_period = 0;
    int rms = 0;  // decoded 9-bit value
    bool repeated = false;  // invalid pitch or uncorrectable EC: last frame reused
    int ec_corrected = 0;
    int ec_uncorrectable = 0;
    std::uint8_t sync = 0;
};

class Lpc10Decoder {
public:
    Lpc10Decoder();

    // bits: kLpc10FrameBits, one per byte. out: at least kLpc10FrameSamples.
    [[nodiscard]] Status decode(std::span<const std::uint8_t> bits, std::span<float> out);

    // The same, from parameters a caller already has.
    [[nodiscard]] Status synthesize(const Lpc10Frame& frame, std::span<float> out);

    void reset();

    [[nodiscard]] std::string_view name() const noexcept { return "LPC-10"; }
    [[nodiscard]] const Lpc10FrameReport& last_frame() const noexcept { return report_; }
    [[nodiscard]] std::string describe_last_frame() const;

private:
    struct Decoded {
        Lpc10Voicing voicing = Lpc10Voicing::Unvoiced;
        double pitch = 60.0;
        double rms = 0.0;
        std::array<double, kLpc10Order> rc{};
    };

    void run(const Decoded& now, std::span<float> out);
    void emit_epoch(int length, bool voiced, double frac);

    Decoded previous_{};
    double last_voiced_pitch_ = 60.0;
    bool have_previous_ = false;
    Lpc10Frame last_good_{};
    bool have_last_good_ = false;

    // Pending output and the tail of the overlapped excitation (3.8.1).
    std::vector<double> pending_;
    std::vector<double> excitation_tail_;
    std::array<double, kLpc10Order> filter_memory_{};
    double deemphasis_ = 0.0;
    std::uint32_t noise_ = 0x1234567U;
    std::uint8_t expected_sync_ = 0;

    // Per-frame interpolation endpoints while run() is producing epochs.
    const Decoded* from_ = nullptr;
    const Decoded* to_ = nullptr;

    Lpc10FrameReport report_{};
};

// ---- Encoder ----------------------------------------------------------------

// This project's own analysis (see note 1 above): 3.2.3 preemphasis,
// Hamming-windowed autocorrelation over 240 samples ending at the frame's
// end, Levinson-Durbin for the reflection coefficients, and a normalised
// autocorrelation pitch and voicing detector on a 1 kHz lowpassed copy,
// split per half frame to find transitions.
class Lpc10Encoder {
public:
    Lpc10Encoder();

    // pcm: kLpc10FrameSamples float samples. bits: kLpc10FrameBits.
    [[nodiscard]] Status encode(std::span<const float> pcm, std::span<std::uint8_t> bits);

    // The parameters only, unpacked; encode() is analyse() then lpc10_pack().
    [[nodiscard]] Expected<Lpc10Frame> analyse(std::span<const float> pcm);

    // The unquantized reflection coefficients of the last analysis, for the
    // tests' quantization distortion figure.
    [[nodiscard]] const std::array<double, kLpc10Order>& last_rc() const noexcept {
        return last_rc_;
    }

    void reset();

private:
    std::vector<double> history_;   // preemphasized, for LPC
    std::vector<double> lowpass_;   // lowpassed, for pitch
    double preemphasis_state_ = 0.0;
    std::array<double, 4> lowpass_state_{};
    std::uint8_t sync_ = 0;
    std::array<double, kLpc10Order> last_rc_{};
};

// ---- Frame synchronisation ---------------------------------------------------

// 3.3.4 is the only framing the stream has: bit 54 alternates. The
// synchroniser scores every one of the 54 phases by how many consecutive
// frames have alternated there, locks onto a phase that reaches kLockFrames
// and is strictly ahead of every other phase, and drops lock after
// kLoseFrames failures in a row. Random bits alternate half the time, so a
// wrong phase reaches 16 with odds of about 1 in 65536 per frame per phase.
class Lpc10FrameSync {
public:
    static constexpr int kLockFrames = 16;
    static constexpr int kLoseFrames = 4;

    // Feeds bits, one per byte. Each complete frame found while locked is
    // appended to frames.
    void push(std::span<const std::uint8_t> bits,
              std::vector<std::array<std::uint8_t, kLpc10FrameBits>>& frames);

    [[nodiscard]] bool locked() const noexcept { return locked_; }
    void reset();

private:
    std::deque<std::uint8_t> window_;  // the last kLpc10FrameBits + 1 bits
    std::array<int, kLpc10FrameBits> run_{};
    std::uint64_t seen_ = 0;
    bool locked_ = false;
    std::size_t phase_ = 0;
    int misses_ = 0;
};

}  // namespace revenant::decode
