// MFSK16 and MFSK8: receiver audio to text.
//
// SPECIFICATION
//
// Four documents by the mode's designers, all hosted at
// https://www.qsl.net/zl1bpu/MFSK/ and fetched 2026-11-02:
//
//   - "MFSK16 Technical Specification", M. Greenman ZL1BPU, Rev 1.2 of
//     11 July 2000, reformatted 15 Oct 2009 with no technical changes
//     (tech.htm). Cited as "spec" with a paragraph number.
//   - "The IZ8BLY MFSK Varicode", N. Porcino IZ8BLY and M. Greenman, Ver 1.0
//     of 10 July 2000 (Varicode.htm). Cited as "Varicode".
//   - "The MFSK FEC Coder", the same two authors, Ver 1.0 of 11 July 2000
//     (FECcoder.htm). Cited as "FEC".
//   - "The IZ8BLY Diagonal Interleaver", N. Porcino, 07 July 2000
//     (Interleaver.htm). Cited as "Interleaver".
//
// The overview page M16.htm on the same site (fetched the same day) names
// the two modes: MFSK16 is 16 tones at 15.625 baud spaced 15.625 Hz, MFSK8
// is 32 tones at 7.8125 baud spaced 7.8125 Hz, both with the same FEC and an
// interleaver of L = 10. The ARRL's MFSK page (arrl.org/mfsk) restates
// 15.625 baud and 316 Hz and adds nothing used here.
//
// WHAT EACH DOCUMENT FIXES
//
//   - Tones spaced at the symbol rate, square keyed and phase continuous,
//     spec C.1 and C.5.
//   - Gray weighting, the lowest tone all zeros, spec C.13: the printed
//     16-tone table is n XOR (n >> 1). MFSK8's 32 tones are not tabled
//     anywhere; the same reflected Gray code over five bits is assumed and
//     labelled where it is used.
//   - Bits enter a symbol "with the first arriving bit given maximum
//     weight", FEC.
//   - R = 1/2 K = 7 with Dibit1 = O1+O3+O4+O6+O7 and Dibit2 = O1+O2+O3+O4+O7,
//     Dibit1 sent first, FEC. The page does not say whether O1 is the newest
//     or the oldest stage. This file takes O1 as the newest (D^0); the
//     other reading is the same code time-reversed, and the two are the
//     usual NASA pair either way. Nothing on air has confirmed the choice.
//   - The dibit pair is not aligned to the symbol, FEC: the receiver must
//     find which of two pairings is right. Done here with two decoders and
//     a re-encoding check, the "two trial decoders" the page suggests.
//   - Ten concatenated n x n diagonal interleavers, spec C.9 and
//     Interleaver. From the worked example on that page: bit i of a symbol
//     (i = 0 the first sent) leaves 10 * i symbols after it went in, the
//     rest of the symbol's bits staying in place. The single-table example
//     earlier on the page reads its diagonal the other way; the concatenated
//     example is the one labelled as the program's, and is followed.
//   - The alphabet, Varicode's table, framed by "001": every codeword ends in
//     "00" and contains no "001", spec C.10.
//   - Idle is NUL followed by zeros, which the receiver rejects as invalid,
//     spec C.11. A transmission opens with 8 symbols of the lowest tone and
//     ends with at least 4 symbols of flushed zeros, spec C.12.
//
// WHAT THIS FILE CHOSE
//
// Soft bits by spec D.2's suggestion (weigh the bins carrying a one against
// those carrying a zero), done max-log: the strongest bin with the bit clear
// minus the strongest with it set, over the symbol's total. Symbol timing,
// AFC and the tone bank are core/decode/mfsk_demod.h's. Where the receiver
// gives up on the wrong pairing is labelled where it is set.
//
// CLEAN ROOM
//
// No MFSK16 implementation was read: not fldigi, gmfsk, Stream, MultiPSK or
// any other. The varicode table is the 256 entries printed on the Varicode
// page, which describes them as "cut directly from the source code" of its
// authors' program; the page is documentation and was read as such.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code. The round trip through core/dsp/synth/mfsk16_mod.h stands in.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/decode/mfsk_demod.h"
#include "core/decode/viterbi_stream.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::SampleIndex;

enum class Mfsk16Mode : std::uint8_t {
    Mfsk16,
    Mfsk8,
};

struct Mfsk16Geometry {
    int tones = 16;
    int bits_per_symbol = 4;
    double symbol_rate = 15.625;
};

// M16.htm: 16 tones at 15.625 Bd, 32 tones at 7.8125 Bd, spacing equal to
// the symbol rate (spec C.5).
[[nodiscard]] Mfsk16Geometry mfsk16_geometry(Mfsk16Mode mode);

// Spec C.9 and Interleaver: ten diagonal tables.
inline constexpr int kMfskInterleaverStages = 10;
// Spec C.12.
inline constexpr int kMfskPreambleSymbols = 8;
inline constexpr int kMfskPostambleSymbols = 4;

// FEC, with O1 taken as D^0 (see the header). Dibit1 first.
inline constexpr std::uint32_t kMfskFecMemory = 6;
inline constexpr std::uint32_t kMfskFecGenerators[2] = {
    0b1101101U,  // O1 + O3 + O4 + O6 + O7
    0b1001111U,  // O1 + O2 + O3 + O4 + O7
};

// Spec C.13: Gray weight of a tone, and its inverse.
[[nodiscard]] int mfsk_gray(int tone);
[[nodiscard]] int mfsk_gray_inverse(int weight);

// ---------------------------------------------------------------------------
// The IZ8BLY varicode
// ---------------------------------------------------------------------------

// Varicode's table, indexed by character code 0 to 255, each code MSB first
// and including its trailing "00".
extern const std::array<std::string_view, 256> kMfskVaricode;
inline constexpr std::size_t kMfskVaricodeMaxBits = 12;

// Appends the code for each byte of text, Latin-1, to bits.
void mfsk_varicode_append(std::string_view latin1, std::vector<std::uint8_t>& bits);

// Bit-serial decoder: returns a character code when a "001" boundary closes
// a codeword the table holds, -1 otherwise. Codes too long or absent from
// the table (idle's run of zeros among them) are dropped.
class MfskVaricodeDecoder {
public:
    [[nodiscard]] int push(std::uint8_t bit);
    void reset();

private:
    std::uint32_t code_ = 0;
    std::size_t length_ = 0;
    int zeros_ = 0;
    bool overflow_ = false;
};

// ---------------------------------------------------------------------------
// The diagonal interleaver
// ---------------------------------------------------------------------------

// Bit i of each symbol is delayed 10 * i symbols on transmit; the receiver
// delays it 10 * (n - 1 - i) so all bits of a symbol line up again. Both
// start full of zeros, as the transmitter's does.
template <typename T>
class MfskDiagonalDelay {
public:
    MfskDiagonalDelay() = default;
    MfskDiagonalDelay(int bits, bool receive);
    // One symbol's bits in, one out.
    void push(std::span<const T> in, std::span<T> out);

private:
    int bits_ = 0;
    std::vector<std::deque<T>> lines_;
};

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct Mfsk16Config {
    SampleRate rate = 8000;
    Mfsk16Mode mode = Mfsk16Mode::Mfsk16;

    // Lowest tone. With search on, the starting estimate is ignored and the
    // signal is looked for between the two edges.
    double base_hz = 1000.0;
    bool search = true;
    double search_low_hz = 300.0;
    double search_high_hz = 2700.0;

    // Spec E.9: a lower sideband receiver hears the tones reversed.
    bool lower_sideband = false;

    // Decision depth of the Viterbi decoders, in data bits. Zero picks the
    // streaming decoder's own default.
    std::size_t traceback = 48;
};

struct Mfsk16Character {
    // Character code 0 to 255, Latin-1.
    std::uint8_t code = 0;
    // Approximate audio sample index of the symbol whose bits completed it.
    SampleIndex position = 0;
};

class Mfsk16Decoder {
public:
    [[nodiscard]] static Expected<Mfsk16Decoder> create(const Mfsk16Config& config);

    // Output depends only on the concatenated input, never on the chunking.
    void process(ConstRealSpan audio, std::vector<Mfsk16Character>& out);
    // Releases what the Viterbi decoders still hold.
    void flush(std::vector<Mfsk16Character>& out);
    void reset();

    [[nodiscard]] bool acquired() const { return demod_.acquired(); }
    [[nodiscard]] double tone0_hz() const { return demod_.tone0_hz(); }
    // Which dibit pairing is in use, 0 or 1.
    [[nodiscard]] int pairing() const { return chosen_; }

private:
    explicit Mfsk16Decoder(MfskDemodulator demod) : demod_(std::move(demod)) {}

    struct Held {
        std::uint8_t code = 0;
        SampleIndex position = 0;
        std::uint64_t decided = 0;
    };

    // One of the two dibit pairings.
    struct Lane {
        StreamingViterbi viterbi;
        MfskVaricodeDecoder varicode{};
        std::deque<std::pair<float, float>> pending{};  // soft pairs not yet decided
        std::deque<SampleIndex> pending_sample{};
        std::deque<Held> held{};
        float half = 0.0F;  // first half of a pair waiting for its second
        bool have_half = false;
        bool skip_one = false;
        std::uint32_t encoder = 0;
        double disagreement = 0.0;
        std::uint64_t decided = 0;
        // Re-encoding disagreement per decided bit, for the squelch.
        std::array<float, 256> wrong{};
    };

    void on_symbol(const MfskSymbol& symbol, std::vector<Mfsk16Character>& out);
    void on_bits(Lane& lane, const std::vector<std::uint8_t>& bits);
    void choose_and_print(std::vector<Mfsk16Character>& out, bool all);

    Mfsk16Config config_{};
    Mfsk16Geometry geometry_{};
    MfskDemodulator demod_;
    MfskDiagonalDelay<float> deinterleave_{};
    std::vector<MfskSymbol> symbols_;
    std::vector<Lane> lanes_;
    int chosen_ = 0;
    SampleIndex last_printed_ = 0;
    bool printed_any_ = false;
    std::vector<float> soft_;
    std::vector<float> aligned_;
    std::vector<std::uint8_t> bits_;
};

}  // namespace revenant::decode
