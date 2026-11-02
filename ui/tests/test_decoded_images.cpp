#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/rpc/types.h"
#include "models/decoded_images.h"

using namespace revenant;
using rpc::DecodedImageEvent;
using ui::DecodedImageChange;
using ui::DecodedImageState;
using ui::DecodedImageStore;

namespace {

DecodedImageEvent opened(std::uint64_t id, std::uint32_t width,
                         rpc::ImageColour colour = rpc::ImageColour::Gray8)
{
    DecodedImageEvent event;
    event.kind = DecodedImageEvent::Kind::Open;
    event.vrx = 3;
    event.decoder = "sstv";
    event.image_id = id;
    event.width = width;
    event.colour = colour;
    event.mode = "Robot 36";
    return event;
}

DecodedImageEvent row(std::uint64_t id, std::uint32_t index, std::vector<std::uint8_t> pixels)
{
    DecodedImageEvent event;
    event.kind = DecodedImageEvent::Kind::Row;
    event.vrx = 3;
    event.decoder = "sstv";
    event.image_id = id;
    event.row = index;
    event.pixels = std::move(pixels);
    return event;
}

DecodedImageEvent closed(std::uint64_t id, bool complete)
{
    DecodedImageEvent event;
    event.kind = DecodedImageEvent::Kind::Close;
    event.vrx = 3;
    event.decoder = "sstv";
    event.image_id = id;
    event.complete = complete;
    return event;
}

}  // namespace

TEST_CASE("an image grows a row at a time and closes", "[decoded_images]")
{
    DecodedImageStore store;
    const DecodedImageChange added = store.apply(opened(1, 4), 1000);
    CHECK(added.kind == DecodedImageChange::Kind::Added);
    CHECK(added.index == 0);
    REQUIRE(store.size() == 1);
    CHECK(store.at(0).height() == 0);
    CHECK(store.at(0).state == DecodedImageState::Receiving);
    CHECK(store.at(0).arrived_ms == 1000);

    CHECK(store.apply(row(1, 0, {1, 2, 3, 4}), 0).kind == DecodedImageChange::Kind::Updated);
    CHECK(store.at(0).height() == 1);
    store.apply(row(1, 1, {5, 6, 7, 8}), 0);
    CHECK(store.at(0).height() == 2);
    CHECK(store.at(0).pixels == std::vector<std::uint8_t>{1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(store.at(0).rows_received == 2);

    store.apply(closed(1, true), 0);
    CHECK(store.at(0).state == DecodedImageState::Complete);
    CHECK(std::string(store.at(0).state_text()) == "complete");

    // A second close changes nothing.
    CHECK(store.apply(closed(1, false), 0).kind == DecodedImageChange::Kind::None);
    CHECK(store.at(0).state == DecodedImageState::Complete);
}

TEST_CASE("a lost row is a black hole and a resent one overwrites", "[decoded_images]")
{
    DecodedImageStore store;
    store.apply(opened(1, 2), 0);
    store.apply(row(1, 2, {9, 9}), 0);
    CHECK(store.at(0).height() == 3);
    CHECK(store.at(0).pixels == std::vector<std::uint8_t>{0, 0, 0, 0, 9, 9});

    store.apply(row(1, 2, {7}), 0);
    CHECK(store.at(0).pixels == std::vector<std::uint8_t>{0, 0, 0, 0, 7, 9});
}

TEST_CASE("rgb rows are three bytes a pixel", "[decoded_images]")
{
    DecodedImageStore store;
    store.apply(opened(1, 2, rpc::ImageColour::Rgb888), 0);
    CHECK(store.at(0).row_bytes() == 6);
    store.apply(row(1, 0, {1, 2, 3, 4, 5, 6, 99}), 0);
    CHECK(store.at(0).height() == 1);
    CHECK(store.at(0).pixels.size() == 6);
}

TEST_CASE("events for an image never opened are dropped", "[decoded_images]")
{
    DecodedImageStore store;
    CHECK(store.apply(row(5, 0, {1}), 0).kind == DecodedImageChange::Kind::None);
    CHECK(store.apply(closed(5, true), 0).kind == DecodedImageChange::Kind::None);
    CHECK(store.size() == 0);
}

TEST_CASE("the store keeps the newest images and evicts from the front", "[decoded_images]")
{
    DecodedImageStore store;
    for (std::uint64_t id = 1; id <= ui::kDecodedImageCapacity; ++id) {
        store.apply(opened(id, 1), 0);
    }
    REQUIRE(store.size() == ui::kDecodedImageCapacity);
    const DecodedImageChange next = store.apply(opened(ui::kDecodedImageCapacity + 1, 1), 0);
    CHECK(next.kind == DecodedImageChange::Kind::Added);
    CHECK(next.evicted == 1);
    CHECK(next.index == ui::kDecodedImageCapacity - 1);
    CHECK(store.at(0).image_id == 2);
}

TEST_CASE("an ended subscription aborts what it left open", "[decoded_images]")
{
    DecodedImageStore store;
    store.apply(opened(1, 1), 0);
    store.apply(opened(2, 1), 0);
    store.apply(closed(1, true), 0);
    const auto changed = store.abort_open(3, "sstv");
    REQUIRE(changed.size() == 1);
    CHECK(changed.front() == 1);
    CHECK(store.at(1).state == DecodedImageState::Aborted);
    CHECK(store.at(0).state == DecodedImageState::Complete);
}

TEST_CASE("keys, labels and file names", "[decoded_images]")
{
    DecodedImageStore store;
    store.apply(opened(2, 1), 0);
    const auto& image = store.at(0);
    CHECK(image.key() == "3-sstv-2");
    CHECK(store.find("3-sstv-2") == 0);
    CHECK(store.find("3-sstv-9") == 1);
    CHECK(image.label() == "sstv Robot 36 rx3 #2");
    CHECK(image.file_name() == "sstv-rx3-2-Robot_36.png");
    CHECK(store.holds(3, "sstv", 2));
    CHECK_FALSE(store.holds(3, "fax", 2));
    store.drop_front();
    CHECK(store.size() == 0);
}
