#include "core/engine/wideband_tap.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

#include "core/dsp/convert.h"
#include "core/dsp/vrx_reference.h"

namespace revenant::engine {

namespace {

// Stopband the decimation filter reaches. 80 dB is below the 8-bit dongle's
// own dynamic range, so nothing the filter lets fold back is visible over the
// quantisation floor of the commonest source.
constexpr double kAttenuationDb = 80.0;

// Taps per unit of decimation. A Kaiser design at 80 dB needs roughly
// 5.0 / (transition width in cycles per sample); the transition here is from
// 0.45 to 0.55 of the output rate, a tenth of it, so about 50 taps per output
// sample period, which is 50 * D at the input rate. Rounded to odd below.
constexpr std::uint32_t kTapsPerDecimation = 50;

// Worker wake interval when the ring is empty. The delivery thread does not
// signal per block, which would put a futex on it at 146 blocks a second; the
// worker polls instead, at a period well under one block at any rate a
// dongle runs.
constexpr auto kPollInterval = std::chrono::milliseconds(2);

// Renormalise the NCO phasor this often. The recurrence loses about one ulp
// of magnitude a step in double; a thousand steps is far under float's ulp.
constexpr std::uint32_t kRenormEvery = 1024;

[[nodiscard]] std::int32_t sign_extend_24(std::uint32_t raw) {
    return static_cast<std::int32_t>(raw << 8U) >> 8;
}

}  // namespace

Expected<std::unique_ptr<WidebandTap>> WidebandTap::create(const WidebandTapConfig& config,
                                                           dsp::SampleRate source_rate,
                                                           source::SampleFormat format,
                                                           WidebandSink sink) {
    if (!sink) {
        return fail("a wideband tap needs a sink to deliver to");
    }
    if (source_rate <= 0) {
        return fail("a wideband tap needs an open source with a known rate");
    }
    const dsp::SampleRate out_rate = config.output_rate == 0 ? source_rate : config.output_rate;
    if (out_rate <= 0 || out_rate > source_rate) {
        return fail(std::format(
            "a wideband tap delivers at most the source rate, {} S/s, and {} was asked for",
            source_rate, out_rate));
    }
    if (source_rate % out_rate != 0) {
        return fail(std::format(
            "a wideband tap decimates by a whole number and {} S/s does not divide the source "
            "rate of {} S/s; ask for {} divided by an integer",
            out_rate, source_rate, source_rate));
    }
    const std::int64_t half = source_rate / 2;
    if (config.offset < -half || config.offset > half) {
        return fail(std::format(
            "a wideband tap's offset of {} Hz is outside the source's span of plus or minus {} Hz",
            config.offset, half));
    }
    const std::size_t width = source::bytes_per_sample(format);
    if (width == 0) {
        return fail("a wideband tap was given a sample format it does not know");
    }
    if (!(config.ring_seconds > 0.0) || config.ring_seconds > 10.0) {
        return fail("a wideband tap's ring holds between zero and ten seconds");
    }

    std::unique_ptr<WidebandTap> tap(new WidebandTap());
    tap->config_ = config;
    tap->source_rate_ = source_rate;
    tap->output_rate_ = out_rate;
    tap->decimation_ = static_cast<std::uint32_t>(source_rate / out_rate);
    tap->format_ = format;
    tap->bytes_per_sample_ = width;
    tap->sink_ = std::move(sink);

    const auto ring_bytes = static_cast<std::size_t>(
        std::ceil(config.ring_seconds * static_cast<double>(source_rate))) * width;
    auto ring = SpscRing<std::byte>::create(std::bit_ceil(ring_bytes));
    if (!ring) {
        return std::unexpected(ring.error());
    }
    tap->ring_ = std::move(*ring);

    if (tap->decimation_ > 1) {
        // Capped at what the designer takes; a decimation past about 80 gets
        // a shorter filter and a wider transition, which is still well inside
        // the stopband at the fold for any rate a decoder asks for.
        std::uint32_t count = std::min<std::uint32_t>(kTapsPerDecimation * tap->decimation_,
                                                      dsp::kMaxAudioTaps - 1);
        count |= 1U;
        auto designed = dsp::design_audio_taps(count, source_rate, out_rate, kAttenuationDb);
        if (!designed) {
            return std::unexpected(designed.error());
        }
        tap->taps_ = std::move(*designed);
        tap->history_.assign(2 * tap->taps_.size(), 0.0F);
    }

    if (config.offset != 0) {
        const double radians = -2.0 * std::numbers::pi * static_cast<double>(config.offset) /
                               static_cast<double>(source_rate);
        tap->step_re_ = std::cos(radians);
        tap->step_im_ = std::sin(radians);
    }

    tap->worker_ = std::thread([raw = tap.get()] { raw->run(); });
    return tap;
}

WidebandTap::~WidebandTap() { (void)stop(); }

void WidebandTap::push(const source::SourceBlock& block, dsp::Hertz center) noexcept {
    if (failed_.load(std::memory_order_acquire)) {
        return;
    }
    if (center != 0) {
        center_.store(center, std::memory_order_relaxed);
    }
    if (block.format != format_) {
        // Recorded by the worker, which owns error_; here only the flag.
        failed_.store(true, std::memory_order_release);
        return;
    }
    // Whole blocks or nothing, so the ring never holds half a sample and the
    // worker never has to guess where a sample boundary fell.
    if (ring_->writable() < block.bytes.size()) {
        pending_drop_.fetch_add(block.sample_count, std::memory_order_relaxed);
        dropped_.fetch_add(block.sample_count, std::memory_order_relaxed);
        return;
    }
    (void)ring_->write(block.bytes);
    if (block.dropped_before != 0) {
        pending_drop_.fetch_add(block.dropped_before, std::memory_order_relaxed);
    }
}

Status WidebandTap::stop() {
    stopping_.store(true, std::memory_order_release);
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (error_ && failed_.load(std::memory_order_acquire)) {
        return fail("the wideband tap stopped: the source's sample format changed under it");
    }
    return error_;
}

void WidebandTap::run() {
    // Room for about 20 ms of input per pass, whole samples.
    const std::size_t pass_samples = std::max<std::size_t>(
        4096, static_cast<std::size_t>(source_rate_ / 50));
    scratch_.resize(pass_samples * bytes_per_sample_);
    out_.reserve(2 * (pass_samples / decimation_ + 1));

    std::size_t carried = 0;  // bytes of a partial sample left at scratch_'s front
    while (true) {
        const bool stopping = stopping_.load(std::memory_order_acquire);
        const std::size_t got =
            ring_->read(std::span<std::byte>(scratch_).subspan(carried));
        const std::size_t have = carried + got;
        const std::size_t samples = have / bytes_per_sample_;
        if (samples > 0) {
            process(std::span<const std::byte>(scratch_.data(), samples * bytes_per_sample_),
                    samples);
            carried = have - samples * bytes_per_sample_;
            if (carried != 0) {
                std::memmove(scratch_.data(), scratch_.data() + samples * bytes_per_sample_,
                             carried);
            }
        } else {
            carried = have;
        }
        if (failed_.load(std::memory_order_acquire)) {
            return;
        }
        if (got == 0) {
            if (stopping) {
                return;
            }
            std::unique_lock lock(wake_lock_);
            wake_.wait_for(lock, kPollInterval);
        }
    }
}

void WidebandTap::process(std::span<const std::byte> packed, std::size_t samples) {
    out_.clear();

    // A loss is applied at the start of the pass that follows it. That is
    // exact when the ring was drained before the drop, which is the case a
    // drop happens in (the ring was full because this thread was behind, and
    // it is now catching up), and late by at most one pass otherwise.
    std::uint64_t lost_in = pending_drop_.exchange(0, std::memory_order_relaxed);
    std::uint64_t lost_out = 0;
    if (lost_in != 0) {
        // The filter's history spans the gap and would smear stale samples
        // into the first outputs after it, so it restarts from zero, and the
        // output index moves by the whole outputs the gap covered.
        std::fill(history_.begin(), history_.end(), 0.0F);
        const std::uint64_t total = phase_ + lost_in;
        lost_out = total / decimation_;
        phase_ = static_cast<std::uint32_t>(total % decimation_);
        out_index_ += lost_out;
        // The NCO must advance by the lost samples too, or every chunk after
        // a drop sits at the wrong phase. Exact by angle rather than by
        // stepping: the gap can be millions of samples.
        if (config_.offset != 0) {
            const double turns = std::fmod(
                -static_cast<double>(config_.offset) * static_cast<double>(lost_in) /
                    static_cast<double>(source_rate_),
                1.0);
            const double radians = 2.0 * std::numbers::pi * turns;
            const double c = std::cos(radians);
            const double s = std::sin(radians);
            const double re = nco_re_ * c - nco_im_ * s;
            nco_im_ = nco_re_ * s + nco_im_ * c;
            nco_re_ = re;
        }
    }
    const dsp::SampleIndex chunk_start = out_index_;

    const std::byte* p = packed.data();
    const std::size_t ntaps = taps_.size();
    for (std::size_t n = 0; n < samples; ++n) {
        float i = 0.0F;
        float q = 0.0F;
        switch (format_) {
            case source::SampleFormat::Cu8:
                i = dsp::cu8_to_float(static_cast<std::uint8_t>(p[0]));
                q = dsp::cu8_to_float(static_cast<std::uint8_t>(p[1]));
                break;
            case source::SampleFormat::Cs8:
                i = dsp::cs8_to_float(static_cast<std::int8_t>(p[0]));
                q = dsp::cs8_to_float(static_cast<std::int8_t>(p[1]));
                break;
            case source::SampleFormat::Cs16: {
                std::int16_t pair[2];
                std::memcpy(pair, p, sizeof pair);
                i = dsp::cs16_to_float(pair[0]);
                q = dsp::cs16_to_float(pair[1]);
                break;
            }
            case source::SampleFormat::Cf32: {
                float pair[2];
                std::memcpy(pair, p, sizeof pair);
                i = pair[0];
                q = pair[1];
                break;
            }
            case source::SampleFormat::Cs24: {
                const auto byte = [p](std::size_t k) {
                    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(p[k]));
                };
                i = dsp::cs24_to_float(sign_extend_24(byte(0) | (byte(1) << 8U) | (byte(2) << 16U)));
                q = dsp::cs24_to_float(sign_extend_24(byte(3) | (byte(4) << 8U) | (byte(5) << 16U)));
                break;
            }
        }
        p += bytes_per_sample_;

        if (config_.offset != 0) {
            const double mi = static_cast<double>(i) * nco_re_ - static_cast<double>(q) * nco_im_;
            const double mq = static_cast<double>(i) * nco_im_ + static_cast<double>(q) * nco_re_;
            i = static_cast<float>(mi);
            q = static_cast<float>(mq);
            const double re = nco_re_ * step_re_ - nco_im_ * step_im_;
            nco_im_ = nco_re_ * step_im_ + nco_im_ * step_re_;
            nco_re_ = re;
            if (++since_renorm_ == kRenormEvery) {
                since_renorm_ = 0;
                const double mag = std::sqrt(nco_re_ * nco_re_ + nco_im_ * nco_im_);
                nco_re_ /= mag;
                nco_im_ /= mag;
            }
        }

        if (decimation_ == 1) {
            out_.push_back(i);
            out_.push_back(q);
            continue;
        }

        // Circular history, newest at history_pos_. Evaluated only on the
        // samples that produce an output, which is the whole saving of a
        // decimating FIR.
        history_[2 * history_pos_] = i;
        history_[2 * history_pos_ + 1] = q;
        if (++phase_ == decimation_) {
            phase_ = 0;
            float acc_i = 0.0F;
            float acc_q = 0.0F;
            std::size_t k = history_pos_;
            for (std::size_t t = 0; t < ntaps; ++t) {
                acc_i += taps_[t] * history_[2 * k];
                acc_q += taps_[t] * history_[2 * k + 1];
                k = (k == 0) ? ntaps - 1 : k - 1;
            }
            out_.push_back(acc_i);
            out_.push_back(acc_q);
        }
        history_pos_ = (history_pos_ + 1 == ntaps) ? 0 : history_pos_ + 1;
    }

    out_index_ += out_.size() / 2;
    if (out_.empty() && lost_out == 0) {
        return;
    }

    const dsp::Hertz centre = center_.load(std::memory_order_relaxed);
    WidebandChunk chunk;
    chunk.start = chunk_start;
    chunk.rate = output_rate_;
    chunk.center = centre == 0 ? 0 : centre + config_.offset;
    chunk.iq = out_;
    chunk.dropped_before = lost_out;
    try {
        if (auto handed = sink_(chunk); !handed) {
            error_ = std::unexpected(handed.error());
            failed_.store(true, std::memory_order_release);
        }
    } catch (const std::exception& thrown) {
        error_ = fail(std::format("the wideband sink threw an exception: {}", thrown.what()));
        failed_.store(true, std::memory_order_release);
    } catch (...) {
        error_ = fail("the wideband sink threw an exception that is not a std::exception");
        failed_.store(true, std::memory_order_release);
    }
}

}  // namespace revenant::engine
