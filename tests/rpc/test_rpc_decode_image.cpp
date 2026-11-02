// Decoded images, from a decoder adapter through the engine and out over a
// socket.
//
// No image decoder ships yet, so the path is exercised with "test_raster", an
// adapter registered only in this binary through register_test_decoder. It
// reads an nfm receiver's audio and ignores it: on the first chunk it opens a
// kWidth-wide gray8 image, hands over kRowsPerChunk rows of a known gradient
// per chunk after that, and closes the image complete after kHeight rows. A
// flush before then closes it aborted.
//
// So a pass says the adapter sink, the route's fan-out, the capnp schema, the
// conversion both ways and the client's per-subscription delivery carry an
// image intact: open first, every row once in order with the right bytes,
// close last.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/decoders.h"
#include "core/rpc/types.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/decoded_log.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;

namespace {

constexpr std::uint32_t kWidth = 64;
constexpr std::uint32_t kHeight = 32;
constexpr std::uint32_t kRowsPerChunk = 4;

[[nodiscard]] std::uint8_t gradient(std::uint32_t x, std::uint32_t y) {
    return static_cast<std::uint8_t>((x * 4U + y * 8U) & 0xFFU);
}

class TestRasterDecoder final : public rpc::ChunkDecoder {
public:
    static constexpr std::string_view kName = "test_raster";

    static Expected<std::unique_ptr<rpc::ChunkDecoder>> make(const rpc::DecoderBuild&) {
        return std::unique_ptr<rpc::ChunkDecoder>(std::make_unique<TestRasterDecoder>());
    }

    Status consume(const rpc::DecoderChunk& chunk,
                   std::vector<rpc::DecodedMessage>&) override {
        if (done_) {
            return {};
        }
        if (image_ == 0) {
            image_ = open_image(chunk, kWidth, rpc::ImageColour::Gray8, 2.0, "gradient");
        }
        std::vector<std::uint8_t> row(kWidth);
        for (std::uint32_t n = 0; n < kRowsPerChunk && next_row_ < kHeight; ++n, ++next_row_) {
            for (std::uint32_t x = 0; x < kWidth; ++x) {
                row[x] = gradient(x, next_row_);
            }
            image_row(image_, next_row_, row);
        }
        if (next_row_ == kHeight) {
            close_image(image_, true);
            done_ = true;
        }
        return {};
    }

    void flush(std::vector<rpc::DecodedMessage>&) override {
        if (image_ != 0 && !done_) {
            close_image(image_, false);
            done_ = true;
        }
    }

    void reset() override {}

private:
    std::uint64_t image_ = 0;
    std::uint32_t next_row_ = 0;
    bool done_ = false;
};

constexpr std::string_view kRasterModes[] = {"nfm"};

const rpc::DecoderSpec kRasterSpec{
    TestRasterDecoder::kName, rpc::DecoderInput::RealAudio,
    "A synthetic gradient, for the image path's wire test. Reads an nfm receiver's audio",
    &TestRasterDecoder::make, kRasterModes};

class ImageLog {
public:
    void record(const rpc::DecodedImageEvent& event) {
        const std::lock_guard<std::mutex> held(lock_);
        events_.push_back(event);
    }
    [[nodiscard]] std::vector<rpc::DecodedImageEvent> events() const {
        const std::lock_guard<std::mutex> held(lock_);
        return events_;
    }

private:
    mutable std::mutex lock_;
    std::vector<rpc::DecodedImageEvent> events_;
};

}  // namespace

TEST_CASE("an image decoder's rows cross the wire in order and the close arrives",
          "[gpu][rpc][decode][image]") {
    REVENANT_NEEDS_GPU();
    rpc::register_test_decoder(kRasterSpec);

    Harness harness;
    const auto ready = harness.open(HarnessOptions{});
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());

    auto vrx = harness.client().add_vrx(
        rpc::VrxParams{.center = 131'072, .bandwidth = 12'000, .demod = rpc::Demod::Nfm});
    INFO(test::message_of(vrx));
    REQUIRE(vrx.has_value());

    auto messages = std::make_shared<test::MessageLog>();
    auto images = std::make_shared<ImageLog>();
    auto resolved = harness.client().subscribe_decoded(
        *vrx, "test_raster", test::into(messages), test::ending(messages),
        [images](const rpc::DecodedImageEvent& event) { images->record(event); });
    INFO(test::message_of(resolved));
    REQUIRE(resolved.has_value());
    CHECK(*resolved == "test_raster");

    // Not listed to an operator.
    auto listed = harness.client().decoders();
    REQUIRE(listed.has_value());
    CHECK(std::ranges::none_of(*listed, [](const rpc::DecoderInfo& info) {
        return info.name == "test_raster";
    }));

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const auto closed = [](const std::vector<rpc::DecodedImageEvent>& seen) {
        return std::ranges::any_of(seen, [](const rpc::DecodedImageEvent& event) {
            return event.kind == rpc::DecodedImageEvent::Kind::Close;
        });
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(test::kDecodedWaitMs);
    std::vector<rpc::DecodedImageEvent> seen = images->events();
    while (!closed(seen) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        seen = images->events();
    }
    static_cast<void>(harness.stop_engine());

    REQUIRE(seen.size() == kHeight + 2);

    const rpc::DecodedImageEvent& open = seen.front();
    CHECK(open.kind == rpc::DecodedImageEvent::Kind::Open);
    CHECK(open.vrx == *vrx);
    CHECK(open.decoder == "test_raster");
    CHECK(open.image_id == 1);
    CHECK(open.width == kWidth);
    CHECK(open.colour == rpc::ImageColour::Gray8);
    CHECK(open.line_rate_hz == 2.0);
    CHECK(open.mode == "gradient");
    CHECK(open.sample_rate > 0);

    for (std::uint32_t y = 0; y < kHeight; ++y) {
        const rpc::DecodedImageEvent& row = seen[1 + y];
        INFO("row " << y);
        REQUIRE(row.kind == rpc::DecodedImageEvent::Kind::Row);
        CHECK(row.image_id == 1);
        CHECK(row.row == y);
        CHECK(row.dropped_before == 0);
        REQUIRE(row.pixels.size() == kWidth);
        bool exact = true;
        for (std::uint32_t x = 0; x < kWidth; ++x) {
            exact = exact && row.pixels[x] == gradient(x, y);
        }
        CHECK(exact);
    }

    const rpc::DecodedImageEvent& close = seen.back();
    CHECK(close.kind == rpc::DecodedImageEvent::Kind::Close);
    CHECK(close.image_id == 1);
    CHECK(close.complete);

    // Images ride beside messages and take nothing from them.
    CHECK(messages->messages().empty());
    CHECK_FALSE(messages->ended());
}
