// NOAA APT: FM-demodulated audio to video lines, telemetry and an image.
//
// SPECIFICATION
//
// NOAA KLM User's Guide, April 2014 revision, section 4.2 "APT System", from
// the NCEI podguides PDF "2.1 Section 4.0 Real Time Data Systems for Local
// Users". A US Government work. Cited below as "KLM" with a table or figure.
//
//   - Table 4.2.2-1 and 4.2.2: a 2400 Hz subcarrier, amplitude modulated by
//     the data, maximum modulation at grey wedge 8 with an index of 87 percent;
//     120 lines per minute.
//   - Table 4.2.3-2: 2080 words a line, 4160 words a second, 909 words per
//     sensor channel, 128 lines a frame; 8 MSBs of each 10-bit word.
//   - Figure 4.2.3-2: the line is Sync A (39 words), space and minute markers
//     (47), channel A video (909), telemetry A (45), then the same four for
//     channel B with Sync B. Minute markers are 4 lines, 2 black and 2 white.
//   - Figure 4.2.3-3: Sync A is 7 cycles of 1040 Hz, two words high and two
//     low, starting at word 4; Sync B is 7 pulses at 832 pps, three words high
//     and two low, starting at word 4; D/A levels 11 low and 244 high.
//   - Figure 4.2.2-1: 16 telemetry wedges of 8 lines each. Wedges 1 to 8 are
//     the grey scale, 9 is zero modulation, 10 to 13 thermistors, 14 patch
//     temperature, 15 back scan, 16 channel identification. 4.2.3: wedge 16
//     equals one of wedges 1 to 6; 3 means channel 3A and 6 means 3B.
//
// Deviations and interpretations, each the reader's and not the document's:
//
//   - The document does not state the D/A value of each grey wedge, only that
//     8 is maximum modulation and 9 is zero. The decoder assumes nothing about
//     them beyond monotonic order: a telemetry frame is recognised by wedges 1
//     to 8 rising evenly and wedge 9 falling below wedge 1.
//   - Figure 4.2.2-1 note 2 says one telemetry frame per 84 seconds; Table
//     4.2.3-2 says one frame per 64 seconds, which is what 128 lines at 2 a
//     second gives. 64 is used.
//   - Calibration of the envelope to D/A counts uses the Sync A levels: the
//     mean over words 4 to 31 is (11 + 244) / 2 whatever the filtering, and
//     words 33 to 38 sit at 11. The amplitude is taken to be linear in the
//     word value, which the document implies and does not state.
//
// WHAT IS DECODED
//
// Envelope of the 2400 Hz subcarrier, Sync A found by correlation over a whole
// line and then tracked line to line, with Sync B scored at the half line as a
// second check. Each line is sampled at 2080 words on the measured line
// period, so a sample clock off nominal costs nothing. Every line is emitted as
// one Gray8 image row 2080 wide, sync and telemetry included; the channel
// crops are at kAptChannelAVideo and kAptChannelBVideo. Telemetry frames are
// reported with all 16 wedges per channel and the channel identification.
//
// CLEAN ROOM
//
// No APT decoder was read: not aptdec, noaa-apt, SatDump or any other. Written
// from the KLM text and figures above.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code. tests/decode/test_apt.cpp round-trips against
// core/dsp/synth/apt_mod.h in its place.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "core/decode/image_sink.h"
#include "core/error.h"

namespace revenant::decode {

// Table 4.2.3-2 and 4.2.2.
inline constexpr double kAptWordRate = 4160.0;
inline constexpr std::size_t kAptLineWords = 2080;
inline constexpr std::size_t kAptChannelWords = 909;
inline constexpr std::size_t kAptFrameLines = 128;
inline constexpr double kAptSubcarrierHz = 2400.0;

// Figure 4.2.3-2: word offsets within a line.
inline constexpr std::size_t kAptSyncWords = 39;
inline constexpr std::size_t kAptSpaceWords = 47;
inline constexpr std::size_t kAptTelemetryWords = 45;
inline constexpr std::size_t kAptChannelASync = 0;
inline constexpr std::size_t kAptChannelASpace = 39;
inline constexpr std::size_t kAptChannelAVideo = 86;
inline constexpr std::size_t kAptChannelATelemetry = 995;
inline constexpr std::size_t kAptChannelBSync = 1040;
inline constexpr std::size_t kAptChannelBSpace = 1079;
inline constexpr std::size_t kAptChannelBVideo = 1126;
inline constexpr std::size_t kAptChannelBTelemetry = 2035;

// Figure 4.2.3-3.
inline constexpr std::uint8_t kAptSyncLow = 11;
inline constexpr std::uint8_t kAptSyncHigh = 244;

// The 39 words of each sync, Figure 4.2.3-3.
[[nodiscard]] std::array<std::uint8_t, kAptSyncWords> apt_sync_a();
[[nodiscard]] std::array<std::uint8_t, kAptSyncWords> apt_sync_b();

// The AVHRR channel a wedge 16 value names, 4.2.3: index 1..6 of the grey
// wedge it equals. Returns "" outside 1..6.
[[nodiscard]] std::string apt_channel_name(int grey_wedge);

struct AptConfig {
    double sample_rate = 11025.0;
    // Lines of lost sync, free-running on the last period, before the image
    // is closed and the search starts again.
    std::size_t max_missed_lines = 16;
};

struct AptLine {
    std::uint64_t line = 0;          // counted from the image's first line
    std::uint32_t image_id = 0;
    double start_sample = 0.0;       // input sample of word 0, Sync A
    double period_samples = 0.0;     // measured line length
    float sync_a = 0.0F;             // normalised correlation, -1..1
    float sync_b = 0.0F;
    bool free_running = false;       // sync not found, placed by the period
    std::array<std::uint8_t, kAptLineWords> words{};
};

struct AptTelemetry {
    std::uint32_t image_id = 0;
    std::uint64_t first_line = 0;    // image line of wedge 1's first line
    // Wedges 1..16 at indices 0..15, D/A counts, per channel A and B.
    std::array<std::array<float, 16>, 2> wedges{};
    // AVHRR channel names from wedge 16, "1".."5", "3A", "3B", or "".
    std::array<std::string, 2> channel{};
};

struct AptEvents {
    ImageSink image;
    std::function<void(const AptLine&)> on_line;
    std::function<void(const AptTelemetry&)> on_telemetry;
};

class AptDecoder {
public:
    [[nodiscard]] static Expected<AptDecoder> create(const AptConfig& config, AptEvents events);

    // Any blocking of the same audio gives the same events.
    void push(std::span<const float> audio);

    // Closes an open image. Audio still buffered short of a whole line is
    // dropped.
    void finish();

private:
    AptDecoder() = default;

    void process();
    bool try_line();
    void emit_line(double start, float a, float b, bool free_running);
    void check_telemetry();
    [[nodiscard]] float correlate(const std::vector<float>& tmpl, double at) const;
    [[nodiscard]] float envelope_at(double sample) const;
    // complete: ended by finish(), not by lost sync.
    void close_image(bool complete);

    AptConfig config_{};
    AptEvents events_{};

    // Front end: mixer, low-pass, envelope.
    std::vector<float> taps_;
    std::vector<float> hist_i_, hist_q_;
    std::size_t hist_pos_ = 0;
    double phase_ = 0.0;
    double phase_step_ = 0.0;
    std::uint64_t consumed_ = 0;  // input samples taken

    // Envelope buffer: env_[k] is input sample env_base_ + k, delay removed.
    std::vector<float> env_;
    double env_base_ = 0.0;

    std::vector<float> sync_a_tmpl_, sync_b_tmpl_;
    double nominal_period_ = 0.0;

    // Sync state.
    bool locked_ = false;
    double search_from_ = 0.0;
    double last_start_ = 0.0;
    double period_ = 0.0;
    std::size_t missed_ = 0;

    // Calibration, envelope to D/A counts.
    bool calibrated_ = false;
    double cal_low_ = 0.0;
    double cal_mid_ = 0.0;

    // Image.
    std::uint32_t next_image_id_ = 1;
    std::uint32_t image_id_ = 0;
    std::uint64_t rows_ = 0;
    std::vector<float> telemetry_;  // per image line, mean of A and B
    std::vector<std::array<float, 2>> telemetry_ab_;
    std::uint64_t last_frame_line_ = 0;
    bool any_frame_ = false;
};

}  // namespace revenant::decode
