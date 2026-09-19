// An RTL-SDR dongle, presented as a source.
//
// The first real radio in the engine, and therefore the first Paced source.
// Everything awkward about this backend follows from that one fact: the
// RTL2832U's crystal sets the rate, nothing can ask it to wait, and a
// consumer that falls behind does not slow the device down, it loses samples.
// So the loss is counted in SourceStats and the gap is carried in the stream
// index, and there is no path anywhere below that blocks the device to avoid
// it.
//
// LICENCE. This file links librtlsdr, which is GPL-2.0-or-later, so Revenant
// as a whole is GPL-3.0-or-later. Clean-room is still the default everywhere
// else in the tree; see docs/clean-room.md for the policy and
// docs/rtlsdr-provenance.md for why the RTL2832U specifically could not be
// done that way. Nothing here was derived from reading librtlsdr's internals:
// it is written against the published rtl-sdr.h interface.
//
// NATIVE BYTES ONLY. The device puts unsigned 8-bit offset-binary I/Q on the
// bus and that is exactly what reaches the sink. The widening to Complex32 is
// core/shaders/convert_cu8_cf32.comp, which runs during the upload, so a
// 2.4 MS/s stream costs 4.8 MB/s across PCIe instead of 19.2. A host-side
// conversion here would put a pass over every sample back on the CPU and
// quadruple the bus traffic, which is the cost the architecture exists to
// avoid.
//
// THE BIAS TEE DEFAULTS OFF, AND THAT DEFAULT IS LOAD-BEARING. bias=on puts
// 4.5 V DC on the centre conductor of the antenna port. Plenty of things that
// get plugged into an SDR present a DC short at the connector: a discone, a
// mag-mount whip, a passive splitter, a noise source, most transmit-capable
// gear. Powering into one of those at best folds the regulator back and at
// worst damages whatever is on the other end. Worse for a default, the bias
// tee is a GPIO latch inside the dongle rather than process state, so it
// survives the program exiting and the next person to plug an antenna in has
// no way to know it is live. A setting that can damage hardware after the
// program that set it is gone has to be asked for explicitly every time, so
// this backend writes the requested state on every open rather than leaving
// whatever the last run left behind.

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

// Direct sampling taps the RTL2832U's ADC ahead of the tuner, which is how an
// RTL-SDR v3 hears HF at all. The two branches are the I and Q inputs of the
// ADC; the v3 wires its HF input to Q. The values match
// rtlsdr_set_direct_sampling's argument so the mapping is one cast and not a
// table that can be got backwards.
enum class DirectSampling : std::uint8_t { Off = 0, IBranch = 1, QBranch = 2 };

// Everything the backend needs, already parsed. The URI grammar and the
// spelling of every key live in registry.cpp, which owns the text layer.
struct RtlSdrSourceConfig {
    // Echoed into SourceCapabilities::uri so a session can be reproduced from
    // what was written down.
    std::string uri;

    // Exactly one of these selects the device. See registry.cpp for the rule
    // that decides which, and rtlsdr_source.cpp for how a serial is resolved.
    bool by_serial = false;
    std::uint32_t index = 0;
    std::string serial;

    dsp::SampleRate rate = 2'400'000;

    // A source opened without freq= is tuned nowhere in particular and the
    // caller is expected to call tune(). Tracked separately from the value so
    // that freq=0, which is a legitimate request under direct sampling, is
    // not confused with silence.
    bool center_given = false;
    dsp::Hertz center_hz = 0;

    // Hardware AGC in the tuner. A number instead snaps to the nearest step
    // rtlsdr_get_tuner_gains reports and the achieved value is read back.
    bool gain_auto = true;
    double gain_db = 0.0;

    // Frequency correction, parts per million. librtlsdr applies this to the
    // resampler as well as to the tuner, so it moves the timebase too, which
    // is why a stated correction narrows the clock model's declared
    // tolerance.
    bool ppm_given = false;
    int ppm = 0;

    // The RTL2832U's digital AGC, which is a different loop from the tuner's.
    bool digital_agc = false;

    // Read the header. Off unless the URI said otherwise, every time.
    bool bias_tee = false;

    DirectSampling direct = DirectSampling::Off;

    // Only written to the device when true. rtlsdr_set_offset_tuning fails on
    // the R820T family whichever value it is handed, so asking for the state
    // the device is already in would turn the default into an error on the
    // most common dongle in the world.
    bool offset_tuning = false;
};

// One attached dongle, as enumeration sees it.
struct RtlSdrDevice {
    std::uint32_t index = 0;

    // From librtlsdr's USB id table, not from the device's string
    // descriptors: reading those means opening the device.
    std::string name;
};

// Attached dongles, opening none of them.
//
// rtlsdr_get_device_count and rtlsdr_get_device_name answer from the USB
// descriptors libusb already has, so this holds to the promise in registry.h
// that enumeration opens nothing and therefore still answers on a machine
// where opening would fail. The serial number is deliberately not read here,
// because rtlsdr_get_device_usb_strings opens the device to get it and would
// fail on a dongle that is already streaming, which is exactly when somebody
// is running a device list to find out what is going on.
[[nodiscard]] Expected<std::vector<RtlSdrDevice>> enumerate_rtlsdr_devices();

// Opens the device briefly to read the tuner type and its gain table, then
// closes it. That is what registry.h means by describe_sources costing more:
// the tune range and the gain steps are properties of the tuner IC, and the
// tuner IC does not answer over the USB descriptors.
//
// Fails when the device cannot be opened, rather than returning a plausible
// capability set with the tuner-dependent parts left empty. A GUI that reads
// an empty gain table has no way to tell "this tuner has no gain stages" from
// "nobody could ask".
[[nodiscard]] Expected<SourceCapabilities> describe_rtlsdr_source(const RtlSdrSourceConfig& config);

[[nodiscard]] Expected<std::unique_ptr<Source>> open_rtlsdr_source(const RtlSdrSourceConfig& config);

// The hardware's two sample-rate windows, as rtl-sdr.h documents them. Public
// so registry.cpp can name them in a message without restating the numbers.
inline constexpr dsp::SampleRate kRtlSdrLowRateMin = 225'001;
inline constexpr dsp::SampleRate kRtlSdrLowRateMax = 300'000;
inline constexpr dsp::SampleRate kRtlSdrHighRateMin = 900'001;
inline constexpr dsp::SampleRate kRtlSdrHighRateMax = 3'200'000;

// Above this the USB host controller on most machines cannot keep up and the
// dongle drops samples inside itself, where nothing can count them. It is not
// a hardware limit and the device accepts rates above it, so this is not a
// refusal, only the number the capability description and the documentation
// quote.
inline constexpr dsp::SampleRate kRtlSdrReliableRateMax = 2'560'000;

inline constexpr dsp::SampleRate kRtlSdrDefaultRate = 2'400'000;

[[nodiscard]] constexpr bool rtlsdr_rate_supported(dsp::SampleRate rate)
{
    return (rate >= kRtlSdrLowRateMin && rate <= kRtlSdrLowRateMax) ||
           (rate >= kRtlSdrHighRateMin && rate <= kRtlSdrHighRateMax);
}

}  // namespace revenant::source
