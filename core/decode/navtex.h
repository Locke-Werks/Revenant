// NAVTEX: SITOR-B characters to messages with their preamble.
//
// SPECIFICATION
//
// Recommendation ITU-R M.540-2 (1990), "Operational and technical
// characteristics for an automated direct-printing telegraph system for
// promulgation of navigational and meteorological warnings and urgent
// information to ships", read from the ITU's own publication service. Annex
// II: clause 2 (collective B-mode per M.476 and M.625, which is
// core/decode/sitor_b.h), Figure 1 (the transmission format), clauses 2.1
// and 2.2 (the B1 to B4 characters), clause 3 (printing only on an error-free
// preamble) and clause 6 (serial number 00).
//
// Figure 1, read as a sequence: phasing signals, "ZCZC", one space, B1 B2 B3
// B4, carriage return and line feed, the message, "NNNN", carriage return
// and two line feeds; repeated for further messages after at least 5 s of
// phasing; and at least 2 s of idle signal alpha to end.
//
// WHAT IS NOT HERE
//
// The meaning of the letters. M.540-2 clause 2.1 says B1 identifies the
// transmitter coverage area and B2 the type of message, "defined by IMO",
// and the definitions are in the IMO NAVTEX Manual, which was not held when
// this was written. This decoder reports the letters and the serial number
// and says nothing about what they mean, which is the gap docs/modes.md
// records for this row. Filtering by B1 and B2 (clauses 2.1.1 and 2.1.2) and
// suppressing repeats (clause 4) are a receiver's policy over these fields
// and are left to the caller.
//
// CLEAN ROOM
//
// No NAVTEX implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/sitor_b.h"
#include "core/error.h"

namespace revenant::decode {

struct NavtexMessage {
    // Annex II clause 2: B1 a letter A to Z for the coverage area, B2 a
    // letter A to Z for the message type, B3 B4 a two-digit serial number.
    char area = 0;
    char subject = 0;
    int serial = -1;

    // Clause 3: "The printer should only be activated if the preamble B1-B4
    // is received without errors." True when all four arrived unmutilated
    // and in the forms clause 2 gives them. A caller following clause 3
    // prints only these, except that clause 6 says a serial of 00 is always
    // printed.
    bool preamble_clean = false;

    // The message between the preamble's line end and "NNNN", UTF-8, with
    // a lost character as the SITOR decoder's error glyph.
    std::string text;
    std::size_t mutilated_characters = 0;

    // "NNNN" arrived. False for a message cut off by a loss of phase or the
    // end of the stream.
    bool complete = false;

    // Audio sample index of the first "Z" of "ZCZC".
    SampleIndex position = 0;
};

struct NavtexConfig {
    SitorConfig sitor{};

    // Characters a message may run to before the decoder gives up waiting
    // for "NNNN" and reports it incomplete. An engineering choice, well
    // above any single NAVTEX message.
    std::size_t max_message_characters = 16'384;
};

class NavtexDecoder {
   public:
    [[nodiscard]] static Expected<NavtexDecoder> create(const NavtexConfig& config);

    // Consumes audio and appends every message whose "NNNN" has arrived, or
    // which a new phasing or a new "ZCZC" shows was cut off. Block
    // invariant: the cut is found from the characters, not from where a
    // call happened to end.
    void process(ConstRealSpan audio, std::vector<NavtexMessage>& out);

    // The stream has ended: reports a message still open as incomplete.
    void flush(std::vector<NavtexMessage>& out);

    [[nodiscard]] const SitorStats& sitor_stats() const { return sitor_->stats(); }

    void reset();

   private:
    NavtexDecoder() = default;

    void on_character(const SitorCharacter& c, std::vector<NavtexMessage>& out);

    NavtexConfig config_{};
    // Optional only because SitorBDecoder has no default state to be in
    // before create() fills it; it is always engaged after create().
    std::optional<SitorBDecoder> sitor_;
    std::vector<SitorCharacter> characters_;

    // The last few printing characters, for spotting "ZCZC" and "NNNN".
    std::string tail_;
    std::vector<SampleIndex> tail_positions_;

    enum class State : std::uint8_t { Hunt, Preamble, Body };
    State state_ = State::Hunt;
    NavtexMessage current_;
    std::vector<SitorCharacter> preamble_;
    std::size_t body_characters_ = 0;
    bool body_started_ = false;
    // The SITOR phasing the characters being assembled came from.
    std::uint64_t phasing_ = 0;
};

}  // namespace revenant::decode
