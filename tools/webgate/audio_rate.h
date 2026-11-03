// A receiver's audio, at whatever rate the engine delivers it, to the 48 kHz
// both phone encoders take.
//
// WHY THIS EXISTS. Opus is run at 48000 only and the AAC MFT offers 44100 and
// 48000, but a digital voice receiver plays decoded voice at 8000 S/s (P25's
// IMBE, and the DMR and D-STAR vocoder plugins, whose silence before a plugin
// is loaded is at the same rate), and an analogue receiver runs at whatever
// audio rate it was given. Refusing those left the phone silent on every
// digital voice channel while the transcript, which resamples on its own,
// kept arriving.
//
// THE FILTER IS core/transcribe's. It is header-only with nothing but the
// standard library behind it, polyphase with exact phases for every ratio
// that matters here (8000 to 48000 is six rows), zero delay by construction,
// and bit-identical however the input is chunked, which is what keeps chunk
// boundaries silent. Its passband is 0.8125 of the lower Nyquist: 3.25 kHz
// from an 8000 S/s vocoder, which is all the vocoder carries. What it holds
// back is half a filter of input, 27 samples or 3.4 ms at 8000.
//
// 48000 PASSES THROUGH UNTOUCHED. No filter is built and the caller's span is
// returned as it came, so the analogue path is bit for bit what it was.
//
// A RATE CHANGE FLUSHES, THEN STARTS AGAIN. The old stream's last half
// filter is finished against zeros and handed out ahead of the new rate's
// first samples, so nothing already received is lost and the new stream
// starts from silence rather than from the old one's history.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/transcribe/resample.h"

namespace revenant::webgate {

inline constexpr std::uint32_t kPhoneAudioRate = 48'000;

// Per receiver. Not thread safe; the gateway runs it on its strand.
class PhoneRateConverter {
public:
    // Mono samples at `rate` in, the same stretch at kPhoneAudioRate out. The
    // span is good until the next call. A rate of zero gives nothing.
    [[nodiscard]] std::span<const float> convert(std::uint32_t rate, std::span<const float> mono)
    {
        out_.clear();
        if (rate != rate_) {
            if (resampler_) {
                resampler_->finish(out_);
                resampler_.reset();
            }
            rate_ = rate;
            if (rate != 0 && rate != kPhoneAudioRate) {
                resampler_.emplace(rate, kPhoneAudioRate);
            }
        }
        if (rate == 0) {
            return out_;
        }
        if (!resampler_) {
            if (out_.empty()) {
                return mono;
            }
            out_.insert(out_.end(), mono.begin(), mono.end());
            return out_;
        }
        resampler_->process(mono, out_);
        return out_;
    }

    // The rate the last chunk came at, zero before the first.
    [[nodiscard]] std::uint32_t input_rate() const { return rate_; }

    // Whether a filter sits in the path at all.
    [[nodiscard]] bool resampling() const { return resampler_.has_value(); }

private:
    std::uint32_t rate_ = 0;
    std::optional<transcribe::Resampler> resampler_;
    std::vector<float> out_;
};

}  // namespace revenant::webgate
