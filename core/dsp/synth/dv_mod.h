// Transmitters for the digital voice modes, written from the same clauses as
// the demodulators in core/decode.
//
// WHY THESE EXIST
//
// The reason core/dsp/synth/modulators.h gives for every other mode, and the
// reason core/dsp/synth/rds_mod.h gives at more length: writing the modulator
// beside the demodulator is how the specification gets checked. None of these
// three decoders has a GPU kernel behind it, so none of them has the
// bit-exact scalar twin the rest of this engine is held to. A round trip
// through a transmitter written from the same clause is what stands in its
// place, and a round trip that fails means one of the two ends misread the
// document, which is the failure a clean-room decoder is most likely to have.
//
// WHAT A ROUND TRIP DOES AND DOES NOT PROVE
//
// It proves the two ends agree. It does not prove either agrees with a real
// radio, and no test in this tree can, because there is no recording. Where a
// standard leaves something unstated and this project had to choose, the
// choice is the same at both ends and the round trip is blind to it. Both
// such places are labelled: the D-STAR bandwidth-time product in
// core/decode/dstar.h and the P_FCS byte order in core/decode/dstar.cpp.
//
// THE CONSTANTS LIVE IN THE DECODERS
//
// Every specified value these transmitters use is included from the matching
// decode header rather than restated here. One definition, one citation, and
// no way for the two ends to drift apart in a way a round trip would hide by
// making the same mistake twice. That is the arrangement rds_mod.h uses for
// the EN 50067 clause 1.7 shaping filter and the reason is the same.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/decode/dstar.h"
#include "core/decode/p25p1.h"
#include "core/decode/tetra.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// P25 Phase 1, C4FM
// ---------------------------------------------------------------------------

struct P25ModConfig {
    SampleRate rate = 48000;

    // Peak amplitude of the constant-envelope output.
    double amplitude = 1.0;

    // Taps in the cascaded clause 9.3 and 9.4 filters. Odd.
    //
    // 65 is enough, and the reason is a property of clause 9.3 rather than a
    // guess. Its raised cosine reaches zero at 2880 Hz with zero slope,
    // because the cosine's argument is 3*pi there, so the response has no
    // corner for a windowed design to ring on and the impulse response decays
    // fast. This was briefly raised to 181 on the reasoning that a 960 Hz
    // transition needs 3.3 times the sample rate over the tap count; that
    // rule is for a transition with corners and it made the round trip worse,
    // by lengthening the filter's transient past the frame sync at the head
    // of a capture.
    std::size_t filter_taps = 65;
};

// One Header Data Unit followed by a terminator, which is the shortest thing
// a P25 transmitter can send that carries a header.
struct P25HeaderMessage {
    std::uint16_t network_access_code = 0x293;
    decode::P25Header header;
};

// Renders the clause 10.2 Header Data Unit and a clause 8 simple terminator
// as C4FM complex baseband, including the clause 8.4 status symbols and the
// five null symbols the annex ends with.
[[nodiscard]] Expected<std::vector<Complex32>> p25_render_header_message(
    const P25ModConfig& config, const P25HeaderMessage& message);

// Renders an arbitrary dibit sequence as C4FM, with no framing added. Used by
// the tests to measure a symbol error rate against a known pattern without a
// frame structure in the way.
[[nodiscard]] Expected<std::vector<Complex32>> p25_render_dibits(
    const P25ModConfig& config, std::span<const std::uint8_t> dibits);

// Every dibit of that message, in transmission order and including the
// clause 8.4 status symbols and the clause 10.2 null symbols.
//
// Exposed so a caller can concatenate messages and render them as one stream.
// That is not a convenience: a receiver loses the tail of a capture to its
// own filter delay and to the timing estimator's window, so a test that wants
// the last data unit of a transmission decoded has to put something after it,
// exactly as a real transmitter does.
[[nodiscard]] Expected<std::vector<std::uint8_t>> p25_header_message_dibits(
    const P25HeaderMessage& message);

// ---------------------------------------------------------------------------
// D-STAR DV, GMSK
// ---------------------------------------------------------------------------

struct DStarModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;

    // Not a specified value: the JARL standard names GMSK and states no
    // bandwidth-time product. See core/decode/dstar.h.
    double bandwidth_time = 0.5;

    std::size_t filter_taps = 65;
};

struct DStarMessage {
    decode::DStarHeader header;

    // The AMBE payload is not synthesised, because there is no published
    // algorithm to synthesise it from. Each entry is the 72 bits that go in a
    // voice slot, supplied by the caller, and the tests supply a pseudorandom
    // pattern whose only job is to be recovered unchanged.
    std::vector<std::array<std::uint8_t, decode::kDStarVoiceBits>> voice_frames;

    // The 24 bit data slot for each frame. Entries beyond the size of this
    // vector are filled with zeros, and every frame where clause 4.1.2 c
    // requires the resynchronisation signal gets it regardless of what is
    // here.
    std::vector<std::array<std::uint8_t, decode::kDStarDataBits>> data_frames;

    // Appends the clause 4.1.2 h last frame.
    bool terminate = true;
};

// Renders a complete D-STAR voice transmission: the clause 4.1.1 a bit sync,
// the clause 4.1.1 b frame sync, the error-corrected radio header, and the
// voice and data frames with the clause 4.1.2 c resynchronisation signals in
// place.
[[nodiscard]] Expected<std::vector<Complex32>> dstar_render(const DStarModConfig& config,
                                                            const DStarMessage& message);

// The bit sequence that transmission puts on the air, before modulation.
[[nodiscard]] Expected<std::vector<std::uint8_t>> dstar_message_bits(const DStarMessage& message);

// Renders an arbitrary bit sequence as GMSK with no framing.
[[nodiscard]] Expected<std::vector<Complex32>> dstar_render_bits(
    const DStarModConfig& config, std::span<const std::uint8_t> bits);

// ---------------------------------------------------------------------------
// TETRA V+D, pi/4-DQPSK
// ---------------------------------------------------------------------------

struct TetraModConfig {
    SampleRate rate = 72000;
    double amplitude = 1.0;
    std::size_t filter_taps = 65;
};

// Renders one clause 9.4.4.2.6 synchronisation continuous downlink burst,
// carrying the given SYNC PDU on the broadcast synchronisation channel.
//
// The clause 8 content of block 2 and the broadcast bits is not synthesised:
// those channels are scrambled with the cell's real colour code and carry
// signalling this project does not decode, so the transmitter fills them with
// a pseudorandom pattern from `filler_seed`. The burst is still a valid
// synchronisation burst as far as everything this tree reads is concerned,
// and the test says so rather than the transmitter pretending otherwise.
[[nodiscard]] Expected<std::vector<Complex32>> tetra_render_sync_burst(
    const TetraModConfig& config, const decode::TetraSyncPdu& pdu, std::uint64_t filler_seed);

// The 510 bits of that burst, before modulation.
[[nodiscard]] Expected<std::vector<std::uint8_t>> tetra_sync_burst_bits(
    const decode::TetraSyncPdu& pdu, std::uint64_t filler_seed);

// Renders an arbitrary bit sequence as pi/4-DQPSK with no burst structure.
// The sequence must be a whole number of bit pairs, per clause 5.4.
[[nodiscard]] Expected<std::vector<Complex32>> tetra_render_bits(
    const TetraModConfig& config, std::span<const std::uint8_t> bits);

}  // namespace revenant::siggen
