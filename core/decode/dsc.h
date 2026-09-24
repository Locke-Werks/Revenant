// DSC, digital selective calling on VHF channel 70: an FM receiver's audio to
// calls.
//
// SPECIFICATION
//
// Recommendation ITU-R M.493-15 (01/2019), "Digital selective-calling system
// for use in the maritime mobile service", Annex 1, read from the ITU's own
// publication service. Clauses used: 1.1 and 1.1.1 with Table A1-1 (the
// ten-bit error-detecting code and its bit order), 1.2.1 (time diversity,
// DX and RX), 1.3.2 (the VHF tones, rate and pre-emphasis), 1.4 (which tone is
// which state), 1.5.1 (symbols 0 to 99 carry two digits, 100 to 127 are
// commands), Table A1-2 (digits packed two to a character), Table A1-3 (the
// meanings of symbols 100 to 127), 2.1 (the call sequence), 3.1 to 3.4.2 (dot
// pattern and phasing), 4.1 and 4.2 (format specifiers, and detecting the
// format specifier twice), 5.2 (addresses), 6 (category), 7.1
// (self-identification), 8.1 to 8.3.3 (messages), Tables A1-4.1 to A1-4.9
// (the call formats), Table A1-5 (frequency and channel information), 9 (end
// of sequence), 10.1 to 10.4 (the error-check character), and Figure 1 (the
// order of DX and RX on the air).
//
// Channel 70 itself, 156.525 MHz, is RR Appendix 18, not M.493; docs/modes.md
// has it and this file does not need it.
//
// M.493-16 (12/2023) superseded -15 and is in force. The brief this was
// written to named -15. -16 was read beside it for the clauses above: it
// removes the EPIRB entry, symbol 112, from the natures of distress in clause
// 8.1.1, adds an ACS category and second telecommands 120 to 125 for its
// automatic connection system, and leaves the physical layer, the code, the
// phasing and the formats decoded here as they were. A -16 call with those
// symbols decodes; their meaning is reported as the symbol number.
//
// WHAT THIS TAKES AS INPUT
//
// The audio of an FM receiver on channel 70. Clause 1.3.2 puts the call on an
// audio subcarrier, 1300 and 2100 Hz at 1200 bit/s, by frequency modulation
// with 6 dB per octave of pre-emphasis. An nfm receiver in the engine applies
// no de-emphasis (core/engine/vrx.h), so the 2100 Hz tone arrives
// 20 log10(2100 / 1300) = 4.2 dB above the 1300 Hz one. core/decode/fsk.h's
// ToneDiscriminator decides each bit on the difference of the two tones'
// energies over their sum, and only one tone is keyed at a time.
// tests/decode/test_dsc.cpp decodes every call from ideal audio with the
// tilt and without it, and makes its measurements in noise with it.
//
// WHERE THIS STOPS
//
// At the call: format specifier, address, category, self-identification, the
// telecommands, the distress information where the format carries it, the
// channel or frequency message, the end-of-sequence character and whether the
// error-check character agreed. The Recommendation ITU-R M.821 expansion
// sequence a distress alert may be followed by (clause 11.4) is not read:
// M.821 was not held. Nothing here acknowledges, relays or alarms; a receiver
// that decodes a distress alert reports it and does nothing else.
//
// CLEAN ROOM
//
// No DSC implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/fsk.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants, M.493-15 Annex 1
// ---------------------------------------------------------------------------

// Clause 1.3.2: the VHF subcarrier shifts between 1300 and 2100 Hz at
// 1200 bit/s. Clause 1.4: "The higher frequency corresponds to the B-state and
// the lower frequency corresponds to the Y-state". Table A1-1: B is 0 and Y
// is 1.
inline constexpr Hertz kDscVhfYHz = 1300;
inline constexpr Hertz kDscVhfBHz = 2100;
inline constexpr double kDscVhfBitRate = 1200.0;

// Clause 1.3.2: "the index of modulation is 2.0". Phase modulation, so each
// tone's frequency deviation is twice its own frequency.
inline constexpr double kDscVhfModulationIndex = 2.0;

// Clause 1.1: ten bits to a character, the first seven information.
inline constexpr std::size_t kDscCharacterBits = 10;
inline constexpr std::size_t kDscInformationBits = 7;

// Clause 3.4.2: a dot pattern of 20 bits "At VHF for all calls".
inline constexpr std::size_t kDscVhfDotBits = 20;

// Clauses 3.2 to 3.2.2 and Table A1-3: six DX phasing characters, symbol 125,
// and in the RX positions symbols 111 down to 104.
inline constexpr std::uint8_t kDscPhasingDx = 125;
inline constexpr std::size_t kDscPhasingDxCount = 6;
inline constexpr std::array<std::uint8_t, 8> kDscPhasingRx = {111, 110, 109, 108,
                                                               107, 106, 105, 104};

// Clause 4.1.
inline constexpr std::uint8_t kDscFormatGeographicArea = 102;
inline constexpr std::uint8_t kDscFormatDistress = 112;
inline constexpr std::uint8_t kDscFormatGroup = 114;
inline constexpr std::uint8_t kDscFormatAllShips = 116;
inline constexpr std::uint8_t kDscFormatIndividual = 120;
inline constexpr std::uint8_t kDscFormatAutomatic = 123;

// Table A1-3's category column and clause 6.
inline constexpr std::uint8_t kDscCategoryRoutine = 100;
inline constexpr std::uint8_t kDscCategorySafety = 108;
inline constexpr std::uint8_t kDscCategoryUrgency = 110;
inline constexpr std::uint8_t kDscCategoryDistress = 112;

// Table A1-3's first telecommand column: 110 is "Distress acknowledgement"
// and 112 "Distress alert relay".
inline constexpr std::uint8_t kDscTelecommandDistressAcknowledgement = 110;
inline constexpr std::uint8_t kDscTelecommandDistressRelay = 112;

// Clause 9.
inline constexpr std::uint8_t kDscEosAcknowledgeRq = 117;
inline constexpr std::uint8_t kDscEosAcknowledgeBq = 122;
inline constexpr std::uint8_t kDscEos = 127;

// Table A1-3 note "*": "Symbol transmitted in place of unused message
// information."
inline constexpr std::uint8_t kDscNoInformation = 126;

// ---------------------------------------------------------------------------
// The code
// ---------------------------------------------------------------------------

// Clause 1.1.1: the ten bits of a symbol in transmission order, one per byte,
// Y as 1. The seven information bits go least significant first, then three
// check bits counting the B elements among them, most significant first.
[[nodiscard]] std::array<std::uint8_t, kDscCharacterBits> dsc_encode_symbol(std::uint8_t symbol);

// The reverse, or nothing when the check bits do not count the B elements.
// Every single bit error is detected; clause 1.1's code is error detecting
// and corrects nothing.
[[nodiscard]] std::optional<std::uint8_t> dsc_decode_symbol(
    std::span<const std::uint8_t, kDscCharacterBits> bits);

// Clause 10.2: the seven information bits of the error-check character are
// the even vertical parity of the information characters, which are the
// symbols from one format specifier to the end-of-sequence character.
[[nodiscard]] std::uint8_t dsc_ecc(std::span<const std::uint8_t> information);

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Clause 8.1.2: a quadrant digit, latitude in degrees and minutes, longitude
// in degrees and minutes; north and east positive.
struct DscPosition {
    double latitude = 0.0;
    double longitude = 0.0;
};

// Table A1-5, one channel or frequency message element.
struct DscFrequency {
    enum class Kind : std::uint8_t {
        // HM digit 0, 1 or 2: a frequency in multiples of 100 Hz.
        Frequency,
        // HM digit 3: an HF or MF working channel number.
        HfChannel,
        // HM digit 9: a VHF channel, M H T U; M of 1 or 2 is the simplex note.
        VhfChannel,
        // Anything else: symbol 126 three times for "no information", or an
        // HM digit Table A1-5 does not assign.
        Other,
    };
    Kind kind = Kind::Other;
    std::uint64_t value = 0;
    std::array<std::uint8_t, 3> symbols{};
};

struct DscCall {
    std::uint8_t format = 0;

    // Clause 5.2: a nine-digit maritime identity from the ten digits of five
    // characters. Absent for distress alerts and all ships calls, clause 5.1.
    // A geographic area call's address is clause 5.3's ten digits and is in
    // `area_digits` instead.
    std::optional<std::uint64_t> address;
    std::optional<std::string> area_digits;

    // Clause 6. Absent for a distress alert, clause 6.1.
    std::optional<std::uint8_t> category;

    // Clause 7.1.
    std::uint64_t self_id = 0;

    // Clause 8.3.1, and clause 8.2 for a distress relay or acknowledgement,
    // which carries one telecommand.
    std::optional<std::uint8_t> telecommand1;
    std::optional<std::uint8_t> telecommand2;

    // Clauses 8.1 and 8.2: a distress alert's, relay's or acknowledgement's
    // distress information. `distress_id` is clause 8.2.1's Message 0, absent
    // on the alert itself.
    std::optional<std::uint64_t> distress_id;
    std::optional<std::uint8_t> nature_of_distress;
    // Clause 8.1.2: absent when sent as ten nines.
    std::optional<DscPosition> distress_position;
    // Clause 8.1.3, hours and minutes: absent when sent as 8888.
    std::optional<std::uint16_t> utc_hhmm;
    // Clause 8.1.4: the first telecommand the station in distress prefers.
    std::optional<std::uint8_t> subsequent_communications;

    // Clause 8.3.2: the channel or frequency elements, when Message 2 is made
    // of three-character elements.
    std::vector<DscFrequency> frequencies;

    // Every information character after the fixed fields and before the
    // end-of-sequence character, as sent, so nothing a format this file
    // does not interpret is lost.
    std::vector<std::uint8_t> message_symbols;

    // Clause 9: 117, 122 or 127.
    std::uint8_t eos = 0;

    // The information characters from the first format specifier to the
    // end-of-sequence character, and the error-check character.
    std::vector<std::uint8_t> symbols;
    std::uint8_t ecc = 0;

    // Characters whose DX copy failed the ten-bit check and whose RX copy was
    // used, clause 1.2.1's time diversity at work; and characters whose two
    // copies both checked and disagreed, settled by the error-check
    // character.
    int from_rx = 0;
    int disagreements = 0;

    // Audio sample index of the first bit of the phasing sequence's first
    // character and of the last bit of the error-check character's RX copy.
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct DscConfig {
    SampleRate rate = 48000;
    Hertz y_hz = kDscVhfYHz;
    Hertz b_hz = kDscVhfBHz;
    double bit_rate = kDscVhfBitRate;
};

struct DscStats {
    // Phasing found per clause 3.3.
    std::uint64_t phasings = 0;
    // Calls reported.
    std::uint64_t calls = 0;
    // Calls with a character lost in both copies, or no end of sequence
    // within kDscMaximumCharacters: not reported.
    std::uint64_t lost = 0;
    // Calls whose error-check character disagreed and could not be made to
    // agree by taking the RX copy where the two disagreed: not reported,
    // clause 10.3.
    std::uint64_t ecc_failures = 0;
    // Distress alerts and all ships calls whose two format specifiers
    // disagreed, which clause 4.2 asks a receiver to refuse.
    std::uint64_t format_mismatches = 0;
    // Calls that decoded but did not parse against the format their
    // specifier names: too short, or a symbol where a digit belongs.
    std::uint64_t malformed = 0;
};

// The longest call Tables A1-4.1 to A1-4.10 describe is an automatic service
// call: two format specifiers, five address, one category, five self-ID, two
// telecommands, three of frequency and up to nine of number, the end of
// sequence, 28 in all. A sequence that has not ended by this many is not one.
inline constexpr std::size_t kDscMaximumCharacters = 40;

class DscDecoder {
public:
    [[nodiscard]] static Expected<DscDecoder> create(const DscConfig& config);

    // Consumes audio and appends every call whose error-check character's RX
    // copy has arrived. State carries across calls, so any blocking of the
    // audio gives the same calls at the same sample indices.
    void process(ConstRealSpan audio, std::vector<DscCall>& out);

    [[nodiscard]] const DscStats& stats() const { return stats_; }

    void reset();

private:
    DscDecoder() = default;

    void on_bit(std::uint8_t bit, SampleIndex sample, std::vector<DscCall>& out);
    bool try_phasing();
    void on_slot(std::vector<DscCall>& out);
    void finish(std::vector<DscCall>& out);

    [[nodiscard]] std::optional<std::uint8_t> slot_symbol(std::size_t slot) const;

    DscConfig config_{};
    ToneDiscriminator discriminator_{};
    BitClock clock_{};
    std::vector<float> soft_;
    std::vector<SoftBit> bits_;

    // Every bit since the last trim, with the audio sample it came from, and
    // the stream index of bits_history_[0].
    std::vector<std::uint8_t> history_;
    std::vector<SampleIndex> history_samples_;
    std::uint64_t history_base_ = 0;
    std::uint64_t bit_count_ = 0;

    // Locked to a call: the stream index of the first bit of phasing slot 0,
    // and the next slot whose ten bits are awaited.
    bool locked_ = false;
    std::uint64_t slot0_bit_ = 0;
    std::size_t next_slot_ = 0;

    // The information characters resolved so far, and where the end of
    // sequence was.
    struct Resolved {
        std::optional<std::uint8_t> dx;
        std::optional<std::uint8_t> rx;
    };
    std::vector<Resolved> characters_;
    std::optional<std::size_t> eos_at_;

    DscStats stats_{};
};

// Parses resolved information characters, format specifier to end of
// sequence, against the format the first specifier names. Fails on a
// sequence too short for its format or with a command symbol where the
// format puts digits.
[[nodiscard]] Expected<DscCall> dsc_parse(std::span<const std::uint8_t> information);

}  // namespace revenant::decode
