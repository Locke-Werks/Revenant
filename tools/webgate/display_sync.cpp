#include "tools/webgate/display_sync.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace revenant::webgate {

namespace colour_map = ui::colour_map;

namespace {

[[nodiscard]] std::optional<double> parse_double(std::string_view text)
{
    double value = 0.0;
    const char* end = text.data() + text.size();
    const auto [at, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc{} || at != end || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view text)
{
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "true" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "0") {
        return false;
    }
    return std::nullopt;
}

[[nodiscard]] double read_double(const DisplayValueReader& read, std::string_view name,
                                 double fallback)
{
    const auto text = read(name);
    const auto value = text ? parse_double(*text) : std::nullopt;
    return value.value_or(fallback);
}

[[nodiscard]] bool read_bool(const DisplayValueReader& read, std::string_view name, bool fallback)
{
    const auto text = read(name);
    const auto value = text ? parse_bool(*text) : std::nullopt;
    return value.value_or(fallback);
}

void put_hex(std::string& out, const colour_map::Srgb8& c)
{
    constexpr char kDigits[] = "0123456789abcdef";
    for (const std::uint8_t channel : {c.r, c.g, c.b}) {
        out.push_back(kDigits[channel >> 4U]);
        out.push_back(kDigits[channel & 0x0FU]);
    }
}

[[nodiscard]] std::string hex_of(const Palette& palette)
{
    std::string out;
    out.reserve(palette.size() * 6);
    for (const auto& c : palette) {
        put_hex(out, c);
    }
    return out;
}

[[nodiscard]] std::string colour_text(const colour_map::Srgb8& c)
{
    std::string out = "#";
    put_hex(out, c);
    return out;
}

// The pivot the waterfall is coloured at before any history: the stored one,
// which is also where the desktop's drawn pivot starts.
[[nodiscard]] float stored_pivot(const ui::DisplaySettings& s)
{
    return static_cast<float>(s.waterfall_pivot);
}

}  // namespace

ui::DisplaySettings read_display_settings(const DisplayValueReader& read)
{
    ui::DisplaySettings s;
    if (!read) {
        return s;
    }
    s.pins.floor_pinned = read_bool(read, "floorPinned", s.pins.floor_pinned);
    s.pins.floor_db = static_cast<float>(read_double(read, "floorDb", s.pins.floor_db));
    s.pins.ceiling_pinned = read_bool(read, "ceilingPinned", s.pins.ceiling_pinned);
    s.pins.ceiling_db = static_cast<float>(read_double(read, "ceilingDb", s.pins.ceiling_db));
    s.waterfall_contrast =
        ui::clamp_waterfall_contrast(read_double(read, "waterfallContrast", s.waterfall_contrast));
    s.waterfall_pivot =
        ui::clamp_waterfall_pivot(read_double(read, "waterfallPivot", s.waterfall_pivot));
    s.waterfall_auto_pivot = read_bool(read, "waterfallAutoPivot", s.waterfall_auto_pivot);
    if (const auto theme = read("waterfallTheme")) {
        s.waterfall_theme = colour_map::theme_by_id(*theme);
    }
    s.oled_black = read_bool(read, "oledBlack", s.oled_black);
    s.spectrum_range_fit =
        ui::clamp_range_fit(read_double(read, "spectrumRangeFit", s.spectrum_range_fit));
    s.spectrum_smoothing =
        ui::clamp_smoothing(read_double(read, "spectrumSmoothing", s.spectrum_smoothing));
    s.spectrum_floor_smoothing =
        read_bool(read, "spectrumFloorSmoothing", s.spectrum_floor_smoothing);
    return s;
}

DisplayValueReader registry_display_reader()
{
#ifdef _WIN32
    return [](std::string_view name) -> std::optional<std::string> {
        const std::wstring key = L"Software\\Locke Werks\\Revenant\\display";
        const std::wstring value(name.begin(), name.end());
        constexpr DWORD kTypes = RRF_RT_REG_SZ | RRF_RT_REG_DWORD | RRF_RT_REG_QWORD;
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), kTypes, &type, nullptr,
                         &bytes) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> data(bytes + sizeof(wchar_t), 0);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value.c_str(), kTypes, &type,
                         data.data(), &bytes) != ERROR_SUCCESS) {
            return std::nullopt;
        }
        if (type == REG_DWORD && bytes >= sizeof(DWORD)) {
            DWORD number = 0;
            std::memcpy(&number, data.data(), sizeof number);
            return std::to_string(number);
        }
        if (type == REG_QWORD && bytes >= sizeof(std::uint64_t)) {
            std::uint64_t number = 0;
            std::memcpy(&number, data.data(), sizeof number);
            return std::to_string(number);
        }
        if (type != REG_SZ) {
            return std::nullopt;
        }
        const auto* wide = reinterpret_cast<const wchar_t*>(data.data());
        const int length = static_cast<int>(wcsnlen(wide, data.size() / sizeof(wchar_t)));
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(std::max(needed, 0)), '\0');
        if (needed > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wide, length, out.data(), needed, nullptr, nullptr);
        }
        return out;
    };
#else
    return [](std::string_view) { return std::optional<std::string>{}; };
#endif
}

DisplaySettingsSource::DisplaySettingsSource(DisplayValueReader reader,
                                             std::function<Clock::time_point()> clock,
                                             std::chrono::milliseconds interval)
    : reader_(std::move(reader)), clock_(std::move(clock)), interval_(interval)
{
}

ui::DisplaySettings DisplaySettingsSource::current()
{
    const auto now = clock_ ? clock_() : Clock::now();
    const std::lock_guard lock(mutex_);
    if (!read_at_ || now - *read_at_ >= interval_) {
        settings_ = read_display_settings(reader_);
        read_at_ = now;
    }
    return settings_;
}

Palette waterfall_palette(std::size_t theme, bool black, const ui::ContrastCurve& curve)
{
    // CodeColourer's arithmetic: the curve folded into a 1024 entry table,
    // then a level a fraction x of the window read at x * 1023 + 0.5.
    const auto& table = colour_map::table(theme, black);
    const auto last = static_cast<float>(colour_map::kTableSize - 1);
    Palette out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto j = static_cast<std::size_t>(static_cast<float>(i) / 255.0F * last + 0.5F);
        if (curve.amount > 0.0F) {
            const float y = ui::apply_curve(curve, static_cast<float>(j) / last);
            out[i] = table[static_cast<std::size_t>(y * last + 0.5F)];
        } else {
            out[i] = table[j];
        }
    }
    return out;
}

Palette spectrum_palette(std::size_t theme, bool black)
{
    Palette out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const float t = static_cast<float>(i) / 255.0F;
        out[i] = colour_map::table(theme, black)[static_cast<std::size_t>(
            std::lround(t * static_cast<float>(colour_map::kTableSize - 1)))];
    }
    return out;
}

std::string display_event(const ui::DisplaySettings& s, float pivot)
{
    const ui::ContrastCurve curve =
        ui::curve_for(static_cast<float>(s.waterfall_contrast), pivot);
    const auto& table = colour_map::table(s.waterfall_theme, s.oled_black);
    // SpectrumItem's background node: black under OLED, else #080a0e.
    const colour_map::Srgb8 background = s.oled_black ? colour_map::Srgb8{0, 0, 0}
                                                      : colour_map::Srgb8{8, 10, 14};
    std::string out = R"({"event":"display","waterfall":")";
    out += hex_of(waterfall_palette(s.waterfall_theme, s.oled_black, curve));
    out += R"(","spectrum":")";
    out += hex_of(spectrum_palette(s.waterfall_theme, s.oled_black));
    out += R"(","trace":")";
    out += colour_text(table.back());
    out += R"(","background":")";
    out += colour_text(background);
    out += R"("})";
    return out;
}

PhoneDisplay::PhoneDisplay(const ui::DisplaySettings& settings)
    : settings_(settings), drawn_pivot_(stored_pivot(settings))
{
}

void PhoneDisplay::set_settings(const ui::DisplaySettings& next)
{
    if (next == settings_) {
        return;
    }
    const ui::DisplaySettings was = settings_;
    settings_ = next;
    // ScaleSettings::setWaterfallAutoPivot: the follower starts again, and
    // turning it off puts the stored pivot back.
    if (next.waterfall_auto_pivot != was.waterfall_auto_pivot) {
        follower_.reset();
        if (!next.waterfall_auto_pivot) {
            drawn_pivot_ = stored_pivot(next);
        }
    } else if (!next.waterfall_auto_pivot) {
        drawn_pivot_ = stored_pivot(next);
    }
    if (next.waterfall_theme != was.waterfall_theme || next.oled_black != was.oled_black ||
        next.waterfall_contrast != was.waterfall_contrast ||
        next.waterfall_pivot != was.waterfall_pivot ||
        next.waterfall_auto_pivot != was.waterfall_auto_pivot) {
        event_dirty_ = true;
    }
}

void PhoneDisplay::view_changed()
{
    view_offsets_.valid = false;
}

DisplayRow PhoneDisplay::take(const DisplayFrame& frame, std::span<const float> whole,
                              std::span<const float> shown, std::span<const float> columns,
                              std::size_t bins_per_column)
{
    DisplayRow row;

    // SpectrumItem::sourceSecondsSinceLast: source time between frames, and
    // none across a retune or a stream that went backwards.
    const double dt = have_last_ && frame.start > last_start_ && frame.rate > 0 &&
                              frame.first_bin_hz == last_first_bin_hz_
                          ? static_cast<double>(frame.start - last_start_) /
                                static_cast<double>(frame.rate)
                          : 0.0;
    have_last_ = true;
    last_start_ = frame.start;
    last_first_bin_hz_ = frame.first_bin_hz;

    // SpectrumItem::takeFrame, in its order.
    row.trace_columns.assign(columns.begin(), columns.end());
    ui::smooth_trace(smooth_state_, row.trace_columns, dt, settings_.spectrum_smoothing,
                     settings_.spectrum_floor_smoothing);
    peak_ = ui::hold_peak(peak_, ui::drawn_max(row.trace_columns), dt);
    view_offsets_.track(whole, shown, dt, scratch_);
    row.trace = ui::resolve_ends_fit(
        frame.floor_db + view_offsets_.floor_db, frame.ceiling_db + view_offsets_.ceiling_db,
        ui::peak_reduction_headroom_db(std::max<std::size_t>(bins_per_column, 1)), peak_,
        settings_.pins, static_cast<float>(settings_.spectrum_range_fit));

    // WaterfallItem::takeFrame: the row into the history as levels, the
    // oldest out, and the ends eased toward the history's percentiles.
    std::vector<std::uint16_t> codes(columns.size());
    for (std::size_t i = 0; i < columns.size(); ++i) {
        codes[i] = ui::encode_level(columns[i]);
    }
    histogram_.add_row(codes.data(), static_cast<int>(codes.size()));
    rows_.push_back(std::move(codes));
    while (rows_.size() > kHistoryRows) {
        histogram_.remove_row(rows_.front().data(), static_cast<int>(rows_.front().size()));
        rows_.pop_front();
    }
    const std::uint64_t frame_end = frame.start + frame.count;
    double seconds = 0.0;
    if (frame.rate > 0) {
        const std::uint64_t elapsed = frame_end > last_sample_end_ && last_sample_end_ != 0
                                          ? frame_end - last_sample_end_
                                          : frame.count;
        seconds = std::min(static_cast<double>(elapsed) / static_cast<double>(frame.rate), 1.0);
    }
    last_sample_end_ = frame_end;
    levels_.update(histogram_.percentile(ui::kHistoryNoisePermille),
                   histogram_.percentile(ui::kHistoryStrongPermille), seconds,
                   histogram_.percentile(ui::kHistoryWeakPermille));
    row.waterfall = levels_.ends(settings_.pins, static_cast<float>(settings_.waterfall_contrast));

    // ScaleSettings::offerAutoPivotAfter and WaterfallItem::onPivotChanged:
    // eased per row, recoloured only once it has moved the tolerance.
    if (settings_.waterfall_auto_pivot) {
        if (const std::optional<float> pivot = ui::auto_pivot(histogram_, row.waterfall)) {
            const auto next = static_cast<float>(
                ui::clamp_waterfall_pivot(follower_.step(*pivot, seconds)));
            if (std::abs(next - drawn_pivot_) >= ui::kAutoPivotRecolourTolerance) {
                drawn_pivot_ = next;
                event_dirty_ = true;
            }
        }
    }
    return row;
}

const std::string& PhoneDisplay::event()
{
    if (event_dirty_) {
        event_ = display_event(settings_, drawn_pivot_);
        event_dirty_ = false;
    }
    return event_;
}

}  // namespace revenant::webgate
