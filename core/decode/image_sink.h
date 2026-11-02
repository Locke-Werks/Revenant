// Where an image decoder in core/decode hands its raster, a row at a time.
//
// The decoders here know nothing of the RPC layer. Each takes one of these
// and calls it as the image arrives; core/rpc/decoders.h implements it on top
// of ChunkDecoder's open_image, image_row and close_image, and a test
// implements it on top of a vector.
//
// The contract is the RPC one, restated so a decoder can be written against
// this header alone: open once, then rows of exactly width pixels (one byte
// each for Gray, R G B for Rgb), then close once, complete or not. Rows may
// arrive out of order and a row index may repeat with better data. A decoder
// never opens a second image before closing the first.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace revenant::decode {

enum class ImagePixels : std::uint8_t {
    Gray,
    Rgb,
};

class ImageSink {
public:
    virtual ~ImageSink() = default;

    // `start_sample` is the stream index the image began at, at the rate the
    // decoder was built for. `line_rate_hz` is the nominal rows per second.
    virtual void open(std::uint64_t start_sample, std::uint32_t width, ImagePixels pixels,
                      double line_rate_hz, std::string_view mode) = 0;
    virtual void row(std::uint32_t index, std::span<const std::uint8_t> pixels) = 0;
    virtual void close(bool complete) = 0;
};

}  // namespace revenant::decode
