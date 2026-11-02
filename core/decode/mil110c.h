// MIL-STD-188-110 Appendix C serial (single-tone) HF modem, the high data
// rates: receiver audio to user bits at 3200, 4800, 6400, 8000, 9600 and
// 12800 bit/s, fixed-frequency operation.
//
// SPECIFICATION
//
// MIL-STD-188-110C with Change Notice 1 (3 January 2012), Appendix C, "HF
// data modem waveforms for data rates above 2400 bps in 3 kHz bandwidth",
// distribution statement A, read from everyspec.com. A US Government work;
// the tables below are transcribed from it. Clauses used:
//
//   C.4         rates; 12800 bit/s is uncoded
//   C.5.1       2400 symbols/s on an 1800 Hz carrier, I on cos and Q on -sin
//   C.5.1.1     known symbols are unscrambled 8PSK, table C-I
//   C.5.1.2     table C-II (modulation per rate), C-III (dibit transcoding),
//               C-IV (tribit transcoding), C-V, C-VI, C-VII (16, 32 and
//               64QAM points)
//   C.5.1.3     scrambler x^9 + x^4 + 1, figure C-5, reset each data frame
//   C.5.2       figure C-6: 287-symbol preamble, then 72 frames of 256 data
//               and 31 known symbols, then 72 symbols of reinserted preamble
//   C.5.2.1.1   table C-VIII (the preamble), table C-IX (D0 D1 D2) and table
//               C-X (the three-bit rate and interleaver codes)
//   C.5.2.1.2   table C-XI (the 103-symbol reinserted preamble)
//   C.5.2.2     mini-probes and their signs, tables C-XII and C-XIII
//   C.5.3       tables C-XIV (input block), C-XV (interleaver size) and C-XVI
//               (interleaver increment); full tail-biting K=7 rate 1/2 code,
//               figure C-7, punctured to 3/4 by 111001; load at n * increment
//               modulo the size, fetch linearly
//   C.5.4.3     EOM 4B65A5B2, zeros to the end of the input block
//
// READINGS
//
// Four places where the document needs a reading, each chosen so the
// modulator in core/dsp/synth/mil110_mod.h and this receiver agree, and each
// settled at once by a real recording if wrong:
//
//   1. The text of table C-X is printed with its rows offset by one. Read
//      with the rate and interleaver codes on consecutive rows from 001 (3200
//      bit/s, 1 frame) to 110 (12800 bit/s, 72 frames), every entry of tables
//      C-IX and C-XII follows from it as C.5.2.1.1 and C.5.2.2 describe; the
//      test checks each printed D0 D1 D2 against that derivation.
//   2. Figure C-5 is read as a nine-cell register shifting right, the
//      rightmost cell's output added to the output of the x^4 cell and fed
//      into the leftmost, initialised to 000000001 with the 1 in the
//      rightmost cell. "The last (rightmost) three bits" are taken with the
//      rightmost as the least significant, which is how the worked example
//      (010 has the value 2) reads them.
//   3. Mini-probe k (1 to 72) of each 72-frame group takes its sign from
//      position (k - 1) mod 18 of "- - - - - - - + S0..S8 +", with S0..S5
//      the rate and interleaver codes of table C-X, a 1 being "-", and
//      S6..S8 table C-XIII for set (k - 1) / 18.
//   4. Interleaver blocks start at the first data frame after the
//      synchronisation preamble as well as after each reinserted preamble
//      (C.5.3.1 names only the reinserted one).
//
// The preamble's optional AGC blocks (C.5.2.1.1, N of 0 to 7) are supported
// by the transmitter; the receiver needs none.
//
// RECEIVER
//
// Not specified. Acquisition by differential correlation against the 184
// fixed preamble symbols, as in mil110.h; D0, D1 and D2 read from the phase
// of each 13-symbol Barker group; a least-squares solve of the decision
// feedback equaliser over the 287 known preamble symbols, then normalised LMS
// on every mini-probe and reinserted preamble and, with a smaller step, on
// decisions; the phase and timing loops of mil110_rx.h; max-log soft bits
// into a tail-biting Viterbi decoder, run as wrap-around Viterbi over the
// block with 48 steps of overlap at each end (Anderson and Hladik's
// circular decoding, as described in Lin and Costello, Error Control Coding,
// 2nd ed., section 12.7).
//
// The receiver acquires on the synchronisation preamble only. Acquisition on
// the reinserted preamble or on the mini-probe signs (C.4, the "autobaud"
// from the data portion) is not done; neither is the maximum message length
// of C.5.4.5.3.
//
// CLEAN ROOM
//
// No MIL-STD-188-110 modem implementation was read. STANAG 4539 was not read.

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dv_codes.h"
#include "core/decode/mil110_rx.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// C.5.2: frame structure.
inline constexpr std::size_t kMil110cPreambleSymbols = 287;
inline constexpr std::size_t kMil110cSyncSymbols = 184;
inline constexpr std::size_t kMil110cDataSymbols = 256;
inline constexpr std::size_t kMil110cProbeSymbols = 31;
inline constexpr std::size_t kMil110cFramesPerGroup = 72;
inline constexpr std::size_t kMil110cReinserted = 72;

// Table C-VIII, the first 184 symbols.
inline constexpr std::array<std::uint8_t, kMil110cSyncSymbols> kMil110cSyncPreamble = {
    1, 5, 1, 3, 6, 1, 3, 1, 1, 6, 3, 7, 7, 3, 5, 4, 3, 6, 6, 4, 5, 4, 0,
    2, 2, 2, 6, 0, 7, 5, 7, 4, 0, 7, 5, 7, 1, 6, 1, 0, 5, 2, 2, 6, 2, 3,
    6, 0, 0, 5, 1, 4, 2, 2, 2, 3, 4, 0, 6, 2, 7, 4, 3, 3, 7, 2, 0, 2, 6,
    4, 4, 1, 7, 6, 2, 0, 6, 2, 3, 6, 7, 4, 3, 6, 1, 3, 7, 4, 6, 5, 7, 2,
    0, 1, 1, 1, 4, 4, 0, 0, 5, 7, 7, 4, 7, 3, 5, 4, 1, 6, 5, 6, 6, 4, 6,
    3, 4, 3, 0, 7, 1, 3, 4, 7, 0, 1, 4, 3, 3, 3, 5, 1, 1, 1, 4, 6, 1, 0,
    6, 0, 1, 3, 1, 4, 1, 7, 7, 6, 3, 0, 0, 7, 2, 7, 2, 0, 2, 6, 1, 1, 1,
    2, 7, 7, 5, 3, 3, 6, 0, 5, 3, 3, 1, 0, 7, 1, 1, 0, 3, 0, 4, 0, 7, 3};

// C.5.2.2: the "+" mini-probe; "-" adds 4 to each symbol.
inline constexpr std::array<std::uint8_t, kMil110cProbeSymbols> kMil110cMiniProbe = {
    0, 0, 0, 0, 0, 2, 4, 6, 0, 4, 0, 4, 0, 6, 4, 2,
    0, 0, 0, 0, 0, 2, 4, 6, 0, 4, 0, 4, 0, 6, 4};

// C.5.2.1.1: the 13-chip code each of D0, D1, D2 is added to.
inline constexpr std::array<std::uint8_t, 13> kMil110cBarker = {0, 4, 0, 4, 0, 0, 4,
                                                                 4, 0, 0, 0, 0, 0};

// Table C-III: dibit (first fetched bit most significant) to 8PSK symbol.
inline constexpr std::array<std::uint8_t, 4> kMil110cDibit = {0, 2, 6, 4};
// Table C-IV: tribit to 8PSK symbol.
inline constexpr std::array<std::uint8_t, 8> kMil110cTribit = {1, 0, 2, 3, 6, 7, 5, 4};

// C.5.1.3: scrambler register initial value.
inline constexpr std::uint16_t kMil110cScramblerSeed = 1;

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

enum class Mil110cRate : std::uint8_t { Bps3200, Bps4800, Bps6400, Bps8000, Bps9600, Bps12800 };

// Table C-X names, by length in frames.
enum class Mil110cInterleave : std::uint8_t {
    UltraShort,  // 1 frame, 0.12 s
    VeryShort,   // 3
    Short,       // 9
    Medium,      // 18
    Long,        // 36
    VeryLong,    // 72, 8.64 s
};

struct Mil110cMode {
    Mil110cRate rate = Mil110cRate::Bps9600;
    Mil110cInterleave interleave = Mil110cInterleave::Short;
};

[[nodiscard]] int mil110c_bit_rate(Mil110cRate rate);
[[nodiscard]] std::size_t mil110c_frames(Mil110cInterleave interleave);
// Bits a data symbol carries: 2, 3, 4, 5, 6, 6 (table C-II).
[[nodiscard]] int mil110c_bits_per_symbol(Mil110cRate rate);

// Table C-X: three-bit codes; empty for 12800 bit/s with any interleaver but
// one frame (table C-IX: reserved).
[[nodiscard]] std::optional<std::array<std::uint8_t, 2>> mil110c_codes(Mil110cMode mode);
// Table C-IX, derived as C.5.2.1.1 describes.
[[nodiscard]] std::optional<std::array<std::uint8_t, 3>> mil110c_designators(Mil110cMode mode);
[[nodiscard]] std::optional<Mil110cMode> mil110c_mode_from_designators(
    std::array<std::uint8_t, 3> d);

struct Mil110cLayout {
    int bits_per_symbol = 0;
    std::size_t frames = 0;         // per interleaver block
    std::size_t interleaver = 0;    // table C-XV, bits; 0 at 12800
    std::size_t increment = 0;      // table C-XVI
    std::size_t input_bits = 0;     // table C-XIV; one frame of bits at 12800
    bool coded = true;
};

[[nodiscard]] Expected<Mil110cLayout> mil110c_layout(Mil110cMode mode);

// ---------------------------------------------------------------------------
// Building blocks, exposed for the transmitter and the tests
// ---------------------------------------------------------------------------

// The constellation a rate's data symbols are drawn from, indexed by symbol
// number: 8PSK (table C-I) for 3200 and 4800, else tables C-V to C-VII.
[[nodiscard]] std::span<const std::complex<float>> mil110c_constellation(Mil110cRate rate);

// C.5.1.3: the scrambler value for each of the 256 data symbols of a frame,
// `bits` bits at a time (3 for both PSK rates).
[[nodiscard]] std::array<std::uint8_t, kMil110cDataSymbols> mil110c_scrambler(int bits);

// The 103 known symbols of table C-XI, mini-probe 72 first.
[[nodiscard]] std::array<std::uint8_t, 103> mil110c_reinserted(std::array<std::uint8_t, 3> d);

// Mini-probe signs for probes 1 to 72 of a group: true is "-".
[[nodiscard]] std::array<bool, kMil110cFramesPerGroup> mil110c_probe_signs(Mil110cMode mode);

// C.5.3.2: the punctured, tail-biting code block for one input block.
[[nodiscard]] std::vector<std::uint8_t> mil110c_encode(ConstBitSpan input);

// The inverse: soft values for the punctured block in, SoftSpan convention
// (positive for 0), the input block out.
[[nodiscard]] std::vector<std::uint8_t> mil110c_decode(std::span<const float> soft);

// The whole transmission as complex symbols at unit peak amplitude: the
// preamble with `agc_blocks` blocks of AGC symbols first, then `data` with
// the EOM, padded to whole input blocks. One bit per byte.
[[nodiscard]] Expected<std::vector<std::complex<float>>> mil110c_transmit_symbols(
    Mil110cMode mode, ConstBitSpan data, int agc_blocks = 0);

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct Mil110cConfig {
    // Receiver audio, upper sideband, carrier at 1800 Hz. Any rate of at
    // least 6000 S/s.
    dsp::SampleRate rate = 9600;
    double carrier_hz = 1800.0;

    // Normalised differential correlation over the 184 fixed preamble
    // symbols. Noise alone reads about 0.08.
    double detect_threshold = 0.25;

    // Equaliser span either side of the symbol at T/2, and feedback taps.
    std::size_t ff_symbols = 7;
    std::size_t fb_taps = 8;
};

struct Mil110cMessage {
    Mil110cMode mode{};
    std::array<std::uint8_t, 3> designators{};
    // Input sample index of the first data symbol.
    std::int64_t position = 0;
    double offset_hz = 0.0;
    // Mean squared error on the mini-probes, unit 8PSK, and the SNR it
    // implies.
    double mse = 0.0;
    double snr_db = 0.0;
    // Symbol clock error, as in Mil110Message.
    double clock_ppm = 0.0;
    bool eom = false;
    std::vector<std::uint8_t> bits;   // one per byte
    std::vector<std::uint8_t> bytes;  // first bit least significant
};

class Mil110cDecoder {
public:
    [[nodiscard]] static Expected<Mil110cDecoder> create(const Mil110cConfig& config);

    // Appends every message the audio completes. The output depends only on
    // the concatenated input, never on how it was split into calls.
    void process(dsp::ConstRealSpan audio, std::vector<Mil110cMessage>& messages);
    void flush(std::vector<Mil110cMessage>& messages);
    void reset();

    [[nodiscard]] bool synced() const { return state_ != State::Search; }

private:
    using C = dsp::Complex32;

    Mil110cDecoder() = default;

    enum class State : std::uint8_t { Search, Data };

    void run(std::vector<Mil110cMessage>& messages);
    [[nodiscard]] C sample(std::int64_t pos, std::size_t m) const {
        return fe_.at(pos + mil110_rx::kSps * static_cast<std::int64_t>(m));
    }
    [[nodiscard]] double detect_metric(std::int64_t pos) const;
    [[nodiscard]] bool acquire(std::int64_t pos);
    void on_symbol(std::vector<Mil110cMessage>& messages);
    void on_block(std::vector<Mil110cMessage>& messages);
    void finish(bool eom, std::vector<Mil110cMessage>& messages);

    Mil110cConfig config_;
    mil110_rx::FrontEnd fe_;
    std::vector<C> sync_ref_;  // conjugated

    State state_ = State::Search;
    std::int64_t search_pos_ = 0;
    std::int64_t trigger_ = -1;

    std::int64_t origin_ = 0;
    double omega_ = 0.0;
    Mil110cMode mode_{};
    std::array<std::uint8_t, 3> designators_{};
    Mil110cLayout layout_{};
    std::array<std::uint8_t, 103> tail_{};
    std::array<bool, kMil110cFramesPerGroup> signs_{};
    std::array<std::uint8_t, kMil110cDataSymbols> scrambler_{};
    std::vector<std::complex<float>> points_;

    mil110_rx::Equaliser eq_;
    mil110_rx::Snapshot snap_;
    std::int64_t symbol_ = 0;

    std::vector<float> block_;
    std::size_t block_fill_ = 0;
    std::uint32_t shift_ = 0;
    std::size_t shift_count_ = 0;
    double mse_sum_ = 0.0;
    std::size_t mse_n_ = 0;
    double frame_err_ = 0.0;
    std::size_t frame_known_ = 0;
    int bad_frames_ = 0;
    Mil110cMessage message_;
};

}  // namespace revenant::decode
