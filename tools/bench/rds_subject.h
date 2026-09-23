// The RDS subject: core/dsp/synth/rds_mod.h as the transmitter,
// core/decode/rds_bits.h as the receiver, and AWGN between them at a stated
// Eb/N0.
//
// This is the first decoder the harness sweeps. sweep.h was written against a
// reference BPSK detector because there was no decoder to put in its place;
// the reference stays, because it is still the only subject with a closed
// form and so the only one that can prove the harness's own calibration.
//
// WHAT A TRIAL IS
//
// The payload's bits, most significant first, become the RDS data bits. They
// are differentially encoded and biphase coded onto a 57 kHz subcarrier on a
// composite with the 19 kHz pilot and a loud mono programme tone, at the
// clause 1.3 recommended 2 kHz of injection. Real AWGN is added calibrated
// against the RDS component's own power and the 1187.5 bit/s information
// rate, per docs/snr-convention.md, so the SNR axis is Eb/N0 as for the BPSK
// reference and the two curves share an axis. The decoder runs over the whole
// composite and the bits it recovers are aligned against the payload.
//
// HOW A TRIAL IS SCORED
//
// A trial whose decoder locked is scored on the bits it recovered that
// overlap the payload, which is what tests/decode/test_rds_bits.cpp's
// "bit error rate against Eb/N0" case measures and so what the two can be
// compared on. The bits before lock are acquisition, not errors: counting
// them would put a floor under the curve set by the payload length.
//
// A trial that did not lock, or locked on less than a quarter of the payload,
// recovered nothing, and nothing is worth a coin toss per bit. It is scored
// as every payload bit with half of them wrong and reported as not decoded,
// so the decode-rate column is the lock rate. Scoring it as zero bits instead
// would drop the worst trials out of the average, and the curve would improve
// as the decoder got worse at locking.
//
// The alignment is found once, over the first 256 recovered bits, and held.
// A decoder that slips a bit after that scores every later bit against the
// wrong neighbour, which is about half wrong. That is deliberate: a bit pipe
// that slips has delivered wrong bits, and the group layer's resync is a
// separate stage with a separate test. It is also why this curve sits above
// the single-realisation figures in the decoder test. Measured on a debug
// build on 2026-09-22 at 4 dB Eb/N0, one trial per seed over seeds 1 to 12:
// eleven trials between 2.1 and 4.4 percent, which is the decoder test's 3.0
// percent, and one at 29 percent, which is what a slip looks like to this
// score. That trial's cause was not examined.
//
// Everything is a pure function of the payload, the SNR and the seed, as
// sweep.h requires of every subject and generator.

#pragma once

#include <cstddef>
#include <cstdint>

#include "core/dsp/types.h"
#include "tools/bench/sweep.h"

namespace revenant::bench {

struct RdsSubjectConfig {
    // 171000 is 144 samples per bit and three per subcarrier cycle, the rate
    // the decoder tests use and the one a WFM receiver hands the decoder.
    dsp::SampleRate rate = 171000;

    // EN 50067:1998 clause 1.3's recommended injection.
    dsp::Hertz rds_deviation_hz = 2000;

    // An ordinary loud mono broadcast, 24 dB above the subcarrier, so the
    // decoder is asked to reject something. The decoder tests use the same.
    dsp::Hertz mono_tone_hz = 1000;
    dsp::Hertz mono_deviation_hz = 40000;
};

// Payload bits a trial needs before the score means anything. The decoder
// spends its 96-bit acquisition scan and its 256-bit lock window before it
// emits, so a payload much shorter than this is mostly acquisition.
inline constexpr std::size_t kRdsMinimumPayloadBytes = 128;

[[nodiscard]] Generator make_rds_generator(RdsSubjectConfig config = {});
[[nodiscard]] Subject make_rds_subject(RdsSubjectConfig config = {});

}  // namespace revenant::bench
