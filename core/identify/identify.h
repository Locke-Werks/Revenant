// Protocol identification: which published protocol a probe's extract carries,
// claimed only on a verified sync.
//
// WHY THIS IS NOT THE CHARACTERISER
//
// core/characterise names a modulation family from numbers that any signal of
// that family shares: a symbol rate, a tone count, an order. Several protocols
// share a physical layer exactly, and docs/modes.md lists which, so a family
// can never be narrowed to a protocol by measuring it harder. What narrows it
// is the protocol's own framing: a sync word at the place the standard puts it,
// followed by a field that checks under the standard's own code. That is
// decoding, which is why this lives beside core/decode and reuses it rather
// than inside core/characterise.
//
// THE RULE, THE OWNER'S OF 2026-09-23
//
// "Digital protocols should be detected on the waterfall and spectrum along
// with modulation." A protocol is claimed only on a verified sync, never on
// modulation alone. Each row below states what it counts as one verification
// and how many it needs, and the count comes from the decoder in core/decode
// that the rest of the tree already runs on live receivers, not from a second
// copy of any sync search. A family and a symbol rate decide only which rows
// are worth running; a row that is run and does not verify leaves the answer at
// None, which is a real answer.
//
// WHAT COUNTS, PER PROTOCOL, and each figure is this file's choice:
//
//   P25 Phase 1   a data unit whose frame sync correlated and whose Network
//                 Identifier decoded under its BCH code, TIA-102.BAAA-A
//                 clause 8.5. Two of them.
//   D-STAR        a radio header whose CRC checks, JARL version 7.0 clause
//                 4.1.1 by way of 4.1.2 a, counting two, or a
//                 resynchronisation signal found where
//                 clause 4.1.2 c puts it, every 21st frame, counting one. Two.
//   TETRA         a synchronisation burst whose BSCH passed its clause 8.2.3.3
//                 CRC. One, because that CRC sits under a training sequence
//                 correlation and a rate 2/3 code.
//   M17           a link setup frame whose CRC checks, counting two, or a
//                 stream frame whose LICH decoded under its Golay code behind
//                 the preamble or frame-number rule m17.h states, counting
//                 one. Two.
//   POCSAG        a batch synchronised on its codeword and confirmed by the
//                 preamble or by the next synchronisation codeword, ITU-R
//                 M.584-2 clauses 1.1 and 1.2, or a page whose address
//                 codeword decoded under BCH(31,21). Two, at any of the three
//                 rates.
//   AX.25         a frame whose FCS checks, AX.25 2.2 clause 3.7. One.
//   RTTY          a character framed by a start element and a stop element,
//                 ITU-T S.1 and S.3, whose weakest soft reading is at least a
//                 half. Ten, with four in five of the run that clean and
//                 framing errors under a tenth of it: RTTY carries no code, and
//                 noise and SITOR-B both frame characters that read less
//                 cleanly. identify.cpp has the measurement.
//   SITOR-B       phasing, then a character whose DX or RX copy passes the
//                 3-in-7 constant-ratio check, ITU-R M.625-4 clause 4.3. Eight.
//   PSK31         a Varicode character in the table, with the unrecognised
//                 ones under a quarter of them. Six.
//   CW            a Morse character of two elements or more in the ITU-R
//                 M.1677-1 table, with the timing locked and at most one
//                 unrecognised. Four. Single elements do not count, because
//                 a fading carrier keys itself into E and T.
//   DMR           a TS 102 361-1 Table 9.2 sync at core/decode/dmr.h's own
//                 threshold that sits a whole number of 144-symbol slots from
//                 the best one, clause 4.2's TDMA frame. Three. The owner put
//                 DMR back in scope on 2026-09-23 and its decoder landed the
//                 same day; attempt_dmr calls dmr_sync_score and nothing else.
//                 IdentifyConfig::dmr switches the row off whole.
//
//                 WHAT THIS ROW USED TO SAY: "PENDING", reporting Unavailable
//                 until core/decode/dmr.* existed. No structure-only fallback
//                 was ever built, so none is left behind.
//   RDS           NOT THROUGH A PROBE. RDS rides a WFM composite at 57 kHz,
//                 and a WFM carrier is wider than the largest probe bucket can
//                 hold at a quarter of its rate. The row exists so the answer
//                 says so rather than leaving it out.
//
// These are the decoders' own gates, counted. None of them was loosened for
// this: a sync that the decoder would not report to an operator does not
// count here either.
//
// PURITY
//
// A function of the samples, the rate and the hints. Every decoder is built
// fresh per call, so nothing carries from one probe to the next.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/characterise/catalogue.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::identify {

// The protocols this tree has decoders for.
// Appended only: the value crosses a ring as plain data and reaches the wire
// through core/rpc's own mirror of it.
enum class Protocol : std::uint8_t {
    None = 0,
    P25Phase1,
    DStar,
    Tetra,
    M17,
    Dmr,
    Pocsag,
    Ax25,
    Rtty,
    SitorB,
    Psk31,
    Cw,
    Rds,
};

inline constexpr std::size_t kProtocolCount = static_cast<std::size_t>(Protocol::Rds) + 1;

// The label an operator reads: "P25", "D-STAR", "TETRA", "M17", "DMR",
// "POCSAG", "AX.25", "RTTY", "SITOR-B", "PSK31", "CW", "RDS", and "" for None.
[[nodiscard]] const char* protocol_name(Protocol protocol);

// What became of one row.
enum class AttemptResult : std::uint8_t {
    // The hints put the signal outside what this protocol can be. Not run.
    NotPlausible = 0,

    // Plausible, and the extract cannot carry it (RDS through a probe), or
    // the decoder could not be built at this rate. Not run.
    Unavailable,

    // Run, and the decoder verified fewer than the row needs.
    NotVerified,

    // Run, and verified.
    Verified,
};

struct Attempt {
    Protocol protocol = Protocol::None;
    AttemptResult result = AttemptResult::NotPlausible;

    // Verifications counted, and how many the row needs.
    std::uint32_t verified = 0;
    std::uint32_t required = 0;
};

// What the characteriser and the detector already know, which decides which
// rows are run. All optional: an Unknown family and zeros run every row whose
// rate the extract can carry.
struct IdentifyHints {
    characterise::ModulationFamily family = characterise::ModulationFamily::Unknown;
    double symbol_rate_hz = 0.0;

    // The detection's occupied bandwidth, hertz.
    double occupied_hz = 0.0;
};

struct IdentifyConfig {
    dsp::SampleRate rate = 0;

    // The DMR row, on by default. One flag so the row can be removed cleanly
    // if the owner's decision on US8306071 changes, since the row correlates
    // DMR's sync patterns: with it off the row is never attempted and never
    // listed.
    bool dmr = true;
};

struct Identification {
    Protocol protocol = Protocol::None;

    // 0.9 at exactly the verifications the row needs, halving the distance
    // to one with each verification past it. A count of independent checks
    // that passed, not a calibrated probability, and stated as such: every
    // verified protocol is well past chance, and the number says how much
    // evidence there was on top of that.
    double confidence = 0.0;

    // How many verifications the winning row counted.
    std::uint32_t verified = 0;

    // Every row, in the order they were tried, with what became of each.
    std::vector<Attempt> attempts;
};

// The rows tried, in this order, which runs from the strongest check to the
// weakest: rows verified by a code over their fields first, then SITOR-B's
// constant-ratio code, then the framing-only rows, RTTY's start and stop
// elements and PSK31's Varicode gaps, and CW's Morse table last. When two rows
// verify the earlier one is the answer and both are listed.
//
// WHY ORDER AND NOT A SCORE. One tone of an RTTY pair keys on and off the way
// a carrier does and sits inside the CW decoder's capture range, so CW
// verified 13 characters on RTTY at 20 dB in 2500 Hz in tests/characterise/
// test_identify.cpp, against RTTY's own 29. Comparing counts across rows
// compares things with different units; which check is harder to satisfy by
// accident does not change with the signal.
inline constexpr std::array<Protocol, kProtocolCount - 1> kIdentifyOrder = {
    Protocol::P25Phase1, Protocol::M17,    Protocol::Dmr,  Protocol::DStar,
    Protocol::Tetra,     Protocol::Pocsag, Protocol::Ax25, Protocol::SitorB,
    Protocol::Rtty,      Protocol::Psk31,  Protocol::Cw,   Protocol::Rds,
};

// Whether a row would be run on this extract. Pure, and exposed so a test can
// referee the plausibility table without running a decoder.
[[nodiscard]] bool plausible(Protocol protocol, const IdentifyHints& hints,
                             dsp::SampleRate rate);

// Complex baseband centred on the detection, as a probe hands it over.
[[nodiscard]] Expected<Identification> identify(dsp::ConstComplexSpan samples,
                                                const IdentifyHints& hints,
                                                const IdentifyConfig& config);

}  // namespace revenant::identify
