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
    rpc::VrxParams changed = sent;
    changed.center = 131'072;
    changed.bandwidth = 5'127;
    changed.audio_rate = 24'000;
    changed.squelch_dbfs = -41.5;
    changed.agc_attack_ms = 17.25;
    changed.agc_decay_ms = 250.5;
    changed.agc_enabled = true;
    changed.cw_pitch = 421;

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

    // And the rule the paragraph above rests on, so that an engine which
    // later allows a mode change in place makes this case say so rather than
    // leaving a comment that has quietly become false.
    rpc::VrxParams retimbred = changed;
    retimbred.demod = rpc::Demod::Lsb;
    const auto refused = harness.client().set_vrx_params(*id, retimbred);
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find("cannot become") != std::string::npos);
}

TEST_CASE("all eight demodulator modes survive a round trip", "[gpu][rpc][m1]") {
    REVENANT_NEEDS_GPU();

    Harness harness;
    bring_up(harness, HarnessOptions{});

    // Ordinal for ordinal with engine::Demod, which is what convert.h's
    // static_asserts hold true. A mode reordered on one side and not the
    // other would retune every receiver in a saved session, so the list is
    // written out rather than iterated as a range of integers.
    constexpr rpc::Demod kModes[] = {rpc::Demod::Raw, rpc::Demod::Am,  rpc::Demod::Nfm,
                                     rpc::Demod::Wfm, rpc::Demod::Usb, rpc::Demod::Lsb,
                                     rpc::Demod::Dsb, rpc::Demod::Cw};

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
    constexpr auto kUnknownOrdinal = 9;

    rpc::VrxParams params = distinctive_params();
    params.demod = static_cast<rpc::Demod>(kUnknownOrdinal);

    auto added = harness.client().add_vrx(params);
    REQUIRE_FALSE(added.has_value());
    INFO(added.error().message);
    CHECK(added.error().message.find("demodulator ordinal 9") != std::string::npos);

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
    CHECK(applied.error().message.find("demodulator ordinal 9") != std::string::npos);

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
    REQUIRE(remote->size() == local->size());
    for (std::size_t i = 0; i < local->size(); ++i) {
        INFO("entry " << i << " is " << (*local)[i].uri);
        CHECK((*remote)[i].uri == (*local)[i].uri);
        CHECK((*remote)[i].backend == (*local)[i].backend);
        CHECK((*remote)[i].display_name == (*local)[i].display_name);
        CHECK((*remote)[i].unavailable == (*local)[i].unavailable);
        CHECK((*remote)[i].available() == (*local)[i].available());
    }
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

    auto first = rpc::Server::create(eng, rpc::ServerOptions{});
    INFO(test::message_of(first));
    REQUIRE(first.has_value());
    CHECK((*first)->port() != 0);

    // A second server would replace the first's spectrum sink, and the first
    // would then hold subscriptions that never receive another frame. That
    // is silent, so it is refused here rather than discovered by a display
    // that stopped drawing.
    auto second = rpc::Server::create(eng, rpc::ServerOptions{});
    REQUIRE_FALSE(second.has_value());
    INFO(second.error().message);
    CHECK(second.error().message.find("already has an RPC server") != std::string::npos);

    (*first)->stop();

    // And the claim is released rather than held for the life of the
    // process, so a service that restarts its listener does not have to
    // rebuild the engine to do it.
    auto again = rpc::Server::create(eng, rpc::ServerOptions{});
    INFO(test::message_of(again));
    REQUIRE(again.has_value());
    CHECK((*again)->port() != 0);
}

TEST_CASE("connect refuses what cannot be a connection", "[rpc][m1]") {
    // No GPU and no engine. These are argument errors the client answers on
    // its own, and answering them before a thread is started is the point.
    auto no_address = rpc::Client::connect("", 47'000);
    REQUIRE_FALSE(no_address.has_value());
    INFO(no_address.error().message);
    CHECK(no_address.error().message.find("no address") != std::string::npos);

    // ServerOptions::port defaults to zero meaning "bind whatever is free",
    // so a caller forwarding its own options here would otherwise ask the OS
    // to connect to port zero and get a message about nothing listening.
    auto zero_port = rpc::Client::connect("127.0.0.1", 0);
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

    auto refused = rpc::Client::connect("127.0.0.1", port);

    // The whole reason client.cpp connects on the loop thread and waits
    // rather than using EzRpcClient: a client that connected in the
    // background would return a handle here and report the refusal as a
    // failed call minutes later, by which time nothing points at the port.
    REQUIRE_FALSE(refused.has_value());
    INFO(refused.error().message);
    CHECK(refused.error().message.find(std::format("127.0.0.1:{}", port)) !=
          std::string::npos);
}
