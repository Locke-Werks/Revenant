// What a client sees, in plain structs that depend on nothing.
//
// WHY THIS IS NOT JUST core/engine/engine.h
//
// The obvious client API hands back engine::EngineInfo and
// engine::SpectrumFrame, and it cannot: returning them means linking
// revenant_core, revenant_core is /MT, and the Qt process is /MD. That is
// the constraint the whole two-process split exists to satisfy, and a client
// header that reaches into the engine puts it straight back.
//
// So these are deliberate near-duplicates of the engine structs. The
// duplication is the cost of the process boundary and is bounded: the schema
// is the contract, these mirror the schema, and core/rpc/convert.cpp is the
// only place the engine side is touched. A field added to the schema and not
// here is a field the UI cannot see, which is a compile-time gap in the
// client rather than a silent zero.
//
// Nothing here includes capnp either, so a consumer can hold these without
// the generated header in its translation unit.
//
// Frequencies stay rational for the reason docs/conventions.md gives. A
// display converts once, where it draws the label.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace revenant::rpc {

struct Rational {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;

    [[nodiscard]] constexpr double hertz() const {
        return denominator == 0 ? 0.0
                                : static_cast<double>(numerator) /
                                      static_cast<double>(denominator);
    }
};

// Mirrors revenant::engine::Demod ordinal for ordinal. The static_asserts
// that hold that true live in core/rpc/convert.h, which is the only file
// that sees both this and the engine.
enum class Demod : std::uint8_t { Raw, Am, Nfm, Wfm, Usb, Lsb, Dsb, Cw };

struct DeviceInfo {
    std::uint32_t index = 0;
    std::string name;
    std::string vendor;
    bool discrete = false;
    std::uint32_t api_version = 0;
    std::uint32_t driver_version = 0;
};

struct GridParams {
    std::uint32_t channels = 0;
    std::uint32_t taps_per_branch = 0;
    std::uint32_t decimation = 0;
};

struct SpectrumGeometry {
    std::uint32_t transform = 0;
    std::uint32_t bins_per_channel = 0;
    std::uint32_t channels = 0;
    std::uint32_t bins = 0;
    Rational bin_width;
    Rational bin_zero;

    [[nodiscard]] constexpr bool enabled() const { return bins != 0; }
};

struct EngineInfo {
    DeviceInfo device;
    GridParams grid;
    std::uint32_t source_rate = 0;
    std::uint32_t channel_rate = 0;
    std::int64_t channel_spacing = 0;
    SpectrumGeometry spectrum;
    std::int64_t source_center = 0;
    std::uint64_t ring_samples = 0;
    double ring_seconds = 0.0;

    // The engine did not build what was asked for, and why. See the schema:
    // this is also how a clamped channel count or block size is reported,
    // because it is the only field in EngineInfo that can carry a sentence.
    bool ring_clamped = false;
    std::string ring_clamp_reason;
};

struct SourceDescriptor {
    std::string uri;
    std::string backend;
    std::string display_name;
    std::string unavailable;

    [[nodiscard]] bool available() const { return unavailable.empty(); }
};

struct SourceStats {
    std::uint64_t blocks_delivered = 0;
    std::uint64_t samples_delivered = 0;
    std::uint64_t overrun_events = 0;
    std::uint64_t samples_lost = 0;
    std::uint64_t last_loss_index = 0;
    std::uint64_t write_index = 0;
};

struct VrxParams {
    std::int64_t center = 0;
    std::int64_t bandwidth = 12'000;
    Demod demod = Demod::Nfm;
    std::uint32_t audio_rate = 0;
    double squelch_dbfs = -200.0;
    double agc_attack_ms = 10.0;
    double agc_decay_ms = 500.0;
    bool agc_enabled = true;
    std::int64_t cw_pitch = 700;
};

struct VrxPlacement {
    std::uint32_t channel = 0;
    Rational channel_centre;
    Rational residual;
    std::uint32_t channel_rate = 0;
    bool bandwidth_clamped = false;
};

struct VrxStatus {
    std::uint64_t id = 0;
    VrxParams params;
    VrxPlacement placement;
    double level_dbfs = -200.0;
    bool squelch_open = false;
    std::uint64_t audio_samples = 0;
    std::uint64_t audio_dropped = 0;
};

// Unlike engine::SpectrumFrame, this one owns its bins.
//
// The engine's version hands out a span valid only for the duration of the
// sink call, which is right there and wrong here: by the time a client sees
// a frame it has already been copied onto the wire, and handing a UI a span
// into a capnp message it does not own would be a lifetime bug waiting for
// the first consumer that keeps a row.
struct SpectrumFrame {
    std::vector<float> power_db;
    SpectrumGeometry geometry;
    std::uint64_t start = 0;
    std::uint64_t count = 0;

    // Frames the ENGINE produced before this one, which is not the number
    // this subscription received. A client that asked for decimation can
    // tell its own skipping from the engine's by watching this jump by more
    // than it asked for.
    std::uint64_t sequence = 0;

    float floor_db = 0.0F;
    float ceiling_db = 0.0F;
    float percentile_low_db = 0.0F;
    float percentile_high_db = 0.0F;
};

}  // namespace revenant::rpc
