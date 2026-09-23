// The command palette's search in models/palette_match.h: which entries a
// query finds and the order they come back in.
//
// EVERY CASE NAMES THE WRONG MATCHER IT REJECTS. A palette's failures are all
// ranking failures that still return something: the entry the operator meant
// is in the list, three rows down, and Return runs the one on top. So these
// assert order, not membership alone.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "models/palette_match.h"

using revenant::ui::fuzzy_match;
using revenant::ui::key_have;
using revenant::ui::KeyState;
using revenant::ui::kBands;
using revenant::ui::kKeyActions;
using revenant::ui::palette_entries;
using revenant::ui::PaletteEntry;
using revenant::ui::PaletteHit;
using revenant::ui::PaletteScope;
using revenant::ui::PaletteState;
using revenant::ui::rank_palette;

namespace {

// A window with everything: an engine, a radio that retunes, a receiver in a
// mode AFT and the auto filter both work on, and a spectrum on screen.
[[nodiscard]] PaletteState everything()
{
    KeyState state;
    state.connected = true;
    state.source_open = true;
    state.can_retune = true;
    state.receiver = true;
    state.second_receiver = true;
    state.aft_offered = true;
    state.auto_filter_offered = true;
    state.spectrum_drawing = true;
    PaletteState out;
    out.have = key_have(state);
    out.tune_low_hz = 24'000'000;
    out.tune_high_hz = 1'766'000'000;
    return out;
}

[[nodiscard]] std::string top(std::string_view query,
                              PaletteScope scope = PaletteScope::Everything,
                              const PaletteState& state = everything())
{
    static const std::vector<PaletteEntry> entries = palette_entries();
    const std::vector<PaletteHit> hits = rank_palette(entries, query, scope, state);
    return hits.empty() ? std::string() : entries[hits.front().entry].id;
}

[[nodiscard]] int score(std::string_view pattern, std::string_view text)
{
    const auto match = fuzzy_match(pattern, text);
    return match ? match->score : -1;
}

}  // namespace

// Rejects a substring matcher, which is what a first palette usually is, and
// which finds nothing for the abbreviations people actually type.
TEST_CASE("the letters have to be there in order, and nothing more")
{
    CHECK(fuzzy_match("wdn", "widen the filter").has_value());
    CHECK_FALSE(fuzzy_match("ndw", "widen the filter").has_value());
    CHECK_FALSE(fuzzy_match("widest", "widen").has_value());
    CHECK(fuzzy_match("MUTE", "mute or unmute").has_value());
    CHECK(fuzzy_match("", "anything").has_value());
}

// Rejects the first placement found. "20m" in "SW 120 m" lands first on the
// "2" inside "120", and a greedy matcher would stop there; the best placement
// is the one that starts a word, and the ranking below depends on taking it.
TEST_CASE("the best placement of the letters is the one scored")
{
    const auto match = fuzzy_match("fl", "fill level floor");
    REQUIRE(match.has_value());
    // "f" at the start of the text and "l" at the start of "level" outscores
    // "fl" run together at the start of "floor", because starting the text
    // is worth more than a run; either way the positions are the best ones.
    CHECK(match->positions.size() == 2);
    CHECK(match->positions.front() == 0);

    const auto band = fuzzy_match("20m", "SW 120 m");
    REQUIRE(band.has_value());
    CHECK(band->positions == std::vector<std::size_t>{4, 5, 7});
}

// Rejects a matcher that scores every letter alike. Typing the start of a
// word is what people do, and the entry they meant has to outrank one that
// merely contains the letters somewhere.
TEST_CASE("the start of a word and a run of letters outscore scattered letters")
{
    CHECK(score("ab", "a big") > score("ab", "cabin"));
    CHECK(score("pin", "pin the floor") > score("pin", "spinning"));
    CHECK(score("flo", "floor") > score("flo", "fill level old"));
    CHECK(score("20m", "20 m") > score("20m", "SW 120 m"));
}

// Rejects a palette that makes its operator type the whole label. Each of
// these is a query somebody reaching for the action would type first.
TEST_CASE("the everyday queries find their action first")
{
    CHECK(top("mute") == "audio.mute");
    CHECK(top("wid") == "filter.widen");
    CHECK(top("narrow") == "filter.narrow");
    CHECK(top("aft") == "receiver.aft");
    CHECK(top("key map") == "keymap.open");
    CHECK(top("bookmark") == "memory.save");
    CHECK(top("frequency manager") == "panel.memories");
    CHECK(top("import") == "panel.memories");
    CHECK(top("remove") == "receiver.remove");
    CHECK(top("receiver window") == "window.receivers");
    CHECK(top("pin floor") == "scale.floor");
    CHECK(top("ceiling") == "scale.ceiling");
    CHECK(top("page up") == "tune.page_up");
}

// Rejects searching labels alone. "mode" is in no mode action's label and
// "usb" is in no group, so a query naming both finds the action only because
// each word may be found in a different field.
TEST_CASE("each word may be found in the label, the group or the keywords")
{
    CHECK(top("mode usb") == "mode.usb");
    CHECK(top("p25") == "mode.p25p1");
    CHECK(top("tracking") == "receiver.aft");
}

// Rejects bands missing from the palette, and a band match that ranks under
// an action which only happens to contain the same letters.
TEST_CASE("bands are found by name, by group and by the word band")
{
    const auto band_named = [](std::string_view id) -> std::string_view {
        if (!id.starts_with("band.")) {
            return {};
        }
        return kBands[std::stoul(std::string(id.substr(5)))].name;
    };
    CHECK(band_named(top("airband")) == "Airband");
    CHECK(band_named(top("20m", PaletteScope::Bands)) == "20 m");
    CHECK(band_named(top("band 2 m")) == "2 m");

    // The band scope lists bands and only bands, all of them for no query.
    static const std::vector<PaletteEntry> entries = palette_entries();
    const auto hits = rank_palette(entries, "", PaletteScope::Bands, everything());
    CHECK(hits.size() == kBands.size());
    for (const PaletteHit& hit : hits) {
        CHECK(entries[hit.entry].band >= 0);
    }
}

// Rejects hiding what cannot run, and ranking it where Return would run it.
// On a recording nothing retunes, so the tuning entries sink below the ones
// that work and say why, rather than vanishing from a search for them.
TEST_CASE("what cannot run sinks, stays listed, and says why")
{
    PaletteState recording = everything();
    KeyState state;
    state.connected = true;
    state.source_open = true;
    state.receiver = true;
    state.spectrum_drawing = true;
    recording.have = key_have(state);

    static const std::vector<PaletteEntry> entries = palette_entries();
    const auto hits = rank_palette(entries, "tuning", PaletteScope::Everything, recording);
    REQUIRE_FALSE(hits.empty());
    bool seen_disabled = false;
    bool found_step = false;
    for (const PaletteHit& hit : hits) {
        if (!hit.enabled) {
            seen_disabled = true;
            CHECK_FALSE(hit.reason.empty());
        } else {
            // Nothing enabled after the first disabled entry.
            CHECK_FALSE(seen_disabled);
        }
        if (entries[hit.entry].id == "tune.up") {
            found_step = true;
            CHECK_FALSE(hit.enabled);
            CHECK(hit.reason == "this radio does not retune");
        }
    }
    CHECK(found_step);
}

// Rejects a band offered on a radio that cannot reach it, which would retune
// to the nearest edge and land somewhere the operator did not ask for.
TEST_CASE("a band out of the radio's range is listed and not offered")
{
    PaletteState dongle = everything();
    static const std::vector<PaletteEntry> entries = palette_entries();
    const auto hits = rank_palette(entries, "160", PaletteScope::Bands, dongle);
    REQUIRE_FALSE(hits.empty());
    CHECK(entries[hits.front().entry].label == "160 m");
    CHECK_FALSE(hits.front().enabled);
    CHECK(hits.front().reason == "out of this radio's range");
}

// Rejects an empty query that filters or shuffles. With nothing typed the
// palette is the table, in the table's order, so it reads as the menu it
// replaces; the one reordering is the rule every query gets, what can run
// before what cannot, and within each of those the table's order holds.
TEST_CASE("no query lists everything in the table's order")
{
    static const std::vector<PaletteEntry> entries = palette_entries();
    const auto hits = rank_palette(entries, "   ", PaletteScope::Everything, everything());
    REQUIRE(hits.size() == entries.size());
    for (std::size_t i = 1; i < hits.size(); ++i) {
        if (hits[i].enabled == hits[i - 1].enabled) {
            CHECK(hits[i].entry > hits[i - 1].entry);
        } else {
            CHECK(hits[i - 1].enabled);
        }
    }

    // With everything to hand, the only entries that cannot run are the bands
    // an R820T does not reach, which are the ones below 24 MHz.
    for (const PaletteHit& hit : hits) {
        if (!hit.enabled) {
            const PaletteEntry& entry = entries[hit.entry];
            REQUIRE(entry.band >= 0);
            CHECK(kBands[static_cast<std::size_t>(entry.band)].centre_hz < 24'000'000);
        }
    }

    // Window actions only: the filter display's keys and the overlay's own
    // are not things to run from the palette.
    std::size_t window_actions = 0;
    for (const auto& action : kKeyActions) {
        window_actions += action.context == revenant::ui::KeyContext::Window ? 1U : 0U;
    }
    CHECK(entries.size() == window_actions + kBands.size());
}

// Rejects marking letters the query did not land on, or marking a word found
// only in the group inside a label it is not in.
TEST_CASE("the marked letters are the ones the query landed on in the label")
{
    static const std::vector<PaletteEntry> entries = palette_entries();
    const auto hits = rank_palette(entries, "mute", PaletteScope::Everything, everything());
    REQUIRE_FALSE(hits.empty());
    CHECK(hits.front().label_positions == std::vector<std::size_t>{0, 1, 2, 3});

    const auto by_group = rank_palette(entries, "mode usb", PaletteScope::Everything,
                                       everything());
    REQUIRE_FALSE(by_group.empty());
    const std::string& label = entries[by_group.front().entry].label;
    for (const std::size_t at : by_group.front().label_positions) {
        CHECK(at < label.size());
    }
    // "usb" is in the label and "mode" is not, so three letters are marked.
    CHECK(by_group.front().label_positions.size() == 3);
}
