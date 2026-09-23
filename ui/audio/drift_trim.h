// The slow loop that holds how much audio the lead ring has buffered by
// trimming the rate the mix reads it at.
//
// WHY ONE IS NEEDED. The engine's sample clock and the sound card's are two
// crystals, and nothing disciplines one to the other. Measured 2026-09-20
// against a synthetic source at pace 1.00 on an nfm receiver at 48 kHz, the
// lead ring's depth rose from 23 ms to 42 ms in 64 seconds: the engine about
// 0.03 percent fast of the card. Left alone that fills a 200 ms ring in about
// ten minutes and then evicts a 27 ms chunk every 90 seconds, which the mix
// hears as a skip; a card fast of the engine starves at the same rate.
//
// WHAT IT DOES. It is handed the lead's buffered audio in seconds at every
// pull, which AudioMix measures as the lead's lag behind its arrivals when it
// can and as the ring's fill when it cannot; MixPull::lag_seconds has the
// difference and why. The loop averages it, takes the average over its
// first kSettleS as the level to hold, and from then on reads the lead, and
// every stream aligned to it, 1 + trim() times as fast as its nominal rate.
// A level above the one to hold makes trim() positive and drains it; below,
// negative.
//
// THE LOOP. The plant is an integrator: with the producer d fast of the card
// and the mix reading at 1 + trim, the buffered audio in seconds moves at
// d - trim per second of card time. A proportional and integral controller
// on its error closes it as s^2 + Kp s + Ki, and kProportional and kIntegral
// place both roots at -1 / kTimeConstantS, critically damped: the error from
// a step in drift peaks at d * kTimeConstantS / e, 1.1 ms for 100 ppm, and
// the integral carries the drift afterwards with no standing error.
//
// WHAT IT HOLDS, measured in ui/tests/test_drift_trim.cpp on ten simulated
// minutes of an engine 100 ppm fast and 100 ppm slow of the card, 27 ms
// chunks arriving up to 4 ms late, a 48000 S/s receiver leading and a P25
// receiver's 8000 S/s beside it: the averaged level within 1.3 ms of the
// one locked on throughout, the trim within 9 ppm of the drift after three
// minutes, never more than 1 ppm a step, and no frame starved, evicted,
// skipped or realigned after the lock. Read at the nominal step instead, the
// same engines took the level 59 ms over where it locked, and starved the
// lead for 2064 frames.
//
// WHY IT IS INAUDIBLE, in numbers. A ratio of 1 + r moves every frequency by
// r. The clamp, kLimit, is 500 ppm, 0.87 cent; the smallest change in a
// steady tone near 1 kHz a listener detects is about 0.2 percent, 3.5 cents
// (Wier, Jesteadt and Green, JASA 61(1), 1977). The trim moves by
// at most kMaxStep, 2 ppm, per update of kUpdateS, 100 ms: 20 ppm a second
// at the fastest. And the playhead is continuous across a change, since
// only the step between output frames moves, so there is no discontinuity
// in the waveform for a change to click on.
//
// WHAT RESTARTS THE MEASUREMENT. rebase() forgets the level to hold and the
// average and measures again for kSettleS, holding the trim at the integral
// meanwhile, since the integral is the drift and the drift belongs to the
// two clocks, not to the stream. AudioMix calls it when the lead changes,
// when the lead is placed or moved past a hole, and when the lead starves:
// a starve holds the playhead, which raises the latency by the hold, and
// measuring again keeps that raise rather than draining it back towards the
// level that starved.
//
// Qt-free and header-only, so ui/tests drives it on its own and inside
// AudioMix against a simulated producer.

#pragma once

#include <algorithm>
#include <cmath>

namespace revenant::ui {

class DriftTrim {
public:
    // The loop's time constant, in seconds of card time. Both closed-loop
    // roots sit at -1 / kTimeConstantS.
    static constexpr double kTimeConstantS = 30.0;

    // Per second, and per second squared.
    static constexpr double kProportional = 2.0 / kTimeConstantS;
    static constexpr double kIntegral = 1.0 / (kTimeConstantS * kTimeConstantS);

    // The furthest the trim goes either way, as a ratio: 500 ppm. The engine
    // measured 0.03 percent fast of the card on 2026-09-20, which is 300 ppm
    // and sat right on the 300 ppm this was first written with; 500 covers
    // it with 200 to spare and is still under a cent.
    static constexpr double kLimit = 500e-6;

    // How often the trim moves, in seconds of card time, and the most it
    // moves each time.
    static constexpr double kUpdateS = 0.1;
    static constexpr double kMaxStep = 2e-6;

    // The one-pole average the error is taken on. The lag carries the
    // arrival anchor's own wander, a tenth of a millisecond or so that moves
    // over seconds; averaged over two seconds the trim wandered 16 ppm
    // either side of the drift in the ten-minute case, over five 9 ppm, with
    // the level's worst error going from 1.2 ms to 1.3 ms.
    static constexpr double kSmoothingS = 5.0;

    // How long the level is measured before its mean becomes the level to
    // hold.
    static constexpr double kSettleS = 2.0;

    // How much faster than its nominal rate the lead is read.
    [[nodiscard]] double ratio() const { return 1.0 + trim_; }
    [[nodiscard]] double trim() const { return trim_; }

    // The level the loop holds, and its average of the level handed to it,
    // in seconds. Meaningful once locked().
    [[nodiscard]] bool locked() const { return locked_; }
    [[nodiscard]] double target() const { return target_; }
    [[nodiscard]] double smoothed() const { return smoothed_; }

    // One pull: the lead's buffered audio after it, in seconds, and the card
    // time it covered.
    void observe(double level_s, double elapsed_s)
    {
        if (!(elapsed_s > 0.0)) {
            return;
        }
        if (locked_) {
            smoothed_ += (level_s - smoothed_) * (1.0 - std::exp(-elapsed_s / kSmoothingS));
        } else {
            settle_sum_ += level_s * elapsed_s;
            settle_time_ += elapsed_s;
            if (settle_time_ >= kSettleS) {
                target_ = settle_sum_ / settle_time_;
                smoothed_ = target_;
                locked_ = true;
            }
        }
        since_update_ += elapsed_s;
        while (since_update_ >= kUpdateS) {
            since_update_ -= kUpdateS;
            update();
        }
    }

    // Measure the level to hold again. See WHAT RESTARTS THE MEASUREMENT.
    void rebase()
    {
        locked_ = false;
        settle_sum_ = 0.0;
        settle_time_ = 0.0;
    }

    // Everything, the drift included: a new sink.
    void reset() { *this = DriftTrim{}; }

private:
    void update()
    {
        double goal = integral_;
        if (locked_) {
            const double error = smoothed_ - target_;
            const double integral = integral_ + kIntegral * error * kUpdateS;
            const double wanted = kProportional * error + integral;

            // No integrating further into the clamp: a level that ran away
            // while the trim was held at the limit would otherwise wind the
            // integral up to the limit too, and the trim would carry on
            // draining well past the level it holds once the level came
            // back; ui/tests/test_drift_trim.cpp measures by how much.
            const bool pinned =
                (wanted > kLimit && error > 0.0) || (wanted < -kLimit && error < 0.0);
            if (!pinned) {
                integral_ = std::clamp(integral, -kLimit, kLimit);
            }
            goal = kProportional * error + integral_;
        }
        goal = std::clamp(goal, -kLimit, kLimit);
        trim_ += std::clamp(goal - trim_, -kMaxStep, kMaxStep);
    }

    double trim_ = 0.0;
    double integral_ = 0.0;
    bool locked_ = false;
    double target_ = 0.0;
    double smoothed_ = 0.0;
    double settle_sum_ = 0.0;
    double settle_time_ = 0.0;
    double since_update_ = 0.0;
};

}  // namespace revenant::ui
