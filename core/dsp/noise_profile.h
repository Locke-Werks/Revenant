// The adaptive noise profile: learns the steady part of a receiver's noise
// spectrum, hum, whine and birdies, from passages with nothing else in them,
// and takes it out of the audio.
//
// WHERE IT RUNS. On the host, on the completion thread, over audio that has
// already been read back for the meter, the squelch and the sinks. It is one
// receiver's mono audio at a few tens of kilohertz: a 1024 point transform
// pair every 512 samples is about 0.1 ms of one core per 10 ms of audio, so a
// GPU stage would add a dispatch, a barrier and a twin to save nothing. The
// generic NR (core/shaders/noise_line.comp) stays on the device because it
// runs ahead of the copy out; this runs after it, on what the listener hears.
//
// WHAT IT DOES. STFT with a periodic sqrt-Hann window at half overlap, which
// reconstructs exactly when the gain is one. Per bin, a Wiener gain against
// the noise estimate, floored at -strength dB, so a bin that is only noise is
// cut by the strength and a bin well above it passes untouched. The gain is
// smoothed across frames and across neighbouring bins, which is what keeps
// the residual from turning into musical noise.
//
// WHERE THE ESTIMATE COMES FROM, in order:
//   learned   "learn now" averages the next second of power and freezes it.
//             Used until reset, whatever auto-learn says.
//   quiet     with auto-learn on, a running average of the frames taken while
//             the squelch is shut, which is the cleanest sample of the noise
//             there is. Held through open passages once there is one.
//   minimum   with auto-learn on and the squelch never shut since a reset or
//             retune, minimum statistics: the smallest smoothed power over
//             the last two seconds, scaled for its bias. Speech never sits
//             still for two seconds in a bin; hum does, and so does a
//             carrier, which is why the quiet average wins when there is one.
//
// Pure arithmetic, no device, no clock. tests/reference/test_noise_profile.cpp
// drives it with synthesized hum, tones and noise.

#pragma once

#include <complex>
#include <cstdint>
#include <span>
#include <vector>

namespace revenant::dsp {

struct NoiseProfileSettings {
    bool enabled = false;
    double strength_db = 12.0;
    bool auto_learn = true;
    std::uint32_t learn_count = 0;
    std::uint32_t reset_count = 0;
};

inline constexpr double kNoiseProfileMaxStrengthDb = 20.0;

class NoiseProfile {
public:
    // Mono audio in place. `quiet` is whether the squelch was shut for this
    // block. A change of rate rebuilds the transform and drops the estimate.
    void process(std::span<float> audio, double rate, bool quiet,
                 const NoiseProfileSettings& settings);

    // A retune: the automatic estimate belonged to the old frequency. A
    // learned profile is kept, since the operator asked for it and hum from
    // the bench follows the receiver.
    void retune();

    [[nodiscard]] bool learning() const { return learn_left_ > 0; }
    [[nodiscard]] bool learned() const { return have_learned_; }

    // Samples between a sample going in and its coming out, while enabled.
    [[nodiscard]] std::uint32_t latency() const { return size_; }

    // The estimate in use, power per bin, empty until there is one.
    [[nodiscard]] std::span<const double> profile() const;

private:
    void rebuild(double rate);
    void clear_state();
    void run_frame(bool quiet, bool auto_learn, double floor);

    double rate_ = 0.0;
    std::uint32_t size_ = 0;
    std::uint32_t hop_ = 0;
    std::vector<double> window_;
    std::vector<std::complex<double>> twiddles_;
    std::vector<std::complex<double>> bins_;

    std::vector<double> in_;
    std::vector<double> ola_;
    std::vector<double> out_;
    std::uint32_t in_fill_ = 0;
    std::uint32_t out_pos_ = 0;

    std::vector<double> smoothed_;
    std::vector<double> quiet_avg_;
    bool have_quiet_ = false;
    std::vector<double> sub_min_;
    std::vector<double> mins_;
    std::uint32_t sub_frames_ = 0;
    std::uint32_t sub_index_ = 0;
    std::uint32_t frames_seen_ = 0;
    std::vector<double> gain_;

    std::vector<double> learned_;
    std::vector<double> learn_acc_;
    std::uint32_t learn_left_ = 0;
    std::uint32_t learn_total_ = 0;
    bool have_learned_ = false;

    std::vector<double> noise_;
    bool have_noise_ = false;

    bool enabled_last_ = false;
    bool counts_seen_ = false;
    std::uint32_t learn_seen_ = 0;
    std::uint32_t reset_seen_ = 0;
};

}  // namespace revenant::dsp
