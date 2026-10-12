// describe_capture: how the control row speaks about the engine's rolling
// capture.
//
// EVERY TEST HERE NAMES THE WRONG IMPLEMENTATION IT REJECTS, on the rule the
// rest of ui/tests follows. Here they are a readout on an engine with no
// capture, which is clutter on every ordinary session; a healthy capture drawn
// as a fault, which teaches the operator to ignore the colour; and a writer
// losing data drawn as healthy, which is a recording with holes nobody
// mentions.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "models/capture_note.h"

using revenant::ui::CaptureSample;
using revenant::ui::CaptureState;
using revenant::ui::describe_capture;
using revenant::ui::format_capture_bytes;

namespace {

[[nodiscard]] CaptureSample recording()
{
    CaptureSample sample;
    sample.on = true;
    sample.segments_written = 14;
    sample.segments_deleted = 2;
    sample.bytes_written = 1'800'000'000;
    sample.retained_bytes = 1'440'000'000;
    return sample;
}

[[nodiscard]] bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("a capture that is off says nothing", "[capture]")
{
    // Rejects a "capture off" readout: off is every engine started without
    // --capture, and a label that is always there is one nobody reads.
    const auto line = describe_capture(CaptureSample{});
    CHECK(line.state == CaptureState::Off);
    CHECK(line.text.empty());
    CHECK(line.detail.empty());
}

TEST_CASE("a healthy capture is a quiet readout of what is on disk", "[capture]")
{
    // Rejects counting segments ever written beside the retained size: the
    // ring deletes the oldest, and the two numbers must describe the same
    // files.
    const auto line = describe_capture(recording());
    CHECK(line.state == CaptureState::Recording);
    CHECK(line.text == "rec 1.4 GB, 12 seg");
    CHECK(contains(line.detail, "1.8 GB written"));
}

TEST_CASE("dropped blocks are a fault, named as the capture's", "[capture]")
{
    // Rejects a healthy readout while the writer is losing data, and rejects
    // wording that blames the live graph, which the drops do not touch.
    auto sample = recording();
    sample.blocks_dropped = 1;
    auto line = describe_capture(sample);
    CHECK(line.state == CaptureState::Fault);
    CHECK(line.text == "capture: 1 block dropped");
    CHECK(contains(line.detail, "live spectrum is unaffected"));

    sample.blocks_dropped = 3;
    CHECK(describe_capture(sample).text == "capture: 3 blocks dropped");
}

TEST_CASE("a writer error is a fault even once the capture calls itself off", "[capture]")
{
    // Rejects gating the error on captureOn: the writer that died is the one
    // that may have stopped saying it is on, and the schema keeps its error.
    CaptureSample sample;
    sample.last_error = "disk full";
    sample.blocks_dropped = 5;
    const auto line = describe_capture(sample);
    CHECK(line.state == CaptureState::Fault);
    CHECK(line.text == "capture stopped");
    CHECK(contains(line.detail, "disk full"));
    CHECK(contains(line.detail, "5 blocks dropped"));
}

TEST_CASE("sizes are decimal with one place past the kilobyte", "[capture]")
{
    CHECK(format_capture_bytes(0) == "0 B");
    CHECK(format_capture_bytes(999) == "999 B");
    CHECK(format_capture_bytes(1'500) == "1.5 kB");
    CHECK(format_capture_bytes(250'000'000) == "250.0 MB");
    CHECK(format_capture_bytes(2'000'000'000'000) == "2000.0 GB");
}
