// The labels the span overlay draws: band-plan channels such as "20 m FT8" or
// "WX3", and the names the engine gave live detections, "P25" or "NFM".
//
// Qt-free so ui/tests can hold it. models/live_labels_model.cpp measures the
// text and hands the result to render/label_layout.h.
//
// LIVE WINS. A detection the engine has named is a measurement of what is on
// the air now; a plan row is what is usually there. A plan label inside a
// named detection's occupied band, or within kLiveShadowPx of it, is dropped,
// so "NOAA" from the plan never sits beside the "NFM" the detector heard at the
// same frequency. Live labels also outrank every plan label in the layout and
// carry their label confidence as opacity; plan labels are drawn dimmer.
//
// ONLY CHANNEL ROWS. Allocations and sub-bands already have the band bar above
// the trace. A channel is a point an operator tunes to, which is what a label
// with a leader tick is for.
//
// A CROWDED GROUP COLLAPSES TO ITS PARENT. At a wide span the seven NOAA
// weather channels sit within a few pixels of each other and would hide one
// another anyway; when a group's members span less than kGroupSpreadPx each,
// they are drawn as one label carrying the group name, at the member nearest
// their middle. Zooming in spreads them and they come apart.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "models/band_plan.h"
#include "models/label_tune.h"
#include "models/ruler.h"

namespace revenant::ui {

// Which kinds of label the operator wants, a bit each, stored as one integer.
enum LabelCategory : std::uint32_t {
    kLabelLive = 1u << 0,
    kLabelAmateur = 1u << 1,
    kLabelWeather = 1u << 2,
    kLabelAviation = 1u << 3,
    kLabelMarine = 1u << 4,
    kLabelPublicSafety = 1u << 5,
    kLabelPersonal = 1u << 6,
    kLabelBroadcast = 1u << 7,
    kLabelOther = 1u << 8,
};
inline constexpr std::uint32_t kLabelAll = (1u << 9) - 1u;

inline constexpr double kLiveShadowPx = 6.0;
inline constexpr double kGroupSpreadPx = 40.0;

inline constexpr double kPlanOpacity = 0.45;
inline constexpr double kPlanFavouriteOpacity = 0.6;
inline constexpr double kLiveMinOpacity = 0.35;

[[nodiscard]] constexpr std::uint32_t label_category(BandColour colour)
{
    switch (colour) {
        case BandColour::kAmateur: return kLabelAmateur;
        case BandColour::kWeather: return kLabelWeather;
        case BandColour::kAviation: return kLabelAviation;
        case BandColour::kMarine: return kLabelMarine;
        case BandColour::kLandMobile:
        case BandColour::kPublicSafety: return kLabelPublicSafety;
        case BandColour::kPersonal: return kLabelPersonal;
        case BandColour::kBroadcast: return kLabelBroadcast;
        case BandColour::kSatellite:
        case BandColour::kIsm:
        case BandColour::kGovernment:
        case BandColour::kOther: return kLabelOther;
    }
    return kLabelOther;
}

struct SpanLabel {
    std::string text;
    std::int64_t hz = 0;

    // What a click sets, as core/engine/vrx.h spells it; "" leaves it to the
    // receiver's own rule.
    std::string mode;

    bool live = false;
    double opacity = kPlanOpacity;
    double priority = 0.0;

    // Live only: which detection, and its measured width, so a click goes
    // through the same tune path a click on the detection's bracket does.
    std::uint64_t detection_id = 0;
    std::int64_t bandwidth_hz = 0;
};

struct SpanLabelView {
    double low_hz = 0.0;
    double high_hz = 0.0;
    double width_px = 0.0;
    std::uint32_t regions = kRegionUSCA;
    std::uint32_t categories = kLabelAll;
};

[[nodiscard]] inline double live_label_opacity(double confidence)
{
    const double c = std::isfinite(confidence) ? std::clamp(confidence, 0.0, 1.0) : 0.0;
    return kLiveMinOpacity + (1.0 - kLiveMinOpacity) * c;
}

[[nodiscard]] inline std::vector<SpanLabel> merge_span_labels(
    const std::vector<rpc::Detection>& detections, const SpanLabelView& view)
{
    std::vector<SpanLabel> out;
    if (!(view.high_hz > view.low_hz) || !(view.width_px > 0.0)) {
        return out;
    }
    const double hz_per_px = (view.high_hz - view.low_hz) / view.width_px;
    const auto in_view = [&](double hz) { return hz >= view.low_hz && hz <= view.high_hz; };

    struct Shadow {
        double lo = 0.0;
        double hi = 0.0;
    };
    std::vector<Shadow> shadows;

    if ((view.categories & kLabelLive) != 0) {
        for (const rpc::Detection& d : detections) {
            if (d.merged_into != 0 || d.label.kind == rpc::LabelKind::Unknown ||
                d.label.name.empty() || !in_view(static_cast<double>(d.center_hz))) {
                continue;
            }
            const LabelTune tune = label_tune(d.label, static_cast<double>(d.bandwidth_hz));
            SpanLabel label;
            label.text = d.label.name;
            label.hz = d.center_hz;
            label.mode = std::string(tune.mode);
            label.live = true;
            label.opacity = live_label_opacity(d.label.confidence);
            label.priority = 100.0 + 100.0 * label.opacity;
            label.detection_id = d.id;
            label.bandwidth_hz = d.bandwidth_hz;
            out.push_back(std::move(label));

            const double half = static_cast<double>(std::max<std::int64_t>(d.bandwidth_hz, 0)) / 2.0 +
                                kLiveShadowPx * hz_per_px;
            const double c = static_cast<double>(d.center_hz);
            shadows.push_back(Shadow{c - half, c + half});
        }
    }

    const auto shadowed = [&](double hz) {
        return std::any_of(shadows.begin(), shadows.end(),
                           [&](const Shadow& s) { return hz >= s.lo && hz <= s.hi; });
    };

    // Grouped by parent; a row with no parent is a group of one.
    std::map<std::string_view, std::vector<const Band*>> groups;
    std::vector<const Band*> singles;
    for (const Band* band : bands_overlapping(static_cast<std::int64_t>(view.low_hz),
                                              static_cast<std::int64_t>(view.high_hz),
                                              view.regions)) {
        if (band->kind != BandKind::kChannel ||
            (label_category(band->colour) & view.categories) == 0) {
            continue;
        }
        const double hz = static_cast<double>(band->centre_hz);
        if (!in_view(hz) || shadowed(hz)) {
            continue;
        }
        if (band->parent.empty()) {
            singles.push_back(band);
        } else {
            groups[band->parent].push_back(band);
        }
    }

    const auto plan_label = [](const Band& band, std::string_view text, double priority) {
        SpanLabel label;
        label.text = std::string(text);
        label.hz = band.centre_hz;
        label.mode = std::string(band.mode);
        label.opacity = band.favourite ? kPlanFavouriteOpacity : kPlanOpacity;
        label.priority = priority + (band.favourite ? 10.0 : 0.0);
        return label;
    };

    for (const Band* band : singles) {
        out.push_back(plan_label(*band, band->name, 10.0));
    }
    for (auto& [parent, members] : groups) {
        std::sort(members.begin(), members.end(),
                  [](const Band* a, const Band* b) { return a->centre_hz < b->centre_hz; });
        const double spread_px =
            static_cast<double>(members.back()->centre_hz - members.front()->centre_hz) / hz_per_px;
        if (members.size() < 2 || spread_px >= kGroupSpreadPx * static_cast<double>(members.size())) {
            for (const Band* band : members) {
                out.push_back(plan_label(*band, band->name, 10.0));
            }
            continue;
        }
        const double middle = static_cast<double>(members.front()->centre_hz + members.back()->centre_hz) / 2.0;
        const Band* nearest = *std::min_element(members.begin(), members.end(),
                                                [&](const Band* a, const Band* b) {
                                                    return std::abs(static_cast<double>(a->centre_hz) - middle) <
                                                           std::abs(static_cast<double>(b->centre_hz) - middle);
                                                });
        out.push_back(plan_label(*nearest, parent, 15.0));
    }
    return out;
}

// The x of a label's frequency in a view, the same mapping the trace uses.
[[nodiscard]] inline double span_label_x(const SpanLabel& label, const SpanLabelView& view)
{
    return ruler_x(static_cast<double>(label.hz), view.low_hz, view.high_hz, view.width_px);
}

}  // namespace revenant::ui
