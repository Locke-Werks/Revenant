// AX.25 at 9600 bit/s over the G3RUH scrambled FSK modem: an nfm receiver's
// discriminator audio to AX.25 frames.
//
// SPECIFICATION
//
// There is no standards document for the modem. This is written from J. R.
// Miller G3RUH, "9600 Baud Packet Radio Modem Design", Proceedings of the 1st
// RSGB Data Symposium, Harrow, July 1988, as republished by AMSAT at
// amsat.org/amsat/articles/g3ruh/109.html, read 2026-11-02. Sections used:
// the feature summary (FM with the audio applied direct to the varactor, a
// transmit FIR "typically -6 db at 4800 Hz, -60db at 7500 Hz", audio from
// the receiver discriminator), "Transmit Randomiser/Scrambler" and
// "Unscrambler" (the scrambling polynomial 1 + X^12 + X^17: "the currently
// transmitted bit is the EXOR of the current data bit, plus the bits that
// were transmitted 12 and 17 bits earlier", and the unscrambler EXORs the
// bit received now with those received 12 and 17 bits earlier, so it needs
// 17 bits to synchronise), and "Application - TNCs" (the modem replaces a
// TNC's internal modem at its TXData and RXData pins, which is why the TNC's
// own line coding, NRZI, sits inside the scrambler).
//
// Above the modem everything is AX.25 v2.2 (TAPR/ARRL, July 1998) as
// core/decode/ax25.h reads it: NRZI (Finnegan and Benson section 2 as
// ax25.h cites it, a zero is a change), HDLC flags and bit stuffing, and the
// X.25 FCS. This file reuses that header's deframer, FCS and parser.
//
// DEVIATIONS
//
// None. Not specified by the article and chosen here: the receive filter is
// core/decode/fsk.h's one-bit boxcar rather than the article's 6 kHz
// third-order Butterworth, which is the matched filter for the rectangular
// bit the boxcar assumes and close enough to the article's shaped one; and
// the polarity of the audio does not matter, since NRZI after the
// unscrambler reads a change, and an inverted stream unscrambles to the same
// inversion of every bit (the polynomial has an odd number of terms), which
// NRZI then ignores.
//
// WHAT IS NOT HERE
//
// 300 baud HF packet. docs/modes.md lists it beside this modem, but no
// document read for this work gives its tones, shift or sideband, and
// core/decode/ax25.h's Ax25Config would take them as three uncited numbers.
//
// CLEAN ROOM
//
// No G3RUH, AX.25 or TNC implementation was read.

#pragma once

#include <cstdint>
#include <vector>

#include "core/decode/ax25.h"
#include "core/decode/fsk.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr double kG3ruhBaud = 9600.0;
// "The scrambling polynomial is 1 + X^12 + X^17."
inline constexpr unsigned kG3ruhTapShort = 12;
inline constexpr unsigned kG3ruhTapLong = 17;

// The self-synchronising unscrambler: each output bit is the received bit
// EXOR the received bits 12 and 17 earlier.
class G3ruhUnscrambler {
public:
    [[nodiscard]] std::uint8_t push(std::uint8_t received);
    void reset() { history_ = 0; }

private:
    std::uint32_t history_ = 0;
};

// And the transmitter's: each sent bit is the data bit EXOR the sent bits 12
// and 17 earlier.
class G3ruhScrambler {
public:
    [[nodiscard]] std::uint8_t push(std::uint8_t data);
    void reset() { history_ = 0; }

private:
    std::uint32_t history_ = 0;
};

struct G3ruhConfig {
    // 48000 S/s is five samples a bit, which the tests show is enough; more
    // is better.
    SampleRate rate = 48000;
    double baud = kG3ruhBaud;
};

class G3ruhDecoder {
public:
    [[nodiscard]] static Expected<G3ruhDecoder> create(const G3ruhConfig& config);

    // Consumes discriminator audio and appends every frame whose FCS
    // checks. State carries across calls.
    void process(ConstRealSpan audio, std::vector<Ax25Frame>& out);

    [[nodiscard]] const Ax25Stats& stats() const { return stats_; }
    void reset();

private:
    G3ruhDecoder() = default;

    G3ruhConfig config_{};
    LevelDiscriminator discriminator_{};
    BitClock clock_{};
    G3ruhUnscrambler unscrambler_{};
    HdlcDeframer deframer_{};
    std::vector<float> soft_;
    std::vector<SoftBit> bits_;
    std::vector<HdlcFrame> frames_;
    std::uint8_t previous_ = 0;
    Ax25Stats stats_{};
};

}  // namespace revenant::decode
