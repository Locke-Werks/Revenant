// parse_frequency and format_mhz, which are the whole of what a frequency
// box decides before the radio moves.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives: a test that only
// asserts what the code already does certifies one reachable shape and
// reads as though it certified the behaviour.
//
// The wrong implementations that matter on this path are a strtod and a
// multiply, which loses hertz on a long fraction; a bare number read
// literally as hertz, which tunes 95 Hz for "95.1"; and a parser that
// coerces junk to a number instead of refusing it, which moves the radio
// somewhere nobody asked for.

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "models/frequency_entry.h"

using revenant::ui::format_mhz;
using revenant::ui::FrequencyUnit;
using revenant::ui::parse_frequency;

namespace {

// Hertz from a parse that is expected to succeed, so the assertions below
// read as one line each.
[[nodiscard]] std::int64_t hz(const char* text)
{
    const auto parsed = parse_frequency(text);
    REQUIRE(parsed.has_value());
    return parsed->hertz;
}

}  // namespace

TEST_CASE("the four the operator actually typed", "[frequency]")
{
    // THE FOUR FROM THE BENCH. An implementation that reads a bare number
    // literally as hertz passes only the third of these and tunes 95 Hz for
    // the first, which on a VHF source is a refusal at best and a silent
    // nothing at worst.
    CHECK(hz("95.1") == 95'100'000);
    CHECK(hz("95.1M") == 95'100'000);
    CHECK(hz("95100000") == 95'100'000);
    CHECK(hz("98.1 MHz") == 98'100'000);
}

TEST_CASE("a bare number takes its unit from its magnitude", "[frequency]")
{
    // The rule, at both sides of the boundary. An implementation that used
    // "contains a decimal point" instead would put 462 at 462 Hz and 2400000
    // at 2.4 PHz, and both of those look like a working parser until
    // somebody types an integer.
    CHECK(hz("462.5625") == 462'562'500);
    CHECK(hz("462") == 462'000'000);
    CHECK(hz("999999") == 999'999'000'000);
    CHECK(hz("1000000") == 1'000'000);
    CHECK(hz("2400000") == 2'400'000);

    const auto inferred = parse_frequency("95.1");
    REQUIRE(inferred.has_value());
    CHECK(inferred->unit_inferred);
    CHECK(inferred->unit == FrequencyUnit::Megahertz);

    // A stated unit is never inferred, which is what lets a box echo the
    // reading only when it guessed. An implementation that set the flag
    // from the magnitude rather than from the absence of a suffix would
    // make the box nag on every typed "95.1 MHz".
    const auto stated = parse_frequency("95.1 MHz");
    REQUIRE(stated.has_value());
    CHECK_FALSE(stated->unit_inferred);
}

TEST_CASE("the bare-number rule fails loudly rather than plausibly", "[frequency]")
{
    // A band-plan number written bare in kilohertz is the case the rule
    // gets wrong, and the point of the rule's shape is WHERE it is wrong.
    // 14074 reads as 14.074 GHz, which no source tunes, so the tune is
    // refused and says so. A rule that read it as 14074 Hz would have been
    // accepted in silence on an HF source, which is the failure this whole
    // round is about.
    CHECK(hz("14074") == 14'074'000'000);

    // And the suffix is always available to say what was meant.
    CHECK(hz("14074k") == 14'074'000);
    CHECK(hz("14.074M") == 14'074'000);
}

TEST_CASE("no floating point loses a hertz", "[frequency]")
{
    // THE strtod IMPLEMENTATION FAILS HERE. 1234.567891 as a double times
    // 1e6 is not 1234567891, and the nearest-integer repair only holds
    // while the error stays under half a hertz, which it does not at nine
    // significant digits.
    CHECK(hz("1234.567891 MHz") == 1'234'567'891);
    CHECK(hz("0.000001M") == 1);
    CHECK(hz("99.999999M") == 99'999'999);

    // Finer than a hertz rounds half away from zero, once, at the end.
    // An implementation that truncated would be a hertz low on every
    // fraction above the half.
    CHECK(hz("1.0000005M") == 1'000'001);
    CHECK(hz("1.0000004M") == 1'000'000);
    CHECK(hz("1.4Hz") == 1);
    CHECK(hz("1.5Hz") == 2);
}

TEST_CASE("suffixes, spacing and grouping", "[frequency]")
{
    CHECK(hz("1.5G") == 1'500'000'000);
    CHECK(hz("1.5 GHz") == 1'500'000'000);
    CHECK(hz("1500 MHz") == 1'500'000'000);
    CHECK(hz("144k") == 144'000);
    CHECK(hz("144 kHz") == 144'000);
    CHECK(hz("500Hz") == 500);
    CHECK(hz("500 hz") == 500);

    // Case folding on the whole suffix, not just the first letter. An
    // implementation that lowercased only the leading character would take
    // "mHz" and reject "MHZ".
    CHECK(hz("95.1MHZ") == 95'100'000);
    CHECK(hz("95.1mhz") == 95'100'000);
    CHECK(hz("95.1 mHz") == 95'100'000);

    // "khz" must not be read as a stray k in front of "hz". An
    // implementation that tested the shortest suffix first would parse
    // "144khz" as 144k hertz, off by three decades.
    CHECK(hz("144khz") == 144'000);

    // Group separators anywhere in the digits, including inside the
    // fraction.
    CHECK(hz("95,100,000") == 95'100'000);
    CHECK(hz("95 100 000") == 95'100'000);
    CHECK(hz("95_100_000") == 95'100'000);
    CHECK(hz("  98.1 MHz  ") == 98'100'000);
    CHECK(hz("95.100 000M") == 95'100'000);
}

TEST_CASE("junk is refused rather than coerced", "[frequency]")
{
    // EVERY ONE OF THESE MOVES THE RADIO under an implementation that
    // parses a prefix and stops, which is what strtod does: it takes
    // "95.1junk" as 95.1 and reports success through a pointer nobody
    // checks.
    CHECK_FALSE(parse_frequency("").has_value());
    CHECK_FALSE(parse_frequency("   ").has_value());
    CHECK_FALSE(parse_frequency("MHz").has_value());
    CHECK_FALSE(parse_frequency("95.1junk").has_value());
    CHECK_FALSE(parse_frequency("9 5 . 1 . 2").has_value());
    CHECK_FALSE(parse_frequency("95..1").has_value());
    CHECK_FALSE(parse_frequency("95.1.2").has_value());
    CHECK_FALSE(parse_frequency("abc").has_value());
    CHECK_FALSE(parse_frequency("0x5f").has_value());

    // A sign is a typo and not a request. A centre frequency is not
    // negative, and an implementation that accepted one would hand the
    // engine a value its own range check has to catch instead.
    CHECK_FALSE(parse_frequency("-95.1").has_value());
    CHECK_FALSE(parse_frequency("+95.1").has_value());

    // Past the ceiling, and long enough to overflow the mantissa. Both are
    // refused rather than wrapped; an implementation that let the multiply
    // wrap would produce a small positive frequency from an absurd one.
    CHECK_FALSE(parse_frequency("2000 GHz").has_value());
    CHECK_FALSE(parse_frequency("99999999999999999999999").has_value());

    // Zero parses. Whether a source tunes to DC is the source's answer.
    CHECK(hz("0") == 0);
    CHECK(hz("0 Hz") == 0);
}

TEST_CASE("format_mhz holds its width", "[frequency]")
{
    // SIX DECIMALS ALWAYS, INCLUDING THE TRAILING ZEROS. An implementation
    // that trimmed them would change the string's width as the digits
    // changed, which is what makes a readout untrackable down a column, and
    // "95.1" would be indistinguishable from the text an operator typed.
    CHECK(format_mhz(95'100'000) == "95.100000");
    CHECK(format_mhz(98'100'000) == "98.100000");
    CHECK(format_mhz(462'562'500) == "462.562500");
    CHECK(format_mhz(1) == "0.000001");
    CHECK(format_mhz(0) == "0.000000");
    CHECK(format_mhz(1'000'000) == "1.000000");

    // The one hertz a tuning step rounds away has to be visible, which is
    // the whole reason the granted centre is echoed at all.
    CHECK(format_mhz(95'099'999) == "95.099999");
}

TEST_CASE("what a box types round-trips through what it prints", "[frequency]")
{
    // The granted-centre echo is format_mhz over a value that came back
    // from the engine, and an operator may well retype it. An
    // implementation whose printer and parser disagreed on the unit would
    // move the radio by six decades on that retype.
    for (const std::int64_t value :
         {std::int64_t{0}, std::int64_t{1}, std::int64_t{95'100'000},
          std::int64_t{462'562'500}, std::int64_t{1'234'567'891}}) {
        const std::string printed = format_mhz(value);
        const auto reparsed = parse_frequency(printed + " MHz");
        REQUIRE(reparsed.has_value());
        CHECK(reparsed->hertz == value);
    }
}

TEST_CASE("a bare number is hertz for a rate and megahertz for a tuning", "[ui][frequency]")
{
    // The wrong implementation is one parser for both boxes, which is what the
    // device picker shipped with on 2026-09-21. An operator typing 250000 into
    // a RATE box means 250 kS/s; read by the tuning rule that is 250000 MHz,
    // which nothing refuses, because source_choice.h's settle_rate then clamps
    // it to the device's maximum and opens at 3.2 MS/s. A control that silently
    // does something else is worse than one that refuses.
    using revenant::ui::BareNumber;

    // The tuning rule, unchanged and still the default, because every existing
    // caller is a tuning box and "95.1" is 95.1 MHz to every operator alive.
    CHECK(parse_frequency("95.1")->hertz == 95'100'000);
    CHECK(parse_frequency("250000")->hertz == 250'000'000'000);

    // The rate rule reads the same text as hertz.
    CHECK(parse_frequency("250000", BareNumber::Hertz)->hertz == 250'000);
    CHECK(parse_frequency("2400000", BareNumber::Hertz)->hertz == 2'400'000);
    CHECK(parse_frequency("1", BareNumber::Hertz)->hertz == 1);

    // A SUFFIX WINS UNDER BOTH RULES, which is what makes the rate box usable
    // for somebody who thinks in megasamples: the rule only ever decides what a
    // number with no unit on it meant.
    CHECK(parse_frequency("2.4M", BareNumber::Hertz)->hertz == 2'400'000);
    CHECK(parse_frequency("250k", BareNumber::Hertz)->hertz == 250'000);
    CHECK(parse_frequency("2.4M")->hertz == 2'400'000);

    // And the inference flag still says the rule was used, so a box can echo
    // which reading it took. It is about whether a unit was WRITTEN, not about
    // which rule resolved it.
    CHECK(parse_frequency("250000", BareNumber::Hertz)->unit_inferred);
    CHECK_FALSE(parse_frequency("250k", BareNumber::Hertz)->unit_inferred);

    // Above the floor the two rules already agreed, so nothing moves there.
    CHECK(parse_frequency("95100000")->hertz == 95'100'000);
    CHECK(parse_frequency("95100000", BareNumber::Hertz)->hertz == 95'100'000);
}
