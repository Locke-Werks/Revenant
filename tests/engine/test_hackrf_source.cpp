// The HackRF backend, without a HackRF.
//
// No HackRF has been attached to any machine this was written or run on, so
// everything here is the part that needs none: the gain split, the parameter
// limits, the URI layer, enumeration with nothing plugged in, and the claim
// that the bytes the device puts on the bus land correctly through the
// existing cs8 conversion. Streaming, retuning and the overrun accounting
// against real hardware are not covered by anything in the tree yet.

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "core/dsp/convert.h"
#include "core/source/hackrf_source.h"
#include "core/source/registry.h"

namespace {

using namespace revenant;
using namespace revenant::source;

HackrfSourceConfig valid_config()
{
    HackrfSourceConfig config;
    config.serial = "0000000000000000a06063c8234e925f";
    config.center_given = true;
    config.center_hz = 144'800'000;
    return config;
}

}  // namespace

TEST_CASE("hackrf gain split lands on the stages' own steps", "[hackrf]")
{
    // The default is hackrf_transfer's LNA 16, VGA 16.
    const HackrfGains fallback = hackrf_split_gain(kHackrfDefaultGainDb);
    CHECK(fallback.lna_db == 16.0);
    CHECK(fallback.vga_db == 16.0);

    const HackrfGains top = hackrf_split_gain(kHackrfCombinedMaxDb);
    CHECK(top.lna_db == kHackrfLnaMaxDb);
    CHECK(top.vga_db == kHackrfVgaMaxDb);
    CHECK(top.total() == kHackrfCombinedMaxDb);

    const HackrfGains zero = hackrf_split_gain(0.0);
    CHECK(zero.total() == 0.0);

    // Every request lands on an 8 dB LNA step and a 2 dB VGA step, within
    // both stages' ranges, and never above what was asked for.
    for (int db = 0; db <= 102; ++db) {
        const HackrfGains gains = hackrf_split_gain(static_cast<double>(db));
        INFO("requested " << db << " dB");
        CHECK(static_cast<int>(gains.lna_db) % 8 == 0);
        CHECK(static_cast<int>(gains.vga_db) % 2 == 0);
        CHECK(gains.lna_db >= 0.0);
        CHECK(gains.lna_db <= kHackrfLnaMaxDb);
        CHECK(gains.vga_db >= 0.0);
        CHECK(gains.vga_db <= kHackrfVgaMaxDb);
        CHECK(gains.total() <= static_cast<double>(db));
        CHECK(gains.total() >= static_cast<double>(db) - 2.0);
    }
}

TEST_CASE("hackrf gain split clamps what it cannot set", "[hackrf]")
{
    CHECK(hackrf_split_gain(-10.0).total() == 0.0);
    CHECK(hackrf_split_gain(500.0).total() == kHackrfCombinedMaxDb);
    CHECK(hackrf_split_gain(std::numeric_limits<double>::quiet_NaN()).total() == 0.0);
    CHECK(hackrf_split_gain(std::numeric_limits<double>::infinity()).total() == 0.0);
}

TEST_CASE("hackrf stage snapping takes the step at or below", "[hackrf]")
{
    CHECK(hackrf_snap_lna(15.9) == 8.0);
    CHECK(hackrf_snap_lna(16.0) == 16.0);
    CHECK(hackrf_snap_lna(99.0) == kHackrfLnaMaxDb);
    CHECK(hackrf_snap_lna(-1.0) == 0.0);
    CHECK(hackrf_snap_vga(31.0) == 30.0);
    CHECK(hackrf_snap_vga(62.0) == 62.0);
    CHECK(hackrf_snap_vga(63.0) == kHackrfVgaMaxDb);
}

TEST_CASE("hackrf rate and frequency limits", "[hackrf]")
{
    CHECK_FALSE(hackrf_rate_supported(1'999'999));
    CHECK(hackrf_rate_supported(2'000'000));
    CHECK(hackrf_rate_supported(kHackrfDefaultRate));
    CHECK(hackrf_rate_supported(20'000'000));
    CHECK_FALSE(hackrf_rate_supported(20'000'001));

    CHECK_FALSE(hackrf_frequency_supported(999'999));
    CHECK(hackrf_frequency_supported(1'000'000));
    CHECK(hackrf_frequency_supported(6'000'000'000));
    CHECK_FALSE(hackrf_frequency_supported(6'000'000'001));
}

TEST_CASE("hackrf config validation names what is wrong", "[hackrf]")
{
    CHECK(validate_hackrf_config(valid_config()));

    HackrfSourceConfig no_serial = valid_config();
    no_serial.serial.clear();
    CHECK_FALSE(validate_hackrf_config(no_serial));

    HackrfSourceConfig slow = valid_config();
    slow.rate = 1'000'000;
    CHECK_FALSE(validate_hackrf_config(slow));

    HackrfSourceConfig fast = valid_config();
    fast.rate = 40'000'000;
    CHECK_FALSE(validate_hackrf_config(fast));

    HackrfSourceConfig nowhere = valid_config();
    nowhere.center_given = false;
    CHECK_FALSE(validate_hackrf_config(nowhere));

    HackrfSourceConfig too_high = valid_config();
    too_high.center_hz = 7'000'000'000;
    CHECK_FALSE(validate_hackrf_config(too_high));

    HackrfSourceConfig loud = valid_config();
    loud.gain_db = 103.0;
    CHECK_FALSE(validate_hackrf_config(loud));
}

TEST_CASE("hackrf enumeration is empty and not an error with no device", "[hackrf]")
{
    auto attached = enumerate_hackrf_devices();
    REQUIRE(attached);
    if (!attached->empty()) {
        SKIP("a HackRF is attached, so the empty case cannot be checked here");
    }

    auto listed = enumerate_sources();
    REQUIRE(listed);
    for (const SourceDescriptor& descriptor : *listed) {
        CHECK(descriptor.backend != "hackrf");
    }
}

TEST_CASE("hackrf URIs are parsed and refused before any device is touched", "[hackrf]")
{
    auto no_device = open_source("hackrf://");
    REQUIRE_FALSE(no_device);

    auto typo = open_source("hackrf://0000000000000000a06063c8234e925f?freq=144.8M&gian=20");
    REQUIRE_FALSE(typo);
    CHECK(typo.error().message.find("gian") != std::string::npos);

    auto bad_gain =
        open_source("hackrf://0000000000000000a06063c8234e925f?freq=144.8M&gain=auto");
    REQUIRE_FALSE(bad_gain);

    auto bad_rate =
        open_source("hackrf://0000000000000000a06063c8234e925f?freq=144.8M&rate=25M");
    REQUIRE_FALSE(bad_rate);
    CHECK(bad_rate.error().message.find("rate") != std::string::npos);
}

TEST_CASE("hackrf int8 IQ converts to the engine's samples", "[hackrf]")
{
    // What the device puts on the bus: interleaved signed I then Q. Four
    // samples, padded to a whole word as the twin requires.
    const std::array<std::int8_t, 8> wire{-128, 127, 0, 64, -1, 1, 32, -32};
    std::array<std::byte, wire.size()> packed{};
    for (std::size_t i = 0; i < wire.size(); ++i) {
        packed[i] = static_cast<std::byte>(static_cast<std::uint8_t>(wire[i]));
    }

    std::vector<dsp::Complex32> ring(4);
    dsp::ConvertParams params;
    params.ring_mask = 3;
    params.count = 4;
    REQUIRE(dsp::reference_convert_cs8_cf32(packed, params, ring));

    CHECK(ring[0].real() == -1.0F);
    CHECK(ring[0].imag() == 127.0F / 128.0F);
    CHECK(ring[1].real() == 0.0F);
    CHECK(ring[1].imag() == 0.5F);
    CHECK(ring[2].real() == -1.0F / 128.0F);
    CHECK(ring[2].imag() == 1.0F / 128.0F);
    CHECK(ring[3].real() == 0.25F);
    CHECK(ring[3].imag() == -0.25F);
}
