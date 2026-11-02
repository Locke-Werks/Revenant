// From a picture decoder's decode::ImageSink to a ChunkDecoder's image events.
//
// A decoder in core/decode reports images through decode::ImageSink and its
// own 32-bit image ids. ChunkDecoder's open_image, image_row and close_image
// are protected and number images themselves, so an adapter builds the sink
// here from three lambdas that call them, and this keeps the mapping between
// the two numberings. Used by the SSTV adapter; written to be reused by any
// other adapter for a decoder that emits through decode::ImageSink.
//
// This header does not include core/rpc/decoders.h, so decoders.h can
// include it.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>

#include "core/decode/image_sink.h"
#include "core/rpc/types.h"

namespace revenant::rpc {

[[nodiscard]] inline ImageColour image_colour(decode::ImageRow::Model model) {
    return model == decode::ImageRow::Gray8 ? ImageColour::Gray8 : ImageColour::Rgb888;
}

// `open` answers the id open_image gave; `row` and `close` take that id.
using ImageBridgeOpen =
    std::function<std::uint64_t(std::uint32_t width, ImageColour colour, double line_rate_hz,
                                std::string label)>;
using ImageBridgeRow =
    std::function<void(std::uint64_t image, std::uint32_t row, std::span<const std::uint8_t>)>;
using ImageBridgeClose = std::function<void(std::uint64_t image, bool complete)>;

[[nodiscard]] inline decode::ImageSink make_image_bridge(ImageBridgeOpen open, ImageBridgeRow row,
                                                         ImageBridgeClose close) {
    auto ids = std::make_shared<std::map<std::uint32_t, std::uint64_t>>();
    decode::ImageSink sink;
    sink.on_open = [ids, open = std::move(open)](const decode::ImageOpen& o) {
        (*ids)[o.image_id] = open(o.width, image_colour(o.model), o.line_rate_hz, o.label);
    };
    sink.on_row = [ids, row = std::move(row)](const decode::ImageRow& r) {
        if (const auto found = ids->find(r.image_id); found != ids->end()) {
            row(found->second, r.row, r.bytes);
        }
    };
    sink.on_close = [ids, close = std::move(close)](const decode::ImageClose& c) {
        if (const auto found = ids->find(c.image_id); found != ids->end()) {
            close(found->second, c.complete);
            ids->erase(found);
        }
    };
    return sink;
}

}  // namespace revenant::rpc
