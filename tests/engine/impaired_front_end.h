// A synthetic radio with the three defects calibration exists for.
//
// One carrier at a true radio frequency, received through a front end whose
// crystal is off by a stated number of parts per billion, whose I/Q mixer has
// a stated gain and phase mismatch, and whose output carries a stated DC
// offset, plus a little seeded noise. Every number is the test's, so a
// measurement of the corrected stream can be compared with what was put in
// rather than with what some other code believes.
//
// THE CRYSTAL, modelled the way an RTL-SDR's is: one part drives both the
// tuner's synthesiser and the sample clock. Tuned to `device` hertz, the real
// oscillator sits at device * (1 + e). Samples arrive (1 + e) times faster
// than the nominal rate, so a carrier at true F lands in the stream at
// (F - device * (1 + e)) / (1 + e) cycles per nominal second, which is
// F / (1 + e) - device. That is the number generated below, and it is why an
// uncorrected label reads F / (1 + e).
//
// THE MIXER follows core/dsp/front_end_correction.h's model exactly:
// I_r = I, Q_r = g (Q cos(phi) + I sin(phi)).
//
// Tunable, Demand, cf32, and not seekable. Handed to the engine through
// engine::open_built_source, the way tests/engine/movable_centre.h is. The
// phase runs on from block to block, and a tune takes effect from the next
// block.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <numbers>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/dsp/denormal_mode.h"
#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/source.h"

namespace revenant::test {

struct ImpairedFrontEndConfig {
    dsp::SampleRate rate = 2'400'000;

    // Where the device starts, on its own crystal's scale.
    dsp::Hertz device_center = 162'500'000;

    // Positive when the crystal runs fast, core/source/frequency_correction.h's
    // sign.
    std::int64_t clock_error_ppb = 0;

    // The carrier's true radio frequency and its amplitude, full scale 1.0.
    dsp::Hertz carrier_hz = 162'550'000;
    double carrier_amplitude = 0.1;

    // Added to I and Q after the mixer.
    std::complex<double> dc{0.0, 0.0};

    // The mixer's Q arm, relative to I.
    double gain = 1.0;
    double phase_rad = 0.0;

    // Per component, and seeded.
    double noise_rms = 1.0e-4;
    std::uint64_t seed = 20260923;

    // Zero is unbounded.
    dsp::SampleIndex length = 0;

    std::size_t block_samples = 32'768;

    // What calibration keys by. Empty is a device with none.
    std::string serial;
    bool device_corrects_frequency = false;
};

class ImpairedFrontEnd final : public source::Source {
public:
    explicit ImpairedFrontEnd(ImpairedFrontEndConfig config)
        : config_(std::move(config)), device_center_(config_.device_center) {
        caps_.uri = std::format("impaired:front-end?serial={}", config_.serial);
        caps_.backend = "impaired";
        caps_.display_name = "impaired synthetic front end";
        caps_.serial = config_.serial;
        caps_.device_corrects_frequency = config_.device_corrects_frequency;
        caps_.tune_ranges = {source::TuneRange{.low = 24'000'000, .high = 1'766'000'000, .step = 0}};
        caps_.min_rate = config_.rate;
        caps_.max_rate = config_.rate;
        caps_.native_format = source::SampleFormat::Cf32;
        caps_.bits_per_component = 32;
        caps_.flow = source::FlowControl::Demand;
        caps_.seekable = false;
        caps_.length_samples = config_.length;
        caps_.preferred_block_samples = config_.block_samples;
    }

    ~ImpairedFrontEnd() override {
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    [[nodiscard]] const source::SourceCapabilities& capabilities() const override { return caps_; }

    [[nodiscard]] Expected<dsp::Hertz> tune(dsp::Hertz center) override {
        if (!caps_.can_tune(center)) {
            return fail(std::format("the impaired front end cannot tune {} Hz", center));
        }
        tunes_.fetch_add(1, std::memory_order_relaxed);
        device_center_.store(center, std::memory_order_release);
        return center;
    }

    [[nodiscard]] dsp::Hertz center() const override {
        return device_center_.load(std::memory_order_acquire);
    }

    // How many times tune() was called, so a case can tell a label change from
    // a retune.
    [[nodiscard]] std::uint64_t tunes() const { return tunes_.load(std::memory_order_relaxed); }

    [[nodiscard]] Expected<dsp::SampleRate> set_sample_rate(dsp::SampleRate rate) override {
        if (rate != config_.rate) {
            return fail("the impaired front end runs at one rate");
        }
        return rate;
    }
    [[nodiscard]] dsp::SampleRate sample_rate() const override { return config_.rate; }
    [[nodiscard]] Expected<double> set_gain(std::string_view, double) override {
        return fail("the impaired front end has no gain stage");
    }
    [[nodiscard]] Status set_gain_auto(std::string_view, bool) override {
        return fail("the impaired front end has no gain stage");
    }

    [[nodiscard]] Status start(const source::StreamOptions& options,
                               source::BlockSink sink) override {
        const std::scoped_lock lock(control_);
        if (thread_.joinable()) {
            return fail("the impaired front end is already streaming");
        }
        block_ = options.block_samples == 0 ? config_.block_samples : options.block_samples;
        sink_ = std::move(sink);
        stop_requested_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        try {
            thread_ = std::thread([this] { run(); });
        } catch (const std::system_error& error) {
            running_.store(false, std::memory_order_release);
            return fail(error.what());
        }
        return {};
    }

    [[nodiscard]] Status stop() override {
        const std::scoped_lock lock(control_);
        stop_requested_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (has_error_) {
            return std::unexpected(error_);
        }
        return {};
    }

    [[nodiscard]] bool running() const override {
        return running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] Status seek(dsp::SampleIndex) override {
        return fail("the impaired front end does not seek");
    }
    [[nodiscard]] source::SourceStats stats() const override {
        source::SourceStats out;
        out.blocks_delivered = blocks_.load(std::memory_order_relaxed);
        out.samples_delivered = position_.load(std::memory_order_relaxed);
        out.write_index = out.samples_delivered;
        return out;
    }
    [[nodiscard]] source::ClockQuality clock() const override { return {}; }

    // The carrier's frequency in the stream, in cycles per nominal second,
    // with the device at `device_hz`. Public so a case can state the
    // uncorrected label it expects.
    [[nodiscard]] static double baseband_hz(const ImpairedFrontEndConfig& config,
                                            dsp::Hertz device_hz) {
        const double scale = 1.0 + static_cast<double>(config.clock_error_ppb) * 1.0e-9;
        return static_cast<double>(config.carrier_hz) / scale - static_cast<double>(device_hz);
    }

private:
    void run() {
        const dsp::ScopedDenormalFlush flush;
        std::mt19937_64 random(config_.seed);
        std::normal_distribution<double> noise(0.0, config_.noise_rms);
        std::vector<dsp::Complex32> buffer(block_);
        double phase = 0.0;
        std::uint64_t sequence = 0;
        const double cos_phi = std::cos(config_.phase_rad);
        const double sin_phi = std::sin(config_.phase_rad);

        while (!stop_requested_.load(std::memory_order_acquire)) {
            const dsp::SampleIndex at = position_.load(std::memory_order_relaxed);
            std::size_t want = block_;
            if (config_.length > 0) {
                if (at >= config_.length) {
                    break;
                }
                want = static_cast<std::size_t>(
                    std::min<dsp::SampleIndex>(want, config_.length - at));
            }

            const double step = 2.0 * std::numbers::pi *
                                baseband_hz(config_, device_center_.load(std::memory_order_acquire)) /
                                static_cast<double>(config_.rate);
            for (std::size_t n = 0; n < want; ++n) {
                const double i = config_.carrier_amplitude * std::cos(phase);
                const double q = config_.carrier_amplitude * std::sin(phase);
                const double i_r = i + config_.dc.real() + noise(random);
                const double q_r =
                    config_.gain * (q * cos_phi + i * sin_phi) + config_.dc.imag() + noise(random);
                buffer[n] = dsp::Complex32{static_cast<float>(i_r), static_cast<float>(q_r)};
                phase += step;
                if (phase > std::numbers::pi) {
                    phase -= 2.0 * std::numbers::pi;
                } else if (phase < -std::numbers::pi) {
                    phase += 2.0 * std::numbers::pi;
                }
            }

            source::SourceBlock block;
            block.stamp = dsp::BlockTimestamp{at, 0, config_.rate};
            block.format = source::SampleFormat::Cf32;
            block.sample_count = want;
            block.bytes = std::as_bytes(std::span<const dsp::Complex32>(buffer.data(), want));
            block.sequence = sequence++;
            if (Status delivered = sink_(block); !delivered) {
                error_ = delivered.error();
                has_error_ = true;
                break;
            }
            position_.store(at + want, std::memory_order_relaxed);
            blocks_.fetch_add(1, std::memory_order_relaxed);
        }
        running_.store(false, std::memory_order_release);
    }

    ImpairedFrontEndConfig config_;
    source::SourceCapabilities caps_;
    std::atomic<dsp::Hertz> device_center_;
    std::atomic<std::uint64_t> tunes_{0};

    std::mutex control_;
    std::thread thread_;
    source::BlockSink sink_;
    std::size_t block_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<dsp::SampleIndex> position_{0};
    std::atomic<std::uint64_t> blocks_{0};
    Error error_{};
    bool has_error_ = false;
};

}  // namespace revenant::test
