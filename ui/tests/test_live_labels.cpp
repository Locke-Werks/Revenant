// merge_span_labels: live detections against band-plan channels.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, in its own
// comment, for the reason test_audio_ring.cpp gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "models/live_labels.h"

using revenant::rpc::Detection;
using revenant::rpc::LabelKind;
using namespace revenant::ui;

namespace {

[[nodiscard]] Detection named(std::int64_t hz, std::string name, double confidence = 0.9)
{
    Detection d;
    d.id = static_cast<std::uint64_t>(hz);
    d.center_hz = hz;
    d.bandwidth_hz = 16'000;
    d.label.kind = LabelKind::AnalogModulation;
    d.label.name = std::move(name);
    d.label.confidence = confidence;
    d.label.may_drive = true;
    return d;
}

[[nodiscard]] bool has(const std::vector<SpanLabel>& labels, const std::string& text)
{
    return std::any_of(labels.begin(), labels.end(),
                       [&](const SpanLabel& l) { return l.text == text; });
}

[[nodiscard]] SpanLabelView weather_view()
{
    SpanLabelView view;
    view.low_hz = 162'300'000.0;
    view.high_hz = 162'700'000.0;
    view.width_px = 1000.0;
    return view;
}

}  // namespace

// Rejects drawing the plan's name beside the live one at the same frequency.
TEST_CASE("a live label shadows the plan channel it sits on", "[live_labels]")
{
    const auto labels = merge_span_labels({named(162'475'000, "NFM")}, weather_view());
    CHECK(has(labels, "NFM"));
    CHECK_FALSE(has(labels, "WX3"));
    CHECK(has(labels, "WX4"));
}

// Rejects ranking a live label below a plan label, or ignoring its confidence.
TEST_CASE("live labels outrank plan labels and fade with confidence", "[live_labels]")
{
    const auto labels = merge_span_labels({named(162'475'000, "NFM", 0.2)}, weather_view());
    const auto live = std::find_if(labels.begin(), labels.end(), [](const SpanLabel& l) { return l.live; });
    REQUIRE(live != labels.end());
    for (const SpanLabel& l : labels) {
        if (!l.live) {
            CHECK(live->priority > l.priority);
        }
    }
    CHECK(live_label_opacity(0.2) < live_label_opacity(0.9));
    CHECK(live_label_opacity(1.0) == 1.0);
    CHECK(live->mode == "nfm");
}

// Rejects seven overlapping WX labels at a wide span.
TEST_CASE("a crowded group collapses to its parent at a wide span", "[live_labels]")
{
    SpanLabelView view;
    view.low_hz = 160'000'000.0;
    view.high_hz = 165'000'000.0;
    view.width_px = 1000.0;
    const auto labels = merge_span_labels({}, view);
    CHECK(has(labels, "NOAA weather"));
    CHECK_FALSE(has(labels, "WX1"));

    const auto zoomed = merge_span_labels({}, weather_view());
    CHECK_FALSE(has(zoomed, "NOAA weather"));
    CHECK(has(zoomed, "WX1"));
}

// Rejects a category switch that does nothing.
TEST_CASE("categories filter plan and live labels", "[live_labels]")
{
    SpanLabelView view = weather_view();
    view.categories = kLabelAll & ~kLabelWeather & ~kLabelLive;
    const auto labels = merge_span_labels({named(162'475'000, "NFM")}, view);
    CHECK(labels.empty());
}

// Rejects labelling a detection nobody has identified; its plate already shows it.
TEST_CASE("unknown detections get no overlay label", "[live_labels]")
{
    Detection d = named(162'475'000, "");
    d.label.kind = LabelKind::Unknown;
    const auto labels = merge_span_labels({d}, weather_view());
    CHECK(std::none_of(labels.begin(), labels.end(), [](const SpanLabel& l) { return l.live; }));
    CHECK(has(labels, "WX3"));
}

// Rejects the watering holes going missing from the plan.
TEST_CASE("digital watering holes are in the plan", "[live_labels]")
{
    SpanLabelView view;
    view.low_hz = 14'000'000.0;
    view.high_hz = 14'200'000.0;
    view.width_px = 2000.0;
    const auto hf = merge_span_labels({}, view);
    CHECK(has(hf, "20 m FT8"));
    CHECK(has(hf, "20 m WSPR"));

    view.low_hz = 144'300'000.0;
    view.high_hz = 144'500'000.0;
    view.regions = kRegionUS;
    CHECK(has(merge_span_labels({}, view), "APRS"));
}
