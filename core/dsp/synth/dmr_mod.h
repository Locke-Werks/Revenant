// A DMR transmitter, written from the same clauses of ETSI TS 102 361-1 as
// the decoder in core/decode/dmr.h.
//
// WHY THIS EXISTS
//
// The reason core/dsp/synth/dv_mod.h gives for the other digital voice
// modes: none of these decoders has a GPU kernel and so none has the
// bit-exact scalar twin the engine is held to, and a round trip through a
// transmitter written from the same document is what stands in its place.
// It proves the two ends agree and cannot prove either agrees with a radio,
// because there is no recording; the one choice the document leaves to the
// receiver, the discriminator's polarity, is labelled in core/decode/dmr.h.
//
// THE CONSTANTS LIVE IN THE DECODER
//
// Every specified value is taken from core/decode/dmr.h and
// core/decode/dmr_codes.h rather than restated, so the two ends cannot drift
// apart in a way the round trip would hide by making the same mistake twice.
//
// WHAT IT BUILDS
//
// Bursts, bit for bit as Annex E orders them, and a timeline of 30 ms slots
// rendered as 4FSK through the clause 10.2.2.2 filter. A slot on an outbound
// channel carries its CACH in the 12 symbols in front of the burst; on an
// inbound or direct mode channel those symbols are guard time and the carrier
// is off, and so is it through a slot with no burst, ramping up and down in
// the 1,5 ms either side of each burst that figure 10.3 gives the power mask.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/dmr.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;
using dsp::SampleRate;

struct DmrModConfig {
    SampleRate rate = 48000;
    double amplitude = 1.0;

    // Taps in the clause 10.2.2.2 filter. Odd. The same span the receiver
    // uses, since the clause splits one Nyquist filter between the two.
    std::size_t filter_taps = 121;
};

using DmrBurstBits = std::array<std::uint8_t, decode::kDmrBurstBits>;
using DmrCachBurstBits = std::array<std::uint8_t, decode::kDmrCachBits>;
using DmrVoiceBits = std::array<std::uint8_t, decode::kDmrVoiceBits>;

// ---------------------------------------------------------------------------
// Bursts
// ---------------------------------------------------------------------------

// Figure 6.3: vocoder bits either side of a voice sync, burst A.
[[nodiscard]] DmrBurstBits dmr_voice_burst_with_sync(const DmrVoiceBits& voice,
                                                     decode::DmrSyncType sync);

// Figure 6.4: vocoder bits either side of the EMB and 32 bits of embedded
// signalling, bursts B to F.
[[nodiscard]] DmrBurstBits dmr_voice_burst_embedded(
    const DmrVoiceBits& voice, std::uint8_t colour_code, bool pi, std::uint8_t lcss,
    std::span<const std::uint8_t, decode::kDmrEmbeddedFragmentBits> fragment);

// Figure 6.5: a general data burst with a sync, the slot type, and 196
// information bits already through their FEC.
[[nodiscard]] DmrBurstBits dmr_data_burst(decode::DmrSyncType sync, std::uint8_t colour_code,
                                          decode::DmrDataType data_type,
                                          std::span<const std::uint8_t, decode::kDmrBptcBits> info);

// A data burst whose payload is one BPTC (196,96) block built from octets by
// decode::dmr_data_block_bits: a voice LC header, a terminator, a CSBK, a data
// header and the rest of Table 6.1's BPTC types.
[[nodiscard]] Expected<DmrBurstBits> dmr_bptc_burst(decode::DmrSyncType sync, std::uint8_t colour_code,
                                                    decode::DmrDataType data_type,
                                                    std::span<const std::uint8_t> octets);

// Clause 7.3 and Table D.2: an Idle burst.
[[nodiscard]] DmrBurstBits dmr_idle_burst(decode::DmrSyncType sync, std::uint8_t colour_code);

// Figure B.9: a CACH burst from its TACT and seventeen payload bits. `channel`
// is the TDMA channel of the burst that follows, 1 or 2.
[[nodiscard]] DmrCachBurstBits dmr_cach(bool access_busy, std::uint8_t channel, std::uint8_t lcss,
                                        std::span<const std::uint8_t, decode::kDmrCachPayloadBits> payload);

// Figure B.6: a Short LC's four CACH payloads, with the LCSS each goes out
// under, first, two continuations and last.
struct DmrShortLcFragments {
    std::array<std::array<std::uint8_t, decode::kDmrCachPayloadBits>, decode::kDmrShortLcFragments> payload{};
    std::array<std::uint8_t, decode::kDmrShortLcFragments> lcss{};
};

// `slco` and the 24 data bits of TS 102 361-2 clause 7.1.3.
[[nodiscard]] DmrShortLcFragments dmr_short_lc_fragments(std::uint8_t slco, std::uint32_t data);

// ---------------------------------------------------------------------------
// A voice call on one slot
// ---------------------------------------------------------------------------

struct DmrVoiceCall {
    std::uint8_t colour_code = 1;

    // The voice sync the call's A bursts carry; its complement, Table 9.2,
    // is the data sync of the header and the terminator.
    decode::DmrSyncType voice_sync = decode::DmrSyncType::BsVoice;

    // Figure 7.1, the nine octets of Full LC every carrier of it repeats.
    std::array<std::uint8_t, 9> lc{};

    // Clause 5.1.2.2: a voice LC header first. Leaving it out is how a test
    // stands in for a receiver joining after it.
    bool header = true;

    // Clause 5.1.2.3: a terminator with LC after the last superframe.
    bool terminator = true;

    // The vocoder socket bits of every voice burst, six to a superframe.
    // AMBE+2 has no document, so a test supplies patterns whose only job is
    // to come back unchanged.
    std::vector<DmrVoiceBits> voice;
};

// The bursts of that call in order: header, six voice bursts per superframe
// with the Full LC embedded in B to E and the Null embedded message of Annex
// D.1 in F (clause 7.1.3.2), and the terminator.
[[nodiscard]] Expected<std::vector<DmrBurstBits>> dmr_voice_call_bursts(const DmrVoiceCall& call);

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// One 30 ms slot: the 12 symbols ahead of the burst and the burst.
struct DmrSlot {
    std::optional<DmrCachBurstBits> cach;
    std::optional<DmrBurstBits> burst;
};

// Renders slots back to back as 4FSK: Table 10.3's deviations through the
// clause 10.2.2.2 filter into a frequency modulator, with the carrier off
// across any 12 symbols with no CACH and any burst that is absent.
[[nodiscard]] Expected<std::vector<Complex32>> dmr_render_slots(const DmrModConfig& config,
                                                                std::span<const DmrSlot> slots);

// Renders dibits as 4FSK with no framing and the carrier always on.
[[nodiscard]] Expected<std::vector<Complex32>> dmr_render_dibits(const DmrModConfig& config,
                                                                 std::span<const std::uint8_t> dibits);

}  // namespace revenant::siggen
