// AIS, the automatic identification system: an FM receiver's discriminator
// audio to messages.
//
// SPECIFICATION
//
// Recommendation ITU-R M.1371-5 (02/2014), "Technical characteristics for an
// automatic identification system using time division multiple access in the
// VHF maritime mobile band", read from the ITU's own publication service.
// Clauses used, all of Annex 2 unless another annex is named: 2.1.1 and Table 3
// (the two channels, the bit rate, the training sequence, the BT products and
// the modulation index), 2.3.1 (GMSK after NRZI), 2.3.2 (modulation index
// 0.5), 2.3.3 (frequency stability), 2.4 (9600 bit/s), 2.5 and 3.2.2.3 (the
// 24-bit training sequence), 2.6 (NRZI), 3.2.2 (HDLC per ISO/IEC 13239 with
// the control field omitted), 3.2.2.1 (bit stuffing), 3.2.2.4 and 3.2.2.7 (the
// flags), 3.2.2.6 (the FCS), 3.2.2.11 (up to five slots), 3.3.7 and Table 17
// (field order and the bit order on the air), 3.3.7.1 (the message ID), and
// Annex 8 clause 3 with Table 47 (six-bit ASCII), Tables 48 and 49 (Messages
// 1, 2 and 3), Table 51 (Message 4), Table 52 and Figure 41 (Message 5), Table
// 70 (Message 18), Table 71 (Message 19), Table 73 (Message 21), and Tables 78
// and 79 (Message 24).
//
// M.1371-6 (02/2026) superseded it and is the edition in force; docs/modes.md
// records that. The brief this was written to named -5. -6 was read beside it
// for the messages decoded here: it moves the message descriptions from Annex
// 8 to Annex 7, renumbers the tables, and gives one spare bit a meaning in
// Messages 1 to 3 and 18 each, "Transmit power". Every field width and
// position this file reads is the same in both, checked by script against the
// two documents' own tables on 2026-09-23. The transmit power bit is not read.
//
// ISO/IEC 13239 was not held. Clause 3.2.2.6 fixes the FCS as its 16-bit CRC
// with the register preset to ones; what this file takes beyond that, the
// generator and the complemented result sent low octet first, is HDLC's
// frame check sequence as core/decode/ax25.h already implements it from
// Finnegan and Benson's statement of ISO 3309, whose FCS ISO/IEC 13239
// carries forward. It is not checked against a real transmitter here. So
// that a real capture can say whether the complement is right, a frame whose
// FCS matches only when NOT complemented is counted in
// AisStats::fcs_uncomplemented and not reported.
//
// WHAT THIS TAKES AS INPUT
//
// The audio an FM receiver's discriminator makes of the 25 kHz channel: an
// nfm receiver in the engine, whose kernel is the plain discriminator with no
// de-emphasis (core/engine/vrx.h resolves Nfm to Deemphasis::None) and whose
// default passband of plus and minus 8 kHz holds the GMSK signal: modulation
// index 0.5 at 9600 bit/s is a peak deviation of 2400 Hz. Not a complex tap.
// A decoder given complex baseband would discriminate it first and then do
// exactly what follows, so the engine's own discriminator is the one used.
//
// At least kAisMinimumRate samples a second: fewer than three samples to a
// bit leaves the receive filter below nothing to shape.
//
// HOW IT RECOVERS BITS
//
// As bursts, each on its own terms. The discriminator audio is summed over one
// bit, the integrate-and-dump filter matched to the rectangular bit the GMSK
// pulse is shaped from; textbook reception and not a clause. Then there is no
// bit clock: the stream is read at kAisPhases evenly spaced phases of the nominal
// 9600 bit/s at once. A transmission is at most five slots, 1280 bits by
// clause 3.2.2.11, and clause 2.4's 50 ppm moves the bit instant by 0.064 of
// a bit over that, so a fixed phase holds for a whole frame and there is
// nothing for a clock loop to track in the 24 training bits a loop would
// otherwise spend pulling in.
//
// Each phase fits its last 32 readings, by least squares, to the levels the
// training sequence and the start flag put on the air after NRZI. Where they
// correlate above AisConfig::sync_threshold a burst starts, and the fit's
// offset is its slicing level: the carrier's error from the channel centre,
// up to clause 2.3.3's 500 Hz, as the discriminator reports it. The burst is
// then read against that level, NRZI decoded and deframed on its own until
// its end flag, and a frame whose FCS checks is reported from whichever phase
// finished it first, the copies other phases found dropped.
//
// WHY A BOXCAR AND NOT CLAUSE 2.3.1.3'S BT 0.5. The clause says a receiver
// "should be designed for a BT-product of maximum 0.5", which reads as the
// transmitters it has to cope with rather than a filter to build, and the
// second version built one: a Gaussian of BT 0.5 over a one-bit rectangle.
// Against the one-bit boxcar alone, over the same 90 packets through an nfm
// receiver's 16 kHz channel, with the SNR stated against the mean power of
// the bursts and the 64-bit gaps between them together, which puts each
// burst about 0.9 dB above the figure, it lost 0.378 of them at 20 dB where
// the boxcar lost 0.144, and 0.722 at 18 dB where the boxcar lost 0.544.
// Two more things were tried and bought nothing measurable: limiting the
// discriminator's clicks at 3600 Hz, as core/decode/fsk.h does for POCSAG,
// ahead of a near-rectangular filter (a Gaussian of BT 3) left it losing
// 0.156 at 20 dB, as it did unlimited; and summing the phase over the bit
// wrapped into plus and minus pi, which removes a click's whole turn exactly,
// gave 0.144 and 0.544, the boxcar's own figures. Neither is here.
//
// WHY PER BURST AND NOT A RUNNING MEAN. The first version removed a running
// mean, core/decode/fsk.h's LevelDiscriminator, and lost 8 of 90 packets at
// 30 dB, on the same footing, with a 16-bit average, seven of them Message 1, and at
// least 40 of 90 with a 128-bit one. The channel between two
// bursts is noise, a running mean carries the noise's mean into the start of
// each burst, and the longer the average the longer that lasts. The training
// sequence is there to set a receiver up, clause 2.5, and fitting it gives
// each burst its own level with nothing carried over from the noise.
//
// WHERE THIS STOPS
//
// At the fields of Messages 1, 2, 3, 4, 5, 18, 19, 21 and 24, and of Message
// 11, which Table 51 lays out as Message 4. Any other message whose FCS
// checks is reported with its message ID and its octets and nothing parsed.
// Communication states are handed out raw. The binary messages' application
// data, the long-range Message 27 and the satellite channels are not
// decoded; docs/modes.md lists them.
//
// CLEAN ROOM
//
// No AIS implementation was read. The gpsd project's AIVDM/AIVDO protocol
// write-up, which is documentation, was read for two sentences real stations
// sent and the IEC 61162 armouring they are written in, and is used by
// tests/decode/test_ais.cpp and nowhere here; docs/clean-room.md records it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstRealSpan;
using dsp::Hertz;
using dsp::SampleIndex;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Specified constants, M.1371-5 Annex 2
// ---------------------------------------------------------------------------

// Table 3, PH.AIS1 and PH.AIS2: the default channels, 2087 and 2088.
inline constexpr Hertz kAisChannel1Hz = 161'975'000;
inline constexpr Hertz kAisChannel2Hz = 162'025'000;

// Clause 2.4 and Table 3, PH.BR.
inline constexpr double kAisBitRate = 9600.0;

// Clause 2.3.2 and Table 3, PH.MI: modulation index 0.5, so the peak
// deviation is a quarter of the bit rate.
inline constexpr double kAisModulationIndex = 0.5;
inline constexpr double kAisDeviationHz = kAisModulationIndex * kAisBitRate / 2.0;

// Clause 2.3.1.2 and Table 3, PH.TXBT: the transmitter's Gaussian filter.
// Clause 2.3.1.3 and PH.RXBT: the product a demodulator is designed for.
inline constexpr double kAisTransmitBt = 0.4;
inline constexpr double kAisReceiveBt = 0.5;

// Clause 2.5 and 3.2.2.3: 24 bits of alternating zeros and ones before the
// start flag, not bit stuffed.
inline constexpr std::size_t kAisTrainingBits = 24;

// Clauses 3.2.2.4 and 3.2.2.7: both flags are the HDLC flag.
inline constexpr std::uint8_t kAisFlag = 0x7E;

// Clause 3.2.2.1: a zero is inserted after five consecutive ones.
inline constexpr int kAisStuffAfterOnes = 5;

// Clause 3.2.2.11: at most five consecutive slots, and Table 6 and clause
// 3.2.2.2 put 256 bits in a slot. The longest frame this looks for is five
// slots of data, FCS and stuffing, which bounds a deframer that has lost the
// end flag.
inline constexpr std::size_t kAisMaximumFrameBits = 5 * 256;

// The shortest message the Recommendation defines is 72 bits, Message 10
// (Annex 8 Table 60); Message 25 with no binary data is 40 (Table 80). A frame
// shorter than 40 bits of data is not a message.
inline constexpr std::size_t kAisMinimumMessageOctets = 5;

// Clause 3.3.7.1: six bits.
inline constexpr unsigned kAisMessageIdBits = 6;

// Annex 8 Table 48: longitude and latitude in 1/10000 minute, and their "not
// available" values, 181 and 91 degrees, printed as 6791AC0h and 3412140h.
inline constexpr std::int32_t kAisLongitudeNotAvailable = 0x6791AC0;
inline constexpr std::int32_t kAisLatitudeNotAvailable = 0x3412140;
inline constexpr double kAisPositionUnitsPerDegree = 600000.0;

// Table 48: SOG 1023, COG 3600 and heading 511 are "not available".
inline constexpr std::uint32_t kAisSogNotAvailable = 1023;
inline constexpr std::uint32_t kAisCogNotAvailable = 3600;
inline constexpr std::uint32_t kAisHeadingNotAvailable = 511;

// ---------------------------------------------------------------------------
// The receiver's own choices
// ---------------------------------------------------------------------------

// Slicing phases per bit. An engineering choice: eight puts the nearest phase
// within a sixteenth of a bit of the true instant, where GMSK at BT 0.4 has
// lost next to nothing of its eye, and it costs eight deframers a bit.
inline constexpr std::size_t kAisPhases = 8;

// Three samples a bit, 28800 S/s. See WHAT THIS TAKES AS INPUT.
inline constexpr SampleRate kAisMinimumRate = 28800;

// ---------------------------------------------------------------------------
// The link layer
// ---------------------------------------------------------------------------

// Clause 3.2.2.6. The HDLC frame check sequence over the data portion, the
// value clause 3.3.7's bit order puts on the air low octet first.
[[nodiscard]] std::uint16_t ais_fcs(std::span<const std::uint8_t> data);

// Clause 3.3.7: the message's fields are defined most significant bit first
// and grouped into octets top to bottom, and each octet goes on the air least
// significant bit first. An octet assembled in the order its bits arrived,
// the first into bit 0, is therefore the octet as the message tables write
// it, and bit k of the message is bit 7 - (k mod 8) of octet k / 8.
[[nodiscard]] std::uint32_t ais_field(std::span<const std::uint8_t> octets, std::size_t first_bit,
                                      unsigned width);
[[nodiscard]] std::int32_t ais_signed_field(std::span<const std::uint8_t> octets,
                                            std::size_t first_bit, unsigned width);

// Annex 8 Table 47: the six-bit character set. 0 to 31 are "@" to "_", ASCII
// 64 to 95, and 32 to 63 are ASCII 32 to 63.
[[nodiscard]] char ais_sixbit_char(unsigned value);

// `characters` six-bit characters from `first_bit`. Clause 3.3.7: unused
// characters are "@" and sit at the end, so trailing "@" are removed.
// Trailing spaces are removed as well, which no clause asks for: stations
// pad with them, and a name that ends in a space cannot be told apart from
// one that does not.
[[nodiscard]] std::string ais_text(std::span<const std::uint8_t> octets, std::size_t first_bit,
                                   std::size_t characters);

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Figure 41: dimensions A and B of 9 bits and C and D of 6, in metres, A the
// most significant field. Also Figure 41bis's AtoN reading of the same bits.
struct AisDimensions {
    std::uint32_t to_bow = 0;
    std::uint32_t to_stern = 0;
    std::uint32_t to_port = 0;
    std::uint32_t to_starboard = 0;
};

// Longitude and latitude in degrees, east and north positive. Absent when
// the station sent the "not available" value.
struct AisPosition {
    double longitude = 0.0;
    double latitude = 0.0;
};

struct AisMessage {
    // Clause 3.3.7.1 and every table's first three rows.
    std::uint8_t message_id = 0;
    std::uint8_t repeat = 0;
    std::uint32_t mmsi = 0;

    // Messages 1 to 4, 18, 19 and 21.
    std::optional<AisPosition> position;
    bool position_accuracy = false;
    bool raim = false;
    // Table 48: UTC second of the report, 60 to 63 for the unavailable cases.
    std::optional<std::uint8_t> timestamp;

    // Messages 1 to 3, 18 and 19, raw and as the units the tables give.
    // SOG in knots and COG in degrees are absent when "not available".
    std::optional<std::uint16_t> sog_tenths;
    std::optional<std::uint16_t> cog_tenths;
    std::optional<std::uint16_t> heading;

    // Messages 1 to 3.
    std::optional<std::uint8_t> navigational_status;
    // Table 48 ROTAIS as sent, -128 to 127; -128 is "no turn information".
    std::optional<std::int8_t> rate_of_turn;
    std::optional<std::uint8_t> special_manoeuvre;

    // Messages 1 to 4 and 18: the 19-bit communication state, raw.
    std::optional<std::uint32_t> communication_state;

    // Message 4.
    struct Utc {
        std::uint16_t year = 0;
        std::uint8_t month = 0;
        std::uint8_t day = 0;
        std::uint8_t hour = 0;
        std::uint8_t minute = 0;
        std::uint8_t second = 0;
    };
    std::optional<Utc> utc;

    // Messages 4, 5, 19, 21 and 24B: Table 51's type of position fixing device.
    std::optional<std::uint8_t> epfd;

    // Messages 5, 19, 21 and 24.
    std::optional<std::string> name;
    std::optional<std::string> callsign;
    std::optional<std::uint8_t> ship_type;
    std::optional<AisDimensions> dimensions;

    // Message 5.
    std::optional<std::uint8_t> ais_version;
    std::optional<std::uint32_t> imo_number;
    struct Eta {
        std::uint8_t month = 0;
        std::uint8_t day = 0;
        std::uint8_t hour = 0;
        std::uint8_t minute = 0;
    };
    std::optional<Eta> eta;
    std::optional<std::uint8_t> draught_tenths;
    std::optional<std::string> destination;
    std::optional<bool> dte_not_available;

    // Message 18.
    struct ClassBFlags {
        bool carrier_sense = false;
        bool display = false;
        bool dsc = false;
        bool whole_band = false;
        bool message_22 = false;
    };
    std::optional<ClassBFlags> class_b;
    std::optional<bool> assigned_mode;

    // Message 21.
    std::optional<std::uint8_t> aton_type;
    std::optional<bool> off_position;
    std::optional<std::uint8_t> aton_status;
    std::optional<bool> virtual_aton;

    // Message 24: 0 for Part A, 1 for Part B. Part B's vendor ID is Table
    // 79A's three fields: the manufacturer's three six-bit characters, the
    // unit model code and the serial number.
    std::optional<std::uint8_t> part_number;
    std::optional<std::string> vendor;
    std::optional<std::uint8_t> unit_model;
    std::optional<std::uint32_t> unit_serial;

    // True when the message ID is one this file parses and the frame was long
    // enough for its table. False for any other message whose FCS checked,
    // which comes out with the fields above empty but for the first three.
    bool parsed = false;

    // The data portion as received, FCS removed.
    std::vector<std::uint8_t> octets;

    // Audio sample index of the first bit after the start flag and of the
    // last bit of the end flag, less the receive filter's delay.
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;

    // Which of the kAisPhases slicing phases recovered it first.
    std::uint8_t phase = 0;
};

// Parses a data portion whose FCS has checked. Never fails: a message ID this
// file does not parse, or a frame shorter than its table, comes back with
// `parsed` false and the octets.
[[nodiscard]] AisMessage ais_parse(std::span<const std::uint8_t> octets);

// Table 48's longitude and latitude fields to degrees, or nothing for the
// "not available" values and for a value outside the plus and minus 180 and
// 90 degrees the table allows, which no position can be.
[[nodiscard]] std::optional<AisPosition> ais_position(std::int32_t longitude_units,
                                                      std::int32_t latitude_units);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct AisConfig {
    SampleRate rate = 48000;

    // The correlation between the last 32 bits a phase read and the levels
    // the training sequence and start flag put on the air, above which a
    // burst is taken to start there. An engineering choice. Noise alone gets
    // over it: measured in tests/decode/test_ais.cpp on two minutes of an nfm
    // receiver's discriminator with no signal, 174 false starts, of which 6
    // ran to a whole number of octets and none passed the FCS, and a false
    // start costs only the readings until it ends. A correlation r between a
    // pattern and a reading of it in white noise is a reading signal to noise
    // ratio of r^2 / (1 - r^2), 0.96 at 0.7, and a burst read at 0 dB loses
    // its bits anyway.
    double sync_threshold = 0.7;
};

struct AisStats {
    // Bursts started on the training sequence, on every phase.
    std::uint64_t syncs = 0;
    // Flag-bounded runs of whole octets, from every phase, long enough to be
    // a message.
    std::uint64_t candidates = 0;
    // Of those, the ones whose FCS did not check.
    std::uint64_t fcs_failures = 0;
    // Of the failures, the ones whose FCS would have checked had the CRC not
    // been complemented. See SPECIFICATION.
    std::uint64_t fcs_uncomplemented = 0;
    // Frames that checked on more than one phase, reported once.
    std::uint64_t duplicates = 0;
    // Frames reported.
    std::uint64_t messages = 0;
};

class AisDecoder {
public:
    [[nodiscard]] static Expected<AisDecoder> create(const AisConfig& config);

    // Consumes discriminator audio and appends every message whose end flag
    // has arrived and whose FCS checks, once, in the order their end flags
    // arrived. State carries across calls, so any blocking of the audio gives
    // the same messages at the same sample indices.
    void process(ConstRealSpan audio, std::vector<AisMessage>& out);

    [[nodiscard]] const AisStats& stats() const { return stats_; }

    void reset();

private:
    AisDecoder() = default;

    // One burst being read on one phase, from the end of its start flag: the
    // slicing level its training sequence gave, the NRZI and deframing state,
    // and the data bits so far.
    struct Attempt {
        double threshold = 0.0;
        bool previous_level = false;
        int ones = 0;
        std::size_t run_start = 0;
        std::vector<std::uint8_t> bits;
        SampleIndex first_sample = 0;
        bool have_first = false;
    };

    // The last kSyncBits readings of a phase, oldest first once full, and
    // the bursts it is reading.
    static constexpr std::size_t kSyncBits = kAisTrainingBits + 8;
    struct Phase {
        std::array<float, kSyncBits> ring{};
        std::size_t head = 0;
        std::size_t filled = 0;
        std::vector<Attempt> attempts;
    };

    // Bursts one phase reads at once. A false start in noise and a real
    // burst behind it both get read; beyond this a new start is dropped.
    static constexpr std::size_t kAttemptsPerPhase = 4;

    void on_reading(Phase& phase, std::uint8_t phase_index, float value, SampleIndex sample,
                    std::vector<AisMessage>& out);
    // False when the attempt has ended, by a frame, an abort or its length.
    bool on_bit(Attempt& attempt, std::uint8_t phase_index, float value, SampleIndex sample,
                std::vector<AisMessage>& out);
    void on_frame(const Attempt& attempt, std::uint8_t phase_index, SampleIndex last_sample,
                  std::vector<AisMessage>& out);

    AisConfig config_{};

    // The one-bit boxcar. See HOW IT RECOVERS BITS.
    RealFir filter_{};
    std::size_t filter_delay_ = 0;
    std::vector<float> soft_;

    // The training sequence and start flag as NRZI levels, starting from +1,
    // and the constants of a least-squares fit against them.
    std::array<float, kSyncBits> pattern_{};
    double pattern_mean_ = 0.0;
    double pattern_spread_ = 0.0;

    // The soft stream's last two samples, so an instant between two calls
    // can still be interpolated, and how many soft samples have been seen.
    float previous_soft_ = 0.0F;
    std::uint64_t soft_count_ = 0;

    // The next slicing instant, counted in kAisPhases-ths of a bit from the
    // start of the stream. Instant m falls at m * rate / (9600 * kAisPhases)
    // soft samples and belongs to phase m mod kAisPhases, so the instants
    // are exact rationals and never accumulate rounding.
    std::uint64_t next_instant_ = 0;
    std::uint64_t rate_ = 0;
    std::uint64_t instant_denominator_ = 0;

    std::array<Phase, kAisPhases> phases_{};

    // Recently reported frames, for dropping the copies other phases find.
    struct Reported {
        std::vector<std::uint8_t> octets;
        SampleIndex last_sample = 0;
    };
    std::vector<Reported> recent_;

    AisStats stats_{};
};

}  // namespace revenant::decode
