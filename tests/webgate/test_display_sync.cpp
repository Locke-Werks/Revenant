// tools/webgate/display_sync.h: the phone following the desktop's display
// settings. Each case names the wrong implementation it rejects.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "tools/webgate/display_sync.h"

namespace {

using revenant::ui::DisplaySettings;
using revenant::webgate::DisplayFrame;
using revenant::webgate::DisplayRow;
using revenant::webgate::DisplaySettingsSource;
using revenant::webgate::DisplayValueReader;
using revenant::webgate::PhoneDisplay;
namespace ui = revenant::ui;
namespace colour_map = revenant::ui::colour_map;

[[nodiscard]] DisplayValueReader reader_of(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](std::string_view name) -> std::optional<std::string> {
        const auto at = values.find(std::string(name));
        if (at == values.end()) {
            return std::nullopt;
        }
        return at->second;
    };
}

// What the registry on the owner's machine held on 2026-11-03, as Qt wrote it.
[[nodiscard]] std::map<std::string, std::string> owners_key()
{
    return {
        {"waterfallContrast", "1.44"},
        {"spectrumRangeFit", "0.96"},
        {"spectrumSmoothing", "0.27"},
        {"spectrumFloorSmoothing", "true"},
        {"waterfallPivot", "0.949999988079071"},
        {"floorPinned", "false"},
        {"floorDb", "0"},
        {"ceilingPinned", "false"},
        {"ceilingDb", "-42.29999923706055"},
        {"waterfallAutoPivot", "true"},
        {"waterfallTheme", "dusk"},
        {"oledBlack", "true"},
        {"timeMarkerSeconds", "10"},
    };
}

}  // namespace

// Rejects a parser that expects REG_DWORD booleans or locale-dependent
// decimals: Qt writes both as text.
TEST_CASE("the desktop's registry text reads back as its settings", "[webgate][display]")
{
    const DisplaySettings s = revenant::webgate::read_display_settings(reader_of(owners_key()));
    CHECK(s.waterfall_contrast == 1.44);
    CHECK(s.spectrum_range_fit == 0.96);
    CHECK(s.spectrum_smoothing == 0.27);
    CHECK(s.spectrum_floor_smoothing);
    CHECK(s.waterfall_pivot == ui::clamp_waterfall_pivot(0.949999988079071));
    CHECK(s.waterfall_auto_pivot);
    CHECK(s.waterfall_theme == colour_map::theme_by_id("dusk"));
    CHECK(s.oled_black);
    CHECK_FALSE(s.pins.floor_pinned);
    CHECK_FALSE(s.pins.ceiling_pinned);
    CHECK(s.pins.ceiling_db == -42.3F);
}

// Rejects a missing key read as zeros: a fresh install has no display key
// at all, and the phone must look like a fresh desktop.
TEST_CASE("a missing key is the desktop's defaults", "[webgate][display]")
{
    const DisplaySettings s = revenant::webgate::read_display_settings(
        [](std::string_view) { return std::optional<std::string>{}; });
    CHECK(s == DisplaySettings{});
    CHECK(s.spectrum_smoothing == ui::kDefaultSmoothDecaySeconds);
    CHECK(s.waterfall_pivot == ui::kDefaultCurvePivot);
    CHECK(s.spectrum_floor_smoothing);
    CHECK(s.waterfall_theme == colour_map::kDefaultTheme);
}

// Rejects trusting a hand-edited value: garbage is the default, and a number
// out of range takes the desktop's clamp rather than extrapolating.
TEST_CASE("malformed values are defaults and numbers are clamped", "[webgate][display]")
{
    const DisplaySettings s = revenant::webgate::read_display_settings(reader_of({
        {"waterfallContrast", "9"},
        {"spectrumRangeFit", "lots"},
        {"spectrumSmoothing", "1.5x"},
        {"waterfallPivot", "2"},
        {"waterfallTheme", "plaid"},
        {"oledBlack", "perhaps"},
        {"floorPinned", "true"},
        {"floorDb", "-101.5"},
    }));
    CHECK(s.waterfall_contrast == ui::kMaxWaterfallContrast);
    CHECK(s.spectrum_range_fit == 0.0);
    CHECK(s.spectrum_smoothing == ui::kDefaultSmoothDecaySeconds);
    CHECK(s.waterfall_pivot == ui::kMaxCurvePivot);
    CHECK(s.waterfall_theme == colour_map::kDefaultTheme);
    CHECK_FALSE(s.oled_black);
    CHECK(s.pins.floor_pinned);
    CHECK(s.pins.floor_db == -101.5F);
}

// Rejects reading the registry per frame, or never again after the first.
TEST_CASE("the settings are re-read once an interval", "[webgate][display]")
{
    int reads = 0;
    std::string contrast = "0.5";
    const DisplayValueReader counting = [&](std::string_view name) -> std::optional<std::string> {
        if (name == "waterfallContrast") {
            ++reads;
            return contrast;
        }
        return std::nullopt;
    };
    auto now = DisplaySettingsSource::Clock::now();
    DisplaySettingsSource source(counting, [&] { return now; }, std::chrono::seconds(2));
    CHECK(source.current().waterfall_contrast == 0.5);
    contrast = "1.5";
    now += std::chrono::milliseconds(1900);
    CHECK(source.current().waterfall_contrast == 0.5);
    CHECK(reads == 1);
    now += std::chrono::milliseconds(200);
    CHECK(source.current().waterfall_contrast == 1.5);
    CHECK(reads == 2);
}

namespace {

[[nodiscard]] std::vector<float> noisy_columns(std::size_t count, std::uint32_t seed, float carrier_db)
{
    std::vector<float> out(count);
    std::uint32_t x = seed;
    for (std::size_t i = 0; i < count; ++i) {
        x = x * 1664525U + 1013904223U;
        out[i] = -95.0F + static_cast<float>(x >> 24) / 25.0F;
    }
    out[count / 2] = carrier_db;
    return out;
}

[[nodiscard]] DisplayFrame frame_at(std::uint64_t start)
{
    DisplayFrame f;
    f.start = start;
    f.count = 2048;
    f.rate = 2'048'000;
    f.first_bin_hz = 100e6;
    f.floor_db = -98.0F;
    f.ceiling_db = -60.0F;
    return f;
}

}  // namespace

// Rejects a phone window computed any way but the desktop's: the same frames
// through the desktop's own smoother, hold and range fit give the same ends,
// for several settings.
TEST_CASE("the trace window is the desktop spectrum's", "[webgate][display]")
{
    DisplaySettings settings;
    SECTION("defaults") {}
    SECTION("the owner's range fit and smoothing")
    {
        settings.spectrum_range_fit = 0.96;
        settings.spectrum_smoothing = 0.27;
    }
    SECTION("a pinned floor")
    {
        settings.pins.floor_pinned = true;
        settings.pins.floor_db = -110.0F;
        settings.spectrum_range_fit = 0.5;
    }
    SECTION("smoothing off and a pinned ceiling")
    {
        settings.spectrum_smoothing = 0.0;
        settings.pins.ceiling_pinned = true;
        settings.pins.ceiling_db = -30.0F;
    }

    PhoneDisplay display(settings);
    constexpr std::size_t kBinsPerColumn = 4;
    std::vector<float> state;
    ui::PeakHold peak;
    for (std::uint64_t n = 0; n < 4; ++n) {
        const DisplayFrame f = frame_at(n * 2048);
        const std::vector<float> columns = noisy_columns(600, static_cast<std::uint32_t>(n + 1),
                                                         n == 1 ? -40.0F : -70.0F);
        // A view of the whole frame, so the view offsets are zero on both.
        const DisplayRow row = display.take(f, columns, columns, columns, kBinsPerColumn);

        std::vector<float> want = columns;
        const double dt = n == 0 ? 0.0 : 2048.0 / 2'048'000.0;
        ui::smooth_trace(state, want, dt, settings.spectrum_smoothing,
                         settings.spectrum_floor_smoothing);
        peak = ui::hold_peak(peak, ui::drawn_max(want), dt);
        const ui::MapEnds ends = ui::resolve_ends_fit(
            f.floor_db, f.ceiling_db, ui::peak_reduction_headroom_db(kBinsPerColumn), peak,
            settings.pins, static_cast<float>(settings.spectrum_range_fit));
        CHECK(row.trace.floor_db == ends.floor_db);
        CHECK(row.trace.ceiling_db == ends.ceiling_db);
        CHECK(row.trace_columns == want);
    }
}

// Rejects the waterfall drawn against the spectrum's ends, or against each
// row's own: the desktop measures the whole history and eases toward it.
TEST_CASE("the waterfall window is the desktop waterfall's", "[webgate][display]")
{
    DisplaySettings settings;
    settings.waterfall_contrast = 1.44;
    PhoneDisplay display(settings);
    ui::LevelHistogram histogram;
    ui::HistoryLevels levels;
    for (std::uint64_t n = 0; n < 5; ++n) {
        const DisplayFrame f = frame_at(n * 2048);
        const std::vector<float> columns =
            noisy_columns(500, static_cast<std::uint32_t>(n + 7), -50.0F);
        const DisplayRow row = display.take(f, columns, columns, columns, 1);

        for (const float c : columns) {
            histogram.add(ui::encode_level(c));
        }
        const double seconds = 2048.0 / 2'048'000.0;
        levels.update(histogram.percentile(ui::kHistoryNoisePermille),
                      histogram.percentile(ui::kHistoryStrongPermille), seconds,
                      histogram.percentile(ui::kHistoryWeakPermille));
        const ui::MapEnds ends = levels.ends(settings.pins, 1.44F);
        CHECK(row.waterfall.floor_db == ends.floor_db);
        CHECK(row.waterfall.ceiling_db == ends.ceiling_db);
    }
}

// Rejects a palette sampled off by one entry, or one that forgets the bottom
// has to be the background to the bit.
TEST_CASE("the palettes are the desktop's tables", "[webgate][display]")
{
    const std::size_t dusk = colour_map::theme_by_id("dusk");
    const auto& table = colour_map::table(dusk, true);
    const auto straight = revenant::webgate::waterfall_palette(dusk, true, ui::ContrastCurve{});
    const auto fill = revenant::webgate::spectrum_palette(dusk, true);
    for (std::size_t i = 0; i < 256; ++i) {
        const auto& want = table[static_cast<std::size_t>(std::lround(i / 255.0 * 1023.0))];
        CHECK(fill[i].r == want.r);
        CHECK(fill[i].g == want.g);
        CHECK(fill[i].b == want.b);
        CHECK(straight[i].r == want.r);
    }
    CHECK(straight[0].r == 0);
    CHECK(straight[0].g == 0);
    CHECK(straight[0].b == 0);

    // Under the curve the noise below the pivot goes darker.
    const auto curved = revenant::webgate::waterfall_palette(dusk, true, ui::curve_for(1.8F, 0.5F));
    CHECK(curved[64].g < straight[64].g);
    CHECK(curved[255].g == straight[255].g);
}

// Rejects an event sent once and never again, or one sent every poll.
TEST_CASE("a palette change is a new display event", "[webgate][display]")
{
    DisplaySettings settings;
    PhoneDisplay display(settings);
    const std::string first = display.event();
    CHECK(first.find(R"("event":"display")") != std::string::npos);
    CHECK(first.find(R"("waterfall":")") != std::string::npos);

    display.set_settings(settings);
    CHECK(display.event() == first);

    settings.waterfall_theme = colour_map::theme_by_id("amber");
    display.set_settings(settings);
    const std::string themed = display.event();
    CHECK(themed != first);

    settings.waterfall_contrast = 1.5;
    display.set_settings(settings);
    CHECK(display.event() != themed);

    // A spectrum setting moves the window, not the colours.
    const std::string contrasted = display.event();
    settings.spectrum_range_fit = 0.7;
    display.set_settings(settings);
    CHECK(display.event() == contrasted);

    settings.oled_black = true;
    display.set_settings(settings);
    CHECK(display.event().find(R"("background":"#000000")") != std::string::npos);
}

// Rejects an automatic pivot that recolours on every row, or never.
TEST_CASE("the automatic pivot recolours once it has moved", "[webgate][display]")
{
    DisplaySettings settings;
    settings.waterfall_contrast = 1.8;
    settings.waterfall_auto_pivot = true;
    settings.waterfall_pivot = 0.9;
    PhoneDisplay display(settings);
    const std::string before = display.event();
    CHECK(display.drawn_pivot() == 0.9F);

    // Noise and a band of strong signal: two populations, a split between.
    std::vector<float> columns = noisy_columns(400, 3, -40.0F);
    for (std::size_t i = 300; i < 400; ++i) {
        columns[i] = -40.0F;
    }
    const auto row = display.take(frame_at(0), columns, columns, columns, 1);
    (void)row;
    CHECK(display.drawn_pivot() != 0.9F);
    CHECK(display.event() != before);
}

// Rejects the web build's fallback palette drifting from the gateway's: the
// phone draws it until the first display event arrives.
TEST_CASE("the web build's default colours are the gateway's", "[webgate][display]")
{
    std::ifstream file(std::string(REVENANT_SOURCE_DIR) + "/web/src/display_defaults.ts");
    REQUIRE(file.good());
    std::stringstream text;
    text << file.rdbuf();
    const std::string event =
        revenant::webgate::display_event(DisplaySettings{}, ui::kDefaultCurvePivot);
    const auto field = [&event](std::string_view key) {
        const std::string open = "\"" + std::string(key) + "\":\"";
        const auto at = event.find(open);
        REQUIRE(at != std::string::npos);
        const auto from = at + open.size();
        return event.substr(from, event.find('"', from) - from);
    };
    for (const char* key : {"waterfall", "spectrum", "trace", "background"}) {
        // The value printed is what display_defaults.ts should hold.
        INFO(key << " = " << field(key));
        CHECK(text.str().find("\"" + field(key) + "\"") != std::string::npos);
    }
}
