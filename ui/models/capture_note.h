// What the engine's rolling capture is doing, as a readout for the control row.
//
// Qt-free, so ui/tests can link it, on the argument models/front_end_note.h
// makes: the label cannot be wrong, the rule can.
//
// QUIET UNLESS IT IS LOSING DATA. A capture that is running is a fact the
// operator chose on the engine's command line, so it gets a dim readout and
// nothing more: what it holds on disk and how many segments. A capture that is
// off says nothing at all, because off is the ordinary engine. The one state
// that speaks up is a writer that has dropped blocks or stopped on an error,
// since that is a hole in a recording the operator believes is complete, and
// nothing else on screen would tell them.
//
// Dropped blocks are a gap in the capture and not in the live graph
// (core/rpc/revenant.capnp, captureBlocksDropped), so the fault never reaches
// the spectrum's own wording; it is named as the capture's.

#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace revenant::ui {

// SourceStats' capture fields, copied across by the caller so this header
// needs no wire types.
struct CaptureSample {
    bool on = false;
    std::uint64_t segments_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t blocks_dropped = 0;
    std::uint64_t segments_deleted = 0;
    std::uint64_t retained_bytes = 0;
    std::string last_error;
};

enum class CaptureState : std::uint8_t { Off, Recording, Fault };

struct CaptureLine {
    CaptureState state = CaptureState::Off;

    // The readout: empty when off, "rec 1.4 GB, 12 seg" while recording,
    // "capture: 3 blocks dropped" or "capture stopped" on a fault.
    std::string text;

    // The longer sentence for the hover and the status drawer. The writer's
    // own error text verbatim when there is one.
    std::string detail;
};

// Decimal units, one decimal past the kilobyte, because the number is read
// against a disk's free space and disks are sold in decimal.
[[nodiscard]] inline std::string format_capture_bytes(std::uint64_t bytes)
{
    constexpr double kKilo = 1000.0;
    const auto value = static_cast<double>(bytes);
    if (value < kKilo) {
        return std::format("{} B", bytes);
    }
    if (value < kKilo * kKilo) {
        return std::format("{:.1f} kB", value / kKilo);
    }
    if (value < kKilo * kKilo * kKilo) {
        return std::format("{:.1f} MB", value / (kKilo * kKilo));
    }
    return std::format("{:.1f} GB", value / (kKilo * kKilo * kKilo));
}

[[nodiscard]] inline CaptureLine describe_capture(const CaptureSample& sample)
{
    CaptureLine line;

    // An error is reported even with captureOn false: the schema keeps the
    // last stream's figures, and a writer that died is exactly the one that
    // may no longer call itself on.
    if (!sample.last_error.empty()) {
        line.state = CaptureState::Fault;
        line.text = "capture stopped";
        line.detail = "the capture writer stopped: " + sample.last_error;
        if (sample.blocks_dropped > 0) {
            line.detail += std::format(" ({} blocks dropped before it did)",
                                       sample.blocks_dropped);
        }
        return line;
    }
    if (!sample.on) {
        return line;
    }
    // Segments on disk, not segments ever written: the ring deletes the
    // oldest, and the count beside the retained size has to describe the same
    // files the size does.
    const std::uint64_t kept = sample.segments_deleted < sample.segments_written
                                   ? sample.segments_written - sample.segments_deleted
                                   : 0;

    if (sample.blocks_dropped > 0) {
        const char* plural = sample.blocks_dropped == 1 ? "" : "s";
        line.state = CaptureState::Fault;
        line.text = std::format("capture: {} block{} dropped", sample.blocks_dropped, plural);
        line.detail = std::format(
            "the capture writer could not keep up and dropped {} block{}, so the recording on "
            "disk has gaps. The live spectrum is unaffected. {} retained in {} segments.",
            sample.blocks_dropped, plural, format_capture_bytes(sample.retained_bytes), kept);
        return line;
    }

    line.state = CaptureState::Recording;
    line.text = std::format("rec {}, {} seg", format_capture_bytes(sample.retained_bytes), kept);
    line.detail = std::format("capturing to disk: {} retained in {} segments, {} written in all",
                              format_capture_bytes(sample.retained_bytes), kept,
                              format_capture_bytes(sample.bytes_written));
    return line;
}

}  // namespace revenant::ui
