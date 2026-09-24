#include "scene_frames.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <memory>
#include <numbers>
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

double FrontEndModel::gain_at(dsp::SampleIndex index, dsp::SampleRate rate) const {
    if (swing_db == 0.0 || swing_period_seconds <= 0.0 || rate == 0) {
        return gain;
    }

    // Reduced into one period before the multiply, so a long scene does not
    // lose the phase into the exponent of a double. The index is absolute,
    // which is what keeps a batch boundary invisible.
    const double period_samples = swing_period_seconds * static_cast<double>(rate);
    const double phase = std::fmod(static_cast<double>(index), period_samples);
    const double turns = phase / period_samples;

    // Half the swing either side of the stated gain, so the mean of the
    // sweep in decibels is the stated gain rather than something above it.
    const double db = 0.5 * swing_db * std::sin(2.0 * std::numbers::pi * turns);
    return gain * std::pow(10.0, db / 20.0);
}

void FrontEndModel::apply(dsp::SampleIndex start, dsp::SampleRate rate,
                          dsp::ComplexSpan out) const {
    if (!active()) {
        return;
    }

    for (std::size_t i = 0; i < out.size(); ++i) {
        const double g = gain_at(start + i, rate);
        const double re = static_cast<double>(out[i].real()) * g;
        const double im = static_cast<double>(out[i].imag()) * g;

        // y = x' - a3 |x'|^2 x', x' being the gained input. Compressive for
        // a positive a3, which is the sign every real amplifier has: the
        // third-order term opposes the linear one, which is what makes the
        // transfer curve bend over rather than run away.
        const double squared = re * re + im * im;
        const double scale = 1.0 - third_order * squared;
        out[i] = dsp::Complex32(static_cast<float>(re * scale),
                                static_cast<float>(im * scale));
    }
}

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
    const std::uint32_t hop = hop_blocks != 0 ? hop_blocks : transform;
    return static_cast<std::uint64_t>(hop) * grid().decimation;
}

std::uint64_t SceneGeometry::window_samples() const {
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
                                          const siggen::SceneSpec& spec,
                                          const FrontEndModel& front_end) {
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
    frames.front_end_ = front_end;
    frames.scene_ = std::make_shared<siggen::Scene>(std::move(*scene));
    frames.prototype_ = std::move(prototype->taps);
    frames.coarse_twiddles_ = std::move(*coarse);
    frames.fine_twiddles_ = std::move(*fine);
    frames.window_ = std::move(*window);

    const std::uint32_t blocks = geometry.transform * kFramesPerBatch;
    const std::uint64_t batch_samples =
        static_cast<std::uint64_t>(blocks) * grid.decimation;
    frames.batch_blocks_ = blocks;

    // Twice the batch so the previous batch's tail is still in the ring when
    // the branch filter walks back over its own history. The filter needs
    // prototype_length() samples of it, which is three orders of magnitude
    // less than a batch, so the factor of two is about keeping the wrap
    // arithmetic trivial rather than about capacity.
    const std::uint32_t capacity = round_up_pow2(batch_samples * 2);

    // Twice the batch here too, for the window rather than the filter: next()
    // renders only until the window it wants is whole, so that window can
    // begin up to a transform before the batch just rendered, and a ring of
    // one batch would already have overwritten its start.
    frames.channel_ring_blocks_ = blocks * 2;

    frames.iq_.assign(capacity, dsp::Complex32{});
    frames.branches_.assign(static_cast<std::size_t>(blocks) * grid.channels, dsp::Complex32{});
    frames.channel_ring_.assign(
        static_cast<std::size_t>(grid.channels) * frames.channel_ring_blocks_, dsp::Complex32{});
    frames.power_db_.assign(geometry.bins(), 0.0F);
    return frames;
}

std::uint64_t SceneFrames::frames_available() const {
    const dsp::SampleIndex duration = scene_->duration_samples();
    const std::uint64_t window = geometry_.window_samples();
    if (duration == 0 || duration < window) {
        return 0;
    }
    // Every frame whose whole window fits. With contiguous windows this is
    // duration / window, which is what it said before a hop existed.
    return (duration - window) / geometry_.frame_step() + 1;
}

Status SceneFrames::fill_batch() {
    const dsp::GridParams grid = geometry_.grid();
    const std::uint32_t blocks = batch_blocks_;
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
    const dsp::ComplexSpan head(iq_.data() + offset, static_cast<std::size_t>(first));
    scene_->render(next_sample_, head);

    // The front end sits between the scene and the channelizer, which is
    // where it sits in a radio: everything below this line, the branch
    // filter and the transform and the detector, is the same code the engine
    // runs and does not know the samples came through one. Applied piecewise
    // over the ring's two halves because the model is memoryless, so a split
    // costs nothing and needs no state carried across it.
    front_end_.apply(next_sample_, geometry_.rate, head);

    if (first < batch_samples) {
        const dsp::ComplexSpan tail(iq_.data(),
                                    static_cast<std::size_t>(batch_samples - first));
        scene_->render(next_sample_ + first, tail);
        front_end_.apply(next_sample_ + first, geometry_.rate, tail);
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
    fft_params.out_ring_blocks = channel_ring_blocks_;
    fft_params.out_ring_mask = channel_ring_blocks_ - 1U;
    if (auto ok = dsp::reference_pfb_fft(fft_params, branches_, coarse_twiddles_, channel_ring_);
        !ok) {
        return ok;
    }

    next_sample_ += batch_samples;
    blocks_rendered_ += blocks;
    return {};
}

Expected<engine::SpectrumFrame> SceneFrames::next() {
    // The frame's window, in coarse-channel blocks from the start of the
    // scene, which is where the engine's own spectrum stage takes it from: the
    // last N channel samples at the end of each dispatch, one dispatch every
    // hop.
    const std::uint32_t hop = geometry_.hop_blocks != 0 ? geometry_.hop_blocks
                                                        : geometry_.transform;
    const std::uint64_t first_block = sequence_ * hop;
    while (blocks_rendered_ < first_block + geometry_.transform) {
        if (auto ok = fill_batch(); !ok) {
            return std::unexpected(ok.error());
        }
    }

    const dsp::GridParams grid = geometry_.grid();

    dsp::SpectrumParams params;
    params.channels = grid.channels;
    params.transform = geometry_.transform;
    params.stages = dsp::fft_stages(geometry_.transform);
    params.chan_blocks = channel_ring_blocks_;
    params.chan_mask = channel_ring_blocks_ - 1U;
    params.in_offset = static_cast<std::uint32_t>(first_block & (channel_ring_blocks_ - 1U));

    if (auto ok = dsp::reference_spectrum(params, channel_ring_, fine_twiddles_, window_,
                                          power_db_);
        !ok) {
        return std::unexpected(ok.error());
    }

    engine::SpectrumFrame frame;
    frame.power_db = power_db_;
    frame.geometry = spectrum_;
    frame.start = static_cast<dsp::SampleIndex>(first_block) * grid.decimation;
    frame.count = geometry_.window_samples();
    frame.sequence = sequence_++;
    return frame;
}

SceneScorer::SceneScorer(const siggen::Scene& scene, dsp::Hertz source_center,
                         double post_window_seconds) {
    source_rate_ = static_cast<double>(std::max<dsp::SampleRate>(1, scene.rate()));
    post_window_samples_ =
        static_cast<dsp::SampleIndex>(std::llround(std::max(0.0, post_window_seconds) *
                                                   source_rate_));

    emitters_.reserve(scene.truth().size());
    for (const siggen::EmitterTruth& truth : scene.truth()) {
        Live live;
        live.score.post_window_seconds =
            truth.end_sample == siggen::kAlwaysOn ? 0.0 : std::max(0.0, post_window_seconds);
        live.score.emitter_id = truth.id;
        live.score.kind = truth.kind;
        live.score.modulation = truth.readable_modulation();
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

            const bool after = post_window_samples_ > 0 &&
                               live.score.truth_end != siggen::kAlwaysOn &&
                               now > live.score.truth_end &&
                               now <= live.score.truth_end + post_window_samples_;
            if (after) {
                observe_after(live, now, tracks);
            }
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

        // What the post-stop window follows, overwritten every decision so
        // that what survives the loop is the last reading taken while the
        // emitter was transmitting. Best by overlap rather than by coverage,
        // because coverage keeps the best of the whole burst and this has to
        // be the most recent one.
        if (best_now != nullptr) {
            live.carried_id = best_now->id;
            live.score.on_centre_hz = best_now->center;
            live.score.on_bandwidth_hz = std::max<dsp::Hertz>(1, best_now->bandwidth);
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

void SceneScorer::observe_after(Live& live, dsp::SampleIndex now,
                                std::span<const detect::Track> tracks) const {
    EmitterScore& score = live.score;
    ++score.after_decisions;

    const dsp::SampleIndex previous = std::max(live.after_previous, score.truth_end);
    const double elapsed = static_cast<double>(now - previous) / source_rate_;
    live.after_previous = now;

    const auto low = static_cast<double>(score.truth_low_hz);
    const auto high = static_cast<double>(score.truth_high_hz);

    const detect::Track* carried = nullptr;
    for (const detect::Track& track : tracks) {
        if (live.carried_id != 0 && track.id == live.carried_id) {
            carried = &track;
            continue;
        }

        // Anything else in the band is a second row for a signal that has
        // stopped, whether it grew out of the residual or drifted in.
        const double half = 0.5 * static_cast<double>(std::max<dsp::Hertz>(1, track.bandwidth));
        const double track_low = static_cast<double>(track.center) - half;
        const double track_high = static_cast<double>(track.center) + half;
        if (std::min(high, track_high) - std::max(low, track_low) <= 0.0) {
            continue;
        }
        if (std::find(live.after_ids.begin(), live.after_ids.end(), track.id) ==
            live.after_ids.end()) {
            live.after_ids.push_back(track.id);
        }
    }

    // Gone. Left false so that the flag answers "was it still there when the
    // window closed" rather than "was it ever there".
    score.still_published_at_window_end = carried != nullptr;
    if (carried == nullptr) {
        return;
    }

    ++score.after_published;
    score.residual_seconds = static_cast<double>(now - score.truth_end) / source_rate_;
    if (carried->state == detect::TrackState::Live) {
        score.after_live_seconds += elapsed;
    } else {
        score.after_held_seconds += elapsed;
    }

    const auto centre_error = static_cast<dsp::Hertz>(carried->center - score.on_centre_hz);
    const double ratio = static_cast<double>(std::max<dsp::Hertz>(1, carried->bandwidth)) /
                         static_cast<double>(std::max<dsp::Hertz>(1, score.on_bandwidth_hz));
    score.after_last_centre_error_hz = centre_error;
    score.after_last_bandwidth_ratio = ratio;

    if (std::abs(centre_error) >= std::abs(score.after_worst_centre_error_hz)) {
        score.after_worst_centre_error_hz = centre_error;
        score.after_state_at_worst = carried->state;
    }

    // Furthest from one in either direction. A band that collapses to a
    // tenth of what was measured is as wrong as one that inflates tenfold,
    // and on a shaped signal the measured drift went narrow.
    const double excursion = std::abs(std::log(std::max(ratio, 1.0e-9)));
    if (excursion >= live.worst_bandwidth_excursion) {
        live.worst_bandwidth_excursion = excursion;
        score.after_worst_bandwidth_ratio = ratio;
    }
}

std::vector<EmitterScore> SceneScorer::finish() const {
    std::vector<EmitterScore> out;
    out.reserve(emitters_.size());
    for (const Live& live : emitters_) {
        EmitterScore score = live.score;
        score.track_ids = live.seen_ids.size();
        score.after_new_ids = live.after_ids.size();
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
