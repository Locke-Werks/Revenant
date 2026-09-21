// One RdsStation turned into what a pane shows, including the part that
// says why there is nothing to show.
//
// AN EMPTY PANE CANNOT TELL YOU WHICH, WHICH IS THE WHOLE PROBLEM
//
// Four different situations produce a station struct with nothing in it: a
// decoder that faulted, a decoder that was cleared for a retune and is
// being fed nothing on purpose, a decoder that has never locked to a
// subcarrier, and a station that is locked and simply carries no RDS. They
// need four different actions and they render identically. core/rpc/types.h
// says so twice, on RdsStation::fault and on ::discarding, and both notes
// end with an instruction to check the field before drawing anything else.
// This is where that check lives.
//
// So the pane always has a sentence, and the sentence is about the decoder
// rather than about the station until the decoder has something to say.
//
// WHY THIS HOLDS NO Qt. ui/tests links it. It includes core/rpc/types.h,
// which is three standard headers and no Cap'n Proto, so the test binary
// stays a test binary. The parts that can be wrong are the precedence of
// the four states, the error rate's denominator and the text; all three are
// pure functions of the struct.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/rpc/types.h"
#include "models/rds_text.h"

namespace revenant::ui {

// What the decoder is doing, in the order a display has to ask.
enum class RdsState : std::uint8_t {
    // No poll has been answered yet. Distinct from Unlocked: nothing has
    // been asked of the engine, so the band has not been given a verdict.
    Idle,

    // RdsStation::fault is non-empty. Terminal for the life of the
    // receiver; the engine's own sentence says which of the two shape
    // faults it was.
    Faulted,

    // RdsStation::discarding. The retune fence is up and the decoder is
    // being fed nothing on purpose, so every zero below is a default and
    // not a measurement.
    Retuning,

    // No subcarrier. Either there is no RDS here or the signal is too weak
    // to bring the biphase loop in.
    Unlocked,

    // The bit layer is coming in and has not reached its threshold. No
    // bits are emitted in this state, so there is nothing partial to draw.
    Acquiring,

    // Locked to the subcarrier and still hunting for the offset words,
    // which is what the first second after a tune looks like.
    Syncing,

    // Groups are arriving.
    Decoding,
};

// What the pane draws.
struct RdsView {
    RdsState state = RdsState::Idle;

    // The sentence about the DECODER, which is always present. Empty is not
    // a state this produces.
    std::string status;

    // The station's own fields, rendered. Only meaningful once state is
    // Decoding, and the pane hides them otherwise rather than showing a
    // struct's defaults.
    RdsText ps;
    RdsText radio_text;

    // Call sign when the region derives one from the PI, the PI in hex
    // when it does not, and empty when no PI has arrived.
    std::string identity;

    // "6 Classic Rock", or empty before a PTY has arrived. The name comes
    // off the wire because half the table differs between RDS and RBDS.
    std::string programme_type;

    // Blocks that did not arrive clean, over blocks the decoder looked at.
    // Negative when no blocks have been looked at, which is not the same
    // as a zero rate.
    double block_error_rate = -1.0;

    // Whether the pane should be coloured as a fault. Faulted only:
    // unlocked on a band with no RDS is the ordinary answer and is not bad
    // news.
    bool is_fault = false;
};

namespace detail {

// A percentage to one decimal, in integers, so a test compares the string.
[[nodiscard]] inline std::string percent_text(double fraction)
{
    const auto tenths = static_cast<std::int64_t>(fraction * 1000.0 + 0.5);
    std::string out = std::to_string(tenths / 10);
    out += '.';
    out += std::to_string(tenths % 10);
    out += '%';
    return out;
}

[[nodiscard]] inline std::string hex16(std::uint16_t value)
{
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string out = "0x";
    for (int shift = 12; shift >= 0; shift -= 4) {
        out += kDigits[(value >> shift) & 0xF];
    }
    return out;
}

}  // namespace detail

// Blocks that did not arrive clean, over blocks the decoder looked at.
//
// CORRECTED BLOCKS COUNT AS ERRORS. core/rpc/types.h says a corrected block
// had a burst repaired rather than being received clean and is trusted less
// than a good one, so the figure an operator wants from "block error rate"
// is how much of the bitstream needed help. Counting only dropped blocks
// would report a fading station with a working error corrector as perfect.
//
// Negative when the decoder has looked at no blocks at all, which is the
// first moment after a tune and is not a rate of zero.
[[nodiscard]] inline double block_error_rate(const rpc::RdsHealth& health)
{
    const std::uint64_t seen =
        health.blocks_good + health.blocks_corrected + health.blocks_dropped;
    if (seen == 0) {
        return -1.0;
    }
    return static_cast<double>(health.blocks_corrected + health.blocks_dropped) /
           static_cast<double>(seen);
}

// The whole view.
//
// answered is whether a poll has come back at all. Without it an engine
// that has never been asked is indistinguishable from one that answered
// with an unlocked decoder, and those are Idle and Unlocked.
//
// fault is what stopped the poll from happening or what the engine said
// when it refused one, and it outranks everything including answered. It
// has to, because "answered is false" covers two opposite situations: the
// operator has the RDS switch off, and the window is asking as hard as it
// can and there is no engine to ask. Both used to render as "not asking for
// RDS on this receiver.", so a pane with the switch ON read, on
// disconnection, as a statement that the operator had not asked. That is
// the exact opposite of what was true, and it is worse than a blank pane
// because it tells the operator to go and flip a switch that is already on.
//
// EngineLink::clear_rds is what supplies it: it is called on a lost
// connection, on a receiver going away and on the switch being turned off,
// and only the third of those is silence the operator chose.
[[nodiscard]] inline RdsView make_rds_view(const rpc::RdsStation& station, bool answered,
                                           std::string_view fault = {})
{
    RdsView out;

    if (!fault.empty()) {
        out.state = RdsState::Faulted;
        out.is_fault = true;
        out.status = std::string(fault);
        return out;
    }

    if (!answered) {
        out.state = RdsState::Idle;
        out.status = "not asking for RDS on this receiver.";
        return out;
    }

    // ORDER MATTERS AND IS THE POINT OF THIS FUNCTION. fault outranks
    // everything because the fields beside it are frozen and every counter
    // below would be read as current. discarding outranks the lock state
    // because a decoder being fed nothing has not failed to lock, it has
    // not been given anything to lock to.
    if (!station.fault.empty()) {
        out.state = RdsState::Faulted;
        out.is_fault = true;
        out.status = "the decoder stopped: " + station.fault;
        return out;
    }

    if (station.discarding) {
        out.state = RdsState::Retuning;
        out.status = "retuning: the decoder was cleared and the new tuning has not "
                     "reached it yet.";
        return out;
    }

    out.block_error_rate = block_error_rate(station.health);

    // No default case; see cmake/CompilerFlags.cmake.
    switch (station.health.lock) {
        case rpc::RdsLock::Unlocked:
            out.state = RdsState::Unlocked;
            out.status = "no RDS subcarrier here. Either this station carries none or "
                         "the signal is too weak to lock to it.";
            return out;

        case rpc::RdsLock::Acquiring:
            out.state = RdsState::Acquiring;
            out.status = "acquiring the subcarrier. No bits are emitted until it locks, "
                         "so nothing here is partial text.";
            return out;

        case rpc::RdsLock::Locked:
            break;
    }

    // No default case; see cmake/CompilerFlags.cmake.
    switch (station.health.sync) {
        case rpc::RdsSync::Hunting:
        case rpc::RdsSync::PreSync:
            out.state = RdsState::Syncing;
            out.status = "locked to the subcarrier, hunting for block sync.";
            return out;

        case rpc::RdsSync::Synced:
            break;
    }

    out.state = RdsState::Decoding;

    // RadioText segments are four characters from a 2A group and two from a
    // 2B, and rt_version_b is the only thing that says which. Placing them
    // wrong misaligns every placeholder rather than failing, so it is read
    // rather than assumed.
    const std::size_t rt_segment = station.rt_version_b ? 2 : 4;

    out.ps = render_rds_text(station.ps, station.ps_received, 2, 8);
    out.radio_text = render_rds_text(station.rt, station.rt_received, rt_segment,
                                     station.rt_length);

    if (station.pi_valid) {
        out.identity = station.call_sign.empty() ? detail::hex16(station.pi)
                                                 : station.call_sign;
    }

    if (station.pty_valid) {
        out.programme_type = std::to_string(static_cast<unsigned>(station.pty));
        if (!station.pty_long_name.empty()) {
            out.programme_type += ' ';
            out.programme_type += station.pty_long_name;
        }
    }

    out.status = "decoding.";
    if (out.block_error_rate >= 0.0) {
        out.status += "  " + detail::percent_text(out.block_error_rate) +
                      " of blocks needed correcting or were dropped.";
    }
    return out;
}

}  // namespace revenant::ui
