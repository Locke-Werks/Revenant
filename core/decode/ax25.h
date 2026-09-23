// AX.25 over 1200 baud Bell 202 AFSK: receiver audio to link-layer frames.
//
// SPECIFICATION
//
// The frame is "AX.25 Link Access Protocol for Amateur Packet Radio",
// Version 2.2, July 1998, TAPR, read from the copy at ax25.net
// ("AX25.2.2-Jul 98-2.pdf", 135 pages). Clauses used: 3 (frame structure and
// Figures 3.1a and 3.1b), 3.1 (flag), 3.6 (bit stuffing), 3.7 (the FCS, by
// reference to ISO 3309), 3.8 (order of bit transmission), 3.9 (invalid
// frames), 3.10 (abort), 3.12 with Figures 3.3 to 3.8 (address encoding), 3.4
// (PID) and 4.2.1 with Figures 4.1a and 4.4 (control field).
//
// The modem has no standards document, as docs/modes.md records. It is cited
// to K. W. Finnegan and B. Benson, "Clarifying the Amateur Bell 202 Modem",
// TAPR/ARRL Digital Communications Conference 2014, read from
// files.tapr.org, which collects what the amateur modem inherited from
// Bell System Technical Reference PUB 41212 and from HDLC. Section 2 of that
// paper for the tones, the rate and NRZI; section 3.2 and its Figure 5 for how
// the ISO 3309 FCS is computed and put on the air. Its Appendix A is a C
// listing and was not read.
//
// ISO 3309 itself was not held. What this file takes from it, the CRC-CCITT
// generator with the register preset to ones and the result complemented,
// is stated in the Finnegan and Benson paper's Figure 5, and it is the same
// ITU-T X.25 frame check sequence that core/decode/dv_codes.h already
// implements for TETRA from EN 300 392-2 clause 8.2.3.3. tests/decode/
// test_ax25.cpp checks the two against each other, which is the only
// independent check this file's FCS gets.
//
// WHERE THIS STOPS
//
// At frames whose FCS checks, with the address, control and PID fields
// parsed and the information field handed out. Connected-mode procedure,
// clauses 4.3 onward and 6, is a conversation between two stations and has
// nothing to decode. The control field is read as the one-octet modulo 8
// form: whether a connection negotiated modulo 128 (clause 4.2.1, Figure
// 4.1b) is state a receiver joining partway cannot know, so an I or S frame
// on such a connection has its sequence numbers misread, and the frame is
// still reported.
//
// CLEAN ROOM
//
// No AX.25, HDLC or APRS implementation was read.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/fsk.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// The modem, from Finnegan and Benson 2014
// ---------------------------------------------------------------------------

// Section 2: "an audio frequency shift keyed (AFSK) modulation that encodes
// data by shifting between 1200Hz and 2200Hz audio tones ... at a rate of
// 1200 symbols per second."
inline constexpr Hertz kBell202MarkHz = 1200;
inline constexpr Hertz kBell202SpaceHz = 2200;
inline constexpr double kBell202Baud = 1200.0;

// Section 2: NRZI, where a zero is sent as a change of tone and a one as no
// change. Which tone is which therefore carries nothing, and the decoder
// never needs to know.

// ---------------------------------------------------------------------------
// The frame, from AX.25 2.2
// ---------------------------------------------------------------------------

// Clause 3.1: "a zero followed by six ones followed by another zero, or
// 01111110 (7E hex)."
inline constexpr std::uint8_t kAx25Flag = 0x7E;

// Clause 3.6: a zero is inserted after five contiguous ones.
inline constexpr int kAx25StuffAfterOnes = 5;

// Clause 3.10: an abort is "at least fifteen contiguous 1s". A receiver sees
// seven, since six ones end in a flag and a seventh is something no frame or
// flag can contain; seven is where this decoder drops a frame in progress.
inline constexpr int kAx25AbortOnes = 7;

// Clause 3.9: a frame of fewer than 136 bits, counting both flags, is
// invalid. Without the two flags that is 120 bits, 15 octets.
inline constexpr std::size_t kAx25MinimumFrameBits = 136;
inline constexpr std::size_t kAx25MinimumContentOctets = (kAx25MinimumFrameBits - 16) / 8;

// Clause 3.12: each address subfield is seven octets, six of call sign and
// one of SSID. Clause 3.12.4 limits Layer 2 repeaters to two; the APRS
// Protocol Reference chapter 3 allows eight digipeater addresses in an APRS
// frame, so the parser accepts up to eight and reports how many.
inline constexpr std::size_t kAx25AddressOctets = 7;
inline constexpr std::size_t kAx25MaximumRepeaters = 8;

// Figure 4.4: the UI control field is 000P0011, 0x03 with P clear.
inline constexpr std::uint8_t kAx25ControlUi = 0x03;
inline constexpr std::uint8_t kAx25PollFinalBit = 0x10;

// Figure 3.2: 0xF0 is "No Layer 3 Protocol".
inline constexpr std::uint8_t kAx25PidNoLayer3 = 0xF0;

// Clause 3.7 by way of ISO 3309, computed as Finnegan and Benson Figure 5
// states it: register preset to 0xFFFF, each octet taken least significant
// bit first against the bit-reversed CRC-CCITT generator 0x8408, and the
// result complemented.
//
// Clause 3.8 sends the FCS most significant bit first. The register above
// holds it bit-reversed, so sending the low octet first, each octet least
// significant bit first as for every other field, puts bit 15 of the FCS on
// the air first. That is section 3.2 of the paper, and it is why the
// transmitter needs no special case for the FCS.
[[nodiscard]] std::uint16_t ax25_fcs(std::span<const std::uint8_t> octets);

// One address subfield, clause 3.12.
struct Ax25Address {
    // Upper-case ASCII, trailing space padding removed.
    std::string callsign;
    std::uint8_t ssid = 0;

    // Bit 7 of the SSID octet. The C bit in the destination and source
    // subfields (clause 6.1.2), the H has-been-repeated bit in a repeater
    // subfield (clause 3.12.4).
    bool command_or_repeated = false;

    // Bits 5 and 6, the two reserved R bits, which clause 3.12.2 says are
    // one when not implemented.
    std::uint8_t reserved = 0b11;
};

// Encodes a subfield into its seven octets. `last` sets the extension bit,
// which clause 3.12 puts on the final octet of the whole address field only.
// The call sign must be one to six upper-case letters or digits (clause
// 3.12) and the SSID below 16.
[[nodiscard]] Expected<std::array<std::uint8_t, kAx25AddressOctets>> ax25_encode_address(
    const Ax25Address& address, bool last);

enum class Ax25FrameKind : std::uint8_t {
    // Figure 4.1a: bit 0 clear.
    Information,
    // Bits 0 and 1 are 01.
    Supervisory,
    // Bits 0 and 1 are 11. UI is the one APRS uses and is called out below.
    Unnumbered,
};

struct Ax25Frame {
    Ax25Address destination;
    Ax25Address source;
    std::vector<Ax25Address> repeaters;

    std::uint8_t control = 0;
    Ax25FrameKind kind = Ax25FrameKind::Unnumbered;

    // Figure 4.4 with the P/F bit masked off.
    bool is_ui = false;

    // Clause 3.4: present in I and UI frames only.
    std::optional<std::uint8_t> pid;

    std::vector<std::uint8_t> information;

    // Address through information, as received, FCS removed.
    std::vector<std::uint8_t> octets;

    // Audio sample index of the first bit after the opening flag and of the
    // last bit of the closing flag. Both are where the bit clock placed those
    // bits, less the discriminator's delay.
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;
};

// Parses address through information, FCS already checked and removed.
// Fails on a frame shorter than clause 3.9 allows, on an address field that
// never sets its extension bit or holds more repeaters than
// kAx25MaximumRepeaters, and on a call sign that is not upper-case letters,
// digits and trailing spaces.
[[nodiscard]] Expected<Ax25Frame> ax25_parse(std::span<const std::uint8_t> octets);

// The call sign and SSID as a station writes them, "N7LEM-1", or "N7LEM" for
// SSID 0.
[[nodiscard]] std::string ax25_address_text(const Ax25Address& address);

// ---------------------------------------------------------------------------
// HDLC, bit level
// ---------------------------------------------------------------------------

// A frame's bits, flag to flag, unstuffed, as the deframer hands them over.
struct HdlcFrame {
    // Octets between the flags, least significant bit first as clause 3.8
    // sends them, including the two FCS octets.
    std::vector<std::uint8_t> octets;
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;
};

// Flag search, bit unstuffing, abort detection and octet assembly, clauses
// 3.1, 3.6, 3.9 and 3.10. Fed one decoded bit at a time with the audio
// sample it came from. Emits every flag-bounded run of whole octets at least
// clause 3.9's minimum long; the FCS is the caller's to check, because a
// caller measuring a channel wants the frames that failed it as well.
class HdlcDeframer {
public:
    void push(std::uint8_t bit, SampleIndex sample, std::vector<HdlcFrame>& out);
    void reset();

private:
    std::vector<std::uint8_t> bits_;
    SampleIndex first_sample_ = 0;
    int ones_ = 0;
    bool in_frame_ = false;

    // Length of bits_ when the current run of ones began, so a flag can take
    // its own bits back out of the data.
    std::size_t run_start_ = 0;
    bool awaiting_first_ = false;
};

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

struct Ax25Config {
    SampleRate rate = 48000;
    Hertz mark_hz = kBell202MarkHz;
    Hertz space_hz = kBell202SpaceHz;
    double baud = kBell202Baud;
};

struct Ax25Stats {
    // Flag-bounded runs of whole octets long enough to be a frame.
    std::uint64_t candidates = 0;
    // Of those, the ones whose FCS did not check.
    std::uint64_t fcs_failures = 0;
    // FCS good but the address field did not parse.
    std::uint64_t malformed = 0;
};

class Ax25Decoder {
public:
    [[nodiscard]] static Expected<Ax25Decoder> create(const Ax25Config& config);

    // Consumes audio and appends every frame whose closing flag has arrived
    // and whose FCS checks. State carries across calls, so any blocking of
    // the audio gives the same frames.
    void process(ConstRealSpan audio, std::vector<Ax25Frame>& out);

    [[nodiscard]] const Ax25Stats& stats() const { return stats_; }

    void reset();

private:
    Ax25Decoder() = default;

    ToneDiscriminator discriminator_{};
    BitClock clock_{};
    HdlcDeframer deframer_{};
    std::vector<float> soft_;
    std::vector<SoftBit> bits_;
    std::vector<HdlcFrame> frames_;
    bool previous_level_ = false;
    Ax25Stats stats_{};
};

}  // namespace revenant::decode
