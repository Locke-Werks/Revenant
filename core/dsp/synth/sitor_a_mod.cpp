#include "core/dsp/synth/sitor_a_mod.h"

#include <cmath>
#include <numbers>

namespace revenant::siggen {

namespace {

// One burst of 7-unit signals, continuous phase within the burst, added at a
// fractional sample start.
void put_burst(std::vector<float>& audio, double start, std::span<const std::uint8_t> signals,
               const SitorAModConfig& config, double amplitude) {
    const double spu = static_cast<double>(config.rate) / decode::kSitorBaud;
    const double y = decode::kSitorCentreHz - decode::kSitorShiftHz / 2.0;
    const double b = decode::kSitorCentreHz + decode::kSitorShiftHz / 2.0;
    const double units = static_cast<double>(signals.size() * decode::kSitorSignalUnits);
    const auto first = static_cast<std::size_t>(std::ceil(start));
    const auto end = static_cast<std::size_t>(std::floor(start + units * spu));
    if (audio.size() < end + 1) {
        audio.resize(end + 1, 0.0F);
    }
    double phase = 0.0;
    for (std::size_t n = first; n < end; ++n) {
        const auto u = static_cast<std::size_t>((static_cast<double>(n) - start) / spu);
        const std::uint8_t s = signals[u / decode::kSitorSignalUnits];
        // Table 1 notes 2 and 3: bit position 1 first, Y = 1 the lower
        // emitted frequency; an LSB receiver hears it as the higher tone.
        const bool one = ((s >> (u % decode::kSitorSignalUnits)) & 1U) != 0U;
        const bool low = one == config.upper_sideband;
        const double hz = low ? y : b;
        audio[n] += static_cast<float>(amplitude * std::sin(phase));
        phase = std::fmod(phase + 2.0 * std::numbers::pi * hz / config.rate, 2.0 * std::numbers::pi);
    }
}

}  // namespace

std::vector<std::array<std::uint8_t, decode::kSitorABlockSignals>> sitor_a_blocks(
    std::span<const std::uint8_t> combinations) {
    std::vector<std::array<std::uint8_t, decode::kSitorABlockSignals>> out;
    for (std::size_t i = 0; i < combinations.size(); i += decode::kSitorABlockSignals) {
        std::array<std::uint8_t, decode::kSitorABlockSignals> b{};
        for (std::size_t k = 0; k < decode::kSitorABlockSignals; ++k) {
            b[k] = i + k < combinations.size() ? decode::sitor_encode(combinations[i + k])
                                               : decode::kSitorAIdleBeta;
        }
        out.push_back(b);
    }
    return out;
}

Expected<SitorALink> sitor_a_render(
    const SitorAModConfig& config,
    std::span<const std::array<std::uint8_t, decode::kSitorABlockSignals>> blocks) {
    if (config.rate < 1000) {
        return fail("a SITOR-A link needs a sample rate of at least 1000 S/s");
    }
    if (config.control_delay_ms < 210.0 || config.control_delay_ms > 380.0) {
        return fail("the control signal must fall in the 240 ms pause after the block");
    }
    const double ms = static_cast<double>(config.rate) / 1000.0;
    const double cycle = 450.0 * ms;
    SitorALink link;
    // Clause 3.5.6: the IRS opens with control signal 1, which asks for
    // "information block 1".
    std::uint8_t cs = decode::kSitorACs1;
    double t = config.lead_ms * ms;
    const auto control = [&](double at, std::uint8_t signal) {
        const std::uint8_t one[1] = {signal};
        put_burst(link.audio, at, one, config, config.irs_amplitude);
        link.controls_sent.push_back(signal);
    };
    control(t + (config.control_delay_ms - 450.0) * ms + cycle, cs);
    t += cycle;

    std::vector<std::array<std::uint8_t, decode::kSitorABlockSignals>> queue(blocks.begin(), blocks.end());
    if (config.end_of_communication) {
        queue.push_back({decode::kSitorAIdleAlpha, decode::kSitorAIdleAlpha, decode::kSitorAIdleAlpha});
    }
    std::size_t sent = 0;
    for (std::size_t next = 0; next < queue.size();) {
        put_burst(link.audio, t, queue[next], config, config.iss_amplitude);
        link.blocks_sent.push_back(queue[next]);
        const bool lost = config.lost_at_irs.contains(sent);
        ++sent;
        if (!lost) {
            // Clauses 3.7.5 and 3.7.6: an accepted block flips the control
            // signal, and 3.7.8 and 3.7.9 the ISS moves to the next block.
            cs = cs == decode::kSitorACs1 ? decode::kSitorACs2 : decode::kSitorACs1;
            ++next;
        }
        control(t + config.control_delay_ms * ms, cs);
        t += cycle;
    }
    link.audio.resize(static_cast<std::size_t>(t + config.tail_ms * ms), 0.0F);
    return link;
}

}  // namespace revenant::siggen
