// P25 Phase 1 FDMA: complex baseband to frames, and the frame metadata the
// standard carries in clear.
//
// SPECIFICATION
//
// Implements ANSI/TIA-102.BAAA-A, "Project 25 FDMA Common Air Interface":
// clause 5 (the voice code words: header, Link Control, encryption sync, low
// speed data and the placement of the IMBE frames), clause 8 (channel access,
// frame synchronisation, status symbols and the Network Identifier), clause 9
// (modulation), and the clause 10.2, 10.3 and 10.4 transmit-order annexes for
// the Header Data Unit and the two Logical Link Data Units. Reserved field
// values are ANSI/TIA-102.BAAC, "Common Air Interface Reserved Values".
//
// Both documents were read from the public archive at
// archive.org/details/TIA-102_Series_Documents, items
// "TIA-102-BAAA-A_Project_25_FDMA_CAI.pdf" and
// "TIA-102.BAAC-A_CAI_Reserved_Values.pdf", which is the same collection
// core/decode/imbe.h names for TIA-102.BABA. Every clause number below is the
// document's own.
//
// WHERE THIS STOPS
//
// At the IMBE frame, and deliberately. P25Phase1 recovers every data unit's
// channel bits, decodes the header, the Link Control word and the encryption
// sync through their Reed-Solomon codes, and cuts the nine 144-bit voice
// frames out of each LDU. P25Voice hands those frames to core/decode/imbe.cpp,
// which owns the vocoder and its own error control, and collects the PCM.
//
// It does not decrypt. Where a header, an encryption sync word or a Link
// Control format says a call is encrypted, P25Voice reports that with the
// talkgroup and the network and hands no frame of that call to the vocoder,
// per docs/modes.md.
//
// CLEAN ROOM
//
// No P25 implementation was read. Every constant names the clause, table or
// annex it came from. Where a number is an engineering choice rather than a
// specified value, the comment says so.
//
// The clause 10.3 and 10.4 annexes are 1728 rows of symbol-by-symbol tables.
// They were read out of the same PDF's text layer by script, not retyped, and
// checked there against Table 5-1 (all eighteen voice frames follow it
// exactly) before p25_ldu_layout below was written from Figures 8-3 and 8-4.
// tests/decode/test_p25p1.cpp checks the layout against the annexes' own
// symbol numbers, so the check survives without the PDF in hand.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it, the same as the RDS physical layer, so
// the project's zero-ULP rule does not reach it. What stands in its place is
// the transmitter in core/dsp/synth/dv_mod.h, written from the same clauses,
// and the round trip in tests/decode/test_p25p1.cpp.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/imbe.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstComplexSpan;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// TIA-102.BAAA-A clause 9.2: "The modulation sends 4800 symbols/sec with each
// symbol conveying 2 bits of information."
inline constexpr double kP25SymbolRate = 4800.0;
inline constexpr double kP25BitRate = 9600.0;

// Table 9-1, the C4FM deviation column. The symbol column runs +3, +1, -1, -3
// against dibits 01, 00, 10, 11, and the deviations are +1.80, +0.60, -0.60
// and -1.80 kHz, so one symbol unit is 600 Hz of deviation.
//
// Indexed by the dibit as an integer, so kP25DibitToSymbol[0b01] is +3.
inline constexpr double kP25DeviationPerSymbolUnitHz = 600.0;
inline constexpr int kP25DibitToSymbol[4] = {+1, +3, -1, -3};

// The inverse of the above: the dibit a symbol level represents, indexed by
// (level + 3) / 2, so index 0 is level -3.
inline constexpr std::uint8_t kP25SymbolToDibit[4] = {0b11, 0b10, 0b00, 0b01};

// Clause 9.3, the Nyquist raised cosine filter, and clause 9.4, the shaping
// filter cascaded with it. Both are flat in group delay below 2880 Hz and the
// raised cosine is zero above it.
inline constexpr double kP25NyquistFlatEdgeHz = 1920.0;
inline constexpr double kP25NyquistStopHz = 2880.0;

// Table 8-1. The frame sync word, 48 bits, transmitted left to right.
inline constexpr std::uint64_t kP25FrameSync = 0x5575F5FF77FFULL;
inline constexpr std::size_t kP25FrameSyncBits = 48;
inline constexpr std::size_t kP25FrameSyncSymbols = kP25FrameSyncBits / 2;

// Clause 8.5. The Network Identifier is 64 bits: 12 of Network Access Code,
// 4 of Data Unit ID, 47 of BCH parity and one more parity bit.
inline constexpr std::size_t kP25NidBits = 64;
inline constexpr std::size_t kP25NidSymbols = kP25NidBits / 2;

// Clause 8.4 and the clause 10.2 annex. A status symbol is inserted after
// every 35 information symbols, which the annex shows as absolute symbol
// positions 35, 71, 107 and so on: one every 36th, counting the status
// symbols themselves, with the first at index 35.
inline constexpr std::size_t kP25StatusSymbolInterval = 36;
inline constexpr std::size_t kP25FirstStatusSymbol = 35;

// Table 8-4, the six Data Unit Identifier values this document defines. The
// other ten are reserved for trunking.
enum class P25Duid : std::uint8_t {
    HeaderDataUnit = 0x0,
    TerminatorWithoutLinkControl = 0x3,
    LogicalLinkDataUnit1 = 0x5,
    LogicalLinkDataUnit2 = 0xA,
    PacketDataUnit = 0xC,
    TerminatorWithLinkControl = 0xF,
};

[[nodiscard]] std::string_view p25_duid_name(std::uint8_t duid);

// The clause 10.2 annex, read end to end: the Header Data Unit is 396 symbols
// including 11 status symbols and 5 null symbols, so 385 carry information.
// Those 385 are 24 of frame sync, 32 of Network Identifier, 324 of Golay
// coded header, and the 5 nulls.
inline constexpr std::size_t kP25HduTotalSymbols = 396;
inline constexpr std::size_t kP25HduStatusSymbols = 11;
inline constexpr std::size_t kP25HduNullSymbols = 5;
inline constexpr std::size_t kP25HduGolayWords = 36;
inline constexpr std::size_t kP25HduSymbolsPerGolayWord = 9;

// The other three annexes, read the same way. Clause 10.5's simple terminator
// runs to symbol 71 and clause 10.6's terminator with Link Control to 215;
// clauses 10.3 and 10.4 both run to 863, which is 180 ms at 4800 symbols per
// second, the figure clause 10.3 states in prose.
inline constexpr std::size_t kP25SimpleTerminatorSymbols = 72;
inline constexpr std::size_t kP25TerminatorWithLcSymbols = 216;
inline constexpr std::size_t kP25LduSymbols = 864;

// The simple terminator is the frame sync, the Network Identifier, two status
// symbols and nulls for the rest.
inline constexpr std::size_t kP25SimpleTerminatorNullSymbols =
    kP25SimpleTerminatorSymbols - kP25FrameSyncSymbols - kP25NidSymbols - 2;

// Total symbols in a data unit of this type, including its status symbols, or
// zero where this document does not define the type.
//
// Used to step past a decoded data unit rather than past its sync word alone,
// so a sync pattern appearing inside a payload cannot start a second
// overlapping frame.
[[nodiscard]] std::size_t p25_data_unit_symbols(std::uint8_t duid);

// TIA-102.BAAC clause 2.8: "The ALGID has a standard value for unencrypted
// messages, which is $80."
inline constexpr std::uint8_t kP25AlgidUnencrypted = 0x80;

// ---------------------------------------------------------------------------
// The Logical Link Data Units
// ---------------------------------------------------------------------------

// Clause 8.2.2 and Figures 8-3 and 8-4: nine voice code words per LDU, frames
// 1 to 9 in LDU1 and 10 to 18 in LDU2. Clause 5.3.1: a voice code word is 144
// bits, 72 dibits, transmitted in the Table 5-1 order.
inline constexpr std::size_t kP25VoiceFramesPerLdu = 9;
inline constexpr std::size_t kP25VoiceFrameSymbols = 72;
inline constexpr std::size_t kP25VoiceFrameBits = 144;

// Clauses 5.4 and 5.5: the Link Control word in LDU1 and the encryption sync
// word in LDU2 are each 24 hexbits, each hexbit a (10,6,3) shortened Hamming
// word of five dibits. Clause 5.6: two low speed data octets per LDU, each a
// 16-bit code word of eight dibits.
inline constexpr std::size_t kP25LduHammingWords = 24;
inline constexpr std::size_t kP25HammingWordSymbols = 5;
inline constexpr std::size_t kP25LduLsdOctets = 2;
inline constexpr std::size_t kP25LsdWordSymbols = 8;

// Clause 8.2.2: "24 Status Symbols" in each LDU, which the clause 10.3 and
// 10.4 annexes put at the same positions as every other data unit's.
inline constexpr std::size_t kP25LduStatusSymbols = 24;
inline constexpr std::size_t kP25LduInformationSymbols = kP25LduSymbols - kP25LduStatusSymbols;

// Where each field of an LDU starts, as an index into its information dibits,
// that is after the status symbols are removed. Index 0 is the first frame
// sync dibit.
struct P25LduLayout {
    std::array<std::size_t, kP25VoiceFramesPerLdu> voice{};
    std::array<std::size_t, kP25LduHammingWords> hamming{};
    std::size_t low_speed_data = 0;
};

// Figures 8-3 and 8-4, read against the clause 10.3 and 10.4 annexes: the
// frame sync and NID, voice frames 1 and 2, then one block of four Hamming
// words after each of voice frames 2 through 7, the low speed data after
// voice frame 8, and voice frame 9 last. Both LDUs share it; what the Hamming
// words carry is the difference. The test checks every offset against the
// annex's own absolute symbol numbers.
[[nodiscard]] constexpr P25LduLayout p25_ldu_layout() {
    P25LduLayout layout;
    std::size_t at = kP25FrameSyncSymbols + kP25NidSymbols;
    std::size_t word = 0;
    for (std::size_t frame = 0; frame < kP25VoiceFramesPerLdu; ++frame) {
        layout.voice[frame] = at;
        at += kP25VoiceFrameSymbols;
        if (frame >= 1 && frame <= 6) {
            for (std::size_t i = 0; i < 4; ++i) {
                layout.hamming[word++] = at;
                at += kP25HammingWordSymbols;
            }
        } else if (frame == 7) {
            layout.low_speed_data = at;
            at += kP25LduLsdOctets * kP25LsdWordSymbols;
        }
    }
    return layout;
}

// ---------------------------------------------------------------------------
// Link Control
// ---------------------------------------------------------------------------

// TIA-102.BAAC clause 2.2, the four Link Control Formats the Common Air
// Interface uses. Clause 5.5 and Figure 5-6 of TIA-102.BAAA-A give the layout
// of $00 and $03; $80 and $83 carry the same contents encrypted.
inline constexpr std::uint8_t kP25LcfGroupVoice = 0x00;
inline constexpr std::uint8_t kP25LcfUnitToUnitVoice = 0x03;
inline constexpr std::uint8_t kP25LcfEncryptedBit = 0x80;

// TIA-102.BAAC clause 2.3: the standard MFID. Definitions made before April
// 2001, which both formats above are, use $00.
inline constexpr std::uint8_t kP25MfidStandard = 0x00;

// The decoding report every Reed-Solomon protected word carries.
struct P25CodeReport {
    // Hexbits the Reed-Solomon code changed, and hexbits the inner code
    // (Golay in the header, Hamming in LC and ES) flagged as detected errors
    // and handed over as erasures.
    std::uint32_t rs_corrected = 0;
    std::uint32_t erasures = 0;

    // Inner code words that needed any correction, and the most bits any one
    // of them took.
    std::uint32_t inner_words_corrected = 0;
    std::uint32_t worst_inner_correction = 0;
};

// Clause 5.5, the 72 bits of Link Control from LDU1.
struct P25LinkControl {
    // All 72 bits, most significant first, as recovered. Formats this file
    // does not parse are still here in full.
    std::array<std::uint8_t, 9> octets{};

    std::uint8_t format = 0;
    std::uint8_t manufacturer_id = 0;

    // TIA-102.BAAC clause 2.2: formats $80 and $83 are encrypted Link Control,
    // so nothing past the MFID means anything in clear.
    bool encrypted = false;

    // Figure 5-6, parsed only for formats $00 and $03 with the standard MFID.
    // Clause 5.2 has a standard receiver ignore what a non-standard MFID
    // carries, which is why a manufacturer's own format is left as octets.
    bool emergency = false;
    std::optional<std::uint16_t> talkgroup_id;
    std::optional<std::uint32_t> source_id;
    std::optional<std::uint32_t> destination_id;

    P25CodeReport code;
};

// Clause 5.4, the 96 bits of encryption sync from LDU2.
struct P25EncryptionSync {
    std::array<std::uint8_t, 9> message_indicator{};
    std::uint8_t algorithm_id = 0;
    std::uint16_t key_id = 0;

    // TIA-102.BAAC clause 2.8, as for the header.
    bool encrypted = false;

    P25CodeReport code;
};

// Figure 5-6 applied to 72 recovered bits. The transmitter takes Link Control
// as raw octets and the tests build those octets from the figure themselves,
// so this parse is checked against a second reading of the figure rather than
// against its own inverse.
[[nodiscard]] P25LinkControl p25_parse_link_control(const std::array<std::uint8_t, 9>& octets);

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

struct P25Nid {
    std::uint16_t network_access_code = 0;
    std::uint8_t duid = 0;

    // Bits the BCH decode had to change. The code's minimum distance is 23, so
    // up to 11 is a correction it guarantees; above that the answer is the
    // nearest code word rather than the transmitted one. Carried out so a
    // caller can decide what to trust instead of being told.
    std::uint32_t corrected_bits = 0;
};

// The Header Data Unit's contents, clause 10.2 read through the Golay code.
struct P25Header {
    // Clause 10.2 and TIA-102.BAAC clause 2.6. 72 bits, most significant
    // first, the initialisation vector for encryption. All zero means the call
    // is unencrypted, and the null value is never used for an encrypted one.
    std::array<std::uint8_t, 9> message_indicator{};

    std::uint8_t manufacturer_id = 0;
    std::uint8_t algorithm_id = 0;
    std::uint16_t key_id = 0;
    std::uint16_t talkgroup_id = 0;

    // TIA-102.BAAC clause 2.8. False only when the algorithm identifier holds
    // the standard unencrypted value.
    //
    // This is the status surface docs/modes.md requires: an encrypted call is
    // identified, its talkgroup and network are reported, and nothing tries to
    // turn its payload into audio.
    bool encrypted = false;

    // The worst Golay correction across the 36 code words, and the number of
    // words that needed any. A header whose worst word took three corrections
    // is at the edge of what a distance-8 code can do. A word four or more
    // bits from every code word is handed to the Reed-Solomon code as an
    // erasure and counted in `code`.
    std::uint32_t worst_golay_correction = 0;
    std::uint32_t golay_words_corrected = 0;

    // The (36,20,17) Reed-Solomon decode under the Golay code.
    P25CodeReport code;
};

struct P25Frame {
    P25Nid nid;

    // Present for a Header Data Unit whose Reed-Solomon word decoded.
    std::optional<P25Header> header;

    // Present for an LDU1 and an LDU2 respectively, when the Reed-Solomon word
    // decoded. Link Control carries the talkgroup and source again every
    // 360 ms, so a receiver that joins mid-call has them at its first LDU1
    // rather than at the next header, which for voice might never come.
    std::optional<P25LinkControl> link_control;
    std::optional<P25EncryptionSync> encryption_sync;

    // True when this data unit carries a header, LC or ES word and the
    // Reed-Solomon code could not correct it. The optional above is then
    // empty, because a field that failed its code is not data.
    bool code_word_failed = false;

    // The nine voice frames of an LDU, 144 bits each, one bit per byte in
    // the Table 5-1 transmission order, which is the order
    // ImbeDecoder::decode takes. Empty for every other data unit.
    //
    // Handed out whether or not the call is encrypted: they are channel bits,
    // and deciding what may be done with them is P25Voice's job.
    std::vector<std::array<std::uint8_t, kP25VoiceFrameBits>> voice;

    // Clause 5.6, the two low speed data octets of an LDU, each present when
    // its (16,8,5) code word was within the two errors the code corrects.
    std::array<std::optional<std::uint8_t>, kP25LduLsdOctets> low_speed_data{};

    // Index into the symbol run at which this frame's sync word starts.
    std::size_t first_symbol = 0;

    // Normalised correlation the sync word scored, and whether it matched
    // inverted. An inverted match means the discriminator polarity is
    // reversed, which depends on which sideband the tuner landed on and is
    // corrected here rather than reported as a failure.
    double sync_score = 0.0;
    bool inverted = false;

    // Every information dibit of the data unit after the status symbols are
    // removed, including the sync word and the NID, one dibit per byte in the
    // low two bits. Handed out so a caller can take the payload somewhere this
    // file does not go.
    std::vector<std::uint8_t> dibits;
};

// Decodes the fields of an LDU1 or LDU2 from its information dibits, status
// symbols already removed, into `out`: the voice frames, the low speed data,
// and the Link Control or encryption sync word. Exposed so the tests can
// drive it from the transmitter's dibits with no modem in between.
[[nodiscard]] Status p25_decode_ldu(std::span<const std::uint8_t> information_dibits,
                                    std::uint8_t duid, P25Frame& out);

struct P25Config {
    SampleRate rate = 48000;

    // Correlation a sync word must reach to be called a frame. An engineering
    // choice, not a specified value: 0.9 admits a sync word with two of its
    // 24 symbols wrong, which is about where a C4FM link stops producing
    // usable voice anyway, and rejects the correlation sidelobes of the sync
    // pattern against itself, whose largest is well below it.
    double sync_threshold = 0.9;

    // Taps in the receive filter. Odd, so the group delay is a whole number of
    // samples. 121 at 48 kHz spans 2.5 ms, which is twelve symbol periods.
    //
    // Longer than the transmitter's 65 because this filter does have a corner
    // to resolve. The clause 9.6 response is a sinc that is still at half
    // amplitude when it reaches the edge of the band the clause specifies, so
    // the out-of-band taper starts from a step rather than from zero, and a
    // 65 tap design smears it back into the passband. Measured: 65 taps put
    // the recovered constellation's inner and outer levels at 1.2 and 2.8
    // instead of 1 and 3.
    std::size_t filter_taps = 121;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

class P25Phase1 {
   public:
    [[nodiscard]] static Expected<P25Phase1> create(const P25Config& config);

    // Consumes complex baseband centred on the carrier and appends every frame
    // whose sync word and NID were recovered from it. State carries across
    // calls, so a caller may feed the stream in any blocking.
    //
    // A frame straddling a block boundary is held until the rest of it
    // arrives, so no frame is lost and none is reported twice. That includes
    // the payload: a data unit is reported once all of its symbols are in,
    // so the last one in a capture that stops mid-unit is not reported.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<P25Frame>& out);

    // The soft symbol values the last call recovered, in units where a
    // Table 9-1 symbol is +3, +1, -1 or -3. Exposed for the tests and for a
    // caller measuring modulation quality; not needed to decode.
    [[nodiscard]] std::span<const float> last_symbols() const { return last_symbols_; }

    void reset();

   private:
    P25Phase1() = default;

    // What decode_at found at a sync hit. NeedMore means the NID decoded and
    // named a data unit longer than the symbols held so far.
    enum class Outcome : std::uint8_t { Complete, NeedMore };

    [[nodiscard]] Expected<Outcome> decode_at(std::size_t offset, bool inverted, double score,
                                              P25Frame& out) const;

    P25Config config_{};
    std::vector<float> filter_taps_;
    SymbolSync sync_{};

    // Soft symbols carried across calls, with everything before the last
    // possible frame start trimmed off.
    std::vector<float> symbols_;
    std::vector<float> last_symbols_;
    std::size_t consumed_ = 0;

    // Scratch, kept so the hot path does not allocate per block.
    std::vector<RecoveredSymbol> recovered_;
    std::vector<float> discriminated_;
    std::vector<float> filtered_;
};

// The frame sync word as soft symbol values, for a correlator. Table 8-1 read
// through Table 9-1.
[[nodiscard]] std::array<float, kP25FrameSyncSymbols> p25_frame_sync_pattern();

// Removes the status symbols from a run of symbols that starts at a data
// unit's first sync symbol, per clause 8.4.
[[nodiscard]] std::vector<float> p25_strip_status_symbols(std::span<const float> symbols);

// ---------------------------------------------------------------------------
// Voice
// ---------------------------------------------------------------------------

// What is known about whether the call on the channel is encrypted.
enum class P25CallEncryption : std::uint8_t {
    // Nothing has said. A receiver that joined at an LDU1 is here until the
    // LDU2 that follows it, at most 180 ms later.
    Unknown,
    // A header or an encryption sync word carried ALGID $80.
    Clear,
    // A header or an encryption sync word carried any other ALGID, or the
    // Link Control arrived in format $80 or $83.
    Encrypted,
};

// The status surface docs/modes.md asks for: the call as the channel has
// described it so far, whether or not any audio came out of it.
struct P25CallState {
    // True from the first data unit of a call to its terminator.
    bool active = false;

    std::uint16_t network_access_code = 0;
    std::optional<std::uint16_t> talkgroup_id;
    std::optional<std::uint32_t> source_id;

    P25CallEncryption encryption = P25CallEncryption::Unknown;
    std::optional<std::uint8_t> algorithm_id;
    std::optional<std::uint16_t> key_id;

    // Voice frames handed to the vocoder, withheld because the call is
    // encrypted, and dropped because the call ended or its encryption state
    // could not be established before a second LDU1 arrived.
    std::uint64_t frames_decoded = 0;
    std::uint64_t frames_withheld = 0;
    std::uint64_t frames_dropped = 0;

    // Of the frames decoded, how many the vocoder repeated or muted under
    // TIA-102.BABA section 7.7 and 7.8 rather than synthesizing.
    std::uint64_t frames_repeated = 0;
    std::uint64_t frames_muted = 0;
};

// P25 voice, from frames to 8 kHz PCM.
//
// Feed it every P25Frame a P25Phase1 produced, in order. For each voice frame
// of a call known to be in clear it appends ImbeDecoder::kPcmFrames samples of
// float PCM at ImbeDecoder::kSampleRateHz, full scale +-1, to `pcm`.
//
// THE ENCRYPTION GATE
//
// Voice goes to the vocoder only when a header or an encryption sync word has
// said ALGID $80. A receiver joining at an LDU1 has neither yet, so that LDU's
// nine frames are held until the LDU2 behind it: its encryption sync word
// releases them if the call is in clear and discards them if it is not. So a
// clear call joined mid-transmission loses no audio, and an encrypted one
// never reaches the vocoder, which is the line docs/modes.md draws: identify
// what cannot be decoded, and never attempt it.
class P25Voice {
   public:
    [[nodiscard]] Status push(const P25Frame& frame, std::vector<float>& pcm);

    [[nodiscard]] const P25CallState& call() const noexcept { return call_; }

    // The vocoder's report on the last frame it was handed.
    [[nodiscard]] const ImbeFrameReport& last_voice_frame() const noexcept {
        return imbe_.last_frame();
    }

    void reset();

   private:
    [[nodiscard]] Status decode_frames(
        std::span<const std::array<std::uint8_t, kP25VoiceFrameBits>> frames,
        std::vector<float>& pcm);
    void begin_call(std::uint16_t nac);
    void end_call();

    ImbeDecoder imbe_;
    P25CallState call_;
    std::vector<std::array<std::uint8_t, kP25VoiceFrameBits>> pending_;
};

}  // namespace revenant::decode
