#include "core/engine/timeshift.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace revenant::engine {

dsp::SampleIndex timeshift_capacity_samples(const TimeShiftLimits& limits, dsp::SampleRate rate,
                                            source::SampleFormat format) {
    const std::size_t bps = source::bytes_per_sample(format);
    if (rate <= 0 || bps == 0 || !std::isfinite(limits.seconds) || limits.seconds <= 0.0) {
        return 0;
    }
    const double wanted = limits.seconds * static_cast<double>(rate);
    // Past 2^62 the double no longer converts cleanly, and no host has that
    // much memory anyway.
    const auto by_time = static_cast<dsp::SampleIndex>(std::min(wanted, 4.0e18));
    const dsp::SampleIndex by_budget = limits.budget_bytes / bps;
    return std::min(by_time, by_budget);
}

void TimeShiftBuffer::set_limits(const TimeShiftLimits& limits) {
    const std::scoped_lock held(lock_);
    limits_ = limits;
    if (rate_ > 0) {
        allocate_locked();
    }
}

TimeShiftLimits TimeShiftBuffer::limits() const {
    const std::scoped_lock held(lock_);
    return limits_;
}

void TimeShiftBuffer::begin_stream(dsp::SampleRate rate, source::SampleFormat format) {
    const std::scoped_lock held(lock_);
    const bool same = rate == rate_ && format == format_ && storage_ != nullptr &&
                      capacity_ == timeshift_capacity_samples(limits_, rate, format);
    rate_ = rate;
    format_ = format;
    bytes_per_sample_ = source::bytes_per_sample(format);
    if (same) {
        held_ = false;
        return;
    }
    allocate_locked();
}

void TimeShiftBuffer::allocate_locked() {
    held_ = false;
    allocation_failed_ = false;
    storage_.reset();
    capacity_ = timeshift_capacity_samples(limits_, rate_, format_);
    if (capacity_ == 0) {
        return;
    }
    // Uninitialised on purpose: a zeroed 2 GiB is a second of page faults on
    // the control thread for bytes that are written before they are read.
    storage_.reset(new (std::nothrow) std::byte[capacity_ * bytes_per_sample_]);
    if (storage_ == nullptr) {
        allocation_failed_ = true;
        capacity_ = 0;
    }
}

void TimeShiftBuffer::release() {
    const std::scoped_lock held(lock_);
    storage_.reset();
    capacity_ = 0;
    held_ = false;
    rate_ = 0;
}

void TimeShiftBuffer::clear() {
    const std::scoped_lock held(lock_);
    held_ = false;
}

void TimeShiftBuffer::write_locked(dsp::SampleIndex index, std::span<const std::byte> bytes) {
    std::size_t samples = bytes.size() / bytes_per_sample_;
    const std::byte* from = bytes.data();
    // Only the newest capacity_ samples of a write survive it, so a write
    // longer than the ring skips straight to them.
    if (samples > capacity_) {
        const std::size_t skip = samples - static_cast<std::size_t>(capacity_);
        from += skip * bytes_per_sample_;
        index += skip;
        samples = static_cast<std::size_t>(capacity_);
    }
    while (samples > 0) {
        const auto slot = static_cast<std::size_t>(index % capacity_);
        const std::size_t run = std::min(samples, static_cast<std::size_t>(capacity_) - slot);
        std::memcpy(storage_.get() + slot * bytes_per_sample_, from, run * bytes_per_sample_);
        from += run * bytes_per_sample_;
        index += run;
        samples -= run;
    }
}

void TimeShiftBuffer::push(const source::SourceBlock& block, dsp::Hertz center) {
    const std::scoped_lock held(lock_);
    if (storage_ == nullptr || block.sample_count == 0 || block.format != format_ ||
        block.stamp.rate != rate_) {
        return;
    }
    const dsp::SampleIndex start = block.stamp.start;
    if (held_ && (center != center_ || start < newest_)) {
        held_ = false;
    }
    if (!held_) {
        held_ = true;
        oldest_ = start;
        newest_ = start;
        center_ = center;
    }
    anchor_ns_ = block.stamp.epoch_anchor_ns;

    if (start > newest_) {
        // Zeros rather than a hole, so an index always means one sample. A
        // gap wider than the ring empties it, which the zeros do by
        // themselves once oldest_ moves.
        const dsp::SampleIndex gap = std::min<dsp::SampleIndex>(start - newest_, capacity_);
        const dsp::SampleIndex from = start - gap;
        for (dsp::SampleIndex i = from; i < start;) {
            const auto slot = static_cast<std::size_t>(i % capacity_);
            const auto run = static_cast<std::size_t>(
                std::min<dsp::SampleIndex>(start - i, capacity_ - slot));
            std::memset(storage_.get() + slot * bytes_per_sample_, 0, run * bytes_per_sample_);
            i += run;
        }
        newest_ = start;
    }

    const std::size_t bytes = block.sample_count * bytes_per_sample_;
    write_locked(start, block.bytes.first(std::min(bytes, block.bytes.size())));
    newest_ = start + block.sample_count;
    if (newest_ - oldest_ > capacity_) {
        oldest_ = newest_ - capacity_;
    }
}

std::int64_t TimeShiftBuffer::wall_ns_locked(dsp::SampleIndex index) const {
    const dsp::BlockTimestamp stamp{.start = index, .epoch_anchor_ns = anchor_ns_, .rate = rate_};
    return stamp.wall_clock_ns();
}

TimeShiftExtent TimeShiftBuffer::extent() const {
    const std::scoped_lock held(lock_);
    TimeShiftExtent out;
    out.limits = limits_;
    out.rate = rate_;
    out.capacity_bytes = capacity_ * bytes_per_sample_;
    out.capacity_seconds =
        rate_ > 0 ? static_cast<double>(capacity_) / static_cast<double>(rate_) : 0.0;
    if (!held_ || storage_ == nullptr) {
        return out;
    }
    out.available = true;
    out.oldest_index = oldest_;
    out.newest_index = newest_;
    out.oldest_ns = wall_ns_locked(oldest_);
    out.newest_ns = wall_ns_locked(newest_);
    out.center = center_;
    return out;
}

std::size_t TimeShiftBuffer::read(dsp::SampleIndex start, std::size_t count,
                                  std::span<std::byte> out) const {
    const std::scoped_lock held(lock_);
    if (!held_ || storage_ == nullptr || start < oldest_ || start >= newest_) {
        return 0;
    }
    const auto samples = static_cast<std::size_t>(
        std::min<dsp::SampleIndex>(count, newest_ - start));
    const std::size_t room = out.size() / bytes_per_sample_;
    const std::size_t want = std::min(samples, room);
    std::byte* to = out.data();
    dsp::SampleIndex index = start;
    std::size_t left = want;
    while (left > 0) {
        const auto slot = static_cast<std::size_t>(index % capacity_);
        const std::size_t run = std::min(left, static_cast<std::size_t>(capacity_) - slot);
        std::memcpy(to, storage_.get() + slot * bytes_per_sample_, run * bytes_per_sample_);
        to += run * bytes_per_sample_;
        index += run;
        left -= run;
    }
    return want;
}

dsp::SampleIndex TimeShiftBuffer::index_at(std::int64_t wall_ns) const {
    const std::scoped_lock held(lock_);
    if (!held_ || rate_ <= 0) {
        return 0;
    }
    const std::int64_t oldest_ns = wall_ns_locked(oldest_);
    if (wall_ns <= oldest_ns) {
        return oldest_;
    }
    const auto offset_ns = static_cast<double>(wall_ns - oldest_ns);
    const auto offset = static_cast<dsp::SampleIndex>(
        std::ceil(offset_ns * static_cast<double>(rate_) / 1.0e9));
    return std::min(oldest_ + offset, newest_);
}

std::int64_t TimeShiftBuffer::wall_ns_at(dsp::SampleIndex index) const {
    const std::scoped_lock held(lock_);
    return wall_ns_locked(index);
}

source::SampleFormat TimeShiftBuffer::format() const {
    const std::scoped_lock held(lock_);
    return format_;
}

dsp::SampleRate TimeShiftBuffer::rate() const {
    const std::scoped_lock held(lock_);
    return rate_;
}

}  // namespace revenant::engine
