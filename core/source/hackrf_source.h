// A HackRF One or HackRF Pro, presented as a receive-only source.
//
// The second real radio, and Paced like the first: the HackRF's clock sets the
// rate, nothing can ask it to wait, and a consumer that falls behind loses
// samples, which are counted in SourceStats and carried in the stream index
// exactly as the RTL-SDR backend does it.
//
// RECEIVE ONLY. The HackRF is a transceiver and libhackrf exposes its
// transmitter through the same handle. Nothing in this backend calls a
// transmit function, sets a transmit gain or starts a sweep, and that is a
// rule rather than a gap waiting to be filled: Revenant is a receiver, and a
// stray call on the TX side of a half-duplex radio keys a transmitter into
// whatever is on the antenna port.
//
// LICENCE. libhackrf is BSD-3-Clause and arrives through
// vcpkg-overlays/hackrf, which compiles only hackrf.c and hackrf.h.
// docs/hackrf-provenance.md has the pin and why there is an overlay at all.
// Written against the published hackrf.h interface.
//
// NATIVE BYTES ONLY. The device puts signed 8-bit I/Q on the bus and that is
// what reaches the sink, as SampleFormat::Cs8. The widening to Complex32 is
// core/shaders/convert_cs8_cf32.comp during the upload, with
// dsp::reference_convert_cs8_cf32 as its CPU twin, for the same bus-traffic
// reason the RTL-SDR header gives.
//
// THE ANTENNA PORT POWER DEFAULTS OFF AND IS WRITTEN OFF ON EVERY OPEN, for
// the reason the RTL-SDR header gives about its bias tee: it is a latch in
// the device that outlives the process that set it. This backend has no way
// to turn it on.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

// The sample-rate window this backend accepts. The MAX2837/MAX5864 path is
// specified from 2 MS/s, below which the anti-alias filter cannot follow, to
// 20 MS/s, which is what USB 2.0 high speed carries at two bytes a sample.
inline constexpr dsp::SampleRate kHackrfMinRate = 2'000'000;
inline constexpr dsp::SampleRate kHackrfMaxRate = 20'000'000;
inline constexpr dsp::SampleRate kHackrfDefaultRate = 10'000'000;

// The tuning range Great Scott Gadgets specifies for the HackRF One. The
// firmware accepts frequencies outside it with degraded performance; the
// capability description states the specified range and the open refuses
// anything outside it rather than handing back a quietly poor capture.
inline constexpr dsp::Hertz kHackrfMinFrequency = 1'000'000;
inline constexpr dsp::Hertz kHackrfMaxFrequency = 6'000'000'000;

// The three receive gain stages and their steps, from hackrf.h.
inline constexpr double kHackrfLnaMaxDb = 40.0;
inline constexpr double kHackrfLnaStepDb = 8.0;
inline constexpr double kHackrfVgaMaxDb = 62.0;
inline constexpr double kHackrfVgaStepDb = 2.0;
inline constexpr double kHackrfAmpDb = 14.0;

// The combined stage, which is the one the client's single gain control
// drives. See hackrf_split_gain.
inline constexpr double kHackrfCombinedMaxDb = kHackrfLnaMaxDb + kHackrfVgaMaxDb;

// What a URI that says nothing gets: LNA 16 and VGA 16, hackrf_transfer's own
// starting point. Like the RTL-SDR's 20 dB it is a place to start and not a
// right answer.
inline constexpr double kHackrfDefaultGainDb = 32.0;

struct HackrfGains {
    double lna_db = 0.0;
    double vga_db = 0.0;

    [[nodiscard]] double total() const { return lna_db + vga_db; }
};

// Splits one gain figure across the LNA and the VGA.
//
// WHY ONE FIGURE. The client draws one control, for the first stage the
// device reports (ui/models/engine_link.h, sourceGainStage), because the
// RTL-SDR it was written against has one. The HackRF has two independent
// receive stages and they cannot both be first. So the first stage is a
// combined one and this decides how a figure on it lands on the hardware.
//
// THE RULE. Half to the LNA rounded down to its 8 dB step and capped at 40,
// the rest to the VGA rounded down to its 2 dB step and capped at 62. An even split
// rather than LNA first: the LNA sets the noise figure and the VGA cannot
// recover what the LNA clipped, so filling the LNA before the VGA buys a
// fraction of a dB of sensitivity at the cost of overloading on every strong
// local signal, which is the trade the RTL-SDR's default was changed to stop
// making. The individual stages are still listed after the combined one, for
// a caller that wants to set them itself.
//
// Out-of-range and non-finite requests clamp; the result is what the device
// will be set to, and its total is what the combined stage reports.
[[nodiscard]] HackrfGains hackrf_split_gain(double total_db);

// The step each stage actually sets for a request, which is the one at or
// below it, clamped to the stage's range.
[[nodiscard]] double hackrf_snap_lna(double db);
[[nodiscard]] double hackrf_snap_vga(double db);

[[nodiscard]] constexpr bool hackrf_rate_supported(dsp::SampleRate rate)
{
    return rate >= kHackrfMinRate && rate <= kHackrfMaxRate;
}

[[nodiscard]] constexpr bool hackrf_frequency_supported(dsp::Hertz frequency)
{
    return frequency >= kHackrfMinFrequency && frequency <= kHackrfMaxFrequency;
}

// Everything the backend needs, already parsed. The URI grammar lives in
// registry.cpp.
struct HackrfSourceConfig {
    std::string uri;

    // The device's serial, as libhackrf lists it: 32 hex digits. Always a
    // serial and never an index, because a HackRF's serial is burned in at
    // the factory and unique, so the ambiguity the rtlsdr URI has to resolve
    // does not arise.
    std::string serial;

    dsp::SampleRate rate = kHackrfDefaultRate;

    bool center_given = false;
    dsp::Hertz center_hz = 0;

    double gain_db = kHackrfDefaultGainDb;

    // The 14 dB RF amplifier ahead of the LNA. Off unless asked for: it is
    // the stage that overloads first, and a default that overloads is a
    // default that manufactures signals.
    bool amp = false;
};

// Rejects a configuration the hardware cannot carry, before anything is
// opened, with a message naming the limit.
[[nodiscard]] Status validate_hackrf_config(const HackrfSourceConfig& config);

struct HackrfDevice {
    std::string serial;

    // "HackRF One", "HackRF Pro" and so on, from the USB product id, which
    // libhackrf reads from the descriptors without opening the device.
    std::string name;
};

// Attached HackRFs, opening none of them. Empty, and not an error, when none
// is attached.
[[nodiscard]] Expected<std::vector<HackrfDevice>> enumerate_hackrf_devices();

// Describes the device without opening it. Every property here is fixed by
// the hardware family, so there is nothing a brief open would add, and not
// opening means a listing still answers while the device is streaming.
// Fails when no attached HackRF has the serial.
[[nodiscard]] Expected<SourceCapabilities> describe_hackrf_source(const HackrfSourceConfig& config);

[[nodiscard]] Expected<std::unique_ptr<Source>> open_hackrf_source(const HackrfSourceConfig& config);

}  // namespace revenant::source
