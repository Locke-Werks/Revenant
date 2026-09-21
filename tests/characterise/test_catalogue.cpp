// Naming candidate protocols from measured numbers.
//
// The product of this stage is a LIST. Five systems share the 4FSK 4800
// symbol physical layer and no measurement in core/characterise separates
// them, so a matcher that returns one of them is wrong four times in five
// and reads identically on all five occasions. Every case here is written
// to fail against a nearest-row matcher.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "core/characterise/catalogue.h"

using namespace revenant;
using characterise::ModulationFamily;
using Catch::Approx;

namespace {

[[nodiscard]] bool names(const std::vector<characterise::ProtocolCandidate>& candidates,
                         std::string_view wanted)
{
    return std::any_of(candidates.begin(), candidates.end(),
                       [wanted](const characterise::ProtocolCandidate& candidate) {
                           return candidate.row.name == wanted;
                       });
}

void report(const char* label, const std::vector<characterise::ProtocolCandidate>& candidates)
{
    std::println("{}: {}", label,
                 candidates.empty() ? std::string("nothing in the catalogue")
                                    : characterise::summarise_candidates(candidates));
}

}  // namespace

// REJECTS: a matcher that returns one answer. Four-level FSK at 4800
// symbols a second is five systems sharing one physical layer, and the
// measurements this directory makes cannot tell them apart, so the list is
// the honest result. The assertion that catches a nearest-row matcher is
// the count, not the membership: a single-answer matcher names DMR and
// passes every membership check that mentions DMR.
//
// Also rejects one that ignores the symbol rate, which would drag the
// 2400-symbol half of the same family in with it.
TEST_CASE("4FSK at 4800 baud names every system that shares it", "[characterise]")
{
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 4;
    query.symbol_rate_hz = 4800.0;

    const auto candidates = characterise::match_protocols(query);
    report("4FSK 4800", candidates);
    CAPTURE(candidates.size());
    REQUIRE(candidates.size() >= 4);
    REQUIRE(names(candidates, "DMR Tier I/II/III"));
    REQUIRE(names(candidates, "NXDN at 12.5 kHz"));
    REQUIRE(names(candidates, "P25 Phase 1 C4FM"));
    REQUIRE(names(candidates, "M17"));

    // And not the 6.25 kHz half, which is the same modulation at half the
    // rate.
    REQUIRE_FALSE(names(candidates, "NXDN at 6.25 kHz"));
    REQUIRE_FALSE(names(candidates, "dPMR446 and licensed dPMR modes 2/3"));
    // Nor the two-level systems at the same rate.
    REQUIRE_FALSE(names(candidates, "D-STAR DV"));
}

// REJECTS: a matcher that uses only the symbol rate. A measured bandwidth
// is the one thing that separates the 12.5 kHz block from the 6.25 kHz
// block when both are four-level, and a matcher ignoring it reports six
// candidates where three are ruled out by a number already in hand.
TEST_CASE("a measured bandwidth narrows the four-level family", "[characterise]")
{
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 4;
    query.symbol_rate_hz = 2400.0;
    query.bandwidth_hz = 6000.0;

    const auto candidates = characterise::match_protocols(query);
    report("4FSK 2400 in 6 kHz", candidates);
    REQUIRE(names(candidates, "NXDN at 6.25 kHz"));
    REQUIRE(names(candidates, "dPMR446 and licensed dPMR modes 2/3"));
    REQUIRE(candidates.size() == 2);

    // The same rate with a 25 kHz bandwidth is not either of them.
    query.bandwidth_hz = 25000.0;
    const auto wider = characterise::match_protocols(query);
    report("4FSK 2400 in 25 kHz", wider);
    REQUIRE_FALSE(names(wider, "NXDN at 6.25 kHz"));
}

// REJECTS: a matcher that always answers. The right response to a clean
// measurement of something nobody has a document for is an empty list and
// a stated symbol rate, which is exactly what docs/modes.md's out-of-scope
// section is full of. A nearest-row matcher names a system here, and the
// name is wrong while every number beside it is right, which is the worst
// shape a wrong answer can take.
TEST_CASE("numbers matching nothing produce no candidates, not the nearest one",
          "[characterise]")
{
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 2;
    query.symbol_rate_hz = 3777.0;

    const auto candidates = characterise::match_protocols(query);
    report("2FSK 3777", candidates);
    REQUIRE(candidates.empty());

    // 2400 is in the catalogue and 3777 is 57 percent away from it, so the
    // nearest row exists and is not close. This is the assertion that
    // makes the one above mean something: the catalogue is not simply
    // empty of two-level rows.
    query.symbol_rate_hz = 2400.0;
    REQUIRE_FALSE(characterise::match_protocols(query).empty());
}

// REJECTS: a matcher that reports a row whose stated symbol rate nothing
// measured. An FSK query with no baud in it supports no baud-defined row,
// and returning them anyway turns a candidate list into a list of
// everything the family contains.
TEST_CASE("a row states nothing the measurements did not support", "[characterise]")
{
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 4;
    // No symbol rate.

    const auto candidates = characterise::match_protocols(query);
    report("4FSK, no baud", candidates);
    REQUIRE(candidates.empty());
}

// REJECTS: a catalogue row added without a citation, and one added with a
// citation that names no document. docs/clean-room.md's practice section
// requires a constant to carry the document and clause it came from, and a
// symbol rate is a constant. This is the test that makes that a build
// failure rather than a review item somebody skips when the change looks
// small.
TEST_CASE("every catalogue row cites a document and a docs/modes.md row", "[characterise]")
{
    const auto rows = characterise::protocol_catalogue();
    REQUIRE(rows.size() > 20);

    std::vector<std::string_view> seen;
    for (const characterise::ProtocolRow& row : rows) {
        CAPTURE(row.name, row.citation);
        REQUIRE_FALSE(row.name.empty());
        REQUIRE(row.family != ModulationFamily::Unknown);

        // Long enough that "see the spec" cannot pass, and carrying either
        // the docs/modes.md pointer or, for the two analogue rows, the
        // header in this tree that states the figure.
        REQUIRE(row.citation.size() > 40);
        const bool sourced = row.citation.find("docs/modes.md") != std::string_view::npos ||
                             row.citation.find("core/dsp/synth") != std::string_view::npos;
        REQUIRE(sourced);

        REQUIRE(std::find(seen.begin(), seen.end(), row.name) == seen.end());
        seen.push_back(row.name);
    }
}

// REJECTS: a summary that reads as a single answer. The clause this builds
// is what an operator sees, and "DMR, NXDN at 12.5 kHz, or P25 Phase 1"
// says something a bare "DMR" does not.
TEST_CASE("the summary clause reads as the list it is", "[characterise]")
{
    characterise::ProtocolQuery query;
    query.family = ModulationFamily::Fsk;
    query.tone_count = 4;
    query.symbol_rate_hz = 4800.0;

    const auto candidates = characterise::match_protocols(query);
    const std::string clause = characterise::summarise_candidates(candidates);
    std::println("summary: {}", clause);
    CAPTURE(clause);
    REQUIRE(clause.find(" or ") != std::string::npos);
    REQUIRE(clause.find(", ") != std::string::npos);

    REQUIRE(characterise::summarise_candidates({}).empty());
}
