// The 77-bit structured message shared by FT8, FT4, MSK144, FST4 and Q65:
// source encoding of call signs, locators, reports and free text into fixed
// bit fields, the call sign hash codes, and the CRC-14 that guards the payload.
//
// SPECIFICATION
//
// S. Franke K9AN, B. Somerville G4WJS and J. Taylor K1JT, "The FT4 and FT8
// Communication Protocols", QEX July/August 2020 pp. 7-17: Table 1 (message
// types and their bit fields), Table 2 (the field tags), Table 7 (the c28
// value ranges), section 3 (the CRC) and Appendix A (field definitions). The
// algorithms Appendix A defers to are the short programs in that paper's
// reference [14], ft4_ft8_protocols.tgz, which section 9 of the paper places
// in the public domain: std_call_to_c28, hashcodes, nonstd_to_c58,
// free_text_to_f71, grid4_to_g15, grid6_to_g25 and gen_crc14, plus the two
// lookup tables states_provinces.txt and arrl_rac_sections.txt.
//
// DEVIATIONS AND CHOICES THE DOCUMENTS LEAVE OPEN
//
// - How a received message is printed is a display matter the paper shows
//   only by Table 1's examples. Those are followed exactly. A report with an
//   acknowledgment is printed "R-11", which the examples do not show.
// - Placement of a standard call in the six positions of std_call_to_c28:
//   the program right-adjusts, which suits a one-character prefix only. The
//   alphabets it indexes require the digit in position 3, so a call whose
//   second character is the digit gets one leading blank and any other is
//   left-adjusted. This is read off the alphabets, not stated.
// - c58 is left-adjusted, as getarg delivers it to nonstd_to_c58.
// - Values the tables leave unassigned unpack as "<?>" fields rather than
//   failing, so a valid CRC is never discarded over a display question.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/error.h"

namespace revenant::decode {

using Wsjt77 = std::array<std::uint8_t, 77>;

// Table 7.
inline constexpr std::uint32_t kWsjtNtokens = 2063592;
inline constexpr std::uint32_t kWsjtMax22 = 4194304;
// grid4_to_g15: the number of four-character locators, 18 * 18 * 10 * 10.
inline constexpr std::uint32_t kWsjtMaxGrid4 = 32400;

// What a received payload said, field by field. Empty strings are fields the
// message type does not carry.
struct WsjtMessage {
    int i3 = 0;
    int n3 = 0;
    // The message as Table 1 prints it.
    std::string text;
    // The addressee (or CQ, DE, QRZ), then the sender, as printed. A hashed
    // call reads "<CALL>" when the hash is known and "<...>" when not.
    std::string call_to;
    std::string call_de;
    // Type 0.1 only: the second station worked.
    std::string call_third;
    // Four- or six-character locator.
    std::string grid;
    // Signal report, "RRR", "RR73" or "73" as printed.
    std::string report;
    bool roger = false;
    // Contest exchange (types 0.3, 0.4, 3, 5), printed.
    std::string exchange;
    std::string free_text;
    std::string telemetry;
};

// Call sign hash codes, hashcodes.f90.
[[nodiscard]] std::uint32_t wsjt_hash(std::string_view call, int bits);

// Remembers the full calls seen, so a later hash can be printed as a call.
class WsjtHashTable {
public:
    void remember(std::string_view call);
    [[nodiscard]] std::optional<std::string> lookup(std::uint32_t hash, int bits) const;

private:
    std::unordered_map<std::uint32_t, std::string> h10_;
    std::unordered_map<std::uint32_t, std::string> h12_;
    std::unordered_map<std::uint32_t, std::string> h22_;
};

// Field encoders, each per the reference [14] program of the same name.
// Each fails on input that program would reject or that has no encoding.
[[nodiscard]] Expected<std::uint32_t> wsjt_c28(std::string_view word);
[[nodiscard]] Expected<std::uint32_t> wsjt_g15(std::string_view word);
[[nodiscard]] Expected<std::uint32_t> wsjt_g25(std::string_view grid6);
[[nodiscard]] Expected<std::uint64_t> wsjt_c58(std::string_view call);
// 71 bits, most significant first.
[[nodiscard]] Expected<std::array<std::uint8_t, 71>> wsjt_f71(std::string_view text);

// Whether a call fits std_call_to_c28, a /R or /P suffix aside.
[[nodiscard]] bool wsjt_is_standard_call(std::string_view call);

// Packs a message written the way Table 1 prints it. Handles every type in
// Table 1 except 0.5, which has its own entry point because any short word
// of hexadecimal digits is also valid free text. Text no structured type
// accepts goes as free text if it fits, and fails otherwise.
[[nodiscard]] Expected<Wsjt77> wsjt_pack(std::string_view text);
[[nodiscard]] Expected<Wsjt77> wsjt_pack_telemetry(std::string_view hex);

// Never fails: a field value outside the tables prints as "<?>".
// `hashes`, if given, both resolves hashes and learns every full call seen.
[[nodiscard]] WsjtMessage wsjt_unpack(const Wsjt77& bits, WsjtHashTable* hashes = nullptr);

// Section 3 and gen_crc14: the CRC-14 with polynomial 0x6757 and initial value
// zero over the payload zero-extended from 77 to 82 bits. Most significant bit
// first.
[[nodiscard]] std::array<std::uint8_t, 14> wsjt_crc14(std::span<const std::uint8_t> bits77);

}  // namespace revenant::decode
