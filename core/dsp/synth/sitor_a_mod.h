// A SITOR-A link as a monitor hears it, written from the same clauses of
// ITU-R M.625-4 as core/decode/sitor_a.h.
//
// WHY THIS EXISTS
//
// The reason core/dsp/synth/fsk_mod.h gives. A monitor's input is two
// stations taking turns, so this renders both: the ISS's 210 ms blocks on
// the master's 450 ms cycle (clauses 3.2.3 and 3.3.2), and the IRS's 70 ms
// control signal (clause 3.4.1) at a delay that stands for the two paths and
// the IRS's turnaround, at its own level and with its own fractional timing.
// The IRS follows clauses 3.7.5 and 3.7.6, and can be told to lose chosen
// blocks so that the repetition of clause 3.7.10's other half, the ISS
// sending the same block again, is exercised.

#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <set>
#include <span>
#include <vector>

#include "core/decode/sitor_a.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

struct SitorAModConfig {
    dsp::SampleRate rate = 48000;
    bool upper_sideband = true;
    double iss_amplitude = 0.5;
    double irs_amplitude = 0.3;
    // From the start of a block to the start of the control signal that
    // answers it, as the monitor hears them: more than the 210 ms block, and
    // no more than the 450 ms cycle less the 70 ms signal.
    double control_delay_ms = 287.3;
    // Silence before the first control signal and after the last.
    double lead_ms = 100.0;
    double tail_ms = 300.0;
    // Block indices, counted from 0 over every block sent, that the IRS
    // fails to receive and asks for again.
    std::set<std::size_t> lost_at_irs;
    // Close with clause 3.7.14's end-of-communication block.
    bool end_of_communication = true;
};

// Clause 3.7.2: ITA2 combinations to 7-unit signals, three to a block,
// the last block filled with idle beta.
[[nodiscard]] std::vector<std::array<std::uint8_t, decode::kSitorABlockSignals>> sitor_a_blocks(
    std::span<const std::uint8_t> combinations);

struct SitorALink {
    std::vector<float> audio;
    // Every block put on the air, repetitions included, and the control
    // signals in order, the first being the one before the first block.
    std::vector<std::array<std::uint8_t, decode::kSitorABlockSignals>> blocks_sent;
    std::vector<std::uint8_t> controls_sent;
};

[[nodiscard]] Expected<SitorALink> sitor_a_render(
    const SitorAModConfig& config,
    std::span<const std::array<std::uint8_t, decode::kSitorABlockSignals>> blocks);

}  // namespace revenant::siggen
