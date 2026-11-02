// MIL-STD-188-110 39-tone parallel mode: receiver audio to the data bits the
// transmitting modem was given.
//
// SPECIFICATION
//
// MIL-STD-188-110B, 27 April 2000, Appendix B, "39-tone parallel mode",
// distribution statement A (approved for public release, unlimited), read
// from everyspec.com. A US Government work; the tables below are transcribed
// from it. Clauses used:
//
//   B.4       39 QDPSK data tones plus an unmodulated Doppler tone
//   B.5.2     shortened Reed-Solomon (15,11) over GF(16), x^4 + x + 1, the
//             generator g(x) = x^4 + a^13 x^3 + a^6 x^2 + a^3 x + a^10;
//             (14,10) at 2400 bit/s, (7,3) otherwise
//   B.5.3     table B-I interleaving degrees; figures B-2 and B-3, the data
//             flow through the encoder and the interleaver
//   B.5.4.1   the three part preamble; table B-II, its tones, amplitudes and
//             initial phases; B.5.4.2 the extended preamble
//   B.5.4.3   the framing sequence, f(x) = x^9 + x^7 + x^6 + x^4 + 1 in the
//             register of figure B-5, ending on the bit generated from state
//             111111111; table B-III, insertion intervals and lengths
//   B.5.5     22.5 ms signal elements, table B-IV tone frequencies and bit
//             locations, table B-V dibit to phase change; tones 33 to 39
//             repeat tones 1 to 7 below 2400 bit/s
//   B.5.6     in-band diversity of order d = 1200 / rate: B.5.6.1 and table
//             B-VI time and frequency, B.5.6.2 and table B-VII frequency only
//
// Readings the document leaves open, each an assumption a recording settles:
//
//   - Which bit of a 4-bit Reed-Solomon symbol is most significant. Figures
//     B-2 and B-3 give the order bits leave the encoder (D0 first) but not
//     their weight; this takes the first bit out as the MSB, and the first
//     symbol out as the highest-degree coefficient of a systematic codeword.
//   - Which of a tone's two bit locations in table B-IV is the "earlier bit"
//     of table B-V. This takes the odd-numbered location as the earlier.
//   - What a "phase change at the onset of each signal element" is measured
//     against. This reads it literally: the instantaneous phase of the tone
//     at one onset against its phase at the previous onset, so a data tone's
//     phase within an element runs from that onset. The Doppler tone is
//     unmodulated and runs continuously. The preamble timing search does not
//     depend on this reading; the data demodulator does.
//   - Time/frequency diversity words whose index falls before the first data
//     element are sent as SPACE.
//
// WHERE THIS STOPS
//
// At the synchronous bit stream (B.5.1 to B.5.6). The asynchronous mode of
// B.5.7, with its character tables B-VIII to B-XI, is not decoded: the bits
// are reported, and an asynchronous transmission reads as its packed
// characters with fill. The rate and interleaving degree may be given, or
// found from the framing sequence and, where table B-III gives two degrees
// the same framing length, from the Reed-Solomon failure count.
//
// CLEAN ROOM
//
// No implementation of this mode was read.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// Table B-IV: data tone k (1..39) at 675 + 56.25 (k - 1) Hz; Doppler 393.75.
inline constexpr double kTone39SpacingHz = 56.25;
inline constexpr double kTone39FirstHz = 675.0;
inline constexpr double kTone39DopplerHz = 393.75;
inline constexpr int kTone39DataTones = 39;
// B.5.5: 22.5 ms signal elements, 44.44 Bd.
inline constexpr double kTone39ElementSeconds = 0.0225;

[[nodiscard]] constexpr double tone39_frequency(int tone) {
    return kTone39FirstHz + kTone39SpacingHz * (tone - 1);
}

// Table B-II. Part 1: tones 3, 15, 27, 39 at amplitude 3; part 2: tones 9,
// 21, 33 at amplitude 4; part 3: every data tone at 1 and Doppler at 2.
struct Tone39PreambleTone {
    int tone = 0;  // data tone number, 0 for Doppler
    double amplitude = 0.0;
    double phase_deg = 0.0;
};
[[nodiscard]] std::span<const Tone39PreambleTone> tone39_preamble_part1();
[[nodiscard]] std::span<const Tone39PreambleTone> tone39_preamble_part2();
// Doppler first, then data tones 1..39.
[[nodiscard]] std::span<const Tone39PreambleTone> tone39_preamble_part3();

// B.5.4.1 and B.5.4.2: element counts of the three parts.
inline constexpr std::array<int, 3> kTone39Preamble = {14, 8, 1};
inline constexpr std::array<int, 3> kTone39ExtendedPreamble = {58, 27, 12};

// Table B-V: the phase change, in units of 45 degrees (1, 3, 5, 7), for a
// dibit; earlier and later are 0 (SPACE) or 1 (MARK).
[[nodiscard]] int tone39_phase_step(int earlier, int later);

enum class Tone39Diversity : std::uint8_t {
    TimeFrequency,  // B.5.6.1, table B-VI
    Frequency,      // B.5.6.2, table B-VII
};

struct Tone39Mode {
    int rate = 2400;   // 75, 150, 300, 600, 1200 or 2400 bit/s
    int degree = 1;    // a table B-I interleaving degree for the rate
    Tone39Diversity diversity = Tone39Diversity::TimeFrequency;
};

// One row of table B-III.
struct Tone39Framing {
    int rate = 0;
    int degree = 0;
    int superblocks = 0;    // super blocks between insertions
    int interval_bits = 0;  // the same, in bits
    int length = 0;         // framing sequence bits per insertion
};
[[nodiscard]] std::span<const Tone39Framing> tone39_framing_table();
[[nodiscard]] const Tone39Framing* tone39_framing(int rate, int degree);
[[nodiscard]] bool tone39_valid(const Tone39Mode& mode);

// B.5.4.3: the last `length` bits of the framing sequence, in transmit order,
// the final one being the SPACE generated from state 111111111.
[[nodiscard]] std::vector<std::uint8_t> tone39_framing_sequence(int length);

// ---------------------------------------------------------------------------
// Reed-Solomon over GF(16)
// ---------------------------------------------------------------------------

// B.5.2: g(x) coefficients, x^4 first: 1, a^13, a^6, a^3, a^10.
[[nodiscard]] std::array<std::uint8_t, 5> tone39_rs_generator();

// n = 14 (k = 10) at 2400 bit/s, n = 7 (k = 3) otherwise.
[[nodiscard]] constexpr int tone39_rs_n(int rate) { return rate == 2400 ? 14 : 7; }
[[nodiscard]] constexpr int tone39_rs_k(int rate) { return tone39_rs_n(rate) - 4; }

// Systematic: data symbols first, highest degree first, then four parity.
[[nodiscard]] std::vector<std::uint8_t> tone39_rs_encode(std::span<const std::uint8_t> data);

// Corrects up to two symbol errors in place. Returns the number corrected,
// or -1 when the word is not within two errors of a codeword.
int tone39_rs_decode(std::span<std::uint8_t> word);

// ---------------------------------------------------------------------------
// Coding, interleaving and diversity
// ---------------------------------------------------------------------------

// Data bits in one super block: degree * k * 4.
[[nodiscard]] int tone39_superblock_data_bits(const Tone39Mode& mode);
// Coded bits in one super block: degree * n * 4.
[[nodiscard]] int tone39_superblock_bits(const Tone39Mode& mode);

// Figures B-2 and B-3: one super block of data bits to its interleaved code
// bits in transmit order.
[[nodiscard]] std::vector<std::uint8_t> tone39_encode_superblock(const Tone39Mode& mode,
                                                                 std::span<const std::uint8_t> data);

struct Tone39BlockDecode {
    std::vector<std::uint8_t> data;
    int codewords = 0;
    int corrected = 0;  // symbols
    int failed = 0;     // codewords beyond correction
};
// Hard decisions from soft values, positive meaning MARK.
[[nodiscard]] Tone39BlockDecode tone39_decode_superblock(const Tone39Mode& mode,
                                                         std::span<const float> soft);

// The bits on the air after the preamble, before diversity: framing
// sequences and super blocks per table B-III, as many super blocks as the
// data fills (zero padded), ending with a whole super block.
[[nodiscard]] std::vector<std::uint8_t> tone39_stream(const Tone39Mode& mode,
                                                      std::span<const std::uint8_t> data);

// New stream bits each signal element carries: 78 at 2400, 64 * rate / 1200
// below.
[[nodiscard]] int tone39_bits_per_element(int rate);

// B.5.5 and B.5.6: the 78 table B-IV bit locations of every element that
// carries the stream, including the elements that finish the time diversity.
[[nodiscard]] std::vector<std::array<std::uint8_t, 78>> tone39_elements(
    const Tone39Mode& mode, std::span<const std::uint8_t> stream);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct Tone39Config {
    dsp::SampleRate rate = 8000;
    // Zero to find each from the signal.
    int data_rate = 0;
    int degree = 0;
    Tone39Diversity diversity = Tone39Diversity::TimeFrequency;
    bool auto_diversity = true;
    // Engineering choices. The widest carrier offset searched; and the
    // fraction of received power the four part 1 tones must hold, over 180
    // ms, for a preamble to be taken as present.
    double max_offset_hz = 75.0;
    double detect_threshold = 0.35;
};

struct Tone39Transmission {
    std::int64_t position = 0;     // sample at the start of preamble part 3
    double offset_hz = 0.0;        // carrier offset found from the preamble
    bool extended_preamble = false;
    Tone39Mode mode;
    int elements = 0;              // data signal elements received
    double framing_match = 0.0;    // first framing sequence, 1 when exact
    std::vector<std::uint8_t> data;
    int superblocks = 0;
    int codewords = 0;
    int corrected = 0;
    int failed = 0;
};

class Tone39Decoder {
public:
    [[nodiscard]] static Expected<Tone39Decoder> create(const Tone39Config& config);

    // Appends a transmission each time one ends. The output depends only on
    // the concatenated input, never on how it was split into calls.
    void process(dsp::ConstRealSpan audio, std::vector<Tone39Transmission>& out);
    // Ends the transmission in progress, as the end of a recording does.
    void flush(std::vector<Tone39Transmission>& out);
    void reset();

    [[nodiscard]] bool receiving() const { return state_ == State::Data; }

private:
    explicit Tone39Decoder(const Tone39Config& config);

    enum class State : std::uint8_t { Search, Timing, Data };

    using Cplx = std::complex<double>;
    [[nodiscard]] Cplx correlate(double hz, std::int64_t start) const;
    [[nodiscard]] float at(std::int64_t i) const;
    bool step();
    bool search();
    bool timing();
    bool data_element();
    void finish(std::vector<Tone39Transmission>& out);
    void trim();

    Tone39Config config_;
    double fs_ = 0.0;
    double element_ = 0.0;   // samples
    int window_ = 0;         // samples, 1 / 56.25 Hz
    int hop_ = 0;

    std::vector<float> buf_;
    std::int64_t base_ = 0;  // absolute index of buf_[0]
    std::int64_t end_ = 0;   // absolute index one past the last sample

    State state_ = State::Search;
    std::int64_t next_hop_ = 0;
    std::int64_t detect_ = 0;
    double offset_ = 0.0;

    // Data: window start of element 0 (preamble part 3) and the count.
    double t0_ = 0.0;
    int element_index_ = 0;
    bool extended_ = false;
    bool in_preamble_ = true;
    std::array<Cplx, 40> prev_{};
    double rho_ = 0.0;            // rotation per element, radians
    double doppler_power_ = 0.0;
    int weak_ = 0;
    std::vector<std::array<float, 78>> soft_;
    std::vector<Tone39Transmission> pending_;
};

}  // namespace revenant::decode
