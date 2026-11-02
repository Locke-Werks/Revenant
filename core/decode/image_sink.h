// Image output from the picture decoders: rows, plus an open and a close
// around each image.
//
// A decoder-local interface. The engine's image RPC path adapts these three
// callbacks; nothing here knows about it. An image is opened before its first
// row and closed after its last, rows arrive in order from 0, and every row of
// one image has the width and colour model given when it was opened.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace revenant::decode {

struct ImageRow {
    std::uint32_t image_id = 0;
    std::uint32_t row = 0;
    std::uint32_t width = 0;
    enum Model { Gray8, Rgb888 } model = Gray8;
    // width bytes for Gray8, 3 * width for Rgb888.
    std::vector<std::uint8_t> bytes;
};

struct ImageOpen {
    std::uint32_t image_id = 0;
    std::uint32_t width = 0;
    ImageRow::Model model = ImageRow::Gray8;
    // Short human-readable name of what the image shows, e.g. "NOAA APT".
    std::string label;
    // Nominal rows per second, for a display that scrolls in real time.
    double line_rate_hz = 0.0;
};

struct ImageClose {
    std::uint32_t image_id = 0;
    std::uint32_t rows = 0;
    // False when the image ended on lost signal rather than its natural end.
    bool complete = false;
};

struct ImageSink {
    std::function<void(const ImageOpen&)> on_open;
    std::function<void(const ImageRow&)> on_row;
    std::function<void(const ImageClose&)> on_close;
};

}  // namespace revenant::decode
