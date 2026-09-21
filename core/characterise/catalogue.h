// What a set of measured numbers is consistent with.
//
// A LIST, NOT AN ANSWER
//
// Four-level FSK at 4800 symbols a second is DMR, and it is NXDN at
// 12.5 kHz, and it is P25 Phase 1, and it is M17, and it is Yaesu Fusion.
// Those five share a physical layer, which is why docs/modes.md lists them
// as one piece of shared machinery, and no amount of work on the numbers
// this directory measures will separate them: separating them means reading
// the frame, which is decoding.
//
// So the output is every row the numbers are consistent with, and an honest
// "4FSK at 4800 baud, consistent with DMR, NXDN at 12.5 kHz or P25 Phase 1"
// is the product rather than a failure to produce one. A single guess would
// be right about one time in five and would read, to an operator, exactly
// like the four times it was wrong.
//
// WHERE THE ROWS COME FROM
//
// docs/modes.md, which is this project's scoped list of every mode it
// intends to implement and every mode somebody will ask about. Each row
// there states the modulation, the symbol rate and the channel spacing, and
// names the standard those come from. Every citation below therefore names
// two things: the standards document, and the docs/modes.md table row that
// carries it, so a reader can check the figure without buying the standard
// and can then buy the standard if the figure matters.
//
// Nothing here was read out of an implementation. docs/clean-room.md's
// practice section asks a constant to carry the document and clause it came
// from, and a symbol rate is a constant.
//
// WHY THE OFDM SIDE IS THIN
//
// One row, DAB Mode I, and it is the only multicarrier system in
// docs/modes.md whose row states a figure this can match against: 1536
// carriers at 1 kHz spacing fixes the useful symbol at 1 ms. The DRM30,
// NAVDAT, DSRC and HamDRM rows give a bandwidth and a constellation and no
// symbol duration, so there is nothing to compare a measured symbol period
// with. Adding one of those means taking the figure out of the standard,
// which is a purchase and a citation rather than a recollection, and an
// empty catalogue entry is better than a remembered one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace revenant::characterise {

enum class ModulationFamily : std::uint8_t {
    // Nothing was established. Distinct from Unmodulated, which is a
    // finding.
    Unknown = 0,

    // A single narrow carrier. Keyed or not: the envelope statistics say
    // which, and this family says only that the energy is in one place and
    // the frequency is not moving.
    Unmodulated,

    // Continuous frequency modulation by an analogue signal. Constant
    // envelope, instantaneous frequency sweeping rather than stepping.
    AnalogueFm,

    // Discrete tones. tone_count says how many.
    Fsk,

    // Linear modulation on one carrier: the constellation's order is the
    // exponent at which the M-th power law finds a line.
    Psk,

    // Multicarrier, identified by a cyclic prefix rather than by a symbol
    // rate.
    Ofdm,
};

[[nodiscard]] std::string_view modulation_family_name(ModulationFamily family);

struct ProtocolRow {
    std::string_view name;

    ModulationFamily family = ModulationFamily::Unknown;

    // Zero where the family does not use it.
    std::size_t tone_count = 0;
    int psk_order = 0;
    double symbol_rate_hz = 0.0;

    // Channel spacing, not occupied bandwidth. The two differ by the
    // filtering a system specifies, so the match against a measured
    // bandwidth is loose by design: see ProtocolQuery::bandwidth_factor.
    double channel_spacing_hz = 0.0;

    // Useful symbol duration for a multicarrier row. Zero elsewhere.
    double ofdm_symbol_seconds = 0.0;

    // The standards document, and the docs/modes.md row that carries it.
    std::string_view citation;
};

struct ProtocolQuery {
    ModulationFamily family = ModulationFamily::Unknown;

    // Zero means not measured, and the three do not treat that the same
    // way. The difference is deliberate and it is which attribute is
    // load-bearing for the family.
    //
    // A symbol rate and a tone count are what make a row the row it is:
    // this catalogue splits almost entirely into a 2400 block and a 4800
    // block, and into two-level and four-level. So a row stating one of
    // those and getting no measurement to compare it against does NOT
    // match, because a candidate list is a list of things the numbers
    // support and an unmeasured number supports nothing.
    //
    // A PSK order is the softest measurement here and several rows leave
    // it deliberately unstated, because MIL-STD-188-110 runs from BPSK to
    // 64-QAM on one waveform. An unmeasured order therefore narrows
    // nothing rather than excluding everything.
    std::size_t tone_count = 0;
    int psk_order = 0;
    double symbol_rate_hz = 0.0;
    double ofdm_symbol_seconds = 0.0;

    // Occupied bandwidth, if it was measured. Used only to narrow, never
    // to exclude a row that states no channel spacing.
    double bandwidth_hz = 0.0;

    // Relative tolerance on the symbol rate and on the OFDM symbol
    // duration. Five percent, which is far wider than the estimator's own
    // error on a clean signal and narrow enough to separate the 2400 and
    // 4800 symbol families that most of this catalogue splits into.
    double rate_tolerance = 0.05;

    // A measured bandwidth within this factor of a row's channel spacing
    // keeps the row.
    //
    // 2.5 rather than something tight, because the two quantities are not
    // the same thing. A 12.5 kHz channel carries a signal occupying
    // roughly 9 kHz, and a measured 99 percent containment on a noisy
    // extract runs wider than the specified occupancy. The bandwidth is
    // here to separate a 6.25 kHz system from a 25 kHz one, which it does
    // comfortably at this factor, and not to check anybody's filter mask.
    double bandwidth_factor = 2.5;
};

struct ProtocolCandidate {
    ProtocolRow row;

    // How far the measured symbol rate sits from the row's, as a
    // percentage of the row's. Zero when neither states one.
    double symbol_rate_error_percent = 0.0;
};

// Every row, in declaration order.
[[nodiscard]] std::span<const ProtocolRow> protocol_catalogue();

// Rows consistent with the measurements, nearest symbol rate first.
//
// An empty result is a finding and not a failure: it means the waveform
// was measured cleanly and matches nothing this project has a document
// for. docs/modes.md's own "what is out of scope" section is full of
// systems in exactly that position, and reporting the nearest row instead
// would name one of them wrongly.
[[nodiscard]] std::vector<ProtocolCandidate> match_protocols(const ProtocolQuery& query);

// The candidate names as one clause, for a summary line: "DMR, NXDN at
// 12.5 kHz, or P25 Phase 1". Empty for an empty list.
[[nodiscard]] std::string summarise_candidates(std::span<const ProtocolCandidate> candidates);

}  // namespace revenant::characterise
