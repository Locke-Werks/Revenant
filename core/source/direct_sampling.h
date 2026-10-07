// RTL-SDR direct sampling: which ADC branch a requested centre lands on, and
// what the dongle can therefore reach.
//
// WHY IT IS ITS OWN HEADER WITH NO librtlsdr IN IT. Everything here that can be
// wrong is arithmetic: where Auto switches, where it switches back, and which
// ranges the source then claims. tests/engine/test_direct_sampling.cpp asserts
// all of it without a dongle, and rtlsdr_source.cpp only carries the decision
// to the device.
//
// WHAT DIRECT SAMPLING IS. The RTL2832U digitises the tuner's IF on two ADC
// inputs. Direct sampling takes one of them straight from the antenna instead,
// with the tuner out of circuit, and rtlsdr_set_center_freq then programs the
// demodulator's DDC rather than the tuner. Three consequences follow, and each
// one is handled somewhere:
//
//   The tuner's gain no longer acts on anything, so rtlsdr_source.cpp holds a
//   gain request made in direct mode and applies it when the tuner comes back.
//   The RTL2832U's own digital AGC still acts and is left as configured.
//
//   The input is real rather than complex, so a signal and its mirror are both
//   present and images appear. Nothing here pretends otherwise.
//
//   The reach is set by the 28.8 MHz ADC clock, not by the tuner. The first
//   Nyquist zone ends at 14.4 MHz; above it the DDC reaches a signal through
//   its alias, which works on an RTL-SDR v3 because its HF input has no
//   anti-alias filter, and is how other receivers reach 28.8 MHz on the same
//   hardware. kDirectSamplingCeilingHz claims the whole of that.
//
// THE RTL-SDR BLOG V4 IS DIFFERENT. It has an HF upconverter on the board, and
// the librtlsdr this tree links (osmocom 797f814, which carries the Blog V4
// support and its upconverter GPIO switch, see docs/rtlsdr-provenance.md)
// routes a centre below 28.8 MHz through it without being asked. Its Q input
// is not the HF port the v3 makes it. So on a V4, Auto never selects a branch:
// it stays on the tuner path and lets the library engage the upconverter, and
// the tuner path's reach is extended down to kBlogV4HfLowHz. An explicit I or
// Q is still honoured, because it was asked for by name, and the capability
// notes say it is unlikely to be what the operator wanted.

#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/source/capabilities.h"

namespace revenant::source {

// DirectSampling and DirectSamplingMode live in capabilities.h, beside the
// fields that carry them.

// The RTL2832U's crystal, which clocks the ADC.
inline constexpr dsp::Hertz kRtl2832XtalHz = 28'800'000;

// The highest centre a direct-sampling branch is asked for. See the header
// note: the first Nyquist zone plus the second reached through its alias.
inline constexpr dsp::Hertz kDirectSamplingCeilingHz = kRtl2832XtalHz;

// How far above the tuner's lowest frequency Auto stays on the Q branch once
// it is there. Entering Q happens at the tuner's own floor, because below it
// there is no choice; leaving waits this much higher, so a centre dithered
// around the floor, a scroll wheel or an AFT loop, does not flip the ADC input
// on every step. Each flip pauses the stream for a control change, so a flip
// per step would be a stream that keeps dropping samples.
//
// 2 MHz because it is comfortably wider than one span at the 2.4 MS/s default,
// so stepping a span at a time cannot oscillate either.
inline constexpr dsp::Hertz kAutoDirectHysteresisHz = 2'000'000;

// The low end of an RTL-SDR Blog V4's HF coverage through its upconverter, as
// the maker publishes it.
inline constexpr dsp::Hertz kBlogV4HfLowHz = 500'000;

[[nodiscard]] constexpr std::string_view direct_sampling_name(DirectSamplingMode mode)
{
    switch (mode) {
        case DirectSamplingMode::Off: return "off";
        case DirectSamplingMode::IBranch: return "i";
        case DirectSamplingMode::QBranch: return "q";
        case DirectSamplingMode::Auto: return "auto";
    }
    return "off";
}

[[nodiscard]] constexpr std::string_view direct_sampling_name(DirectSampling branch)
{
    return direct_sampling_name(static_cast<DirectSamplingMode>(branch));
}

// Case-insensitive, and the spellings the URI key takes. Nothing for anything
// else, so the caller can refuse with its own sentence.
[[nodiscard]] inline std::optional<DirectSamplingMode> parse_direct_sampling(std::string_view text)
{
    std::string lowered(text);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (lowered == "off" || lowered == "0") return DirectSamplingMode::Off;
    if (lowered == "i" || lowered == "1") return DirectSamplingMode::IBranch;
    if (lowered == "q" || lowered == "2") return DirectSamplingMode::QBranch;
    if (lowered == "auto") return DirectSamplingMode::Auto;
    return std::nullopt;
}

// Whether the board is an RTL-SDR Blog V4, from the USB strings the open
// handle reports. The V4 identifies itself with product "Blog V4" (and
// manufacturer "RTLSDRBlog"); a v3 reports "RTL2838UHIDIR" and so do the
// clones, which is why this reads the product string and not the USB id.
[[nodiscard]] inline bool is_blog_v4(std::string_view manufacturer, std::string_view product)
{
    static_cast<void>(manufacturer);
    std::string lowered(product);
    for (char& c : lowered) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return lowered.find("blog v4") != std::string::npos;
}

// The lowest frequency the tuner path reaches, or nothing for a tuner
// librtlsdr has no driver for.
[[nodiscard]] inline std::optional<dsp::Hertz> tuner_floor(const std::vector<TuneRange>& tuner)
{
    if (tuner.empty()) {
        return std::nullopt;
    }
    dsp::Hertz low = tuner.front().low;
    for (const TuneRange& range : tuner) {
        low = std::min(low, range.low);
    }
    return low;
}

// The centre above which Auto leaves the Q branch for the tuner.
[[nodiscard]] inline dsp::Hertz auto_leave_hz(const std::vector<TuneRange>& tuner)
{
    const auto floor = tuner_floor(tuner);
    if (!floor) {
        return kDirectSamplingCeilingHz;
    }
    return std::min(*floor + kAutoDirectHysteresisHz, kDirectSamplingCeilingHz);
}

// The branch the hardware should be in for `centre`, given what it is in now.
//
// `tuner` is the tuner path's own reach, before any direct-sampling range is
// added. `upconverter` is true on a Blog V4. `current` only matters to Auto,
// and is what gives it the hysteresis.
[[nodiscard]] inline DirectSampling resolve_direct_sampling(DirectSamplingMode mode,
                                                            DirectSampling current,
                                                            dsp::Hertz centre,
                                                            const std::vector<TuneRange>& tuner,
                                                            bool upconverter)
{
    switch (mode) {
        case DirectSamplingMode::Off: return DirectSampling::Off;
        case DirectSamplingMode::IBranch: return DirectSampling::IBranch;
        case DirectSamplingMode::QBranch: return DirectSampling::QBranch;
        case DirectSamplingMode::Auto: break;
    }

    // The V4's library routes HF through the upconverter on the tuner path,
    // so the tuner path is always the answer there.
    if (upconverter) {
        return DirectSampling::Off;
    }

    const auto floor = tuner_floor(tuner);
    if (!floor) {
        // No tuner driver: the Q branch is the only thing that can tune.
        return DirectSampling::QBranch;
    }

    if (current == DirectSampling::QBranch) {
        return centre > auto_leave_hz(tuner) ? DirectSampling::Off : DirectSampling::QBranch;
    }
    return centre < *floor ? DirectSampling::QBranch : DirectSampling::Off;
}

// Sorted and with overlapping or touching ranges joined, so a client asking
// whether a band is reachable sees one span rather than two that meet.
[[nodiscard]] inline std::vector<TuneRange> merged_ranges(std::vector<TuneRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(),
              [](const TuneRange& a, const TuneRange& b) { return a.low < b.low; });
    std::vector<TuneRange> out;
    for (const TuneRange& range : ranges) {
        if (!out.empty() && range.low <= out.back().high) {
            out.back().high = std::max(out.back().high, range.high);
            continue;
        }
        out.push_back(range);
    }
    return out;
}

// What the source claims it can reach under `mode`.
//
// Under Auto that is the union of both paths, because tune() picks the branch
// per centre; claiming only the path in force at open would refuse an HF
// centre that Auto exists to reach. Under Off on a V4 the tuner path is
// extended down to the upconverter's floor, for the same reason.
[[nodiscard]] inline std::vector<TuneRange> direct_sampling_ranges(
    DirectSamplingMode mode, const std::vector<TuneRange>& tuner, bool upconverter)
{
    const TuneRange direct{0, kDirectSamplingCeilingHz, 0};

    switch (mode) {
        case DirectSamplingMode::IBranch:
        case DirectSamplingMode::QBranch:
            return {direct};
        case DirectSamplingMode::Off:
        case DirectSamplingMode::Auto:
            break;
    }

    if (upconverter) {
        if (tuner.empty()) {
            return {};
        }
        std::vector<TuneRange> out = tuner;
        out.push_back(TuneRange{kBlogV4HfLowHz, kDirectSamplingCeilingHz, 0});
        return merged_ranges(std::move(out));
    }

    if (mode == DirectSamplingMode::Off) {
        return tuner;
    }

    std::vector<TuneRange> out = tuner;
    out.push_back(TuneRange{0, auto_leave_hz(tuner), 0});
    return merged_ranges(std::move(out));
}

}  // namespace revenant::source
