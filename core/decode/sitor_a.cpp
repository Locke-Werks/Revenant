#include "core/decode/sitor_a.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "core/decode/rtty.h"

namespace revenant::decode {

namespace {

// Engineering thresholds, not specified, as ratios of a signal's mean
// tone-energy difference to the noise floor's tone-energy sum. A normalised
// discriminator cannot be used for these, since it reads noise in a pause as
// confidently as a tone; an absolute level would tie the monitor to the
// receiver's gain.
constexpr double kAcquireRatio = 3.0;
constexpr double kTrackRatio = 2.0;
constexpr double kControlRatio = 2.5;
constexpr int kMissesBeforeLoss = 4;
constexpr int kStepsPerUnit = 8;
// The floor is this fraction of the way up a cycle's sorted unit sums: of 45
// units a block and a control signal fill 28, so the lowest fifth is pause.
constexpr double kFloorPercentile = 0.2;

bool is_cs12(SitorAControl c) {
    return c == SitorAControl::Cs1 || c == SitorAControl::Cs2;
}

}  // namespace

SitorAControl sitor_a_control(std::uint8_t signal) {
    switch (signal & 0x7FU) {
        case kSitorACs1: return SitorAControl::Cs1;
        case kSitorACs2: return SitorAControl::Cs2;
        case kSitorACs3: return SitorAControl::Cs3;
        case kSitorACs4: return SitorAControl::Cs4;
        case kSitorACs5: return SitorAControl::Cs5;
        case kSitorARq: return SitorAControl::Rq;
        default: return SitorAControl::Other;
    }
}

Expected<SitorAMonitor> SitorAMonitor::create(const SitorAConfig& config) {
    if (config.rate < 1000) {
        return fail("SITOR-A needs a sample rate of at least 1000 S/s");
    }
    if (config.centre_hz + config.shift_hz >= config.rate / 2) {
        return fail("SITOR-A tones must sit below the Nyquist frequency");
    }
    SitorAMonitor m;
    m.config_ = config;
    m.samples_per_unit_ = static_cast<double>(config.rate) / kSitorBaud;
    m.samples_per_cycle_ = m.samples_per_unit_ * static_cast<double>(kSitorACycleUnits);
    m.window_ = static_cast<std::size_t>(std::llround(m.samples_per_unit_));
    // Clause 1.3, and Table 1 note 2: Y the lower tone on USB, the higher on
    // LSB.
    const double y = config.centre_hz - config.shift_hz / 2.0;
    const double b = config.centre_hz + config.shift_hz / 2.0;
    const double ty = config.upper_sideband ? y : b;
    const double tb = config.upper_sideband ? b : y;
    m.y_step_ = 2.0 * std::numbers::pi * ty / config.rate;
    m.b_step_ = 2.0 * std::numbers::pi * tb / config.rate;
    m.reset();
    return m;
}

void SitorAMonitor::reset() {
    y_ring_.assign(window_, {});
    b_ring_.assign(window_, {});
    y_sum_ = {};
    b_sum_ = {};
    ring_pos_ = 0;
    phase_ = 0.0;
    b_phase_ = 0.0;
    difference_.assign(1, 0.0);
    sum_.assign(1, 0.0);
    prefix_base_ = 0;
    total_ = 0;
    search_from_ = 0.0;
    locked_ = false;
    invert_ = false;
    next_block_ = 0.0;
    missed_ = 0;
    last_control_ = SitorAControl::None;
    last_printed_.reset();
    figures_ = false;
    stats_ = {};
}

double SitorAMonitor::unit_difference(double at) const {
    const auto ia = static_cast<std::uint64_t>(std::max(0.0, std::floor(at + 0.25 * samples_per_unit_)));
    const auto ib = static_cast<std::uint64_t>(std::max(0.0, std::floor(at + 0.75 * samples_per_unit_)));
    if (ia < prefix_base_ || ib > total_ || ib <= ia) {
        return 0.0;
    }
    return (difference_[ib - prefix_base_] - difference_[ia - prefix_base_]) /
           static_cast<double>(ib - ia);
}

double SitorAMonitor::unit_sum(double at) const {
    const auto ia = static_cast<std::uint64_t>(std::max(0.0, std::floor(at + 0.25 * samples_per_unit_)));
    const auto ib = static_cast<std::uint64_t>(std::max(0.0, std::floor(at + 0.75 * samples_per_unit_)));
    if (ia < prefix_base_ || ib > total_ || ib <= ia) {
        return 0.0;
    }
    return (sum_[ib - prefix_base_] - sum_[ia - prefix_base_]) / static_cast<double>(ib - ia);
}

SitorAMonitor::Sliced SitorAMonitor::slice(double at, bool invert) const {
    Sliced s;
    for (std::size_t u = 0; u < kSitorSignalUnits; ++u) {
        double v = unit_difference(at + static_cast<double>(u) * samples_per_unit_);
        if (invert) {
            v = -v;
        }
        // Table 1 note 3: bit position 1 first; Y = 1.
        if (v > 0.0) {
            s.signal = static_cast<std::uint8_t>(s.signal | (1U << u));
        }
        s.strength += std::abs(v);
    }
    s.strength /= static_cast<double>(kSitorSignalUnits);
    // Clause 1.1: three Y in seven.
    s.valid = std::popcount(s.signal) == kSitorOnesPerSignal;
    return s;
}

double SitorAMonitor::block_strength(double at, bool invert, bool* valid) const {
    // The weakest of the three signals: a control signal followed by noise
    // must not pass for a block on the control signal's strength.
    double strength = 1e300;
    bool all = true;
    for (std::size_t k = 0; k < kSitorABlockSignals; ++k) {
        const Sliced s =
            slice(at + static_cast<double>(k * kSitorSignalUnits) * samples_per_unit_, invert);
        strength = std::min(strength, s.strength);
        all = all && s.valid;
    }
    if (valid != nullptr) {
        *valid = all;
    }
    return strength;
}

double SitorAMonitor::floor_before(double at) const {
    std::array<double, kSitorACycleUnits> sums{};
    for (std::size_t u = 0; u < kSitorACycleUnits; ++u) {
        sums[u] = unit_sum(at - samples_per_cycle_ + static_cast<double>(u) * samples_per_unit_);
    }
    const auto k = static_cast<std::size_t>(kFloorPercentile * kSitorACycleUnits);
    std::nth_element(sums.begin(), sums.begin() + static_cast<std::ptrdiff_t>(k), sums.end());
    // A clean recording has silence in its pauses; keep the ratio finite.
    return std::max(sums[k], 1e-12);
}

SitorAControl SitorAMonitor::find_control(double first, double last, double floor) const {
    const double step = samples_per_unit_ / kStepsPerUnit;
    Sliced best;
    double best_score = -1.0;
    for (double c = first; c <= last; c += step) {
        for (const bool invert : {invert_, !invert_}) {
            const Sliced s = slice(c, invert);
            const double score = s.strength + (s.valid ? 1e9 : 0.0);
            if (score > best_score) {
                best_score = score;
                best = s;
            }
        }
    }
    if (!best.valid || best.strength < kControlRatio * floor) {
        return SitorAControl::None;
    }
    return sitor_a_control(best.signal);
}

void SitorAMonitor::process(ConstRealSpan audio, SitorAOutput& out) {
    const double norm = 1.0 / static_cast<double>(window_);
    for (const float x : audio) {
        const std::complex<double> yv = static_cast<double>(x) * std::polar(1.0, -phase_);
        const std::complex<double> bv = static_cast<double>(x) * std::polar(1.0, -b_phase_);
        y_sum_ += yv - y_ring_[ring_pos_];
        b_sum_ += bv - b_ring_[ring_pos_];
        y_ring_[ring_pos_] = yv;
        b_ring_[ring_pos_] = bv;
        ring_pos_ = (ring_pos_ + 1) % window_;
        phase_ = std::fmod(phase_ + y_step_, 2.0 * std::numbers::pi);
        b_phase_ = std::fmod(b_phase_ + b_step_, 2.0 * std::numbers::pi);
        const double ey = std::norm(y_sum_ * norm);
        const double eb = std::norm(b_sum_ * norm);
        difference_.push_back(difference_.back() + (ey - eb));
        sum_.push_back(sum_.back() + (ey + eb));
    }
    total_ += audio.size();

    for (;;) {
        const bool was_locked = locked_;
        const double from = locked_ ? next_block_ : search_from_;
        if (!locked_) {
            search();
        } else {
            track();
        }
        for (SitorABlock& b : ready_) {
            emit(std::move(b), out);
        }
        ready_.clear();
        const double to = locked_ ? next_block_ : search_from_;
        if (was_locked == locked_ && to == from) {
            break;
        }
    }

    // Keep two cycles behind the earliest point either the search or the
    // tracker will look at: one for the floor, one for the control signal
    // before an acquired block.
    const double keep = (locked_ ? next_block_ : search_from_) - 2.0 * samples_per_cycle_ - samples_per_unit_;
    if (keep > static_cast<double>(prefix_base_) + samples_per_cycle_) {
        const auto drop = static_cast<std::uint64_t>(keep) - prefix_base_;
        difference_.erase(difference_.begin(), difference_.begin() + static_cast<std::ptrdiff_t>(drop));
        sum_.erase(sum_.begin(), sum_.begin() + static_cast<std::ptrdiff_t>(drop));
        prefix_base_ += drop;
    }
}

void SitorAMonitor::search() {
    const double step = samples_per_unit_ / kStepsPerUnit;
    const double need = samples_per_cycle_ +
                        static_cast<double>(kSitorABlockUnits + kSitorSignalUnits + 2) * samples_per_unit_;
    while (search_from_ + need <= static_cast<double>(total_)) {
        const double t = search_from_;
        search_from_ += step;
        for (const bool invert : {false, true}) {
            bool v0 = false;
            const double e0 = block_strength(t, invert, &v0);
            if (!v0) {
                continue;
            }
            bool v1 = false;
            const double e1 = block_strength(t + samples_per_cycle_, invert, &v1);
            if (!v1) {
                continue;
            }
            const double floor = floor_before(t + samples_per_cycle_);
            if (e0 < kAcquireRatio * floor || e1 < kAcquireRatio * floor) {
                continue;
            }
            // The search runs forward, so the first alignment to pass can be
            // early by a unit or two whose noise happened to complete valid
            // signals. Take the strongest valid alignment over the next
            // signal's worth of offsets.
            double best_t = t;
            double best = std::min(e0, e1);
            for (int k = 1; k <= kStepsPerUnit * static_cast<int>(kSitorSignalUnits); ++k) {
                const double c = t + k * step;
                bool a = false;
                bool b = false;
                const double s = std::min(block_strength(c, invert, &a),
                                          block_strength(c + samples_per_cycle_, invert, &b));
                if (a && b && s > best) {
                    best = s;
                    best_t = c;
                }
            }
            locked_ = true;
            invert_ = invert;
            next_block_ = best_t;
            missed_ = 0;
            // Clause 3.4.1: the control signal in the pause before the
            // acquired block, which says whether the block is new.
            last_control_ = find_control(
                best_t - samples_per_cycle_ + static_cast<double>(kSitorABlockUnits) * samples_per_unit_,
                best_t - static_cast<double>(kSitorSignalUnits) * samples_per_unit_,
                floor_before(best_t));
            ++stats_.acquisitions;
            return;
        }
    }
}

void SitorAMonitor::track() {
    const double step = samples_per_unit_ / kStepsPerUnit;
    if (next_block_ + samples_per_cycle_ + samples_per_unit_ > static_cast<double>(total_)) {
        return;
    }
    double best_t = next_block_;
    double best_strength = 0.0;
    bool best_valid = false;
    double best_score = -1.0;
    for (int k = -kStepsPerUnit / 2; k <= kStepsPerUnit / 2; ++k) {
        const double c = next_block_ + k * step;
        bool v = false;
        const double s = block_strength(c, invert_, &v);
        // Prefer alignments that give valid signals; among them the strongest.
        const double score = s + (v ? 1e9 : 0.0);
        if (score > best_score) {
            best_score = score;
            best_t = c;
            best_strength = s;
            best_valid = v;
        }
    }
    const double floor = floor_before(best_t);
    if (best_strength < kTrackRatio * floor) {
        ++missed_;
        next_block_ += samples_per_cycle_;
        if (missed_ >= kMissesBeforeLoss) {
            locked_ = false;
            ++stats_.losses;
            search_from_ = next_block_ - samples_per_cycle_;
            last_control_ = SitorAControl::None;
        }
        return;
    }
    missed_ = 0;

    SitorABlock block;
    // The correlators are centred half a window late.
    const double group = static_cast<double>(window_ - 1) / 2.0;
    block.position = static_cast<SampleIndex>(std::max(0.0, std::round(best_t - group)));
    for (std::size_t k = 0; k < kSitorABlockSignals; ++k) {
        block.signals[k] =
            slice(best_t + static_cast<double>(k * kSitorSignalUnits) * samples_per_unit_, invert_).signal;
    }
    const auto& sig = block.signals;
    if (!best_valid) {
        block.kind = SitorABlock::Kind::Mutilated;
    } else if (sig[0] == kSitorARq && sig[1] == kSitorARq && sig[2] == kSitorARq) {
        block.kind = SitorABlock::Kind::Repetition;
    } else if (sig[0] == kSitorAIdleAlpha && sig[1] == kSitorAIdleAlpha &&
               sig[2] == kSitorAIdleAlpha) {
        block.kind = SitorABlock::Kind::IdleAlpha;
    } else if (sig[1] == kSitorARq || sig[2] == kSitorARq) {
        // Clause 3.5.3: signal repetition in place 2 of call block 1, or in
        // place 3 or 1 of call block 2.
        block.kind = SitorABlock::Kind::Call;
    } else if (std::ranges::any_of(sig, [](std::uint8_t s) {
                   return sitor_classify(s).kind == SitorSignal::Kind::Traffic;
               })) {
        block.kind = SitorABlock::Kind::Information;
    } else {
        block.kind = SitorABlock::Kind::Idle;
    }

    // Clause 3.4.1: the control signal in the pause after the block.
    const SitorAControl control = find_control(
        best_t + static_cast<double>(kSitorABlockUnits) * samples_per_unit_,
        best_t + samples_per_cycle_ - static_cast<double>(kSitorSignalUnits) * samples_per_unit_, floor);
    if (control != SitorAControl::None) {
        ++stats_.control_signals;
    }
    block.control_before = last_control_;
    block.control_after = control;
    last_control_ = control;
    ready_.push_back(block);
    next_block_ = best_t + samples_per_cycle_;
}

void SitorAMonitor::emit(SitorABlock block, SitorAOutput& out) {
    ++stats_.blocks;
    if (block.kind == SitorABlock::Kind::Mutilated) {
        ++stats_.mutilated_blocks;
    }
    const bool carries = block.kind == SitorABlock::Kind::Information ||
                         block.kind == SitorABlock::Kind::Mutilated;
    if (carries) {
        if (is_cs12(block.control_before) && is_cs12(block.control_after)) {
            // Clauses 3.7.5 and 3.7.6.
            if (block.control_before != block.control_after) {
                block.accepted_by = SitorABlock::Accepted::ByControlSignals;
            } else {
                ++stats_.repeats_suppressed;
            }
        } else if (block.kind == SitorABlock::Kind::Information) {
            if (last_printed_ && *last_printed_ == block.signals) {
                ++stats_.repeats_suppressed;
            } else {
                block.accepted_by = SitorABlock::Accepted::ByContent;
            }
        }
    }
    if (block.accepted_by != SitorABlock::Accepted::No) {
        last_printed_ = block.signals;
        for (std::size_t k = 0; k < kSitorABlockSignals; ++k) {
            const SitorSignal s = sitor_classify(block.signals[k]);
            SitorCharacter c;
            c.position = block.position +
                         static_cast<SampleIndex>(std::llround(
                             static_cast<double>(k * kSitorSignalUnits) * samples_per_unit_));
            c.phasing = stats_.acquisitions;
            if (s.kind == SitorSignal::Kind::Mutilated) {
                c.mutilated = true;
                c.glyph = config_.error_glyph;
            } else if (s.kind != SitorSignal::Kind::Traffic) {
                // Clause 3.7.2: idle signals fill a block and print nothing.
                continue;
            } else {
                c.combination = s.combination;
                if (c.combination == kIta2LetterShift) {
                    figures_ = false;
                } else if (c.combination == kIta2FigureShift) {
                    figures_ = true;
                } else {
                    c.glyph = figures_ ? ita2_figure(c.combination) : ita2_letter(c.combination);
                }
            }
            c.figures = figures_;
            out.characters.push_back(c);
        }
    }
    out.blocks.push_back(block);
}

}  // namespace revenant::decode
