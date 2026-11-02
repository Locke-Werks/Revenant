// MIL-STD-188-110 serial (single-tone) HF modem: receiver audio to user bits,
// 75 to 4800 bit/s, fixed-frequency operation.
//
// SPECIFICATION
//
// MIL-STD-188-110C with Change Notice 1, Interoperability and Performance
// Standards for Data Modems, distribution statement A (approved for public
// release, unlimited), read from everyspec.com. A US Government work; the
// tables below are transcribed from it. Clauses used:
//
//   5.3.2.2.1   preamble length: 0.6 s for the zero and short interleaver,
//               4.8 s for the long
//   5.3.2.2.2   data frames: 32 unknown then 16 known symbols at 2400 and
//               4800 bit/s, 20 and 20 at 150 to 1200, no probes at 75
//   5.3.2.3.1   the EOM sequence 4B65A5B2, most significant digit first
//   5.3.2.3.2   flush: 144 coder bits, then zeros to the end of the block
//   5.3.2.3.3   figure 4: K=7, T1 = x6+x4+x3+x+1, T2 = x6+x5+x4+x3+1, T1
//               first; table V: repetition by pairs at 300 and 150 bit/s
//   5.3.2.3.4   interleaver load (row step 9 modulo 40; 7 modulo 20 or 10
//               at 75 bit/s) and table VI (matrix dimensions)
//   5.3.2.3.5   interleaver fetch (column step -17; -7 at 75 bit/s) and
//               table VII (bits per channel symbol)
//   5.3.2.3.6   tables VIII and IX, modified Gray decoding
//   5.3.2.3.7   symbol formation, figure 5, tables X, XI, XII and XIII
//   5.3.2.3.8   scramblers: figure 6 (data, period 160) and the 32-symbol
//               sync sequence
//   5.3.2.3.9   8-PSK on an 1800 Hz carrier, 2400 symbols/s
//
// STANAG 4415 AND 4539
//
// 5.3.1.3 f of the same document makes the robust 75 bit/s mode "in
// accordance with STANAG 4415", which is not freely available. The 75 bit/s
// rate here is the 110C mode in 5.3.2.3.7.1.1 (32-symbol Walsh-like sets, no
// probes, the long interleaver giving 4.8 s), which is the public description
// of that waveform; this file does not claim conformance to the STANAG text.
// STANAG 4539 corresponds to the 3200 to 9600 bit/s medium data rate
// waveform of Appendix C (5.3.1.3 c), which this file does not implement.
// 110C's Appendix C still calls itself "a non-mandatory part of
// MIL-STD-188-110B" (C.1.2), so it is the 110B appendix carried forward;
// docs/modes.md records the 4539 equivalence. Neither 110B nor STANAG 4539
// was read for this file.
//
// DEVIATIONS AND READINGS
//
// Five places where the document needs a reading, each chosen so the
// modulator in core/dsp/synth/mil110_mod.h and this receiver agree, and each
// settled at once by a real recording if wrong:
//
//   1. Table XII prints 10 -> 5 and 01 -> 6, but the text above it says a "1"
//      is prefixed to the two-bit value, and the worked note (23 = 010111 ->
//      5, 5, 7) only holds that way. The text and the note are followed:
//      symbol = 4 + count bits.
//   2. 5.3.2.3.7.1.2 says the D1 and D2 probe patterns are Table XIII
//      "repeated twice rather than four times" to make 16 symbols in all.
//      That is 8 symbols each, so each is the 8-symbol unit of Table XIII
//      once.
//   3. "Every 45th set" (75 bit/s exceptional sets, Table X b) is read as
//      sets 45, 90, ... counted from 1, the last set of each interleaver
//      block, matching the D1/D2 probes that precede each new block.
//   4. The D1/D2 probe frame is the last frame of each interleaver block,
//      "preceding the transmission of each new interleaved block".
//   5. Figure 4 shows the input entering the x6 cell. Taking that cell as
//      the newest bit, T1 taps delays 0,2,3,5,6 and T2 delays 0,1,2,3,6.
//
// The transmit pulse shape is not specified (5.3.2.3.9 note only asks for
// filtering); the receiver's matched filter is a root raised cosine with a
// roll-off of 0.35, and the equaliser absorbs whatever the transmitter used.
//
// The zero and short interleaver settings send the same D1/D2 (Table XI
// note and 5.3.2.3.7.2.1 note), so they cannot be told apart on the air;
// Mil110Config::short_is_zero says which to assume.
//
// Data bits are reported as received. bytes packs them first bit into the
// least significant position, the asynchronous serial convention; the
// document does not define a byte order for user data.
//
// RECEIVER
//
// Not specified by the document, which describes only the transmitter. The
// receiver is written from textbook theory:
//
//   - Preamble detection by differential correlation against the first nine
//     channel symbols of a 200 ms segment (288 known symbols): the product of
//     successive matched-filter outputs strips the carrier offset, and its
//     angle estimates it (Mengali and D'Andrea, Synchronization Techniques for
//     Digital Receivers, chapter 3, data-aided frequency estimation).
//   - A fractionally spaced (T/2) decision feedback equaliser adapted by
//     normalised LMS on every known symbol (the preamble, and the probes the
//     standard calls channel probes) and with a small step on decisions
//     (Proakis, Digital Communications, 5th ed., sections 9.5 and 10.1),
//     with a second-order decision-directed phase loop in front of the
//     decision.
//   - Max-log soft bits into a soft-decision Viterbi decoder.
//   - Symbol timing tracked by a minimum-MSE timing loop with an integrator,
//     so a sound card clock 100 ppm off (14 symbols of slip over a minute)
//     decodes; mil110_rx.h describes it. The baseband runs at four samples a
//     symbol so the equaliser's T/2 taps can be read between samples.
//
// CLEAN ROOM
//
// No MIL-STD-188-110 modem implementation was read.

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
#include "core/decode/viterbi_stream.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// 5.3.2.1 and 5.3.2.3.9.
inline constexpr double kMil110CarrierHz = 1800.0;
inline constexpr double kMil110SymbolRate = 2400.0;

// 5.3.2.3.1, most significant digit first.
inline constexpr std::uint32_t kMil110Eom = 0x4B65A5B2U;

// 5.3.2.3.2: coder flush bits.
inline constexpr std::size_t kMil110FlushBits = 144;

// 5.3.2.3.7.2.1: 15 channel symbols per 200 ms segment, each 32 tribits
// (5.3.2.3.7.2.2).
inline constexpr std::size_t kMil110SegmentSymbols = 480;

// 5.3.2.3.8.2: the sync scrambling sequence, 7 first.
inline constexpr std::array<std::uint8_t, 32> kMil110SyncScrambler = {
    7, 4, 3, 0, 5, 1, 5, 0, 2, 2, 1, 1, 5, 7, 4, 3,
    5, 0, 2, 6, 2, 1, 6, 2, 0, 0, 5, 0, 5, 2, 6, 6};

// Table XIII: the 8-tribit unit each 3-bit channel symbol repeats four times.
inline constexpr std::array<std::array<std::uint8_t, 8>, 8> kMil110SyncPatterns = {{
    {0, 0, 0, 0, 0, 0, 0, 0},
    {0, 4, 0, 4, 0, 4, 0, 4},
    {0, 0, 4, 4, 0, 0, 4, 4},
    {0, 4, 4, 0, 0, 4, 4, 0},
    {0, 0, 0, 0, 4, 4, 4, 4},
    {0, 4, 0, 4, 4, 0, 4, 0},
    {0, 0, 4, 4, 4, 4, 0, 0},
    {0, 4, 4, 0, 4, 0, 0, 4},
}};

// 5.3.2.3.7.2.1: the fixed channel symbols that open every segment.
inline constexpr std::array<std::uint8_t, 9> kMil110SegmentHead = {0, 1, 3, 0, 1, 3, 1, 2, 0};

// Table VIII: interleaver tribit (first bit most significant) to tribit
// number. Table IX: interleaver dibit to the 2-bit channel symbol value.
inline constexpr std::array<std::uint8_t, 8> kMil110Mgd3 = {0, 1, 3, 2, 7, 6, 4, 5};
inline constexpr std::array<std::uint8_t, 4> kMil110Mgd2 = {0, 1, 3, 2};

// Figure 6: the data randomiser's initial state, BAD hexadecimal.
inline constexpr std::uint16_t kMil110DataScramblerSeed = 0xBAD;
inline constexpr std::size_t kMil110DataScramblerPeriod = 160;

// Figure 4, reading 5: D^0 (the x6 cell) in the least significant bit.
inline constexpr std::uint32_t kMil110G1 = 0x6D;  // T1: delays 0,2,3,5,6
inline constexpr std::uint32_t kMil110G2 = 0x4F;  // T2: delays 0,1,2,3,6

// ---------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------

enum class Mil110Rate : std::uint8_t {
    Bps75,
    Bps150,
    Bps300,
    Bps600,
    Bps1200,
    Bps2400,
    Bps2400Voice,  // Table XI's "2400 (Digital voice)", same waveform here
    Bps4800,
};

enum class Mil110Interleave : std::uint8_t { Zero, Short, Long };

struct Mil110Mode {
    Mil110Rate rate = Mil110Rate::Bps2400;
    Mil110Interleave interleave = Mil110Interleave::Short;
};

[[nodiscard]] int mil110_bit_rate(Mil110Rate rate);

// Table XI. Zero interleave sends the short designators. Empty for the
// reserved combinations (4800 and digital voice with the long interleaver).
[[nodiscard]] std::optional<std::array<std::uint8_t, 2>> mil110_designators(Mil110Mode mode);

// The inverse of Table XI; empty for a combination not in the table.
[[nodiscard]] std::optional<Mil110Mode> mil110_mode_from_designators(std::uint8_t d1,
                                                                     std::uint8_t d2,
                                                                     bool short_is_zero);

// Everything the data phase needs to know about a mode.
struct Mil110Layout {
    int bits_per_symbol = 0;      // Table VII; 2 per 32-symbol set at 75
    std::size_t unknown = 0;      // 5.3.2.2.2; at 75, 32 (one set)
    std::size_t known = 0;        // 0 at 75
    bool coded = true;            // false at 4800
    int repeat = 1;               // Table V: pairs sent 2 or 4 times
    std::size_t rows = 0;         // Table VI; 0 with no interleaver
    std::size_t cols = 0;
    std::size_t row_step = 9;     // 5.3.2.3.4
    std::size_t col_step = 17;    // 5.3.2.3.5
    std::size_t block_symbols = 0;  // 5.3.2.3.7.1.2: 1440 or 11520
    std::size_t preamble_segments = 0;  // 3 or 24
};

[[nodiscard]] Expected<Mil110Layout> mil110_layout(Mil110Mode mode);

// ---------------------------------------------------------------------------
// Building blocks, exposed for the transmitter and the tests
// ---------------------------------------------------------------------------

// 5.3.2.3.8.1: the 160 scrambler values of one period.
[[nodiscard]] std::array<std::uint8_t, kMil110DataScramblerPeriod> mil110_data_scrambler();

// Table XII as the text reads it (reading 1): count 0..63 to C1, C2, C3.
[[nodiscard]] std::array<std::uint8_t, 3> mil110_count_symbols(int count);

// The interleaver matrix position (row, column) of the k-th bit loaded
// (5.3.2.3.4) and of the j-th bit fetched (5.3.2.3.5).
struct Mil110Cell {
    std::size_t row = 0;
    std::size_t col = 0;
};
[[nodiscard]] Mil110Cell mil110_load_cell(const Mil110Layout& layout, std::size_t k);
[[nodiscard]] Mil110Cell mil110_fetch_cell(const Mil110Layout& layout, std::size_t j);

// The unscrambled tribits of the known part of data frame `frame` (counted
// from 0 at the start of the data phase): zeros, except D1 and D2 in the
// frame that ends each interleaver block.
[[nodiscard]] std::vector<std::uint8_t> mil110_probe(const Mil110Layout& layout,
                                                     std::array<std::uint8_t, 2> designators,
                                                     std::size_t frame);

// The whole transmission as scrambled tribit numbers (0..7, 45 degrees
// each): preamble, then `data` with the EOM and the flush. One bit per byte.
[[nodiscard]] Expected<std::vector<std::uint8_t>> mil110_transmit_symbols(Mil110Mode mode,
                                                                          ConstBitSpan data);

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct Mil110Config {
    // Receiver audio, upper sideband, carrier at 1800 Hz. Any rate of at
    // least 6000 S/s; it is resampled to two samples a symbol inside.
    dsp::SampleRate rate = 9600;
    double carrier_hz = kMil110CarrierHz;

    // Engineering choice: normalised differential correlation over the 288
    // known symbols. Noise alone reads about 0.06; a clean preamble reads
    // near 1, and 0.33 at a symbol SNR of -3 dB.
    double detect_threshold = 0.2;

    // Assume a short-interleave designator means zero interleave.
    bool short_is_zero = false;

    // Equaliser: feedforward span either side of the symbol, in symbols,
    // at T/2; feedback taps on past decisions (unused at 75 bit/s).
    std::size_t ff_symbols = 7;
    std::size_t fb_taps = 5;
};

struct Mil110Message {
    Mil110Mode mode{};
    std::array<std::uint8_t, 2> designators{};
    // Input sample index of the first data symbol.
    std::int64_t position = 0;
    // Carrier offset measured on the preamble, hertz.
    double offset_hz = 0.0;
    // Mean squared error on the known symbols, on a unit constellation, and
    // the SNR it implies. At 75 bit/s, from the decided sets.
    double mse = 0.0;
    double snr_db = 0.0;
    // False when the signal was lost before an EOM.
    bool eom = false;
    // Symbol clock error the timing loop settled on, in parts per million
    // of the nominal 2400 symbols/s as counted in receiver samples.
    double clock_ppm = 0.0;
    std::vector<std::uint8_t> bits;   // one per byte, as received
    std::vector<std::uint8_t> bytes;  // first bit least significant
};

class Mil110Decoder {
public:
    [[nodiscard]] static Expected<Mil110Decoder> create(const Mil110Config& config);

    // Appends every message the audio completes. The output depends only on
    // the concatenated input, never on how it was split into calls.
    void process(dsp::ConstRealSpan audio, std::vector<Mil110Message>& messages);

    // Ends the message in progress, as the end of a recording does.
    void flush(std::vector<Mil110Message>& messages);

    void reset();

    [[nodiscard]] bool synced() const { return state_ != State::Search; }

private:
    using C = dsp::Complex32;

    Mil110Decoder() = default;

    enum class State : std::uint8_t { Search, Data };

    void run(std::vector<Mil110Message>& messages);
    [[nodiscard]] C derot(std::int64_t index) const;
    [[nodiscard]] double detect_metric(std::int64_t pos) const;
    [[nodiscard]] bool acquire(std::int64_t pos);
    [[nodiscard]] C sample(std::int64_t pos, std::size_t m) const {
        return fe_.at(pos + mil110_rx::kSps * static_cast<std::int64_t>(m));
    }

    void on_data_symbol(std::vector<Mil110Message>& messages);
    void on_set_symbol(std::vector<Mil110Message>& messages);
    void on_soft(std::span<const float> soft, std::vector<Mil110Message>& messages);
    void on_bits(std::span<const std::uint8_t> bits, std::vector<Mil110Message>& messages);
    void finish(bool eom, std::vector<Mil110Message>& messages);

    Mil110Config config_;
    mil110_rx::FrontEnd fe_;
    std::vector<C> head_ref_;  // conjugated, scrambled segment head

    State state_ = State::Search;
    std::int64_t search_pos_ = 0;
    std::int64_t trigger_ = -1;

    // Acquisition.
    std::int64_t origin_ = 0;  // baseband index of the acquired segment start
    double omega_ = 0.0;       // radians per baseband sample
    Mil110Mode mode_{};
    std::array<std::uint8_t, 2> designators_{};
    Mil110Layout layout_{};
    std::int64_t data_start_ = 0;  // symbol index from origin_
    int count0_ = 0;               // segment count at origin_

    mil110_rx::Equaliser eq_;
    mil110_rx::Snapshot snap_;
    std::int64_t symbol_ = 0;  // next symbol, from origin_

    // Data phase.
    std::array<std::uint8_t, kMil110DataScramblerPeriod> scrambler_{};
    std::vector<float> block_;
    std::size_t block_fill_ = 0;
    std::vector<std::size_t> deinterleave_;  // load index -> fetch index
    std::vector<float> derep_;
    std::optional<StreamingViterbi> viterbi_;
    std::uint32_t shift_ = 0;
    std::size_t shift_count_ = 0;
    double mse_sum_ = 0.0;
    std::size_t mse_n_ = 0;
    double frame_err_ = 0.0;
    int bad_frames_ = 0;
    std::vector<std::uint8_t> probe_;
    struct SetSymbol {
        mil110_rx::Snapshot s;
        C y{};
    };
    std::vector<SetSymbol> set_;
    std::size_t set_index_ = 0;
    Mil110Message message_;
};

}  // namespace revenant::decode
