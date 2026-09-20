#include "scene_frames.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <memory>
#include <span>
#include <utility>

#include "core/dsp/pfb_branch_reference.h"
#include "core/dsp/pfb_fft_reference.h"
#include "core/dsp/spectrum_reference.h"

namespace revenant::test {

namespace {

// Frames produced per pass of the channelizer. The branch filter costs the
// same per input sample whatever the dispatch length, but its history has to
// be re-walked at every call boundary, and the coarse FFT and the ring
// bookkeeping amortise. Sixteen keeps the intermediate buffers inside a few
// megabytes at the default geometry.
constexpr std::uint32_t kFramesPerBatch = 16;

[[nodiscard]] std::uint32_t round_up_pow2(std::uint64_t value) {
    return static_cast<std::uint32_t>(std::bit_ceil(std::max<std::uint64_t>(2, value)));
}

}  // namespace

dsp::GridParams SceneGeometry::grid() const {
    dsp::GridParams params;
    params.channels = channels;
    params.taps_per_branch = 17;
    // 2x oversampled, which is what the whole project uses. A critically
    // sampled bank splits a signal on a channel edge across two channels.
    params.decimation = channels / 2;
    return params;
}

std::size_t SceneGeometry::bins() const {
    return static_cast<std::size_t>(dsp::spectrum_bin_count(channels, transform));
}

double SceneGeometry::bin_width_hz() const {
    return static_cast<double>(rate) /
           (static_cast<double>(grid().decimation) * static_cast<double>(transform));
}

std::uint64_t SceneGeometry::frame_step() const {
    return static_cast<std::uint64_t>(transform) * grid().decimation;
}

engine::SpectrumGeometry SceneGeometry::spectrum() const {
    const dsp::GridParams params = grid();
    engine::SpectrumGeometry geometry;
    geometry.transform = transform;
    geometry.bins_per_channel = dsp::spectrum_bins_per_channel(transform);
    geometry.channels = channels;
    geometry.bins = dsp::spectrum_bin_count(channels, transform);
    geometry.bin_width_numerator = rate;
    geometry.bin_width_denominator =
        static_cast<std::int64_t>(params.decimation) * static_cast<std::int64_t>(transform);
    geometry.bin_zero_numerator = -rate * (static_cast<std::int64_t>(channels) + 1);
    geometry.bin_zero_denominator = 2 * static_cast<std::int64_t>(channels);
    return geometry;
}

Expected<SceneFrames> SceneFrames::create(const SceneGeometry& geometry,
                                          const siggen::SceneSpec& spec) {
    if (spec.rate != geometry.rate) {
        return fail(std::format("SceneFrames: the scene runs at {} S/s and the grid was asked "
                                "for {} S/s. The two have to agree or every frequency in the "
                                "truth record lands on the wrong bin",
                                spec.rate, geometry.rate));
    }

    const dsp::GridParams grid = geometry.grid();
    if (auto ok = dsp::validate(grid); !ok) {
        return std::unexpected(ok.error());
    }

    auto prototype = dsp::design_prototype(grid, 120.0);
    if (!prototype) {
        return std::unexpected(prototype.error());
    }
    auto coarse = dsp::build_twiddles(grid.channels);
    if (!coarse) {
        return std::unexpected(coarse.error());
    }
    auto fine = dsp::build_twiddles(geometry.transform);
    if (!fine) {
        return std::unexpected(fine.error());
    }
    auto window = dsp::build_spectrum_window(geometry.transform, grid.taps_per_branch, 120.0);
    if (!window) {
        return std::unexpected(window.error());
    }

    auto scene = siggen::Scene::create(spec);
    if (!scene) {
        return std::unexpected(scene.error());
    }

    SceneFrames frames;
    frames.geometry_ = geometry;
    frames.spectrum_ = geometry.spectrum();
    frames.scene_ = std::make_shared<siggen::Scene>(std::move(*scene));
    frames.prototype_ = std::move(prototype->taps);
    frames.coarse_twiddles_ = std::move(*coarse);
    frames.fine_twiddles_ = std::move(*fine);
    frames.window_ = std::move(*window);
    frames.frames_per_batch_ = kFramesPerBatch;

    const std::uint32_t blocks = geometry.transform * kFramesPerBatch;
    const std::uint64_t batch_samples =
        static_cast<std::uint64_t>(blocks) * grid.decimation;

    // Twice the batch so the previous batch's tail is still in the ring when
    // the branch filter walks back over its own history. The filter needs
    // prototype_length() samples of it, which is three orders of magnitude
    // less than a batch, so the factor of two is about keeping the wrap
    // arithmetic trivial rather than about capacity.
    const std::uint32_t capacity = round_up_pow2(batch_samples * 2);

    frames.iq_.assign(capacity, dsp::Complex32{});
    frames.branches_.assign(static_cast<std::size_t>(blocks) * grid.channels, dsp::Complex32{});
    frames.channel_ring_.assign(static_cast<std::size_t>(grid.channels) * blocks,
                                dsp::Complex32{});
    frames.power_db_.assign(geometry.bins(), 0.0F);

    // Forces the first next() to render rather than serve a stale batch.
    frames.frame_in_batch_ = kFramesPerBatch;
    return frames;
}

std::uint64_t SceneFrames::frames_available() const {
    const dsp::SampleIndex duration = scene_->duration_samples();
    if (duration == 0) {
        return 0;
    }
    return duration / geometry_.frame_step();
}

Status SceneFrames::fill_batch() {
    const dsp::GridParams grid = geometry_.grid();
    const std::uint32_t blocks = geometry_.transform * frames_per_batch_;
    const std::uint64_t batch_samples =
        static_cast<std::uint64_t>(blocks) * grid.decimation;
    const std::uint32_t mask = static_cast<std::uint32_t>(iq_.size() - 1);

    // The ring is written exactly as the engine writes it: absolute sample
    // index reduced by the mask, in at most two pieces. Rendering a scene into
    // a ring rather than a flat buffer is what keeps the filter's history
    // across a batch boundary, and the history is the reason a burst's leading
    // edge lands where the truth record says it does.
    const std::uint32_t offset = static_cast<std::uint32_t>(next_sample_ & mask);
    const std::uint64_t first = std::min<std::uint64_t>(batch_samples, iq_.size() - offset);
    scene_->render(next_sample_, dsp::ComplexSpan(iq_.data() + offset,
                                                  static_cast<std::size_t>(first)));
    if (first < batch_samples) {
        scene_->render(next_sample_ + first,
                       dsp::ComplexSpan(iq_.data(), static_cast<std::size_t>(batch_samples - first)));
    }

    const dsp::PfbBranchParams branch_params{
        .ring_mask = mask,
        .base_offset = offset,
        .block_count = blocks,
    };
    if (auto ok = dsp::reference_pfb_branches(grid, prototype_, iq_, branch_params, branches_);
        !ok) {
        return ok;
    }

    dsp::PfbFftParams fft_params;
    fft_params.channels = grid.channels;
    fft_params.decimation = grid.decimation;
    fft_params.stages = dsp::fft_stages(grid.channels);
    fft_params.block_base = static_cast<std::uint32_t>(next_sample_ / grid.decimation);
    fft_params.block_count = blocks;
    fft_params.out_ring_blocks = blocks;
    fft_params.out_ring_mask = blocks - 1U;
    if (auto ok = dsp::reference_pfb_fft(fft_params, branches_, coarse_twiddles_, channel_ring_);
        !ok) {
        return ok;
    }

    frame_start_ = next_sample_;
    next_sample_ += batch_samples;
    frame_in_batch_ = 0;
    return {};
}

Expected<engine::SpectrumFrame> SceneFrames::next() {
    if (frame_in_batch_ >= frames_per_batch_) {
        if (auto ok = fill_batch(); !ok) {
            return std::unexpected(ok.error());
        }
    }

    const dsp::GridParams grid = geometry_.grid();
    const std::uint32_t blocks = geometry_.transform * frames_per_batch_;

    dsp::SpectrumParams params;
    params.channels = grid.channels;
    params.transform = geometry_.transform;
    params.stages = dsp::fft_stages(geometry_.transform);
    params.chan_blocks = blocks;
    params.chan_mask = blocks - 1U;
    params.in_offset = frame_in_batch_ * geometry_.transform;

    if (auto ok = dsp::reference_spectrum(params, channel_ring_, fine_twiddles_, window_,
                                          power_db_);
        !ok) {
        return std::unexpected(ok.error());
    }

    engine::SpectrumFrame frame;
    frame.power_db = power_db_;
    frame.geometry = spectrum_;
    frame.start = frame_start_ +
                  static_cast<dsp::SampleIndex>(frame_in_batch_) * geometry_.frame_step();
    frame.count = geometry_.frame_step();
    frame.sequence = sequence_++;
    ++frame_in_batch_;
    return frame;
}

SceneScorer::SceneScorer(const siggen::Scene& scene, dsp::Hertz source_center) {
    emitters_.reserve(scene.truth().size());
    for (const siggen::EmitterTruth& truth : scene.truth()) {
        Live live;
        live.score.emitter_id = truth.id;
        live.score.modulation = truth.modulation;
        live.score.truth_low_hz = source_center + truth.extent.low_hz;
        live.score.truth_high_hz = source_center + truth.extent.high_hz;
        live.score.truth_bandwidth_hz = truth.extent.bandwidth_hz();
        live.score.truth_snr_occupied_db = truth.snr_in_occupied_bandwidth_db;
        live.score.truth_start = truth.start_sample;
        live.score.truth_end = truth.end_sample;
        emitters_.push_back(std::move(live));
    }
}

void SceneScorer::observe(dsp::SampleIndex now, std::span<const detect::Track> tracks) {
    for (Live& live : emitters_) {
        const bool on = now > live.score.truth_start &&
                        (live.score.truth_end == siggen::kAlwaysOn || now <= live.score.truth_end);
        if (!on) {
            // A run has to be consecutive within the transmission. Clearing
            // here rather than carrying the id across the gap is what makes
            // the lifetime figure answer "did one track last the burst"
            // instead of "did an id ever come back".
            live.run_id = 0;
            live.run_length = 0;
            live.have_previous = false;
            continue;
        }
        ++live.score.decisions_on;

        const auto low = static_cast<double>(live.score.truth_low_hz);
        const auto high = static_cast<double>(live.score.truth_high_hz);
        const double width = std::max(1.0, high - low);

        std::size_t here = 0;
        const detect::Track* best_now = nullptr;
        double best_now_overlap = 0.0;

        for (const detect::Track& track : tracks) {
            const double half = 0.5 * static_cast<double>(std::max<dsp::Hertz>(1, track.bandwidth));
            const double track_low = static_cast<double>(track.center) - half;
            const double track_high = static_cast<double>(track.center) + half;
            const double overlap = std::min(high, track_high) - std::max(low, track_low);
            if (overlap <= 0.0) {
                continue;
            }
            ++here;

            if (std::find(live.seen_ids.begin(), live.seen_ids.end(), track.id) ==
                live.seen_ids.end()) {
                live.seen_ids.push_back(track.id);
            }
            if (overlap > best_now_overlap) {
                best_now_overlap = overlap;
                best_now = &track;
            }

            // Coverage over the truth extent, and spill outside it. Both are
            // needed: a track covering the whole emitter and half the band
            // either side of it is not the answer, and coverage alone cannot
            // say so.
            const double coverage = overlap / width;
            if (coverage > live.score.best_coverage) {
                live.score.best_coverage = coverage;
                live.score.best_track = track.id;
                live.score.best_spill =
                    std::max(0.0, (track_high - track_low) - overlap) / width;
                live.score.best_centre_error_hz =
                    track.center - (live.score.truth_low_hz + live.score.truth_high_hz) / 2;
                live.score.best_bandwidth_hz = track.bandwidth;
            }
        }

        live.score.peak_simultaneous = std::max(live.score.peak_simultaneous, here);
        if (here > 0) {
            ++live.score.decisions_detected;
        }

        const std::uint64_t id = best_now != nullptr ? best_now->id : 0;
        if (id != 0 && id == live.run_id) {
            ++live.run_length;
        } else {
            live.run_id = id;
            live.run_length = id != 0 ? 1 : 0;
        }
        live.best_run = std::max(live.best_run, live.run_length);

        if (best_now == nullptr) {
            live.have_previous = false;
            continue;
        }

        const double best_half =
            0.5 * static_cast<double>(std::max<dsp::Hertz>(1, best_now->bandwidth));
        const double best_low = static_cast<double>(best_now->center) - best_half;
        const double best_high = static_cast<double>(best_now->center) + best_half;

        if (live.have_previous) {
            const double centre_step = std::abs((best_low + best_high) * 0.5 -
                                                (live.previous_low + live.previous_high) * 0.5);
            const double width_step =
                std::abs((best_high - best_low) - (live.previous_high - live.previous_low));
            live.step_sum += centre_step;
            live.width_step_sum += width_step;
            ++live.steps;
            live.score.centre_step_max_hz = std::max(live.score.centre_step_max_hz, centre_step);

            // The same quantity Detector::update_tracks gates association on:
            // overlap over the narrower of the two bands.
            const double overlap =
                std::min(best_high, live.previous_high) - std::max(best_low, live.previous_low);
            const double narrower =
                std::max(1.0, std::min(best_high - best_low, live.previous_high - live.previous_low));
            live.score.worst_self_overlap =
                std::min(live.score.worst_self_overlap, std::max(0.0, overlap) / narrower);
        }
        live.previous_low = best_low;
        live.previous_high = best_high;
        live.have_previous = true;
    }
}

std::vector<EmitterScore> SceneScorer::finish() const {
    std::vector<EmitterScore> out;
    out.reserve(emitters_.size());
    for (const Live& live : emitters_) {
        EmitterScore score = live.score;
        score.track_ids = live.seen_ids.size();
        score.best_lifetime_fraction =
            score.decisions_on > 0
                ? static_cast<double>(live.best_run) / static_cast<double>(score.decisions_on)
                : 0.0;
        if (live.steps > 0) {
            score.centre_step_mean_hz = live.step_sum / static_cast<double>(live.steps);
            score.bandwidth_step_mean_hz = live.width_step_sum / static_cast<double>(live.steps);
        } else {
            score.worst_self_overlap = 0.0;
        }
        out.push_back(score);
    }
    return out;
}

}  // namespace revenant::test
