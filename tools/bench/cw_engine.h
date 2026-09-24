// CW through the engine: a keyed carrier on a radio frequency, received by cw,
// usb and lsb receivers tuned so the tone lands at chosen audio pitches, and
// the "cw" decoder of core/rpc/decoders.h reading each receiver's audio, the
// same adapter a client's decode pane is fed by.
//
// WHY THIS AND NOT `bench sweep --mode cw`
//
// The sweep hands the decoder audio with the tone 20 Hz off 700 Hz, which is
// the one place the decoder was written to look. On the air the tone lands
// wherever the operator's tuning put it: a usb or lsb receiver parked in a CW
// segment hears several stations at several pitches, and a cw receiver hears
// the station it was clicked on at its sidetone pitch plus however far off
// zero beat the click was. This grid measures that, through the demodulators
// and their filters, rather than around them.
//
// WHAT A CELL IS
//
// One receiver variant, one audio pitch, one speed and one SNR, over
// `trials` transmissions of `characters` random letters and figures in words
// of two to six. The SNR is in 2500 Hz at the radio frequency, with the
// signal's power taken over the keyer's own render, lead-in and tail
// included, which is the convention `bench sweep --mode cw` uses for the
// audio; docs/sensitivity.md says why the two agree for a sideband receiver.
// Noise runs over the whole file, so each receiver hears two seconds of it
// before the first element and three and a half after the last.
//
// The error rate is the sweep's: edit distance between the text sent and the
// text printed, capped at the characters sent. When the decoder labels its
// lines with a pitch, only lines within kNearHz of the pitch the transmitter
// landed at are scored, and characters printed on any other pitch are counted
// apart as `elsewhere`; a decoder that labels nothing has everything scored.
//
// Everything is a pure function of the configuration and the seed. The GPU
// work is the engine's, and the engine's output is not bit exact across
// devices, so a cell can move by a character between two machines.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::bench {

struct CwEngineConfig {
    std::vector<double> wpm = {12.0, 20.0, 30.0, 40.0};
    std::vector<double> snr_db = {0.0, 5.0, 10.0, 15.0, 20.0};
    std::vector<std::int64_t> pitch_hz = {300, 500, 700, 900, 1200};

    // cw        a cw receiver at its default +/-250 Hz filter and 700 Hz pitch,
    //           tuned pitch - 700 Hz off the carrier
    // cw-wide   the same with the filter pulled out to -500..+600 Hz, which
    //           passes every pitch from 300 to 1200
    // usb, lsb  sideband receivers at their default 300..2700 Hz, tuned a
    //           pitch below or above the carrier
    std::vector<std::string> receivers = {"cw", "cw-wide", "usb", "lsb"};

    std::uint64_t trials = 4;
    std::uint64_t characters = 40;
    std::uint64_t seed = 20260923;

    // The keyer's random stretch of every element and space, a fraction of
    // its length; zero is a machine. core/dsp/synth/cw_mod.h.
    double jitter = 0.0;

    int gpu_index = -1;

    // Worker threads for the decoders, 0 for the hardware's count.
    unsigned threads = 0;
};

struct CwEngineCell {
    std::string receiver;
    std::int64_t pitch_hz = 0;
    double wpm = 0.0;
    double snr_db = 0.0;

    std::uint64_t sent = 0;
    std::uint64_t errors = 0;
    std::uint64_t elsewhere = 0;

    // The speed each scored line reported, summed, for the mean.
    double wpm_sum = 0.0;
    std::uint64_t wpm_lines = 0;

    // What the first trial sent and printed, for a reader.
    std::string first_sent;
    std::string first_got;

    [[nodiscard]] double error_rate() const {
        return sent == 0 ? 0.0 : static_cast<double>(errors) / static_cast<double>(sent);
    }
};

struct CwEngineReport {
    std::vector<CwEngineCell> cells;
    std::string device;
    double wall_seconds = 0.0;
};

[[nodiscard]] Expected<CwEngineReport> run_cw_engine(const CwEngineConfig& config);

// One Markdown table per receiver variant and speed: pitches down, SNRs
// across, the character error rate in each cell.
[[nodiscard]] std::string cw_engine_tables(const CwEngineConfig& config,
                                           const CwEngineReport& report);

// Every cell with its counts, and the first trial's text.
[[nodiscard]] std::string cw_engine_json(const CwEngineReport& report);

// The same decoder on a recording: receivers placed as offsets from the
// source's centre, and every line the "cw" decoder prints on each, with the
// stream, pitch and speed it carries, which revenant-cli's --decode does not
// show. For reading a real band, where there is no text to score against.
struct CwFileConfig {
    std::string uri;

    // "offset:mode" or "offset:mode:low:high", offset and edges in hertz,
    // mode cw, usb or lsb.
    std::vector<std::string> receivers;

    // Source seconds to run, 0 for the whole source.
    double seconds = 0.0;
    std::uint32_t channels = 16;
    int gpu_index = -1;
};

[[nodiscard]] Status run_cw_file(const CwFileConfig& config);

}  // namespace revenant::bench
