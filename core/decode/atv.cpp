#include "core/decode/atv.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>

namespace revenant::decode {

AtvTiming atv_timing(AtvStandard standard) {
    AtvTiming t;
    if (standard == AtvStandard::M525) {
        // BT.470 table 1-1, system M, the bracketed M/NTSC values.
        t.line_us = 63.5555;
        t.sync_us = 4.7;
        // b is 9.2 to 10.3 and c 1.27 to 2.22: midpoints.
        t.active_start_us = 9.75;
        t.front_porch_us = 1.745;
        // Table 1-2: p (2.3), q (27.1), l = m = n = 3 H; j is 19 to 21 H + a,
        // and the upper bound is taken so no blanked line is shown as picture.
        t.equalising_us = 2.3;
        t.field_sync_us = 27.1;
        t.pre_equalising_lines = 3.0;
        t.field_sync_lines = 3.0;
        t.post_equalising_lines = 3.0;
        t.field_blanking_lines = 21.0;
        // Table 1 items 1 and 4: 525 lines, sync -40, setup 7.5.
        t.lines_per_field = 262.5;
        t.sync_level = -40.0;
        t.black_level = 7.5;
        // 262.5 - 21 lines of picture per field, to the even line below.
        t.rows_per_field = 240;
    } else {
        // BT.470 table 1-1, systems B and G: H 64, d 4.7, b 10.5, c 1.5.
        t.line_us = 64.0;
        t.sync_us = 4.7;
        t.active_start_us = 10.5;
        t.front_porch_us = 1.5;
        // Table 1-2: p 2.35, q 27.3, l = m = n = 2.5 H, j = 25 H + a.
        t.equalising_us = 2.35;
        t.field_sync_us = 27.3;
        t.pre_equalising_lines = 2.5;
        t.field_sync_lines = 2.5;
        t.post_equalising_lines = 2.5;
        t.field_blanking_lines = 25.0;
        // Table 1 items 1 and 4: 625 lines, sync -43, no setup.
        t.lines_per_field = 312.5;
        t.sync_level = -43.0;
        t.black_level = 0.0;
        // 312.5 - 25 lines of picture per field, whole lines only.
        t.rows_per_field = 287;
    }
    return t;
}

double atv_first_line_offset(AtvStandard standard, bool second_field) {
    const AtvTiming t = atv_timing(standard);
    const double from_ov = t.field_blanking_lines - t.pre_equalising_lines;
    return second_field ? std::ceil(from_ov - 0.5) + 0.5 : std::ceil(from_ov);
}

AtvReceiver::AtvReceiver(AtvConfig config, AtvCallbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)), timing_(atv_timing(config_.standard)) {
    samples_per_us_ = static_cast<double>(config_.sample_rate) / 1e6;
    line_samples_ = timing_.line_us * samples_per_us_;
    // Smoothing for the sync path only: 0.4 us, a sixth of an equalising
    // pulse, enough to take the edge off noise without merging pulses.
    smooth_length_ = std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(0.4 * samples_per_us_)));
    smooth_ring_.assign(smooth_length_, 0.0F);
    first_line_ = {atv_first_line_offset(config_.standard, false),
                   atv_first_line_offset(config_.standard, true)};
    stats_.line_period_us = timing_.line_us;
}

Expected<AtvReceiver> AtvReceiver::create(AtvConfig config, AtvCallbacks callbacks) {
    if (config.sample_rate < 2'000'000) {
        return fail("atv: the sample rate must be at least 2 MS/s to resolve sync pulses");
    }
    if (config.width < 16 || config.width > 4096) {
        return fail("atv: width must be 16 to 4096 pixels");
    }
    return AtvReceiver(std::move(config), std::move(callbacks));
}

float AtvReceiver::at(double position) const {
    const double floor_pos = std::floor(position);
    const auto i = static_cast<std::int64_t>(floor_pos) - raw_start_;
    if (i < 0 || i + 1 >= static_cast<std::int64_t>(raw_.size())) {
        return static_cast<float>(blank_level_);
    }
    const auto frac = static_cast<float>(position - floor_pos);
    const float a = raw_[static_cast<std::size_t>(i)];
    const float b = raw_[static_cast<std::size_t>(i + 1)];
    return a + (b - a) * frac;
}

double AtvReceiver::edge_near(double nominal) const {
    const double window = line_samples_ / 4.0;
    double best = nominal;
    double best_distance = window;
    for (auto it = line_edges_.rbegin(); it != line_edges_.rend(); ++it) {
        const double distance = std::abs(*it - nominal);
        if (distance < best_distance) {
            best_distance = distance;
            best = *it;
        }
        if (*it < nominal - window) {
            break;
        }
    }
    return best;
}

void AtvReceiver::close_image() {
    if (!image_open_) {
        return;
    }
    image_open_ = false;
    if (callbacks_.on_close) {
        callbacks_.on_close(ImageClose{image_id_, rows_written_});
    }
}

void AtvReceiver::render_ready() {
    while (field_ && field_->next_row < timing_.rows_per_field) {
        Field& f = *field_;
        const double nominal =
            f.next_row == 0 ? f.ov + first_line_[f.second ? 1 : 0] * line_samples_
                            : f.last_oh + line_samples_;
        // Every edge within a quarter line of nominal has been seen once the
        // whole line is in.
        next_gate_ = nominal + line_samples_ + 2.0 * static_cast<double>(smooth_length_) + 4.0;
        if (static_cast<double>(total_) < next_gate_) {
            return;
        }
        const double oh = edge_near(nominal);
        f.last_oh = oh;
        const double start = oh + timing_.active_start_us * samples_per_us_;
        const double end = oh + (timing_.line_us - timing_.front_porch_us) * samples_per_us_;
        const double step = (end - start) / static_cast<double>(config_.width);
        const double span = blank_level_ - sync_level_;
        ImageRow row;
        row.image_id = image_id_;
        row.row = 2 * f.next_row + (f.second ? 1U : 0U);
        row.width = config_.width;
        row.model = ImageRow::Gray8;
        row.bytes.resize(config_.width);
        // Each pixel is the mean over its own footprint, which is the low-pass
        // a video amplifier would apply when there are more samples than
        // pixels.
        const int taps = std::max(1, static_cast<int>(std::ceil(step)));
        for (std::uint32_t x = 0; x < config_.width; ++x) {
            double v = 0.0;
            for (int i = 0; i < taps; ++i) {
                v += at(start + (static_cast<double>(x) + (i + 0.5) / taps) * step);
            }
            v /= taps;
            // Table 1 item 4: the sync amplitude fixes the scale.
            const double level = span > 0.0 ? (v - blank_level_) / span * -timing_.sync_level : 0.0;
            const double y = (level - timing_.black_level) / (100.0 - timing_.black_level);
            row.bytes[x] = static_cast<std::uint8_t>(std::lround(std::clamp(y, 0.0, 1.0) * 255.0));
        }
        if (image_open_ && callbacks_.on_row) {
            callbacks_.on_row(row);
        }
        if (image_open_) {
            ++rows_written_;
        }
        ++f.next_row;
        if (f.next_row == timing_.rows_per_field) {
            if (f.second) {
                ++stats_.frames;
                close_image();
            }
            field_.reset();
        }
    }
}

void AtvReceiver::on_pulse(double start, double end) {
    const double width_us = (end - start) / samples_per_us_;
    if (width_us >= 3.5 && width_us <= 6.5) {
        ++stats_.line_syncs;
        if (last_line_edge_) {
            const double period = (start - *last_line_edge_) / samples_per_us_;
            if (std::abs(period - timing_.line_us) < 0.02 * timing_.line_us) {
                stats_.line_period_us += 0.02 * (period - stats_.line_period_us);
            }
        }
        last_line_edge_ = start;
        line_edges_.push_back(start);
        while (line_edges_.size() > 2048) {
            line_edges_.pop_front();
        }
        // Sync tip from inside the pulse, blanking from the front porch.
        double tip = 0.0;
        int tip_n = 0;
        for (double p = start + samples_per_us_; p < end - samples_per_us_; p += 1.0) {
            tip += at(p);
            ++tip_n;
        }
        double porch = 0.0;
        int porch_n = 0;
        const double porch_end = start - 0.3 * samples_per_us_;
        for (double p = start - (timing_.front_porch_us - 0.3) * samples_per_us_; p < porch_end; p += 1.0) {
            porch += at(p);
            ++porch_n;
        }
        if (tip_n > 0 && porch_n > 0) {
            sync_level_ += 0.05 * (tip / tip_n - sync_level_);
            blank_level_ += 0.05 * (porch / porch_n - blank_level_);
        }
    } else if (width_us >= 1.2 && width_us < 3.5) {
        ++stats_.equalising;
    } else if (width_us >= 0.6 * timing_.field_sync_us && width_us <= 1.2 * timing_.field_sync_us) {
        ++stats_.field_syncs;
        const bool first_of_sequence = start - last_field_sync_ > 4.0 * line_samples_;
        last_field_sync_ = start;
        if (!first_of_sequence || !last_line_edge_) {
            return;
        }
        // Notes 2 and 3 to BT.470 figure 2-1 (and figure 2-2 for system M):
        // OV on a line-sync edge starts a first field, OV midway between two
        // starts a second.
        const double lines = (start - *last_line_edge_) / line_samples_;
        const double phase = lines - std::floor(lines);
        const bool second = phase > 0.25 && phase < 0.75;
        render_ready();
        if (field_) {
            field_.reset();  // a field cut short by this one
        }
        if (!second) {
            close_image();
        }
        if (!image_open_) {
            ++image_id_;
            image_open_ = true;
            rows_written_ = 0;
            if (callbacks_.on_open) {
                callbacks_.on_open(ImageOpen{image_id_, config_.width, 2 * timing_.rows_per_field,
                                             ImageRow::Gray8});
            }
        }
        field_ = Field{start, second, 0, 0.0};
        next_gate_ = 0.0;
        ++stats_.fields;
    }
}

void AtvReceiver::on_sample(float raw) {
    const float v = config_.inverted ? -raw : raw;
    raw_.push_back(v);
    const std::int64_t n = total_++;

    smooth_sum_ += static_cast<double>(v) - static_cast<double>(smooth_ring_[smooth_pos_]);
    smooth_ring_[smooth_pos_] = v;
    smooth_pos_ = (smooth_pos_ + 1) % smooth_length_;
    if (smooth_count_ < smooth_length_) {
        ++smooth_count_;
        if (smooth_count_ < smooth_length_) {
            return;
        }
    }
    const double smoothed = smooth_sum_ / static_cast<double>(smooth_length_);
    const double centre = static_cast<double>(n) - static_cast<double>(smooth_length_ - 1) / 2.0;

    if (!levels_known_) {
        // The first four line periods set a provisional slicing level: sync
        // lies in the bottom 30 % of the excursion on every system in table 1.
        level_probe_.push_back(static_cast<float>(smoothed));
        if (static_cast<double>(level_probe_.size()) < 4.0 * line_samples_) {
            previous_smoothed_ = smoothed;
            return;
        }
        const auto [lo, hi] = std::minmax_element(level_probe_.begin(), level_probe_.end());
        sync_level_ = *lo;
        blank_level_ = *lo + 0.3 * (*hi - *lo);
        levels_known_ = true;
        level_probe_.clear();
        level_probe_.shrink_to_fit();
    }
    stats_.sync_level = sync_level_;
    stats_.blanking_level = blank_level_;
    const double threshold = 0.5 * (sync_level_ + blank_level_);
    if (!in_pulse_ && previous_smoothed_ >= threshold && smoothed < threshold) {
        in_pulse_ = true;
        pulse_start_ = centre - 1.0 + (previous_smoothed_ - threshold) / (previous_smoothed_ - smoothed);
    } else if (in_pulse_ && previous_smoothed_ < threshold && smoothed >= threshold) {
        in_pulse_ = false;
        const double end = centre - 1.0 + (threshold - previous_smoothed_) / (smoothed - previous_smoothed_);
        on_pulse(pulse_start_, end);
    }
    previous_smoothed_ = smoothed;
    // Rows are rendered at the first sample their gate allows, never at a
    // block boundary, so the levels they are scaled by do not depend on the
    // blocking.
    if (field_ && static_cast<double>(total_) >= next_gate_) {
        render_ready();
    }
}

void AtvReceiver::push(dsp::ConstRealSpan video) {
    for (const float v : video) {
        on_sample(v);
    }
    // Keep raw samples back to the line still to be rendered, or a couple of
    // lines when nothing is pending.
    double keep_from = static_cast<double>(total_) - 3.0 * line_samples_;
    if (field_) {
        const double next = field_->next_row == 0 ? field_->ov : field_->last_oh;
        keep_from = std::min(keep_from, next - line_samples_);
    }
    const auto keep = static_cast<std::int64_t>(std::floor(keep_from));
    // Erase in large steps: the cost of moving the tail is then spread thin.
    const std::int64_t drop = std::min<std::int64_t>(keep - raw_start_, static_cast<std::int64_t>(raw_.size()));
    if (drop > (1 << 20)) {
        raw_.erase(raw_.begin(), raw_.begin() + drop);
        raw_start_ += drop;
    }
}

void AtvFmDemodulator::push(dsp::ConstComplexSpan in, std::vector<float>& out) {
    const double scale = static_cast<double>(rate_) / (2.0 * std::numbers::pi);
    for (const auto x : in) {
        out.push_back(static_cast<float>(std::arg(x * std::conj(previous_)) * scale));
        previous_ = x;
    }
}

void atv_am_envelope(dsp::ConstComplexSpan in, std::vector<float>& out) {
    for (const auto x : in) {
        out.push_back(std::abs(x));
    }
}

}  // namespace revenant::decode
