// render/passband_marks.h repeats dsp::notch_offsets on the wire's Demod so
// the UI need not link the engine. This holds the copy to the original, mode
// by mode, so a change to one that misses the other fails here rather than
// drawing a notch where the engine does not apply it.

#include <catch2/catch_test_macros.hpp>

#include <array>

#include "core/dsp/notch_map.h"
#include "render/passband_marks.h"

namespace {

using revenant::engine::Demod;
namespace rpc = revenant::rpc;

constexpr std::array<Demod, 13> kModes{
    Demod::Raw, Demod::Am,  Demod::Nfm,   Demod::Wfm,   Demod::Usb, Demod::Lsb, Demod::Dsb,
    Demod::Cw,  Demod::P25p1, Demod::Dstar, Demod::Tetra, Demod::Dmr, Demod::Sam,
};

}  // namespace

TEST_CASE("the UI's notch table matches the engine's for every mode", "[passband_marks]")
{
    // The cast below relies on the two enums sharing their order.
    STATIC_REQUIRE(static_cast<int>(Demod::Sam) == static_cast<int>(rpc::Demod::Sam));

    for (const Demod mode : kModes) {
        for (const double audio : {-50.0, 0.0, 300.0, 1'234.5, 2'800.0}) {
            for (const double pitch : {0.0, 700.0}) {
                const auto engine = revenant::dsp::notch_offsets(mode, audio, pitch);
                const auto ui = revenant::ui::mark_offsets(static_cast<rpc::Demod>(mode), audio,
                                                           pitch);
                INFO("mode " << static_cast<int>(mode) << " audio " << audio << " pitch "
                             << pitch);
                REQUIRE(ui.count == engine.count);
                REQUIRE(ui.audio_only == engine.audio_only);
                for (std::uint8_t i = 0; i < engine.count; ++i) {
                    REQUIRE(ui.hz[i] == engine.hz[i]);
                }
            }
        }
    }
}
