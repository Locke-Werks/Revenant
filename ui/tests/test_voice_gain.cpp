// The digital voice level's rules. Each case names the wrong implementation it
// rejects.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "audio/voice_gain.h"

using revenant::ui::clamp_voice_gain_db;
using revenant::ui::kVoiceGainMaxDb;
using revenant::ui::voice_gain_amplitude;
using revenant::ui::voice_gain_text;

TEST_CASE("the voice level at zero leaves the stream as the engine sent it")
{
    // Rejects a control whose bottom is anything but unity, which would change
    // what every existing P25 listener hears before they touch it.
    CHECK(voice_gain_amplitude(0.0) == 1.0F);
}

TEST_CASE("the voice level is decibels of amplitude")
{
    // Rejects a power ratio, which would give half the boost the readout names.
    CHECK(voice_gain_amplitude(20.0) == Catch::Approx(10.0F));
    CHECK(voice_gain_amplitude(6.0) == Catch::Approx(1.9953F).epsilon(1e-4));
}

TEST_CASE("the voice level stays inside its travel")
{
    // Rejects a setter that takes a stored value on trust: a hand-edited
    // registry value of 90 would otherwise put 90 dB on the voice.
    CHECK(clamp_voice_gain_db(-5.0) == 0.0);
    CHECK(clamp_voice_gain_db(90.0) == kVoiceGainMaxDb);
    CHECK(voice_gain_amplitude(90.0) == voice_gain_amplitude(kVoiceGainMaxDb));
}

TEST_CASE("the voice level is whole decibels")
{
    // Rejects a continuous value, which would be a registry write per frame of
    // a drag and a readout that disagreed with the gain applied.
    CHECK(clamp_voice_gain_db(11.6) == 12.0);
    CHECK(clamp_voice_gain_db(11.4) == 11.0);
}

TEST_CASE("a voice level that is not a number does not silence the voice")
{
    // Rejects passing NaN through to pow, which multiplies every voice sample
    // into NaN and the limiter into silence.
    CHECK(clamp_voice_gain_db(std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(voice_gain_amplitude(std::numeric_limits<double>::quiet_NaN()) == 1.0F);
    CHECK(clamp_voice_gain_db(std::numeric_limits<double>::infinity()) == kVoiceGainMaxDb);
}

TEST_CASE("the voice level reads as a boost")
{
    CHECK(voice_gain_text(0.0) == "0 dB");
    CHECK(voice_gain_text(12.0) == "+12 dB");
    CHECK(voice_gain_text(kVoiceGainMaxDb) == "+30 dB");
}
