// A front-end retune through the real engine, on a source with no radio behind
// it.
//
// WHY THIS FILE EXISTS
//
// Engine::set_source_center rebases every receiver, removes the ones it cannot
// carry and cancels every probe, and until this file none of that ran without a
// dongle: the file and synthetic backends both refuse a retune, and each is
// right to. tests/engine/test_engine.cpp covers the retune against a real
// RTL-SDR behind [device], and tests/rpc/retunable_engine.h imitates the
// engine's answer for the server's sake without running the engine's code.
//
// So this file builds its own front end: a synthetic scene wrapped so that
// tune() moves the centre label and nothing else, handed to the engine through
// engine::open_built_source. The samples do not follow the label, and no case
// here asserts on anything a receiver hears. What they assert on is the engine's
// bookkeeping across the move, which is the whole of what a retune changes above
// the device.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/dsp/vrx_reference.h"
#include "core/engine/engine.h"
#include "core/engine/open_built_source.h"
#include "core/engine/probe.h"
#include "core/engine/vrx.h"
#include "core/source/registry.h"
#include "core/source/source.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"

using namespace revenant;

namespace {

// A source whose centre can be moved, and whose samples cannot.
//
// Everything but the centre is the wrapped source's. The capabilities gain one
// tune range, because Engine::source_tuning reads them and a client asks that
// before it retunes; the engine itself calls tune() whatever they say.
class MovableCentre final : public source::Source {
public:
    explicit MovableCentre(std::unique_ptr<source::Source> inner)
        : inner_(std::move(inner)), caps_(inner_->capabilities()), center_(inner_->center()) {
        caps_.tune_ranges = {source::TuneRange{.low = 0, .high = 6'000'000'000, .step = 0}};
    }

    [[nodiscard]] const source::SourceCapabilities& capabilities() const override {
        return caps_;
    }
    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override {
        center_.store(center, std::memory_order_release);
        return center;
    }
    [[nodiscard]] dsp::Hertz center() const override {
        return center_.load(std::memory_order_acquire);
    }
    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override {
        return inner_->set_sample_rate(rate);
    }
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return inner_->sample_rate(); }
    [[nodiscard]] Expected<double> set_gain(std::string_view stage, double db) override {
        return inner_->set_gain(stage, db);
    }
    [[nodiscard]] Status set_gain_auto(std::string_view stage, bool on) override {
        return inner_->set_gain_auto(stage, on);
    }
    [[nodiscard]] Status start(const source::StreamOptions& options,
                               source::BlockSink sink) override {
        return inner_->start(options, std::move(sink));
    }
    [[nodiscard]] Status stop() override { return inner_->stop(); }
    [[nodiscard]] bool running() const override { return inner_->running(); }
    [[nodiscard]] Status seek(dsp::SampleIndex index) override { return inner_->seek(index); }
    [[nodiscard]] source::SourceStats stats() const override { return inner_->stats(); }
    [[nodiscard]] source::ClockQuality clock() const override { return inner_->clock(); }

private:
    std::unique_ptr<source::Source> inner_;
    source::SourceCapabilities caps_;
    std::atomic<dsp::Hertz> center_;
};

// An HF-shaped front end: 2 MS/s over 256 channels, which is what
// source::resolution_for_span asks for below 30 MHz. Channels run at 15625 S/s
// and sit 7812.5 Hz apart, so a 10 kHz AM receiver fits whole only within
// 2812.5 Hz of a channel centre and is narrowed further out.
constexpr dsp::SampleRate kRate = 2'000'000;
constexpr std::uint32_t kChannels = 256;
constexpr dsp::SampleRate kAudioRate = 48'000;
constexpr dsp::Hertz kOpenedAt = 7'100'000;

engine::EngineConfig hf_config(std::uint32_t probes) {
    engine::EngineConfig config;
    config.channels = kChannels;
    config.taps_per_branch = 17;
    config.ring_seconds = 0.5;
    config.audio_rate = kAudioRate;
    config.probe_receivers = probes;
    config.gpu_index = -1;  // honours REVENANT_GPU_INDEX
    return config;
}

Expected<std::unique_ptr<engine::Engine>> open_movable(std::uint32_t probes) {
    auto created = engine::Engine::create(hf_config(probes));
    if (!created) {
        return std::unexpected(created.error());
    }
    auto scene = source::open_source(std::format(
        "synthetic:wideband?rate={}&center={}&emitters=0&samples={}&seed=20260923", kRate,
        kOpenedAt, kRate));
    if (!scene) {
        return std::unexpected(scene.error());
    }
    auto opened = engine::open_built_source(
        **created, std::make_unique<MovableCentre>(std::move(*scene)));
    if (!opened) {
        return std::unexpected(opened.error());
    }
    return std::move(*created);
}

}  // namespace

TEST_CASE("after a retune every receiver is on its own frequency or reported removed",
          "[gpu][engine][retune]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    auto opened = open_movable(0);
    INFO(test::message_of(opened));
    REQUIRE(opened.has_value());
    engine::Engine& eng = **opened;
    REQUIRE(eng.source_tuning().can_retune);
    REQUIRE(eng.info().source_center == kOpenedAt);

    // Offsets that are whole multiples of two channel spacings sit exactly on
    // a channel centre, where a 10 kHz AM receiver is granted all of it. The
    // others are chosen to be narrowed before the move, after it, or both.
    // The last is near the lower edge so the move takes it out of the span,
    // which is the removal that was always reported and is the control.
    struct Placed {
        engine::Demod demod;
        dsp::Hertz offset;
    };
    const std::vector<Placed> wanted = {
        {engine::Demod::Am, 0},          {engine::Demod::Am, 15'625},
        {engine::Demod::Am, -31'250},    {engine::Demod::Am, 3'000},
        {engine::Demod::Am, -2'000},     {engine::Demod::Dsb, 46'875},
        {engine::Demod::Usb, 62'500},    {engine::Demod::Cw, -78'125},
        {engine::Demod::Am, 97'000},     {engine::Demod::Am, -998'000},
    };

    struct Held {
        engine::VrxId id;
        dsp::Hertz frequency;
        engine::VrxParams params;
    };
    std::vector<Held> held;
    for (const Placed& place : wanted) {
        engine::VrxParams params;
        params.center = place.offset;
        params.demod = place.demod;

        // Zero asks for the mode's own channel plan, dsp::default_passband:
        // 10 kHz for AM, 6 kHz for DSB, 300 to 2700 Hz for USB and 500 Hz for
        // CW. VrxParams' own default of 12 kHz is wider than a channel here
        // and would narrow every one of them wherever it sat.
        params.bandwidth = 0;
        params.audio_rate = kAudioRate;
        params.squelch_dbfs = -300.0;
        auto added = eng.add_vrx(params);
        INFO(engine::demod_name(place.demod) << " at " << place.offset << ": "
                                             << test::message_of(added));
        REQUIRE(added.has_value());
        held.push_back(Held{*added, kOpenedAt + place.offset, params});
    }

    // Not a multiple of the channel spacing, so every receiver lands at a
    // different place in its channel than it had.
    constexpr dsp::Hertz kMove = 3'500;
    const dsp::GridParams grid = eng.info().grid;

    // THE PRECONDITION THAT MAKES THIS CASE MEAN ANYTHING: at least one
    // receiver still inside the span needs a different filter after the move.
    // Worked out here with the planner's own arithmetic rather than assumed, so
    // a change to the clamp that made the move harmless would fail this line
    // instead of passing the case below for the wrong reason.
    std::size_t reshaped = 0;
    for (const Held& h : held) {
        engine::VrxParams moved = h.params;
        moved.center = h.params.center - kMove;
        if (moved.center < -kRate / 2) {
            continue;
        }
        auto before = engine::place(grid, kRate, h.params);
        auto after = engine::place(grid, kRate, moved);
        if (!before || !after) {
            continue;
        }
        auto from = dsp::vrx_shape_for(grid, kRate, h.params, *before);
        auto to = dsp::vrx_shape_for(grid, kRate, moved, *after);
        if (from && to && *from != *to) {
            ++reshaped;
            WARN(std::format("{} at {} Hz: granted {} to {} Hz before the move and {} to {} Hz "
                             "after, {} fine taps becoming {}",
                             engine::demod_name(h.params.demod), h.frequency, before->granted_low,
                             before->granted_high, after->granted_low, after->granted_high,
                             from->fine.taps, to->fine.taps));
        }
    }
    INFO("receivers whose filter the move changes: " << reshaped);
    REQUIRE(reshaped > 0);

    auto landed = eng.set_source_center(kOpenedAt + kMove);
    INFO(test::message_of(landed));
    REQUIRE(landed.has_value());
    REQUIRE(landed->center == kOpenedAt + kMove);
    REQUIRE(eng.info().source_center == kOpenedAt + kMove);

    const std::vector<engine::VrxId> alive = eng.vrx_ids();
    for (const engine::RetuneRemoval& gone : landed->removed) {
        WARN(std::format("removed {} at {} Hz: {}", gone.id.value, gone.frequency, gone.reason));
    }

    std::size_t kept = 0;
    for (const Held& h : held) {
        INFO(engine::demod_name(h.params.demod) << " receiver " << h.id.value << " put on "
                                                << h.frequency << " Hz");
        const auto removal =
            std::ranges::find_if(landed->removed, [&](const engine::RetuneRemoval& gone) {
                return gone.id == h.id;
            });
        const bool listed = removal != landed->removed.end();
        const bool running = std::ranges::find(alive, h.id) != alive.end();

        // Exactly one of the two, never both and never neither. Neither is
        // the failure this case was written for: a receiver the engine still
        // runs somewhere nobody put it, with nothing in the answer.
        CHECK(listed != running);

        if (listed) {
            CHECK(removal->frequency == h.frequency);
            CHECK_FALSE(removal->reason.empty());
            continue;
        }
        auto status = eng.vrx_status(h.id);
        INFO(test::message_of(status));
        REQUIRE(status.has_value());
        CHECK(eng.info().source_center + status->params.center == h.frequency);
        ++kept;
    }

    // And the move leaves somebody behind: a case where everything was removed
    // would pass the loop above and prove nothing about the rebase.
    CHECK(kept > 0);

    // The control removal names the span; the rest name the filter, in the
    // graph's own words.
    for (const engine::RetuneRemoval& gone : landed->removed) {
        if (gone.frequency == kOpenedAt - 998'000) {
            CHECK(gone.reason.find("outside the span") != std::string::npos);
        } else {
            CHECK(gone.reason.find("remove and an add") != std::string::npos);
        }
    }
}
