// A real engine whose front end can be retuned without a radio.
//
// WHY THIS EXISTS. Every source in this tree that can be opened without
// hardware refuses a retune: a file's centre is a property of bytes on disk,
// and a synthetic scene's is a label on emitters generated around baseband DC.
// The one backend that retunes is the RTL-SDR, and a wire case that needs a
// dongle does not run in CI, or on a machine where another process holds it.
// So what Session.setSourceCenter does AFTER the engine answers, which is the
// server's half, had no case at all.
//
// This forwards every call to a real engine over a real synthetic scene and
// stands in for exactly one of them. set_source_center does what
// Engine::set_source_center does to the receivers, rebasing each one's offset
// to hold its absolute frequency and removing one whose centre falls strictly
// outside the new span, and moves info().source_center. It does not move the
// samples, which still come from the scene at its original centre; no case
// that uses this asserts on what a receiver hears after the retune, only on
// whether it is still being heard.
//
// The engine's own rebase and removal are refereed against a dongle in
// tests/engine/test_engine.cpp. What a case built on this pins is everything
// the server does with the answer.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"

namespace revenant::test {

class RetunableEngine final : public engine::Engine {
public:
    explicit RetunableEngine(std::unique_ptr<engine::Engine> inner) : inner_(std::move(inner)) {}

    [[nodiscard]] Status open_source(std::string_view uri) override {
        retuned_.store(false, std::memory_order_release);
        return inner_->open_source(uri);
    }
    [[nodiscard]] Status close_source() override {
        retuned_.store(false, std::memory_order_release);
        auto closed = inner_->close_source();

        // The fan-outs attach_audio_sink installed through THIS object live in
        // this object's own map, not the inner engine's, so the inner engine
        // dropping its own leaves these behind.
        drop_audio_fanouts();
        return closed;
    }
    [[nodiscard]] bool has_source() const override { return inner_->has_source(); }
    [[nodiscard]] const source::SourceCapabilities& source_capabilities() const override {
        return inner_->source_capabilities();
    }

    // The inner engine's until the first retune, and a copy with the centre
    // moved after it. Written only by set_source_center, on the server's
    // event loop thread, which is the same single writer the real engine's
    // EngineInfo has.
    [[nodiscard]] const engine::EngineInfo& info() const override {
        return retuned_.load(std::memory_order_acquire) ? info_ : inner_->info();
    }

    [[nodiscard]] engine::SourceTuning source_tuning() const override {
        return engine::SourceTuning{.can_retune = true, .low = 0, .high = 6'000'000'000};
    }

    [[nodiscard]] Expected<engine::SourceRetune> set_source_center(dsp::Hertz center) override {
        if (!inner_->has_source()) {
            return fail("RetunableEngine::set_source_center before a source is open");
        }
        if (!retuned_.load(std::memory_order_acquire)) {
            info_ = inner_->info();
            retuned_.store(true, std::memory_order_release);
        }

        const dsp::Hertz was = info_.source_center;
        const dsp::Hertz moved = center - was;
        const dsp::Hertz half_span = static_cast<dsp::Hertz>(info_.source_rate / 2);
        info_.source_center = center;

        engine::SourceRetune out;
        out.center = center;
        for (const engine::VrxId id : inner_->vrx_ids()) {
            auto status = inner_->vrx_status(id);
            if (!status) {
                continue;
            }
            engine::VrxParams repinned = status->params;
            repinned.center = status->params.center - moved;

            const bool reachable = repinned.center >= -half_span && repinned.center <= half_span;
            if (!reachable || !inner_->set_vrx_params(id, repinned)) {
                // A refused set_vrx_params is the graph's shape refusal here,
                // the only refusal the inner engine gives a receiver it holds
                // at an offset inside the span. No sentence, so the server
                // composes its own, which names both frequencies and is what
                // the cases built on this assert on.
                const engine::RetuneCause cause = reachable ? engine::RetuneCause::ShapeChanged
                                                            : engine::RetuneCause::OutsideSpan;
                if (inner_->remove_vrx(id)) {
                    out.removed.push_back(engine::RetuneRemoval{
                        .id = id, .cause = cause, .frequency = was + status->params.center});
                }
            }
        }
        return out;
    }

    [[nodiscard]] Expected<double> set_source_gain(std::string_view stage, double db) override {
        return inner_->set_source_gain(stage, db);
    }
    [[nodiscard]] Status set_source_gain_auto(std::string_view stage, bool on) override {
        return inner_->set_source_gain_auto(stage, on);
    }
    [[nodiscard]] engine::SourcePacing source_pacing() const override {
        return inner_->source_pacing();
    }

    [[nodiscard]] Expected<engine::VrxId> add_vrx(const engine::VrxParams& params) override {
        return inner_->add_vrx(params);
    }
    [[nodiscard]] Status remove_vrx(engine::VrxId id) override { return inner_->remove_vrx(id); }
    [[nodiscard]] Status set_vrx_params(engine::VrxId id,
                                        const engine::VrxParams& params) override {
        return inner_->set_vrx_params(id, params);
    }
    [[nodiscard]] Expected<engine::VrxStatus> vrx_status(engine::VrxId id) const override {
        return inner_->vrx_status(id);
    }
    [[nodiscard]] std::vector<engine::VrxId> vrx_ids() const override {
        return inner_->vrx_ids();
    }

    [[nodiscard]] Status set_audio_sink(engine::VrxId id, engine::AudioSink sink) override {
        return inner_->set_audio_sink(id, std::move(sink));
    }
    [[nodiscard]] Status set_spectrum_sink(engine::SpectrumSink sink) override {
        return inner_->set_spectrum_sink(std::move(sink));
    }
    [[nodiscard]] Status set_passband_sink(engine::VrxId id, engine::PassbandSink sink) override {
        return inner_->set_passband_sink(id, std::move(sink));
    }

    [[nodiscard]] Status run() override { return inner_->run(); }
    [[nodiscard]] Status stop() override { return inner_->stop(); }
    [[nodiscard]] bool running() const override { return inner_->running(); }
    [[nodiscard]] source::SourceStats source_stats() const override {
        return inner_->source_stats();
    }
    [[nodiscard]] engine::GraphConditions graph_conditions() const override {
        return inner_->graph_conditions();
    }

private:
    std::unique_ptr<engine::Engine> inner_;
    std::atomic<bool> retuned_{false};
    engine::EngineInfo info_;
};

}  // namespace revenant::test
