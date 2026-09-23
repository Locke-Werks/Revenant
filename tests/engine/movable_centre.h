// A source whose centre can be moved, and whose samples cannot.
//
// Handed to engine::open_built_source so a real engine can be retuned without
// a radio. tests/engine/test_engine_retune.cpp says why the file and synthetic
// backends refuse a retune and why neither should be weakened to allow one;
// tests/rpc/rpc_fixture.cpp serves an engine opened on one of these, so the
// wire carries the engine's own retune answer rather than an imitation of it.
//
// Everything but the centre is the wrapped source's. The capabilities gain one
// tune range, because Engine::source_tuning reads them and a client asks that
// before it retunes; the engine itself calls tune() whatever they say.

#pragma once

#include <atomic>
#include <memory>
#include <string_view>
#include <utility>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/source.h"

namespace revenant::test {

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

}  // namespace revenant::test
