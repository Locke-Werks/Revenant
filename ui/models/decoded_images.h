// The images image decoders have sent, held as raw rows a viewer can paint.
//
// Qt-free so ui/tests can assert it without a window, on the same terms as
// models/decoded_log.h. models/decoded_image_model.h wraps it for QML and
// turns an entry into a QImage.
//
// WHAT AN ENTRY IS. One (vrx, decoder, image_id), opened by an Open event.
// Rows land at their index, so a resent row overwrites and a lost one is a
// black line; the height is the highest row seen plus one, because a fax or
// SSTV frame is as tall as the transmission and nobody says so up front. A
// row or close for an image this store never saw opened is dropped: its open
// was lost or evicted, and without the width there is nothing to draw.
//
// THE CAP. kDecodedImageCapacity entries, oldest evicted first, open or not.
// An SSTV evening is a few dozen images and a fax a few hundred kilobytes
// each, so the cap is about memory and not about scrolling.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <string>
#include <vector>

#include "core/rpc/types.h"

namespace revenant::ui {

inline constexpr std::size_t kDecodedImageCapacity = 48;

enum class DecodedImageState : std::uint8_t { Receiving, Complete, Aborted };

struct DecodedImage {
    std::uint64_t vrx = 0;
    std::string decoder;
    std::uint64_t image_id = 0;

    std::uint32_t width = 0;
    rpc::ImageColour colour = rpc::ImageColour::Gray8;
    double line_rate_hz = 0.0;
    std::string mode;

    // Wall clock milliseconds when the open arrived, stamped by the caller.
    std::int64_t arrived_ms = 0;

    // height() rows of row_bytes() each, top to bottom.
    std::vector<std::uint8_t> pixels;
    std::uint32_t rows_received = 0;
    DecodedImageState state = DecodedImageState::Receiving;

    // Bumped on every change, so a view can tell its picture is stale.
    std::uint64_t revision = 0;

    [[nodiscard]] std::uint32_t row_bytes() const { return width * rpc::bytes_per_pixel(colour); }
    [[nodiscard]] std::uint32_t height() const
    {
        const std::uint32_t bytes = row_bytes();
        return bytes == 0 ? 0 : static_cast<std::uint32_t>(pixels.size() / bytes);
    }

    // "<vrx>-<decoder>-<id>", unique in the store and safe in a URL.
    [[nodiscard]] std::string key() const
    {
        return std::format("{}-{}-{}", vrx, decoder, image_id);
    }

    // "sstv robot36 rx3 #2", for a thumbnail caption.
    [[nodiscard]] std::string label() const
    {
        std::string out = decoder;
        if (!mode.empty()) {
            out += ' ';
            out += mode;
        }
        out += std::format(" rx{} #{}", vrx, image_id);
        return out;
    }

    [[nodiscard]] const char* state_text() const
    {
        switch (state) {
            case DecodedImageState::Receiving: return "receiving";
            case DecodedImageState::Complete: return "complete";
            case DecodedImageState::Aborted: return "aborted";
        }
        return "receiving";
    }

    // A file name for Save as PNG, with no character Windows refuses.
    [[nodiscard]] std::string file_name() const
    {
        std::string out = std::format("{}-rx{}-{}", decoder, vrx, image_id);
        if (!mode.empty()) {
            out += '-';
            out += mode;
        }
        for (char& c : out) {
            const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '-' || c == '_';
            if (!keep) {
                c = '_';
            }
        }
        return out + ".png";
    }
};

// What apply changed, for a list model to signal.
struct DecodedImageChange {
    enum class Kind : std::uint8_t { None, Added, Updated };
    Kind kind = Kind::None;

    // Index of the entry added or updated, after any eviction.
    std::size_t index = 0;

    // Entries evicted from the front before an add.
    std::size_t evicted = 0;
};

class DecodedImageStore {
public:
    [[nodiscard]] std::size_t size() const { return images_.size(); }
    [[nodiscard]] const DecodedImage& at(std::size_t index) const { return images_.at(index); }
    [[nodiscard]] const std::deque<DecodedImage>& images() const { return images_; }

    // Index of the entry with this key, or size().
    [[nodiscard]] std::size_t find(const std::string& key) const
    {
        for (std::size_t i = 0; i < images_.size(); ++i) {
            if (images_[i].key() == key) {
                return i;
            }
        }
        return images_.size();
    }

    DecodedImageChange apply(const rpc::DecodedImageEvent& event, std::int64_t now_ms)
    {
        using Kind = rpc::DecodedImageEvent::Kind;
        if (event.kind == Kind::Open) {
            return open(event, now_ms);
        }

        const std::size_t index = find_id(event.vrx, event.decoder, event.image_id);
        if (index == images_.size()) {
            return {};
        }
        DecodedImage& image = images_[index];
        if (event.kind == Kind::Row) {
            const std::uint32_t bytes = image.row_bytes();
            if (bytes == 0) {
                return {};
            }
            const std::size_t needed = (static_cast<std::size_t>(event.row) + 1) * bytes;
            if (image.pixels.size() < needed) {
                image.pixels.resize(needed, 0);
            }
            const std::size_t copy = std::min<std::size_t>(event.pixels.size(), bytes);
            std::copy_n(event.pixels.begin(), copy,
                        image.pixels.begin() + static_cast<std::ptrdiff_t>(
                                                   static_cast<std::size_t>(event.row) * bytes));
            ++image.rows_received;
        } else {
            if (image.state != DecodedImageState::Receiving) {
                return {};
            }
            image.state = event.complete ? DecodedImageState::Complete
                                         : DecodedImageState::Aborted;
        }
        ++image.revision;
        return {DecodedImageChange::Kind::Updated, index, 0};
    }

    // Marks every image still receiving on this subscription aborted, for a
    // subscription that ended without closing them. Answers the indices
    // changed.
    std::vector<std::size_t> abort_open(std::uint64_t vrx, const std::string& decoder)
    {
        std::vector<std::size_t> changed;
        for (std::size_t i = 0; i < images_.size(); ++i) {
            DecodedImage& image = images_[i];
            if (image.vrx == vrx && image.decoder == decoder &&
                image.state == DecodedImageState::Receiving) {
                image.state = DecodedImageState::Aborted;
                ++image.revision;
                changed.push_back(i);
            }
        }
        return changed;
    }

    [[nodiscard]] bool holds(std::uint64_t vrx, const std::string& decoder,
                             std::uint64_t id) const
    {
        return find_id(vrx, decoder, id) != images_.size();
    }

    // The oldest entry, for a list model that signals the eviction itself
    // before the add that would have made it.
    void drop_front()
    {
        if (!images_.empty()) {
            images_.pop_front();
        }
    }

    void clear() { images_.clear(); }

private:
    [[nodiscard]] std::size_t find_id(std::uint64_t vrx, const std::string& decoder,
                                      std::uint64_t id) const
    {
        for (std::size_t i = 0; i < images_.size(); ++i) {
            const DecodedImage& image = images_[i];
            if (image.vrx == vrx && image.image_id == id && image.decoder == decoder) {
                return i;
            }
        }
        return images_.size();
    }

    DecodedImageChange open(const rpc::DecodedImageEvent& event, std::int64_t now_ms)
    {
        // A second open for an id already held replaces it in place: the
        // decoder was rebuilt and is counting from 1 again.
        const std::size_t existing = find_id(event.vrx, event.decoder, event.image_id);
        DecodedImage image;
        image.vrx = event.vrx;
        image.decoder = event.decoder;
        image.image_id = event.image_id;
        image.width = event.width;
        image.colour = event.colour;
        image.line_rate_hz = event.line_rate_hz;
        image.mode = event.mode;
        image.arrived_ms = now_ms;
        if (existing != images_.size()) {
            image.revision = images_[existing].revision + 1;
            images_[existing] = std::move(image);
            return {DecodedImageChange::Kind::Updated, existing, 0};
        }

        std::size_t evicted = 0;
        while (images_.size() >= kDecodedImageCapacity) {
            images_.pop_front();
            ++evicted;
        }
        images_.push_back(std::move(image));
        return {DecodedImageChange::Kind::Added, images_.size() - 1, evicted};
    }

    std::deque<DecodedImage> images_;
};

}  // namespace revenant::ui
