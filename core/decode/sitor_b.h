// SITOR-B, the forward error correcting mode B of maritime narrow-band
// direct printing: receiver audio to ITA2 characters.
//
// SPECIFICATION
//
// Recommendation ITU-R M.625-4 (03/2012), "Direct-printing telegraph
// equipment employing automatic identification in the maritime mobile
// service", read from the ITU's own publication service. Clauses used: 1.1
// (the 7-unit constant ratio code), 1.2 (100 Bd), 1.3 (170 Hz shift, 1700 Hz
// audio centre), Table 1 (traffic information signals) with its notes 2 and
// 3, Table 2 (service information signals, mode B column), 4.2 (time
// diversity), 4.3 (choosing between the two copies), 4.4 (phasing), 4.6.4
// and 4.6.5 (when printing starts and what a lost character prints as) and
// 4.6.7 (end of transmission).
//
// M.476-5 (1995) is the earlier text of the same system and was read beside
// it; its Annex 1 Tables 1 and 2 and clause 3.2 agree with M.625-4 on every
// value used here. M.625-4 is cited because it is the one in force.
//
// HOW THE SIGNAL IS LAID OUT
//
// Every character is sent twice (clause 4.2): once in the DX position, then
// four signals later again in the RX position, 280 ms apart. The stream of
// 7-unit signals therefore alternates DX and RX, and the RX signal in slot
// 2k+5 repeats the DX signal in slot 2k. A signal is valid only if it holds
// exactly three Y and four B (clause 1.1 and Table 1), which is the whole of
// the error detection; clause 4.3 takes whichever copy is valid and treats
// two valid copies that differ as both mutilated.
//
// WHERE THIS STOPS
//
// At characters, collective mode only. The selective B-mode of clause 4.5
// sends its call and its traffic inverted, which reads here as mutilated
// signals, the same as it reads to every receiver it was not addressed to.
// NAVTEX framing is core/decode/navtex.h, on top of this.
//
// CLEAN ROOM
//
// No SITOR, AMTOR or NAVTEX implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/fsk.h"
#include "core/decode/rtty.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Specified constants
// ---------------------------------------------------------------------------

// M.625-4 clause 1.2: 100 Bd. Clause 4.2: a signal is 70 ms, seven units.
inline constexpr double kSitorBaud = 100.0;
inline constexpr std::size_t kSitorSignalUnits = 7;

// Clause 1.3: 170 Hz shift about a 1700 Hz audio centre.
inline constexpr Hertz kSitorCentreHz = 1700;
inline constexpr Hertz kSitorShiftHz = 170;

// Clause 4.2: the RX copy follows its DX copy after four other signals.
inline constexpr std::size_t kSitorDiversitySlots = 5;

// A 7-unit signal is held with bit position 1 in bit 0, per Table 1 note 3
// ("the bit in bit position 1 is transmitted first; B = 0, Y = 1"). Clause
// 1.1's constant ratio: three Y, which is three ones.
inline constexpr int kSitorOnesPerSignal = 3;

// The Table 2 mode B signals. Phasing signal 1 and idle signal alpha are one
// pattern; which it means depends on the position and the phase of the
// transmission.
[[nodiscard]] std::uint8_t sitor_signal(const char* by_units);
inline constexpr std::uint8_t kSitorPhasing1 = 0b1110000;  // BBBBYYY, also idle alpha
inline constexpr std::uint8_t kSitorPhasing2 = 0b0011001;  // YBBYYBB
inline constexpr std::uint8_t kSitorIdleBeta = 0b1001100;  // BBYYBBY

// What a received 7-unit signal is.
struct SitorSignal {
    enum class Kind : std::uint8_t {
        // Not three ones, or three ones in a pattern neither table lists.
        Mutilated,
        // Table 1: an ITA2 combination.
        Traffic,
        // Table 2, mode B.
        Phasing1OrAlpha,
        Phasing2,
        IdleBeta,
    };
    Kind kind = Kind::Mutilated;
    // For Traffic: the ITA2 combination, code element 1 in bit 0, as
    // core/decode/rtty.h holds it.
    std::uint8_t combination = 0;
};

[[nodiscard]] SitorSignal sitor_classify(std::uint8_t signal);

// Table 1, the other way: the 7-unit signal for an ITA2 combination.
[[nodiscard]] std::uint8_t sitor_encode(std::uint8_t combination);

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct SitorConfig {
    SampleRate rate = 48000;
    Hertz centre_hz = kSitorCentreHz;
    Hertz shift_hz = kSitorShiftHz;

    // Table 1 note 2: B is the higher emitted frequency. On an upper
    // sideband receiver it is the higher audio tone as well; on a lower
    // sideband one it is the lower. The decoder also finds the phasing
    // signals complemented and inverts on its own, so this sets only which
    // polarity it tries first.
    bool upper_sideband = true;

    // Clause 4.6.5: a character lost in both copies prints as a space or
    // "an error character (to be user-defined)". U+FFFD by default, so a
    // reader can tell a loss from a space.
    char32_t error_glyph = char32_t{0xFFFD};

    // Clause 4.6.6 leaves both the time and the percentage "predetermined".
    // Engineering choices: over the last 32 characters, half lost in both
    // copies returns the decoder to stand-by, where it waits to phase again.
    std::size_t mutilation_window = 32;
    double mutilation_limit = 0.5;

    // Clause 4.6.4: printing starts at the first carriage return or line
    // feed after phasing. NAVTEX turns this off, because ITU-R M.540-2
    // Annex II Figure 1 puts "ZCZC" straight after the phasing signals with
    // no line end before it, and a receiver waiting for one would lose the
    // start of every message.
    bool wait_for_line_end = true;
};

struct SitorCharacter {
    // Audio sample index of the first unit of the DX copy.
    SampleIndex position = 0;

    // ITA2 combination, meaningful when not mutilated.
    std::uint8_t combination = 0;
    char32_t glyph = 0;
    bool figures = false;

    // Both copies were lost, or both arrived valid and different.
    bool mutilated = false;
    // The DX copy was lost and the RX copy used.
    bool from_rx = false;

    // How many times the decoder had phased when this character was
    // decided. A change means a new transmission, or the same one found
    // again after a loss of phase, which is how a layer above tells that
    // what it was assembling was cut off without depending on how the audio
    // was blocked.
    std::uint64_t phasing = 0;
};

struct SitorStats {
    std::uint64_t phasings = 0;
    std::uint64_t characters = 0;
    std::uint64_t dx_mutilated = 0;
    std::uint64_t rx_mutilated = 0;
    std::uint64_t both_mutilated = 0;
    std::uint64_t ends_of_transmission = 0;
    std::uint64_t losses_of_phase = 0;
};

class SitorBDecoder {
public:
    [[nodiscard]] static Expected<SitorBDecoder> create(const SitorConfig& config);

    // Consumes audio and appends every character decided, which is when its
    // RX copy arrives. Clause 4.6.4: nothing is appended until a carriage
    // return or line feed has been received after phasing.
    void process(ConstRealSpan audio, std::vector<SitorCharacter>& out);

    [[nodiscard]] const SitorStats& stats() const { return stats_; }
    [[nodiscard]] bool phased() const { return phased_; }

    void reset();

private:
    SitorBDecoder() = default;

    void on_bit(std::uint8_t bit, SampleIndex sample, std::vector<SitorCharacter>& out);
    void on_signal(std::uint8_t signal, SampleIndex sample, std::vector<SitorCharacter>& out);
    void stand_by();

    SitorConfig config_{};
    ToneDiscriminator discriminator_{};
    BitClock clock_{};
    std::vector<float> soft_;
    std::vector<SoftBit> bits_;

    // Stand-by: the last 28 bits, four signals, for the phasing search.
    std::uint32_t shift_ = 0;
    std::uint64_t bit_count_ = 0;

    bool phased_ = false;
    bool inverted_ = false;
    bool printing_ = false;
    bool figures_ = false;

    std::uint8_t signal_ = 0;
    std::size_t unit_ = 0;
    SampleIndex signal_sample_ = 0;
    std::uint64_t slot_ = 0;

    struct Pending {
        std::uint8_t signal = 0;
        SampleIndex sample = 0;
    };
    // DX signals waiting for their RX copy.
    std::deque<Pending> dx_;
    int alpha_run_ = 0;
    // Signals left before stand-by once the end of transmission is seen.
    int closing_ = 0;
    std::deque<bool> recent_;
    std::size_t recent_lost_ = 0;

    SitorStats stats_{};
};

}  // namespace revenant::decode
