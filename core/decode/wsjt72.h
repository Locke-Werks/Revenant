// The 72-bit message of JT65, JT9 and JT4, as far as free documents
// describe it.
//
// SPECIFICATION
//
// Plain text: A. Talbot G4JNT, "The JT9 Coding Process (Plain Text messages
// only)", March 2016, http://www.g4jnt.com/JT9_Coding_Process.pdf, section
// "Source Coding", identical in "The JT4 Coding Process", July 2009: 13
// characters from a 42-character alphabet as N1, N2 (five characters each)
// and N3 (three), rearranged to N1 * 2 + bit 15 of N3, N2 * 2 + bit 16 of N3
// and N3 mod 32768 + 32768, where the added 32768 is the plain-text flag, and
// laid end to end as N1 * 2^44 + N2 * 2^16 + N3.
//
// Standard messages: J. Taylor K1JT, "The JT65 Communications Protocol",
// QEX September/October 2005 pp. 3-12, section 4: two 28-bit callsigns and a
// 15-bit locator, with the 72nd bit marking text. The callsign and locator
// integers are the ones G4JNT's "The WSPR Coding Process" (2009) spells out,
// which section 4 attributes to the same source (Clark and Karn, 1996). The
// field order (callsign 1, callsign 2, then the 16-bit word whose top bit is
// the text flag and whose low 15 bits are the locator) is the plain-text
// layout above; that standard messages share it is confirmed by Figure 2 of
// the QEX paper, whose three packed messages this reproduces exactly.
//
// GAPS
//
// Section 4 says some of the 6 million unused callsign values stand for CQ,
// QRZ and CQ with a frequency, and that reports and prefixed or suffixed
// calls replace the locator, but defers the values to the program's source
// code (Appendix A), which this project does not read. So CQ, QRZ, DE,
// reports, RO/RRR/73 and Appendix B prefixes are neither packed nor unpacked:
// such a message unpacks with `standard` false and its fields shown as raw
// numbers.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/error.h"

namespace revenant::decode {

inline constexpr int kWsjt72Bits = 72;
// Most significant bit first.
using Wsjt72 = std::array<std::uint8_t, kWsjt72Bits>;

// The Clark and Karn 28-bit callsign and 15-bit locator, as written out in
// G4JNT's "The WSPR Coding Process", "Source Coding".
[[nodiscard]] std::optional<std::uint32_t> wsjt_call28(std::string_view call);
[[nodiscard]] std::optional<std::string> wsjt_call28_text(std::uint32_t n);
[[nodiscard]] std::optional<std::uint32_t> wsjt_grid15(std::string_view grid);
[[nodiscard]] std::optional<std::string> wsjt_grid15_text(std::uint32_t m1);

// 13 characters at most, padded with spaces at the end.
[[nodiscard]] Expected<Wsjt72> wsjt72_pack_text(std::string_view text);
// "CALL1 CALL2 GRID".
[[nodiscard]] Expected<Wsjt72> wsjt72_pack_standard(std::string_view call1, std::string_view call2,
                                                    std::string_view grid);
// Text if it parses as a standard message, else plain text.
[[nodiscard]] Expected<Wsjt72> wsjt72_pack(std::string_view message);

struct Wsjt72Message {
    bool plain_text = false;
    // True when every field is one the documents define.
    bool standard = false;
    std::string text;
};
[[nodiscard]] Wsjt72Message wsjt72_unpack(const Wsjt72& bits);

// What JT9, JT4 and JT65 report for each message found in a slot.
struct Wsjt72Decode {
    double slot_utc = 0.0;
    // Index of the slot's first sample, in input samples.
    long long slot_start_sample = 0;
    double frequency_hz = 0.0;  // tone 0, mid transmission
    double dt_seconds = 0.0;    // start against the nominal
    double drift_hz = 0.0;
    double snr_db = 0.0;        // in 2500 Hz
    Wsjt72 payload{};
    Wsjt72Message message;
};

}  // namespace revenant::decode
