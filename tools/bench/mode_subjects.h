// Bench subjects for every decoder in core/decode that has a transmitter in
// core/dsp/synth, beside the RDS one in tools/bench/rds_subject.h.
//
// WHY
//
// docs/architecture.md's M4 milestone asks for a BER/SNR curve per mode, and
// the project rule is never to ship a decoder that quietly does worse than the
// free tool it replaces. That makes each mode's sensitivity a published
// number, and a number that is published has to be reproducible from one
// command and watched every night. Until these existed each decoder had an
// error-rate case in tests/decode that reported two or three points, which
// says where a decoder was on the day the case was written and nothing about
// where the curve crosses.
//
// WHAT A TRIAL IS
//
// The harness hands a subject random payload bytes. Each mode turns them into
// what it carries, deterministically, so the generator and the subject agree
// on what was sent without sharing state: characters drawn from a pool by
// byte value for the text modes, whole frames or pages cut from the bytes for
// the framed ones, and the bits themselves for the three digital voice modes,
// whose tests measure the demodulator on a raw stream. The generator renders
// that through the mode's transmitter and adds white noise through
// core/dsp/synth/channel.h or add_real_awgn; the subject runs the decoder and
// scores what came back.
//
// THE AXIS
//
// Every mode here is swept against signal to noise in the 2500 Hz reference
// bandwidth, the convention docs/snr-convention.md fixes for every Revenant
// sensitivity claim. For the audio modes that is the audio's SNR in 2500 Hz
// of one-sided audio spectrum, which is what add_real_awgn and
// analytic_to_noisy_audio calibrate; for the complex baseband modes it is the
// RF signal's SNR in 2500 Hz. Some tests in tests/decode state their figures
// in the full sample-rate bandwidth or in M17's 9 kHz; the conversion is
// exact and docs/sensitivity.md gives both where they differ. The RDS subject
// keeps its Eb/N0 axis.
//
// THE UNIT
//
// The error rate a curve carries is named per mode rather than always being
// a bit error rate, because a bit error rate is not what a reader of RTTY or
// POCSAG experiences:
//
//   characters  edit distance between the text sent and the text decoded,
//               over the characters sent, capped at one per character sent
//               so a decoder printing garbage cannot score above 1
//   frames      frames sent that did not come back byte for byte
//   pages       pages sent whose identity and text did not come back
//   messages    NAVTEX messages not received exactly with a clean preamble
//   bits        demodulated bits wrong after alignment, with a trial that
//               could not be aligned scored as every bit half wrong, the RDS
//               subject's rule and for its reason
//
// The curve file's fields are still called ber and bit_errors, because the
// schema is shared with the RDS and BPSK curves and compare reads them the
// same way whatever they count. The subject string records the unit.
//
// Everything is a pure function of the payload, the SNR and the seed, as
// sweep.h requires.

#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>

#include "core/dsp/types.h"
#include "core/error.h"
#include "tools/bench/sweep.h"

namespace revenant::bench {

struct ModeSubject {
    // The --mode name.
    std::string mode;

    // Recorded in the curve. Names the transmitter settings, the unit and the
    // axis, so a curve file read on its own says what it measured.
    std::string subject;

    // "character", "frame", "page", "message" or "bit".
    std::string unit;

    // For the printed report, for example "SNR in 2500 Hz".
    std::string axis;

    // The error rate at which the published sensitivity is read. 0.01 for
    // every mode here; the field exists so a mode whose natural operating
    // point is elsewhere can say so rather than being read at the wrong one.
    double threshold = 0.01;

    // What one trial carries. A payload must be a whole number of units of
    // payload_multiple bytes, and at least minimum_payload_bytes.
    std::size_t default_payload_bytes = 0;
    std::size_t minimum_payload_bytes = 1;
    std::size_t payload_multiple = 1;

    // The grid and trial count the nightly workflow sweeps and the committed
    // baseline was made with. The CLI uses them when no option overrides.
    double snr_start_db = 0.0;
    double snr_stop_db = 0.0;
    double snr_step_db = 1.0;
    std::uint64_t trials = 0;

    // SweepConfig::min_bit_errors. 100 for the modes counted in characters,
    // frames, pages and messages. Zero for the bit modes: a bit trial carries
    // thousands of bits, so 100 errors would end every point after its first
    // batch whatever the error rate, and one trial that slipped would be most
    // of what the point measured, which is the RDS subject's reason for the
    // same setting in the nightly.
    std::uint64_t min_errors = 100;

    // The rate the generator renders at, and whether its output is real audio
    // carried in the real part of the complex samples the harness moves, or
    // complex baseband.
    dsp::SampleRate rate = 48000;
    bool real_audio = true;

    Generator generator;
    Subject score;

    // What a payload puts on the air, as text: the characters for a text
    // mode, one hex line per frame or page, a line of 0s and 1s for a bit
    // mode. So a trial reproduced by siggen has its ground truth beside it.
    std::function<std::string(std::span<const std::uint8_t>)> truth;
};

// Every mode name make_mode_subject accepts, in the order the nightly sweeps
// them.
[[nodiscard]] std::span<const std::string_view> mode_subject_names();

[[nodiscard]] Expected<ModeSubject> make_mode_subject(std::string_view mode);

}  // namespace revenant::bench
