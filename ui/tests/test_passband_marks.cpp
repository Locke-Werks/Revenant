// The detail spectrum's notch and carrier arithmetic: audio to offset and
// back by mode, hit-testing the drawn bands, and what "notch all" picks.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

#include "render/passband_marks.h"

using namespace revenant::ui;
using Catch::Matchers::WithinAbs;
using revenant::rpc::Demod;

TEST_CASE("an audio notch lands where the mode puts that audio", "[passband_marks]")
{
    const MarkOffsets usb = mark_offsets(Demod::Usb, 1'200.0, 0.0);
    REQUIRE(usb.count == 1);
    REQUIRE_THAT(usb.hz[0], WithinAbs(1'200.0, 1e-9));

    const MarkOffsets lsb = mark_offsets(Demod::Lsb, 1'200.0, 0.0);
    REQUIRE(lsb.count == 1);
    REQUIRE_THAT(lsb.hz[0], WithinAbs(-1'200.0, 1e-9));

    const MarkOffsets am = mark_offsets(Demod::Am, 900.0, 0.0);
    REQUIRE(am.count == 2);
    REQUIRE_THAT(am.hz[0], WithinAbs(-900.0, 1e-9));
    REQUIRE_THAT(am.hz[1], WithinAbs(900.0, 1e-9));

    const MarkOffsets cw = mark_offsets(Demod::Cw, 800.0, 700.0);
    REQUIRE(cw.count == 1);
    REQUIRE_THAT(cw.hz[0], WithinAbs(100.0, 1e-9));

    for (const Demod mode : {Demod::Nfm, Demod::Wfm, Demod::P25p1, Demod::Dmr}) {
        const MarkOffsets out = mark_offsets(mode, 1'000.0, 0.0);
        REQUIRE(out.count == 0);
        REQUIRE(out.audio_only);
    }
    REQUIRE_FALSE(mark_offsets(Demod::Raw, 1'000.0, 0.0).audio_only);
    REQUIRE(mark_offsets(Demod::Usb, 0.0, 0.0).count == 0);
}

TEST_CASE("an offset maps back to the audio it was placed from", "[passband_marks]")
{
    for (const Demod mode : {Demod::Usb, Demod::Lsb, Demod::Am, Demod::Sam, Demod::Dsb,
                             Demod::Cw}) {
        for (const double audio : {150.0, 1'000.0, 2'750.0}) {
            const MarkOffsets out = mark_offsets(mode, audio, 600.0);
            for (std::uint8_t i = 0; i < out.count; ++i) {
                CAPTURE(static_cast<int>(mode), audio, i);
                REQUIRE_THAT(audio_at_offset(mode, out.hz[i], 600.0), WithinAbs(audio, 1e-9));
            }
        }
    }

    // The discarded sideband and the audio-only modes are heard at nothing.
    REQUIRE(audio_at_offset(Demod::Usb, -500.0, 0.0) == 0.0);
    REQUIRE(audio_at_offset(Demod::Lsb, 500.0, 0.0) == 0.0);
    REQUIRE(audio_at_offset(Demod::Nfm, 500.0, 0.0) == 0.0);
    REQUIRE(audio_at_offset(Demod::Cw, -700.0, 700.0) == 0.0);
}

TEST_CASE("a click-to-notch width is clamped to a sane band", "[passband_marks]")
{
    REQUIRE(click_notch_width(3.0) == kClickNotchMinWidthHz);
    REQUIRE(click_notch_width(80.0) == 80.0);
    REQUIRE(click_notch_width(5'000.0) == kClickNotchMaxWidthHz);
}

TEST_CASE("the band under the pointer is the nearest one in reach", "[passband_marks]")
{
    const std::vector<NotchBandPx> bands{{100.0, 10.0, 0}, {115.0, 10.0, 1}, {300.0, 1.0, 2}};
    REQUIRE(notch_hit(bands, 100.0) == 0);
    REQUIRE(notch_hit(bands, 109.0) == 1);
    REQUIRE(notch_hit(bands, 200.0) == -1);

    // A one-pixel band is still grabbable.
    REQUIRE(notch_hit(bands, 304.0) == 2);
    REQUIRE(notch_hit(bands, 306.0) == -1);
}

TEST_CASE("the hover peak is the strongest bin near the pointer", "[passband_marks]")
{
    const std::vector<float> power{-120.0F, -118.0F, -90.0F, -119.0F, -121.0F, -122.0F,
                                   -120.0F, -120.0F, -120.0F, -120.0F};
    bool found = false;
    REQUIRE(hover_peak_db(power, 10.0, -50.0, -5.0, found) == -90.0F);
    REQUIRE(found);
    (void)hover_peak_db(power, 10.0, -50.0, 500.0, found);
    REQUIRE_FALSE(found);
}

TEST_CASE("notch all takes the strongest carriers into the four slots", "[passband_marks]")
{
    const std::vector<CarrierMark> carriers{{400.0, 30.0, 15.0F},  {1'200.0, 5.0, 30.0F},
                                            {-800.0, 40.0, 20.0F}, {2'000.0, 60.0, 25.0F},
                                            {600.0, 50.0, 18.0F},  {900.0, 50.0, 12.0F}};

    const std::vector<NotchRequest> usb = notch_all_requests(carriers, Demod::Usb, 0.0);
    REQUIRE(usb.size() == kNotchSlots);
    REQUIRE(usb[0].audio_hz == 1'200.0);
    REQUIRE(usb[0].width_hz == kClickNotchMinWidthHz);
    REQUIRE(usb[1].audio_hz == 2'000.0);
    REQUIRE(usb[2].audio_hz == 600.0);
    REQUIRE(usb[3].audio_hz == 400.0);

    // AM hears both sidebands at one audio frequency, so a mirror pair is one
    // notch.
    const std::vector<CarrierMark> mirror{{-700.0, 30.0, 20.0F}, {700.0, 30.0, 19.0F}};
    REQUIRE(notch_all_requests(mirror, Demod::Am, 0.0).size() == 1);

    REQUIRE(notch_all_requests(carriers, Demod::Nfm, 0.0).empty());
    REQUIRE(notch_all_requests(carriers, Demod::Raw, 0.0).empty());
}
