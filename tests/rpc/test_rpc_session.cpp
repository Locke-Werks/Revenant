// The session surface, over a real socket.
//
// Every case here drives a real rpc::Client against a real rpc::Server on an
// ephemeral loopback port. Nothing is stubbed, because the defects this layer
// can have are all in the seams rather than in the arithmetic: a field
// dropped on the way into the schema reads as a zero the caller cannot
// distinguish from a real one, and a frequency rounded to an integer hertz
// reads as a tuning offset nobody can source. Both are invisible to a test
// that calls the conversion functions directly with values it chose.
//
// THE SOURCE RATE IS 2400032 AND EVERY FRACTIONAL ASSERTION HERE RESTS ON IT
//
// tests/rpc/rpc_fixture.h chooses it and gives the reasoning in full. The
// short version: 2400032 is 32 * 75001, so the grid's decimation of 32
// divides it and the channel rate is a whole 75001 S/s, while the channel
// count of 64 does not, which puts every odd channel's centre on a half
// hertz. A bin is 2400032 / (32 * 256) hertz, which is 75001/256.
//
// A round 2400000 would put channel k at 37500 * k hertz and bin zero at
// -1218750, both whole, and a schema that carried hertz as an integer would
// then be indistinguishable here from one that did not. The cases below
// assert that the values they receive are fractional for exactly that
// reason, so a fixture edited to a round rate fails loudly instead.
//
// This header used to name 2400001, and no suite that adds a receiver could
// have run at it: engine::place refuses any rate the grid's decimation does
// not divide, because the channel stream's rate has to be a whole number of
// samples per second, and 32 does not divide an odd number. The rate has to
// be a multiple of D and not of M, which is the one bit of freedom a
// 2x-oversampled grid leaves.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "core/engine/engine.h"
#include "core/engine/vrx.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/server.h"
#include "core/rpc/types.h"
#include "core/source/registry.h"
#include "tests/reference/gpu_fixture.h"
#include "tests/reference/reference_diff.h"
#include "tests/rpc/rpc_fixture.h"

using namespace revenant;
using test::Harness;
using test::HarnessOptions;
using test::kChannels;
using test::kOverLargeSpectrumTransform;
using test::kSourceRate;
using test::kSpectrumTransform;
using test::kUnclampedRingSeconds;
using test::scene_uri;

namespace {

// Opens a harness or fails the case with the layer that would not come up.
void bring_up(Harness& harness, const HarnessOptions& options) {
    const auto ready = harness.open(options);
    INFO(test::message_of(ready));
    REQUIRE(ready.has_value());
}

// A receiver whose every field differs from the struct's default.
//
// That is what makes a field dropped in conversion visible: a copy that
// forgot one of these comes back carrying types.h's default rather than
// what was sent, and a test built on defaults could not tell the two apart.
// Every double here is exactly representable in binary, so comparing them
// for equality is a statement about the wire and not about rounding.
[[nodiscard]] rpc::VrxParams distinctive_params() {
    rpc::VrxParams params;
    params.center = 262'144;
    params.bandwidth = 9'376;
    params.demod = rpc::Demod::Usb;
    params.audio_rate = 16'000;
    params.squelch_dbfs = -73.25;
    params.agc_attack_ms = 3.5;
    params.agc_decay_ms = 812.25;
    params.agc_enabled = false;
    params.cw_pitch = 613;
    params.passband_low = -4'313;
    params.passband_high = 5'063;
    return params;
}

void check_params_match(const rpc::VrxParams& got, const rpc::VrxParams& sent) {
    CHECK(got.center == sent.center);
    CHECK(got.bandwidth == sent.bandwidth);
    CHECK(got.demod == sent.demod);
    CHECK(got.audio_rate == sent.audio_rate);
    CHECK(got.squelch_dbfs == sent.squelch_dbfs);
    CHECK(got.agc_attack_ms == sent.agc_attack_ms);
    CHECK(got.agc_decay_ms == sent.agc_decay_ms);
    CHECK(got.agc_enabled == sent.agc_enabled);
    CHECK(got.cw_pitch == sent.cw_pitch);
    CHECK(got.passband_low == sent.passband_low);
    CHECK(got.passband_high == sent.passband_high);
}

// The ordinary channel width per mode, so each one is added with something it
// can actually be built at. Mode is the field under test here; a bandwidth
// the stage refuses would fail the case for an unrelated reason.
[[nodiscard]] std::int64_t bandwidth_for(rpc::Demod mode) {
    switch (mode) {
        case rpc::Demod::Raw: return 12'000;
        case rpc::Demod::Am: return 10'000;
        case rpc::Demod::Nfm: return 16'000;
        case rpc::Demod::Wfm: return 200'000;
        case rpc::Demod::Usb:
        case rpc::Demod::Lsb: return 3'000;
        case rpc::Demod::Dsb: return 6'000;
        case rpc::Demod::Cw: return 500;

        // The three digital voice modes, at their own channel widths:
        // 12.5 kHz for P25 Phase 1, 6.25 kHz for D-STAR DV and 25 kHz for
        // TETRA. core/dsp/vrx_reference.cpp's default_passband cites the
        // clause behind each.
        case rpc::Demod::P25p1: return 12'500;
        case rpc::Demod::Dstar: return 6'000;
        case rpc::Demod::Tetra: return 25'000;
    }
    return 12'000;
}

[[nodiscard]] const char* mode_name(rpc::Demod mode) {
    return engine::demod_name(static_cast<engine::Demod>(mode));
}

}  // namespace

TEST_CASE("the engine's own numbers cross the wire unrounded", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    Harness harness;
    bring_up(harness, HarnessOptions{.spectrum_transform = kSpectrumTransform});

    const engine::EngineInfo& local = harness.engine().info();

    auto remote = harness.client().info();
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    CHECK(remote->device.index == local.device.index);
    CHECK(remote->device.name == local.device.name);
    CHECK(remote->device.vendor == local.device.vendor_name());
    CHECK(remote->device.discrete ==
          (local.device.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU));
    CHECK(remote->device.api_version == local.device.api_version);
    CHECK(remote->device.driver_version == local.device.driver_version);

    CHECK(remote->grid.channels == local.grid.channels);
    CHECK(remote->grid.taps_per_branch == local.grid.taps_per_branch);
    CHECK(remote->grid.decimation == local.grid.decimation);

    CHECK(remote->source_rate == local.source_rate);
    CHECK(remote->channel_rate == local.channel_rate);
    CHECK(remote->channel_spacing == local.channel_spacing);
    CHECK(remote->source_center == local.source_center);
    CHECK(remote->ring_samples == local.ring.capacity_samples);
    CHECK(remote->ring_seconds == local.ring.seconds_retained);

    // Stated before it is compared, because comparing two empty strings is
    // not a test of anything. HarnessOptions::ring_seconds is half a second
    // at a rate whose product is not a power of two, so this engine is
    // always clamped and the sentence is always there to carry. A fixture
    // edited to a ring the device grants in full makes this fire instead of
    // quietly turning the two lines below into a tautology.
    INFO("the engine said: " << local.ring.clamp_reason);
    REQUIRE(local.ring.clamped);
    REQUIRE_FALSE(local.ring.clamp_reason.empty());

    CHECK(remote->ring_clamped == local.ring.clamped);
    CHECK(remote->ring_clamp_reason == local.ring.clamp_reason);

    // The rate this suite runs at, restated as an assertion, because every
    // fractional value below depends on it and a fixture edited to a round
    // rate would quietly turn the rest of this case into a tautology.
    REQUIRE(local.source_rate == kSourceRate);

    REQUIRE(local.spectrum.enabled());
    CHECK(remote->spectrum.transform == local.spectrum.transform);
    CHECK(remote->spectrum.bins_per_channel == local.spectrum.bins_per_channel);
    CHECK(remote->spectrum.channels == local.spectrum.channels);
    CHECK(remote->spectrum.bins == local.spectrum.bins);

    CHECK(remote->spectrum.bin_width.numerator == local.spectrum.bin_width_numerator);
    CHECK(remote->spectrum.bin_width.denominator == local.spectrum.bin_width_denominator);
    CHECK(remote->spectrum.bin_zero.numerator == local.spectrum.bin_zero_numerator);
    CHECK(remote->spectrum.bin_zero.denominator == local.spectrum.bin_zero_denominator);

    // A bin is rate / (D * N) hertz wide. Checked by cross-multiplication
    // rather than by dividing, so it is exact whatever the rational was
    // reduced to, and so it says the value is right rather than merely that
    // it matches whatever the engine happened to hold.
    const std::int64_t points =
        static_cast<std::int64_t>(local.grid.decimation) * local.spectrum.transform;
    INFO(std::format("bin width {} / {} hertz over {} points", remote->spectrum.bin_width.numerator,
                     remote->spectrum.bin_width.denominator, points));
    REQUIRE(remote->spectrum.bin_width.denominator != 0);
    CHECK(remote->spectrum.bin_width.numerator * points ==
          local.source_rate * remote->spectrum.bin_width.denominator);

    // And it is not a whole number of hertz, which is the case the schema was
    // shaped to carry. If this ever passes trivially the rate above changed.
    CHECK(remote->spectrum.bin_width.numerator % remote->spectrum.bin_width.denominator != 0);
    REQUIRE(remote->spectrum.bin_zero.denominator != 0);
    CHECK(remote->spectrum.bin_zero.numerator % remote->spectrum.bin_zero.denominator != 0);
}

TEST_CASE("a clamp the engine applied reaches the client as a sentence", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();
    INFO("running on " << test::shared_context_description());

    // The engine never fails because a device is small. It shrinks the ring,
    // the block size or the spectrum transform to fit and records what it
    // did, and core/engine/engine.cpp overloads RingGeometry::clamp_reason
    // to carry all of them because it is the only field in EngineInfo that
    // can hold a sentence. A client that cannot read it asked for one
    // geometry, got another, and has nothing to say so.
    //
    // The spectrum transform is the shape this asks for, because it is the
    // one that clamps identically on both devices in the matrix. The channel
    // count is the shape engine.cpp's own comment names and it is not
    // reachable here: the device ceiling on both is 4096 channels, while
    // core/dsp/pfb_design.cpp refuses more than 2048, so a request above the
    // cap fails Engine::open_source rather than arriving as a clamp.
    // Measured 2026-09-19 on the RTX 4090: revenant-cli --channels 8192
    // reports "channel count 4096 exceeds 2048", which is the device clamp
    // having already run and validate rejecting what it produced.

    // The control arm, and it is not decoration. Every assertion in the
    // clamped arm below would also pass against a server that hardwired the
    // flag true and the reason to some fixed string, so the same engine
    // configured to be satisfiable has to come back saying nothing was taken
    // away. It asks for the same ring as the clamped arm, which is how the
    // ring is ruled out as the source of the reason down there.
    {
        Harness unclamped;
        bring_up(unclamped, HarnessOptions{.spectrum_transform = kSpectrumTransform,
                                           .ring_seconds = kUnclampedRingSeconds});

        const engine::EngineInfo& local = unclamped.engine().info();
        INFO(std::format("a ring of {} samples, {:.4f} s retained, and the engine said: {}",
                         local.ring.capacity_samples, local.ring.seconds_retained,
                         local.ring.clamp_reason));
        REQUIRE_FALSE(local.ring.clamped);

        auto remote = unclamped.client().info();
        INFO(test::message_of(remote));
        REQUIRE(remote.has_value());
        CHECK_FALSE(remote->ring_clamped);
        CHECK(remote->ring_clamp_reason.empty());

        // And it got the geometry it asked for, which is what makes the
        // reduction below a reduction rather than the only thing this engine
        // can build.
        CHECK(remote->spectrum.transform == kSpectrumTransform);
    }

    Harness harness;
    bring_up(harness, HarnessOptions{.spectrum_transform = kOverLargeSpectrumTransform,
                                     .ring_seconds = kUnclampedRingSeconds});

    const engine::EngineInfo& local = harness.engine().info();
    INFO("the engine said: " << local.ring.clamp_reason);

    // The engine's own view first. If this is not set the case is testing
    // the wire against an engine that was never clamped, and the assertions
    // after it would be about two matching empties.
    REQUIRE(local.ring.clamped);
    REQUIRE_FALSE(local.ring.clamp_reason.empty());

    auto remote = harness.client().info();
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    CHECK(remote->ring_clamped);
    CHECK_FALSE(remote->ring_clamp_reason.empty());

    // Byte for byte. A reason summarised, truncated to a Text field's
    // default, or replaced by a generic "the engine clamped something" at
    // the boundary is the same failure as dropping it: the number that was
    // asked for and the number that was settled on are the whole content.
    CHECK(remote->ring_clamp_reason == local.ring.clamp_reason);

    INFO("the client saw: " << remote->ring_clamp_reason);
    CHECK(remote->ring_clamp_reason.find(std::to_string(kOverLargeSpectrumTransform)) !=
          std::string::npos);
    CHECK(remote->ring_clamp_reason.find(std::to_string(local.spectrum.transform)) !=
          std::string::npos);

    // The geometry that arrived is the reduced one, which is true whatever
    // anyone worded the sentence as. A client that sized a waterfall from
    // the transform it asked for would draw a frame twice the width of the
    // one the engine sends.
    REQUIRE(local.spectrum.enabled());
    CHECK(local.spectrum.transform < kOverLargeSpectrumTransform);
    CHECK(remote->spectrum.transform == local.spectrum.transform);
    CHECK(remote->spectrum.bins == local.spectrum.bins);
    CHECK(remote->spectrum.bins_per_channel == remote->spectrum.transform / 2);
    CHECK(remote->spectrum.bins == kChannels * remote->spectrum.bins_per_channel);
}

TEST_CASE("an asymmetric passband is placed and read back over the wire", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // The shape a width cannot express: a USB receiver whose band starts 300
    // hertz above the suppressed carrier and ends 2700 above it, so neither
    // edge is a half bandwidth from the tuned frequency and the band does not
    // contain the carrier at all.
    constexpr std::int64_t kLow = 300;
    constexpr std::int64_t kHigh = 2'700;

    rpc::VrxParams params;
    params.center = 187'500;
    params.demod = rpc::Demod::Usb;
    params.passband_low = kLow;
    params.passband_high = kHigh;

    // Deliberately left at the struct default and deliberately wrong for
    // this receiver. The pair wins, and this is what proves the shorthand is
    // ignored rather than quietly averaged in.
    REQUIRE(params.bandwidth == 12'000);

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    auto remote = harness.client().vrx_status(*id);
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    CHECK(remote->params.passband_low == kLow);
    CHECK(remote->params.passband_high == kHigh);

    // Nothing here is near the channel's limit, so the grant is the request
    // and the clamp says nothing happened.
    CHECK(remote->placement.granted_low == kLow);
    CHECK(remote->placement.granted_high == kHigh);
    CHECK_FALSE(remote->placement.bandwidth_clamped);

    // The engine's own view, so the wire is checked against what was built
    // rather than against itself.
    auto local = harness.engine().vrx_status(engine::VrxId{static_cast<std::uint32_t>(*id)});
    INFO(test::message_of(local));
    REQUIRE(local.has_value());
    CHECK(local->placement.granted_low == remote->placement.granted_low);
    CHECK(local->placement.granted_high == remote->placement.granted_high);

    // The rate the passband display spans, which a client cannot derive
    // because the audio rate a receiver ended up with is the engine's
    // default when the request named none.
    CHECK(remote->demod_rate == local->demod_rate);
    CHECK(remote->demod_rate > 0);

    // A PAN IS IN PLACE AND A WIDEN IS NOT, WHICH IS THE WHOLE OF WHAT A
    // DRAGGING SURFACE HAS TO KNOW.
    //
    // Sliding a filter of a fixed width across the band keeps the
    // demodulation rate and the tap count, so it is a push constant and a
    // new tap table and the engine takes it without the audio noticing.
    rpc::VrxParams panned = params;
    panned.passband_low = kLow + 200;
    panned.passband_high = kHigh + 200;

    const auto applied = harness.client().set_vrx_params(*id, panned);
    INFO(test::message_of(applied));
    REQUIRE(applied.has_value());

    auto after = harness.client().vrx_status(*id);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    CHECK(after->params.passband_low == kLow + 200);
    CHECK(after->params.passband_high == kHigh + 200);
    CHECK(after->placement.granted_low == kLow + 200);
    CHECK(after->placement.granted_high == kHigh + 200);
    CHECK(after->demod_rate == remote->demod_rate);

    // Changing the WIDTH is refused in place, and this is not a rate
    // boundary being crossed: the tap count moves with every width, because
    // Kaiser sets the filter's length from its transition and the
    // transition is half a width. So a widen is a remove and an add however
    // small it is, and the engine says so rather than storing the request
    // over a stage still running the old filter.
    rpc::VrxParams wider = params;
    wider.passband_low = 100;
    wider.passband_high = 3'900;

    const auto refused = harness.client().set_vrx_params(*id, wider);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("remove and an add") != std::string::npos);

    // Refused means nothing moved, rather than half of it having moved.
    auto unchanged = harness.client().vrx_status(*id);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->params.passband_low == kLow + 200);
    CHECK(unchanged->params.passband_high == kHigh + 200);
}

TEST_CASE("a passband wider than one channel is fitted at the edge that does not fit",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto info = harness.client().info();
    INFO(test::message_of(info));
    REQUIRE(info.has_value());
    const std::int64_t channel_rate = info->channel_rate;
    REQUIRE(channel_rate > 0);

    // On a channel centre, so the residual is zero and the two edges have
    // the same room. One edge is asked for more than the channel carries and
    // the other is well inside, which is the case a single width could not
    // report: it would have taken the same amount off both.
    rpc::VrxParams params;
    params.center = 0;
    params.demod = rpc::Demod::Usb;
    params.passband_low = -1'000;
    params.passband_high = channel_rate;

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    auto status = harness.client().vrx_status(*id);
    INFO(test::message_of(status));
    REQUIRE(status.has_value());

    CHECK(status->placement.bandwidth_clamped);
    CHECK(status->placement.granted_low == -1'000);
    CHECK(status->placement.granted_high < params.passband_high);
    CHECK(status->placement.granted_high > 0);

    // The request is echoed unchanged, so a display can draw both and say
    // which edge moved rather than only that something did.
    CHECK(status->params.passband_high == params.passband_high);
}

TEST_CASE("a channel centre that is not a whole hertz survives exactly", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // Seven channel spacings up. It has to be an ODD channel: at a rate that
    // is a multiple of the decimation and not of the channel count, which is
    // the only shape engine::place accepts that is not a whole hertz per
    // channel, k * rate / M lands on a half hertz for odd k and on a whole
    // one for even k. Channel zero would be zero over one on any rate at all.
    rpc::VrxParams params = distinctive_params();
    params.center = 7 * (kSourceRate / kChannels);

    auto id = harness.client().add_vrx(params);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    auto remote = harness.client().vrx_status(*id);
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    auto local = harness.engine().vrx_status(engine::VrxId{static_cast<std::uint32_t>(*id)});
    INFO(test::message_of(local));
    REQUIRE(local.has_value());

    // Stated rather than assumed. An edit that moved the centre onto an even
    // channel would make every fractionality check below pass vacuously.
    INFO("placed on channel " << remote->placement.channel);
    REQUIRE(remote->placement.channel % 2 == 1);

    CHECK(remote->placement.channel == local->placement.channel);
    CHECK(remote->placement.channel_rate == local->placement.channel_rate);
    CHECK(remote->placement.bandwidth_clamped == local->placement.bandwidth_clamped);

    CHECK(remote->placement.channel_centre.numerator == local->placement.channel_centre.numerator);
    CHECK(remote->placement.channel_centre.denominator ==
          local->placement.channel_centre.denominator);
    CHECK(remote->placement.residual.numerator == local->placement.residual_numerator);
    CHECK(remote->placement.residual.denominator == local->placement.residual_denominator);

    // Channel k's centre is k * rate / M exactly. Cross-multiplied for the
    // same reason as the bin width above.
    const rpc::Rational centre = remote->placement.channel_centre;
    INFO(std::format("channel {} centre {} / {} hertz", remote->placement.channel,
                     centre.numerator, centre.denominator));
    REQUIRE(centre.denominator != 0);
    CHECK(centre.numerator * kChannels ==
          static_cast<std::int64_t>(remote->placement.channel) * kSourceRate *
              centre.denominator);

    // The failure this schema was shaped to prevent. A centre rounded
    // anywhere on the way out would arrive with denominator 1 and a value
    // some fraction of a hertz off, which is invisible in a display and
    // unsourceable afterwards.
    CHECK(centre.denominator != 1);
    CHECK(centre.numerator % centre.denominator != 0);
}

TEST_CASE("every receiver parameter survives a round trip", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    const rpc::VrxParams sent = distinctive_params();

    auto id = harness.client().add_vrx(sent);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    auto added = harness.client().vrx_status(*id);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());
    CHECK(added->id == *id);
    check_params_match(added->params, sent);

    // The other direction of the same conversion. setVrxParams reads the
    // schema on the engine's side and vrxStatus writes it back, so a field
    // dropped in only one of the two shows up here and not above.
    //
    // The mode stays. The engine refuses a demodulator change in place,
    // because the demodulator is the stage, and that refusal is asserted
    // below rather than worked around silently. The mode's own conversion on
    // this path is covered by the case that sends an ordinal the engine does
    // not know through setVrxParams and watches it be refused.
    //
    // The AUDIO RATE stays as well, and that is newer. A change to it moves
    // the demodulation rate, which is a different pipeline and a different
    // ring, and the engine now refuses that in place rather than storing
    // the request and letting the stage quietly keep its old filter. This
    // case used to change it to 24000 and assert the call succeeded, which
    // it did: the params were echoed back, the audio did not change, and
    // nothing anywhere said so. The refusal is asserted on its own below.
    //
    // The passband below MOVES WITHOUT CHANGING WIDTH, and that is not a
    // detail of this case. A width change moves the filter's tap count,
    // because Kaiser sets the length from the transition and the transition
    // is half a width, and a different tap count is a different pipeline:
    // the engine refuses it in place. A pan of a fixed width keeps both the
    // tap count and the rate, so it is a push constant and a new tap table,
    // and it is what this case is here to round trip. The refusal a widen
    // gets is asserted on its own below and in the asymmetric-passband case
    // above.
    rpc::VrxParams changed = sent;
    changed.center = 131'072;
    changed.bandwidth = 5'127;
    changed.squelch_dbfs = -41.5;
    changed.agc_attack_ms = 17.25;
    changed.agc_decay_ms = 250.5;
    changed.agc_enabled = true;
    changed.cw_pitch = 421;
    changed.passband_low = sent.passband_low + 500;
    changed.passband_high = sent.passband_high + 500;

    const auto applied = harness.client().set_vrx_params(*id, changed);
    INFO(test::message_of(applied));
    REQUIRE(applied.has_value());

    auto after = harness.client().vrx_status(*id);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());
    check_params_match(after->params, changed);

    // And the engine holds what the wire said it does, rather than the two
    // agreeing with each other about something neither applied.
    auto local = harness.engine().vrx_status(engine::VrxId{static_cast<std::uint32_t>(*id)});
    REQUIRE(local.has_value());
    CHECK(local->params.center == changed.center);
    CHECK(local->params.bandwidth == changed.bandwidth);
    CHECK(static_cast<std::uint16_t>(local->params.demod) ==
          static_cast<std::uint16_t>(changed.demod));
    CHECK(local->params.audio_rate == changed.audio_rate);
    CHECK(local->params.squelch_dbfs == changed.squelch_dbfs);
    CHECK(local->params.agc_attack_ms == changed.agc_attack_ms);
    CHECK(local->params.agc_decay_ms == changed.agc_decay_ms);
    CHECK(local->params.agc_enabled == changed.agc_enabled);
    CHECK(local->params.cw_pitch == changed.cw_pitch);
    CHECK(local->params.passband_low == changed.passband_low);
    CHECK(local->params.passband_high == changed.passband_high);

    // And the rule the paragraph above rests on, so that an engine which
    // later allows a mode change in place makes this case say so rather than
    // leaving a comment that has quietly become false.
    rpc::VrxParams retimbred = changed;
    retimbred.demod = rpc::Demod::Lsb;
    const auto refused = harness.client().set_vrx_params(*id, retimbred);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("cannot become") != std::string::npos);

    // The other in-place refusal, which is the one a dragged filter edge
    // runs into. Changing the audio rate moves the demodulation rate, so it
    // is a remove and an add, and the engine says so rather than echoing
    // the request back over a stage still running the old filter.
    rpc::VrxParams faster = changed;
    faster.audio_rate = 24'000;
    const auto resized = harness.client().set_vrx_params(*id, faster);
    REQUIRE_FALSE(resized.has_value());
    INFO(resized.error().message);
    CHECK(resized.error().message.find("remove and an add") != std::string::npos);

    // Refused means refused: the receiver is still what it was, rather than
    // half-changed. A refusal that had already stored the params would read
    // as the change having been taken.
    auto unchanged = harness.client().vrx_status(*id);
    REQUIRE(unchanged.has_value());
    CHECK(unchanged->params.audio_rate == changed.audio_rate);
    CHECK(unchanged->demod_rate > 0);

    // And the same refusal for a width, which is the one a drag actually
    // meets: the rate here does not move at all and the engine still
    // refuses, because the tap count does. Asserted beside the audio-rate
    // case rather than folded into it, because a reader who knows only
    // that the rate is the trigger would read this as a bug.
    rpc::VrxParams narrower = changed;
    narrower.passband_low = -1'500;
    narrower.passband_high = 1'500;
    const auto narrowed = harness.client().set_vrx_params(*id, narrower);
    REQUIRE_FALSE(narrowed.has_value());
    INFO(narrowed.error().message);
    CHECK(narrowed.error().message.find("taps") != std::string::npos);
    CHECK(narrowed.error().message.find("remove and an add") != std::string::npos);

    // A pan of the same width is still taken in place, which is what makes
    // a filter followable live at all.
    //
    // DOWNWARDS, AND THE DIRECTION IS NOT ARBITRARY. This receiver's
    // passband is 9376 Hz inside a 16 kS/s demodulation rate, so its upper
    // edge is close to the fold and the transition the planner can afford
    // is bounded by the distance to it rather than by the width. Panning UP
    // narrows that distance, narrows the transition, and lengthens the
    // filter: measured, plus 300 hertz takes it from 82 taps to 90 and the
    // engine refuses in place. Panning down does not.
    //
    // So "a pan is free" holds away from the fold and not against it, and a
    // surface dragging a filter finds that out from the engine's refusal
    // the same way it finds out about a widen. Asserted downwards here
    // because the case is about the round trip; the boundary itself is what
    // the refusals above cover.
    rpc::VrxParams slid = changed;
    slid.passband_low = changed.passband_low - 300;
    slid.passband_high = changed.passband_high - 300;
    const auto panned = harness.client().set_vrx_params(*id, slid);
    INFO(test::message_of(panned));
    REQUIRE(panned.has_value());

    auto after_pan = harness.client().vrx_status(*id);
    REQUIRE(after_pan.has_value());
    CHECK(after_pan->params.passband_low == slid.passband_low);
    CHECK(after_pan->params.passband_high == slid.passband_high);
    CHECK(after_pan->demod_rate == unchanged->demod_rate);
}

TEST_CASE("all eight demodulator modes survive a round trip", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;

    // Eight channels rather than this fixture's sixty-four, because one of
    // the eight modes is wfm and engine::place now refuses a WFM receiver
    // whose channel cannot carry the broadcast channel rather than handing
    // back a truncated one. At kSourceRate a 64-channel grid guarantees
    // 37500 Hz and an 8-channel grid guarantees 300004, so this is the
    // smallest change that keeps the case about mode ordinals instead of
    // about the grid. The clamp cases below are where the grid is the
    // subject.
    bring_up(harness, HarnessOptions{.channels = 8});

    // Ordinal for ordinal with engine::Demod, which is what convert.h's
    // static_asserts hold true. A mode reordered on one side and not the
    // other would retune every receiver in a saved session, so the list is
    // written out rather than iterated as a range of integers.
    constexpr rpc::Demod kModes[] = {rpc::Demod::Raw,   rpc::Demod::Am,    rpc::Demod::Nfm,
                                     rpc::Demod::Wfm,   rpc::Demod::Usb,   rpc::Demod::Lsb,
                                     rpc::Demod::Dsb,   rpc::Demod::Cw,    rpc::Demod::P25p1,
                                     rpc::Demod::Dstar, rpc::Demod::Tetra};

    std::vector<std::uint64_t> ids;
    for (const rpc::Demod mode : kModes) {
        INFO("mode " << mode_name(mode));

        rpc::VrxParams params;
        params.center = 187'500;
        params.bandwidth = bandwidth_for(mode);
        params.demod = mode;

        auto id = harness.client().add_vrx(params);
        INFO(test::message_of(id));
        REQUIRE(id.has_value());
        ids.push_back(*id);

        auto status = harness.client().vrx_status(*id);
        INFO(test::message_of(status));
        REQUIRE(status.has_value());
        CHECK(status->params.demod == mode);
        CHECK(status->params.bandwidth == params.bandwidth);

        // The engine's own view, so the mode is checked against the enum the
        // conversion casts to rather than against itself.
        auto local =
            harness.engine().vrx_status(engine::VrxId{static_cast<std::uint32_t>(*id)});
        REQUIRE(local.has_value());
        CHECK(engine::demod_name(local->params.demod) == std::string(mode_name(mode)));
    }

    auto listed = harness.client().vrx_ids();
    INFO(test::message_of(listed));
    REQUIRE(listed.has_value());
    CHECK(listed->size() == std::size(kModes));
    for (const std::uint64_t id : ids) {
        INFO("looking for receiver " << id);
        CHECK(std::ranges::find(*listed, id) != listed->end());
    }

    // Removing over the wire removes it in the engine, not merely in the
    // reply. A removeVrx that answered without reaching the graph would pass
    // every assertion above.
    const auto removed = harness.client().remove_vrx(ids.front());
    INFO(test::message_of(removed));
    REQUIRE(removed.has_value());
    CHECK(harness.engine().vrx_ids().size() == std::size(kModes) - 1);
}

TEST_CASE("a demodulator ordinal the engine does not know is refused, not cast",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // What a client built against a newer schema puts on the wire. A Cap'n
    // Proto enum field may legally hold a value the reader has never heard
    // of, and a blind cast would land on whichever mode sits at that ordinal
    // in this build. Mode is the one receiver parameter where being wrong is
    // inaudible until the recording turns out to be unintelligible.
    // One past the last enumerator, derived rather than written, so this
    // test keeps testing an unknown ordinal after a mode is appended
    // instead of quietly testing a known one. It was the literal 9 until
    // the digital voice modes landed on 8, 9 and 10 and turned it into a
    // test that a valid mode is refused.
    constexpr auto kUnknownOrdinal = static_cast<int>(rpc::Demod::Tetra) + 1;

    rpc::VrxParams params = distinctive_params();
    params.demod = static_cast<rpc::Demod>(kUnknownOrdinal);

    auto added = harness.client().add_vrx(params);
    REQUIRE_FALSE(added.has_value());
    INFO(added.error().message);
    CHECK(added.error().message.find("demodulator ordinal " + std::to_string(kUnknownOrdinal)) != std::string::npos);

    // Nothing was created. An engine that refused the mode and added the
    // receiver anyway would be worse than one that cast it.
    CHECK(harness.engine().vrx_ids().empty());

    // The same on the retune path, where the receiver already exists and the
    // damage a cast would do is to change a mode nobody asked to change.
    rpc::VrxParams good = distinctive_params();
    good.demod = rpc::Demod::Nfm;
    auto id = harness.client().add_vrx(good);
    INFO(test::message_of(id));
    REQUIRE(id.has_value());

    rpc::VrxParams bad = good;
    bad.demod = static_cast<rpc::Demod>(kUnknownOrdinal);
    const auto applied = harness.client().set_vrx_params(*id, bad);
    REQUIRE_FALSE(applied.has_value());
    INFO(applied.error().message);
    CHECK(applied.error().message.find("demodulator ordinal " + std::to_string(kUnknownOrdinal)) != std::string::npos);

    auto after = harness.client().vrx_status(*id);
    REQUIRE(after.has_value());
    CHECK(after->params.demod == rpc::Demod::Nfm);
}

TEST_CASE("an engine failure reaches the client carrying the engine's own words",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    constexpr std::uint64_t kNoSuchReceiver = 4242;

    auto local =
        harness.engine().vrx_status(engine::VrxId{static_cast<std::uint32_t>(kNoSuchReceiver)});
    REQUIRE_FALSE(local.has_value());

    auto remote = harness.client().vrx_status(kNoSuchReceiver);
    REQUIRE_FALSE(remote.has_value());

    // The engine's sentence, not a generic one composed at the boundary. A
    // client that is told "the call failed" has to guess, and the thing it
    // has to guess is already written down on the other side of the socket.
    INFO("engine said: " << local.error().message);
    INFO("client saw:  " << remote.error().message);
    CHECK(remote.error().message.find(local.error().message) != std::string::npos);

    // The same for a remove, so it is the error path and not one method.
    const auto removed = harness.client().remove_vrx(kNoSuchReceiver);
    REQUIRE_FALSE(removed.has_value());
    INFO(removed.error().message);
    CHECK(removed.error().message.find("4242") != std::string::npos);

    // An id too large for engine::VrxId cannot name a receiver this engine
    // issued, and truncating it would address a different one.
    auto truncated = harness.client().vrx_status(0x1'0000'0001ULL);
    REQUIRE_FALSE(truncated.has_value());
    INFO(truncated.error().message);
    CHECK(truncated.error().message.find("4294967297") != std::string::npos);
}

TEST_CASE("the source listing crosses whole, backend for backend", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto local = source::describe_sources();
    INFO(test::message_of(local));
    REQUIRE(local.has_value());

    // The registry always holds the synthetic backend, so this never fires
    // in practice. It is here because everything below is a loop over this
    // list: an empty registry would make the sizes match at zero and every
    // per-entry assertion vanish, and the case would report as covered.
    REQUIRE_FALSE(local->empty());

    auto remote = harness.client().list_sources();
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    // A backend that could not be described must not be able to hide the
    // ones that could, and its reason must not be dropped on the way out: a
    // missing DLL and an unplugged radio are different problems, and a list
    // that omits both looks identical to a list with nothing attached.
    //
    // TWO LISTINGS TAKEN AT TWO MOMENTS, AND A DEVICE CAN CHANGE HANDS BETWEEN
    // THEM. Describing a dongle opens it, so another process holding it, a
    // second checkout's suite or a capture, makes it unavailable in whichever
    // listing ran while it was held. On 2026-09-23 this case failed on exactly
    // that with parallel checkouts on one machine. The listing a client gets
    // is the server's own and cannot be taken from the snapshot below, so what
    // is compared is what another process cannot change: the same entries in
    // the same order, each with its URI and backend, and the whole description
    // wherever the device's availability agreed. An entry that changed hands
    // is held only to saying why it could not be described.
    REQUIRE(remote->size() == local->size());
    std::size_t changed_hands = 0;
    for (std::size_t i = 0; i < local->size(); ++i) {
        INFO("entry " << i << " is " << (*local)[i].uri);
        CHECK((*remote)[i].uri == (*local)[i].uri);
        CHECK((*remote)[i].backend == (*local)[i].backend);

        if ((*remote)[i].available() != (*local)[i].available()) {
            ++changed_hands;
            const std::string& reason = (*local)[i].available() ? (*remote)[i].unavailable
                                                                : (*local)[i].unavailable;
            INFO("available locally " << (*local)[i].available() << ", over the wire "
                                      << (*remote)[i].available() << ": " << reason);
            CHECK_FALSE(reason.empty());
            continue;
        }

        CHECK((*remote)[i].display_name == (*local)[i].display_name);
        CHECK((*remote)[i].unavailable == (*local)[i].unavailable);
        CHECK((*remote)[i].available() == (*local)[i].available());

        // Everything openSource made worth carrying. A client that can open a
        // device but cannot be told what the device accepts has to guess, or
        // ask the operator to type a URI.
        //
        // The notes are the ones that would go missing quietly: each is the
        // answer to "why is this at the wrong frequency" an hour later, and a
        // listing that dropped them would look complete.
        CHECK((*remote)[i].notes == (*local)[i].notes);

        CHECK((*remote)[i].min_rate == static_cast<std::uint32_t>((*local)[i].min_rate));
        CHECK((*remote)[i].max_rate == static_cast<std::uint32_t>((*local)[i].max_rate));
        REQUIRE((*remote)[i].sample_rates.size() == (*local)[i].sample_rates.size());
        for (std::size_t rate = 0; rate < (*local)[i].sample_rates.size(); ++rate) {
            CHECK((*remote)[i].sample_rates[rate] ==
                  static_cast<std::uint32_t>((*local)[i].sample_rates[rate]));
        }

        // Ordinal for ordinal, which client.cpp static_asserts in both
        // directions. Comparing the two casts rather than naming an enumerator
        // is what makes this a test of the mapping rather than of one value.
        CHECK(static_cast<std::uint8_t>((*remote)[i].native_format) ==
              static_cast<std::uint8_t>((*local)[i].native_format));
        CHECK(static_cast<std::uint8_t>((*remote)[i].flow) ==
              static_cast<std::uint8_t>((*local)[i].flow));
        CHECK((*remote)[i].bits_per_component == (*local)[i].bits_per_component);
        CHECK((*remote)[i].seekable == (*local)[i].seekable);
        CHECK((*remote)[i].length_samples == (*local)[i].length_samples);

        // AN INVERTED RANGE IS DROPPED ON THE WAY OUT, so the two lists are the
        // same length only when every local range is usable. That filter is
        // Engine::source_tuning's, for its reason: a backend that could not
        // describe its tuner leaves an inverted entry rather than guessing, and
        // carrying it would offer a client a control that refuses everything.
        std::size_t usable = 0;
        for (const source::TuneRange& range : (*local)[i].tune_ranges) {
            if (range.high >= range.low) {
                ++usable;
            }
        }
        REQUIRE((*remote)[i].tune_ranges.size() == usable);

        std::size_t at = 0;
        for (const source::TuneRange& range : (*local)[i].tune_ranges) {
            if (range.high < range.low) {
                continue;
            }
            CHECK((*remote)[i].tune_ranges[at].low_hz == range.low);
            CHECK((*remote)[i].tune_ranges[at].high_hz == range.high);
            CHECK((*remote)[i].tune_ranges[at].step_hz == range.step);
            ++at;
        }

        REQUIRE((*remote)[i].gain_stages.size() == (*local)[i].gain_stages.size());
        for (std::size_t stage = 0; stage < (*local)[i].gain_stages.size(); ++stage) {
            const source::GainStage& mine = (*local)[i].gain_stages[stage];
            const rpc::GainStage& theirs = (*remote)[i].gain_stages[stage];
            INFO("gain stage " << mine.name);
            CHECK(theirs.name == mine.name);
            CHECK(theirs.min_db == mine.min_db);
            CHECK(theirs.max_db == mine.max_db);
            CHECK(theirs.has_auto == mine.has_auto);

            // The steps are the field a picker cannot do without and the one a
            // naive mirror drops: an empty list means continuous and a
            // populated one means the stage takes nothing else, so losing it
            // turns 29 discrete tuner gains into a slider showing values the
            // device never took.
            CHECK(theirs.steps_db == mine.steps_db);
        }
    }

    // The synthetic backend is in both listings and nothing can hold it, so
    // at least one entry is always compared in full.
    CHECK(changed_hands < local->size());
    if (changed_hands > 0) {
        WARN(std::format("{} device{} changed availability between the two listings, so only "
                         "its URI, backend and reason were compared",
                         changed_hands, changed_hands == 1 ? "" : "s"));
    }

    // AND AT LEAST ONE ENTRY CARRIES SOMETHING, or the loop above proves
    // nothing. Every assertion in it compares two structs, so two empty ones
    // agree: a writer that forgot the tune ranges and a reader that forgot to
    // read them cancel out, and the case reports as coverage.
    //
    // Every backend on every machine says at least this much. The synthetic
    // scene and the file backend have no tuner and no gain stages, which is
    // correct and is why neither of those is what this checks, but both state a
    // rate range and a native format.
    const bool any_rate = std::ranges::any_of(
        *remote, [](const rpc::SourceDescriptor& one) { return one.max_rate > 0; });
    CHECK(any_rate);
    const bool any_width = std::ranges::any_of(
        *remote, [](const rpc::SourceDescriptor& one) { return one.bits_per_component > 0; });
    CHECK(any_width);

    // The two that only a real radio produces, so they are asserted only when
    // one is present rather than skipping the whole case: with no dongle
    // attached this machine has nothing tunable and nothing with a gain stage,
    // and that is a true description of the machine rather than a gap.
    const auto dongle = std::ranges::find_if(
        *remote, [](const rpc::SourceDescriptor& one) { return one.backend == "rtlsdr"; });
    if (dongle == remote->end()) {
        WARN("no RTL-SDR is attached, so the tune-range and gain-stage fields crossed empty "
             "in both directions and this case did not exercise them");
        return;
    }
    if (!dongle->available()) {
        WARN("the attached RTL-SDR could not be described: " + dongle->unavailable);
        return;
    }

    INFO("dongle " << dongle->display_name);
    CHECK_FALSE(dongle->tune_ranges.empty());
    CHECK_FALSE(dongle->gain_stages.empty());
    CHECK(dongle->max_rate > dongle->min_rate);

    // An R820T's tuner gain is 29 discrete steps, so the steps list is what a
    // picker reads and an empty one would send it to the continuous path.
    // Asserted as "some stage is stepped" rather than by name, because the
    // stage naming is librtlsdr's and the count is the tuner's.
    const bool any_stepped =
        std::ranges::any_of(dongle->gain_stages,
                            [](const rpc::GainStage& stage) { return !stage.steps_db.empty(); });
    CHECK(any_stepped);
}

TEST_CASE("an unavailable backend arrives with its reason rather than omitted",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    // A backend that enumerates and will not open is the case this exists
    // for, and the only one this machine can produce on demand is a dongle
    // already held by somebody. Holding it here is that somebody.
    //
    // With no dongle attached there is nothing on this machine that reports
    // unavailable, so the case skips with that reason rather than asserting
    // over an empty set and calling it a pass.
    auto enumerated = source::enumerate_sources();
    INFO(test::message_of(enumerated));
    REQUIRE(enumerated.has_value());

    std::string dongle;
    for (const source::SourceDescriptor& entry : *enumerated) {
        if (entry.backend == "rtlsdr") {
            dongle = entry.uri;
            break;
        }
    }
    if (dongle.empty()) {
        SKIP("no RTL-SDR dongle is attached, so no backend on this machine can be made "
             "unavailable without unplugging one");
    }

    auto held = source::open_source(dongle);
    if (!held) {
        SKIP("the dongle could not be opened, so it cannot be made busy: " +
             held.error().message);
    }

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto remote = harness.client().list_sources();
    INFO(test::message_of(remote));
    REQUIRE(remote.has_value());

    const auto entry = std::ranges::find_if(
        *remote, [&](const rpc::SourceDescriptor& item) { return item.uri == dongle; });

    // Present, not omitted. That is half the requirement and the half a
    // listing that returned an error for the whole set would fail.
    INFO("looking for " << dongle << " among " << remote->size() << " entries");
    REQUIRE(entry != remote->end());

    if (entry->available()) {
        SKIP("this driver describes a dongle this process already holds, so it reports no "
             "reason to carry");
    }

    INFO("the reason was: " << entry->unavailable);
    CHECK_FALSE(entry->unavailable.empty());
    CHECK_FALSE(entry->backend.empty());
    CHECK_FALSE(entry->display_name.empty());
}

TEST_CASE("running and the source counters report what the engine reports", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{.pace = 1.0, .block_samples = 16'384});

    auto idle = harness.client().running();
    INFO(test::message_of(idle));
    REQUIRE(idle.has_value());
    CHECK_FALSE(*idle);

    const auto started = harness.start_engine();
    INFO(test::message_of(started));
    REQUIRE(started.has_value());

    const std::uint64_t blocks = harness.wait_for_blocks(8, 4000);
    INFO(blocks << " blocks delivered");
    REQUIRE(blocks >= 8);

    auto live = harness.client().running();
    INFO(test::message_of(live));
    REQUIRE(live.has_value());
    CHECK(*live);

    auto stats = harness.client().source_stats();
    INFO(test::message_of(stats));
    REQUIRE(stats.has_value());
    CHECK(stats->blocks_delivered >= 8);
    CHECK(stats->samples_delivered >= stats->blocks_delivered);

    // The front end's verdict rides on this struct and is Unmeasured until
    // something asks for detections, because it is computed from the
    // detector's own arrays and the server builds a detector on demand. An
    // implementation that defaulted it to Steady would report a clean front
    // end on every engine that has never run one, which is a confident
    // answer to a question nobody has measured.
    CHECK(stats->front_end == rpc::FrontEndState::Unmeasured);
    CHECK(stats->front_end_slope == 0.0);

    // Live, and monotone. Read again after the engine has moved on rather
    // than compared against the engine's own numbers at a different instant,
    // which would race.
    //
    // The wait is what makes the second read mean something. Monotonicity
    // alone is satisfied by a server that answered the first call and then
    // returned that snapshot for the rest of the run, and a display driven
    // by a frozen counter shows a radio that stopped receiving. So the
    // engine is given more blocks to deliver first and the counter is
    // required to have followed.
    const std::uint64_t moved_on = harness.wait_for_blocks(blocks + 4, 4000);
    INFO(moved_on << " blocks delivered by the time of the second read");
    REQUIRE(moved_on >= blocks + 4);

    auto later = harness.client().source_stats();
    REQUIRE(later.has_value());
    CHECK(later->blocks_delivered > stats->blocks_delivered);
    CHECK(later->samples_delivered > stats->samples_delivered);

    const auto ran = harness.stop_engine();
    INFO(test::message_of(ran));
    CHECK(ran.has_value());
}

TEST_CASE("one engine serves one server, and is free again when it stops", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    // No source and no harness. Server::create has to answer this before the
    // engine has done anything, and building a source would only make the
    // case slower.
    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = kChannels;
    config.ring_seconds = 0.25;

    auto created = engine::Engine::create(config);
    INFO(test::message_of(created));
    REQUIRE(created.has_value());
    engine::Engine& eng = **created;

    // Every create here carries a token, because Server::create refuses an
    // empty one BEFORE it claims the engine. Passing ServerOptions{} would
    // make the second create below fail for the wrong reason and the case
    // would pass while proving nothing about the claim.
    const rpc::Token token = test::test_token();
    rpc::ServerOptions options;
    options.token.assign(token.begin(), token.end());

    auto first = rpc::Server::create(eng, options);
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    CHECK((*first)->port() != 0);

    // A second server would replace the first's spectrum sink, and the first
    // would then hold subscriptions that never receive another frame. That
    // is silent, so it is refused here rather than discovered by a display
    // that stopped drawing.
    auto second = rpc::Server::create(eng, options);
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().message);
    CHECK(second.error().message.find("already has an RPC server") != std::string::npos);

    (*first)->stop();

    // And the claim is released rather than held for the life of the
    // process, so a service that restarts its listener does not have to
    // rebuild the engine to do it.
    auto again = rpc::Server::create(eng, options);
    INFO(test::message_of(again));
    REQUIRE(again.has_value());
    CHECK((*again)->port() != 0);
}

TEST_CASE("a second server cannot bind a port the first is listening on", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    // kj's own listen() sets SO_REUSEADDR, which on Windows lets a second
    // socket bind a port another is listening on, and before core/rpc/listen.h
    // this create succeeded and both servers reported the same port. Two
    // engines are needed because one engine refuses a second server on other
    // grounds, which the case above covers and which would make this pass for
    // the wrong reason.
    Harness harness;
    bring_up(harness, HarnessOptions{});
    const std::uint16_t taken = harness.port();
    REQUIRE(taken != 0);

    engine::EngineConfig config;
    config.gpu_index = -1;
    config.channels = kChannels;
    config.ring_seconds = 0.25;
    auto other = engine::Engine::create(config);
    INFO(test::message_of(other));
    REQUIRE(other.has_value());

    const rpc::Token token = test::test_token();
    rpc::ServerOptions options;
    options.port = taken;
    options.token.assign(token.begin(), token.end());

    auto second = rpc::Server::create(**other, options);
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().message);
    CHECK(second.error().message.find(std::format("127.0.0.1:{}", taken)) != std::string::npos);
    CHECK(second.error().message.find("WSAEADDRINUSE") != std::string::npos);

    // And the first is still the one answering.
    auto info = harness.client().info();
    INFO(test::message_of(info));
    CHECK(info.has_value());
}

TEST_CASE("connect refuses what cannot be a connection", "[rpc][m1]") {
    const rpc::Token token = test::test_token();

    // No GPU and no engine. These are argument errors the client answers on
    // its own, and answering them before a thread is started is the point.
    auto no_address = rpc::Client::connect("", 47'000, token);
    REQUIRE_FALSE(no_address.has_value());
    INFO(no_address.error().message);
    CHECK(no_address.error().message.find("no address") != std::string::npos);

    // ServerOptions::port defaults to zero meaning "bind whatever is free",
    // so a caller forwarding its own options here would otherwise ask the OS
    // to connect to port zero and get a message about nothing listening.
    auto zero_port = rpc::Client::connect("127.0.0.1", 0, token);
    REQUIRE_FALSE(zero_port.has_value());
    INFO(zero_port.error().message);
    CHECK(zero_port.error().message.find("Server::port()") != std::string::npos);
}

TEST_CASE("connect reports a refused connection rather than a call that fails later",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    // A port that was listening and is not any more, which is the closest
    // this can get to a dead port without guessing one that might be in use.
    std::uint16_t port = 0;
    {
        Harness harness;
        bring_up(harness, HarnessOptions{});
        port = harness.port();
        REQUIRE(port != 0);
    }

    auto refused = rpc::Client::connect("127.0.0.1", port, test::test_token());

    // The whole reason client.cpp connects on the loop thread and waits
    // rather than using EzRpcClient: a client that connected in the
    // background would return a handle here and report the refusal as a
    // failed call minutes later, by which time nothing points at the port.
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find(std::format("127.0.0.1:{}", port)) !=
          std::string::npos);
}

// ---------------------------------------------------------------------------
// Retuning the front end
// ---------------------------------------------------------------------------

TEST_CASE("a source that cannot retune says so before it is asked", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto tuning = harness.client().source_can_retune();
    REQUIRE(tuning.has_value());

    // A synthetic scene declares no tune range, because its centre is a
    // label on emitters generated around it rather than an oscillator. A
    // client greys the control out on this rather than offering one that
    // always refuses, which is the whole reason the question is separate
    // from the answer.
    CHECK_FALSE(tuning->can_retune);
    CHECK(tuning->low_hz == 0);
    CHECK(tuning->high_hz == 0);
}

TEST_CASE("a refused retune comes back in the source's own words", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    auto refused = harness.client().set_source_center(462'000'000);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);

    // Not a category. The synthetic source's sentence names what to do
    // instead, which is to place the emitters where they are wanted, and
    // that instruction is different from the file source's and from a
    // dongle's. A refusal composed in the RPC layer would have replaced all
    // three with "this source cannot retune".
    CHECK(refused.error().message.find("synthetic source cannot tune") != std::string::npos);
    CHECK(refused.error().message.find("span_low") != std::string::npos);
}

TEST_CASE("the open source describes itself without touching a device", "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // THE QUESTION listSources CANNOT ANSWER. It describes candidates and opens
    // every device index to do it; this describes the one already open. A
    // client that attached to an engine somebody else started with a URI has
    // never seen a descriptor, so before this call it could not know the
    // source's gain stages, its flow control or its formats, and could not
    // offer a gain control at all.
    auto described = harness.client().source_descriptor();
    INFO(test::message_of(described));
    REQUIRE(described.has_value());
    REQUIRE(described->has_value());

    const rpc::SourceDescriptor& open = **described;
    INFO("backend " << open.backend << ", " << open.gain_stages.size() << " gain stages");
    CHECK(open.backend == "synthetic");

    // FLOW CONTROL, which is the field a client has to read to report pacing
    // honestly. A synthetic scene is Demand: its consumer sets the rate, so
    // EngineInfo::sourcePacedBy is the setting that matters on it. On a Paced
    // source that setting is ignored entirely, and a client without this field
    // told an operator their dongle was "paced at 1.00x on purpose" when the
    // dongle had never looked at the setting.
    CHECK(open.flow == rpc::FlowControl::Demand);

    // A synthetic scene has no amplifier to turn up, so it offers no stage and
    // a client draws no gain control rather than one that always refuses.
    CHECK(open.gain_stages.empty());
}

TEST_CASE("a refused gain change comes back in the source's own words", "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // A synthetic scene has no gain stage at all: its emitters are generated
    // at the levels the URI asked for, so there is no amplifier to turn up.
    // The refusal has to say that rather than "no such stage", which would
    // read as a client that mistyped a name.
    auto refused = harness.client().set_source_gain("tuner", 20.0);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("tuner") != std::string::npos);

    auto refused_auto = harness.client().set_source_gain_auto("tuner", true);
    REQUIRE_FALSE(refused_auto.has_value());
    INFO(refused_auto.error().message);

    // AND IT IS NOT A SOURCE CHANGE, which is the claim the schema makes about
    // this call and the one a client's correlations rest on. A gain change
    // moves no frequency and starts no new stream, so the epoch every sample
    // index and every audio chunk is numbered against must not move. The
    // refusals above make this the weaker half of the case on a synthetic
    // source; it is asserted anyway, because a wire that bumped the epoch on a
    // refused call would be worse than one that bumped it on a granted one.
    auto before = harness.client().info();
    REQUIRE(before.has_value());
    auto after = harness.client().info();
    REQUIRE(after.has_value());
    CHECK(after->source_epoch == before->source_epoch);
}

TEST_CASE("a refused retune leaves the engine exactly where it was", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{.center_hz = 462'000'000});

    auto before = harness.client().info();
    REQUIRE(before.has_value());
    REQUIRE(before->source_center == 462'000'000);

    const std::uint64_t id = [&] {
        auto added = harness.client().add_vrx(distinctive_params());
        REQUIRE(added.has_value());
        return *added;
    }();

    REQUIRE_FALSE(harness.client().set_source_center(88'500'000).has_value());

    // The centre is the one piece of engine state a successful retune moves,
    // so a refused one must not move it. A client that redrew its axis from
    // a centre the device never took would be labelling the right spectrum
    // with the wrong frequencies.
    auto after = harness.client().info();
    REQUIRE(after.has_value());
    CHECK(after->source_center == before->source_center);

    // And the receiver is still there. Nothing about a retune tears one
    // down even when it succeeds, and a refusal must not either.
    auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(std::ranges::find(*ids, id) != ids->end());
}

TEST_CASE("a retune says why it removed each receiver, and which can come back",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    // THE REAL ENGINE'S ANSWER, NOT AN IMITATION. tests/rpc/retunable_engine.h
    // decides for itself which receivers go; this case opens a synthetic
    // scene through tests/engine/movable_centre.h instead, so what crosses the
    // wire is Engine::set_source_center's own list, including the shape
    // refusal that only the graph produces.
    //
    // The front end tests/engine/test_engine_retune.cpp uses: 2 MS/s over 256
    // channels, 7812.5 Hz apart. There a 10 kHz AM receiver on a channel
    // centre, moved 3500 Hz by a retune, needs a narrower filter from its new
    // place and the graph refuses it; one 998 kHz below the centre leaves the
    // span; a USB receiver keeps its shape and comes along.
    constexpr dsp::Hertz kOpenedAt = 7'100'000;
    constexpr dsp::Hertz kMove = 3'500;
    HarnessOptions options;
    options.source_uri = std::format(
        "synthetic:wideband?rate=2000000&center={}&emitters=0&samples=2000000&seed=20260923",
        kOpenedAt);
    options.channels = 256;
    options.movable_centre = true;

    Harness harness;
    bring_up(harness, options);

    const auto add = [&](rpc::Demod demod, std::int64_t center) {
        rpc::VrxParams params;
        params.center = center;
        params.demod = demod;
        // Zero asks for the mode's own passband, as the engine case does.
        params.bandwidth = 0;
        params.audio_rate = 48'000;
        return harness.client().add_vrx(params);
    };
    auto reshaped = add(rpc::Demod::Am, 0);
    auto stranded = add(rpc::Demod::Am, -998'000);
    auto kept = add(rpc::Demod::Usb, 62'500);
    INFO(test::message_of(reshaped) << " / " << test::message_of(stranded) << " / "
                                    << test::message_of(kept));
    REQUIRE(reshaped.has_value());
    REQUIRE(stranded.has_value());
    REQUIRE(kept.has_value());

    auto retuned = harness.client().retune_source(kOpenedAt + kMove);
    INFO(test::message_of(retuned));
    REQUIRE(retuned.has_value());
    CHECK(retuned->granted_hz == kOpenedAt + kMove);

    for (const rpc::RetuneRemoval& gone : retuned->removed) {
        WARN(std::format("removed {} at {} Hz, cause {}: {}", gone.id, gone.frequency_hz,
                         static_cast<int>(gone.cause), gone.reason));
    }
    REQUIRE(retuned->removed.size() == 2);

    const auto find = [&](std::uint64_t id) {
        return std::ranges::find_if(retuned->removed,
                                    [&](const rpc::RetuneRemoval& gone) { return gone.id == id; });
    };

    // The shape refusal: named as such, with the graph's own sentence, which
    // is what the client's "moved off" sentence got wrong before this field.
    const auto shape = find(*reshaped);
    REQUIRE(shape != retuned->removed.end());
    CHECK(shape->frequency_hz == kOpenedAt);
    CHECK(shape->cause == rpc::RetuneCause::ShapeChanged);
    CHECK(shape->reason.find("remove and an add") != std::string::npos);

    // The span, the cause there always was.
    const auto span = find(*stranded);
    REQUIRE(span != retuned->removed.end());
    CHECK(span->frequency_hz == kOpenedAt - 998'000);
    CHECK(span->cause == rpc::RetuneCause::OutsideSpan);
    CHECK(span->reason.find("outside the span") != std::string::npos);

    auto ids = harness.client().vrx_ids();
    REQUIRE(ids.has_value());
    CHECK(*ids == std::vector<std::uint64_t>{*kept});

    // WHAT THE CAUSE IS FOR. A receiver refused for its shape comes back from
    // an add at the frequency the answer reported, which builds the new shape
    // from scratch; one that left the span does not, because the same
    // frequency is still outside it. The client offers the first and not the
    // second on exactly this distinction.
    auto back = add(rpc::Demod::Am, shape->frequency_hz - (kOpenedAt + kMove));
    INFO(test::message_of(back));
    CHECK(back.has_value());
    CHECK_FALSE(add(rpc::Demod::Am, span->frequency_hz - (kOpenedAt + kMove)).has_value());
}

// ---------------------------------------------------------------------------
// The realtime factor, which is the diagnosis nobody could make
// ---------------------------------------------------------------------------

TEST_CASE("the pace a source was asked for crosses beside what it achieved",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{.pace = 0.0});

    auto idle = harness.client().info();
    REQUIRE(idle.has_value());

    // Zero is NOT MEASURED and is a third state. Between opening the source
    // and running it there is no elapsed time to divide by, and a client
    // that read this as a stalled source would raise an alarm on every
    // engine that has not started yet.
    CHECK(idle->realtime_factor == 0.0);
    CHECK(idle->source_paced_by == 0.0);

    REQUIRE(harness.start_engine().has_value());
    REQUIRE(harness.wait_for_blocks(8, 5'000) >= 8);

    auto running = harness.client().info();
    REQUIRE(running.has_value());
    INFO("realtime factor " << running->realtime_factor);

    // Unthrottled against a synthetic scene on a GPU: faster than realtime
    // is the ordinary outcome and the exact figure is the host's business.
    // What the case pins is that the number is being measured at all, which
    // is what was missing.
    CHECK(running->realtime_factor > 0.0);

    // And that the setting travels beside it. This pair is the whole point:
    // 0.5 is a fault at a pace of zero and is the setting at a pace of 0.5,
    // and neither number answers that on its own.
    CHECK(running->source_paced_by == 0.0);

    REQUIRE(harness.stop_engine().has_value());
}

TEST_CASE("a deliberately paced source reports the pace it was given", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{.pace = 0.5});

    auto info = harness.client().info();
    REQUIRE(info.has_value());

    // Read before the run and still correct, because this is configuration
    // rather than measurement. A client can therefore grey out its own
    // "source is behind" indicator before a single block has arrived.
    CHECK(info->source_paced_by == 0.5);
}

// ---------------------------------------------------------------------------
// A passband one channel cannot carry: refused on the FM modes, clamped and
// reported in words on the linear ones
// ---------------------------------------------------------------------------

TEST_CASE("a WFM receiver a channel cannot carry is refused rather than narrowed",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // The case the operator hit on air. A 64-channel grid on this fixture's
    // 2400032 S/s source puts one coarse channel at 75001 S/s, so a
    // broadcast FM receiver asking for 200 kHz of passband cannot have it.
    //
    // WHAT THIS CASE USED TO ASSERT, AND WHY IT CHANGED. Until 2026-09-21
    // the receiver was created with a 37 kHz passband and the sentence
    // below was carried on VrxPlacement::clampReason, which a client had to
    // poll for and read. Every surface reported it correctly and the radio
    // still sounded broken: the operator hears the audio long before they
    // read a status line, and the audio is not a narrower version of the
    // station, it is the wrong signal. So the placement is refused, with
    // the same facts in the refusal, at the call that asked for it.
    rpc::VrxParams wide;
    wide.center = 0;
    wide.demod = rpc::Demod::Wfm;
    wide.passband_low = -100'000;
    wide.passband_high = 100'000;

    auto added = harness.client().add_vrx(wide);
    REQUIRE_FALSE(added.has_value());

    const std::string& refusal = added.error().message;
    INFO(refusal);

    // What the mode needs, and what one channel of this grid could carry.
    CHECK(refusal.find("wfm receiver needs 200000 Hz") != std::string::npos);
    CHECK(refusal.find("64 channel grid") != std::string::npos);

    // The judgement rather than the number: on an FM mode a truncated
    // passband is not a narrower receiver. A discriminator recovers the
    // instantaneous frequency of whatever reaches it, so the audio is
    // wrong rather than narrow-band, and that is the sentence the operator
    // needed.
    CHECK(refusal.find("wrong audio") != std::string::npos);

    // And what to do about it, which is not something this session can
    // change: the grid is sized when the source is opened. 2400032 over
    // 200000 is 12, and the largest power of two under it is 8.
    CHECK(refusal.find("--channels 8") != std::string::npos);
    CHECK(refusal.find("cannot be changed while it is running") != std::string::npos);

    // Nothing was created. A refusal that left a receiver behind would be
    // worse than the clamp it replaced.
    CHECK(harness.engine().vrx_ids().empty());
}

TEST_CASE("a WFM receiver narrower than its channel is built as asked", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // The other side of the refusal above, and the reason it is gated on
    // the clamp rather than on the mode. A caller that ASKS for a narrow
    // WFM receiver gets one: the RDS cases in tests/rpc/test_rpc_rds.cpp
    // open exactly this to prove a decoder faults on a passband too narrow
    // for the composite, and a rule that refused every narrow WFM receiver
    // would have taken that case with it.
    rpc::VrxParams narrow_wfm;
    narrow_wfm.center = 0;
    narrow_wfm.demod = rpc::Demod::Wfm;
    narrow_wfm.passband_low = -20'000;
    narrow_wfm.passband_high = 20'000;

    auto added = harness.client().add_vrx(narrow_wfm);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    auto status = harness.client().vrx_status(*added);
    REQUIRE(status.has_value());
    CHECK_FALSE(status->placement.bandwidth_clamped);
    CHECK(status->placement.granted_low == -20'000);
    CHECK(status->placement.granted_high == 20'000);
}

TEST_CASE("a receiver that fits its channel carries no clamp sentence", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // The control arm. Without it the case above is asserting a field that
    // is always set, which is the defect shape this round is about in its
    // own right.
    rpc::VrxParams narrow;
    narrow.center = 0;
    narrow.demod = rpc::Demod::Nfm;
    narrow.passband_low = -8'000;
    narrow.passband_high = 8'000;

    auto added = harness.client().add_vrx(narrow);
    REQUIRE(added.has_value());

    auto status = harness.client().vrx_status(*added);
    REQUIRE(status.has_value());
    CHECK_FALSE(status->placement.bandwidth_clamped);
    CHECK(status->placement.clamp_reason.empty());
}

TEST_CASE("a linear mode's clamp is reported without calling the demodulator broken",
          "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // An AM receiver asking for more than a channel can carry gets a
    // narrower filter and that is exactly what it gets: the envelope
    // detector is linear in the passband, so less bandwidth is less audio
    // bandwidth and nothing is broken. Saying otherwise would train an
    // operator to ignore the sentence on the mode where it matters.
    rpc::VrxParams wide_am;
    wide_am.center = 0;
    wide_am.demod = rpc::Demod::Am;
    wide_am.passband_low = -90'000;
    wide_am.passband_high = 90'000;

    auto added = harness.client().add_vrx(wide_am);
    REQUIRE(added.has_value());

    auto status = harness.client().vrx_status(*added);
    REQUIRE(status.has_value());
    REQUIRE(status->placement.bandwidth_clamped);

    const std::string& reason = status->placement.clamp_reason;
    INFO(reason);
    CHECK(reason.find("percent of what was asked for") != std::string::npos);
    CHECK(reason.find("wrong audio") == std::string::npos);
}

TEST_CASE("a client closes the source and opens another over the wire",
          "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{.spectrum_transform = kSpectrumTransform});

    auto first = harness.client().info();
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    const std::uint64_t epoch = first->source_epoch;

    // One, not zero: the harness opened a source before the server was built.
    // Zero on this wire means an engine that has never had one.
    CHECK(epoch == 1);
    CHECK(first->source_rate > 0);

    // A receiver, so the close has something a client can see the absence of.
    rpc::VrxParams params;
    params.center = 0;
    params.bandwidth = 12'000;
    params.demod = rpc::Demod::Nfm;
    params.squelch_dbfs = -200.0;
    params.agc_attack_ms = 5.0;
    params.agc_decay_ms = 200.0;
    auto added = harness.client().add_vrx(params);
    INFO(test::message_of(added));
    REQUIRE(added.has_value());

    // A SECOND OPEN IS REFUSED RATHER THAN REPLACING, and the refusal is the
    // engine's own sentence carried across the wire rather than a category
    // composed by the server.
    auto replace = harness.client().open_source(scene_uri(400'000));
    REQUIRE_FALSE(replace.has_value());
    INFO(replace.error().message);
    CHECK(replace.error().message.find("Close it first") != std::string::npos);

    const auto closed = harness.client().close_source();
    INFO(test::message_of(closed));
    REQUIRE(closed.has_value());

    // Everything describing a source is gone and the device is not.
    auto between = harness.client().info();
    INFO(test::message_of(between));
    REQUIRE(between.has_value());
    CHECK(between->source_rate == 0);
    CHECK(between->grid.channels == 0);
    CHECK_FALSE(between->device.name.empty());

    // THE EPOCH IS KEPT ACROSS THE GAP. An engine between sources still
    // answers truthfully about which stream the indices a client is holding
    // belonged to.
    CHECK(between->source_epoch == epoch);

    // The receiver went with the graph, so the list is empty and the id is
    // refused rather than answered with a zeroed status.
    auto ids = harness.client().vrx_ids();
    INFO(test::message_of(ids));
    REQUIRE(ids.has_value());
    CHECK(ids->empty());

    auto stale = harness.client().vrx_status(*added);
    CHECK_FALSE(stale.has_value());

    // Closing twice is a success. A client that closes before every open
    // should not have to know which state it was in to read the answer.
    CHECK(harness.client().close_source().has_value());

    // A different rate, so the grid and the ring are rebuilt rather than
    // reused. This is the case a replace could not have done safely.
    const std::string other =
        "synthetic:wideband?rate=1200000&emitters=1&modes=am&seed=77&samples=400000";
    const auto reopened = harness.client().open_source(other);
    INFO(test::message_of(reopened));
    REQUIRE(reopened.has_value());

    auto second = harness.client().info();
    INFO(test::message_of(second));
    REQUIRE(second.has_value());
    CHECK(second->source_rate == 1'200'000);

    // THE ONE FIELD THAT SAYS THE INDICES STARTED AGAIN. Without it a client
    // correlating by sample index across this call lines up the new stream's
    // frames against the old stream's audio and finds the arithmetic
    // consistent, because both are honest indices into different streams.
    CHECK(second->source_epoch == epoch + 1);

    // And the session is usable rather than merely not broken: a receiver is
    // placed on the new grid.
    auto after = harness.client().add_vrx(params);
    INFO(test::message_of(after));
    REQUIRE(after.has_value());

    // Ids do not restart, so a client holding the old one cannot address the
    // new receiver by accident.
    CHECK(*after > *added);
}

TEST_CASE("an empty URI is refused before the source registry sees it", "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    REQUIRE(harness.client().close_source().has_value());

    // Said by the session rather than passed down, because what
    // source::open_source makes of an empty string reads as a parse failure
    // and this is a caller who sent nothing. The refusal names where a client
    // gets a URI to start from.
    auto empty = harness.client().open_source("");
    REQUIRE_FALSE(empty.has_value());
    INFO(empty.error().message);
    CHECK(empty.error().message.find("listSources") != std::string::npos);

    // And nothing was opened by the attempt.
    auto info = harness.client().info();
    REQUIRE(info.has_value());
    CHECK(info->source_rate == 0);
}

TEST_CASE("a failed open leaves an engine with no source rather than the old one",
          "[gpu][rpc][m2]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    REQUIRE(harness.client().close_source().has_value());

    // A backend nothing registers. The registry's refusal crosses the wire
    // naming what it does know, which is the answer a picker wants.
    auto nonsense = harness.client().open_source("nosuchbackend://0?rate=2400000");
    REQUIRE_FALSE(nonsense.has_value());
    INFO(nonsense.error().message);

    auto info = harness.client().info();
    REQUIRE(info.has_value());
    CHECK(info->source_rate == 0);

    // THE POINT OF TWO CALLS RATHER THAN A REPLACE. A failed open leaves an
    // engine with no source, which is the state the client asked for when it
    // closed, and it can see that state and try another URI. A replace would
    // have had to answer "the new one failed and the old one is gone" with one
    // bool.
    REQUIRE(harness.client().open_source(scene_uri(400'000)).has_value());
    auto recovered = harness.client().info();
    REQUIRE(recovered.has_value());
    CHECK(recovered->source_rate > 0);
}
