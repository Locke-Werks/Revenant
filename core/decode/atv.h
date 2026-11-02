// Analogue fast-scan television: demodulated composite video to luminance
// images, 525-line system M (NTSC) and 625-line systems B and G (PAL).
//
// SPECIFICATION
//
// Recommendation ITU-R BT.470-6 (1998), "Conventional television systems",
// free from itu.int, cited below as "BT.470" with a table and item. It is
// archived, and ITU-R BT.1700 superseded it for the composite signal. BT.1700
// was not obtainable when this was written (the ITU download returned not
// found), so it was not read and every number here is BT.470's.
//
// Where BT.470 gives a range rather than a value, the midpoint is taken and
// the comment at that constant says so.
//
//   - line period H, sync pulse d, the datum OH and the back edge of line
//     blanking b, and the front porch c: table 1-1;
//   - levels: blanking 0, peak white 100, sync -40 (M) or -43 (B, G), and the
//     7.5 black setup of system M: table 1, item 4;
//   - field structure: equalising pulses p, field-sync pulses q, sequences of
//     l, m and n, the field-blanking interval j: table 1-2 and figure 2;
//     which field is which comes from where the field-sync edge OV falls
//     against the line-sync edges, notes 2 and 3 to figure 2-1 and the
//     corresponding notes to figure 2-2;
//   - radiated levels for negative AM (C3F neg.): sync 100 %, blanking 75 %,
//     peak white 10 to 15 %: table 3, items 7 and 8.
//
// WHAT IS DECODED
//
// Luminance only. Sync is separated by slicing a smoothed copy of the signal
// midway between the measured sync tip and blanking level; pulses are told
// apart by width (line sync, equalising, field sync); each picture line is
// sampled from its own measured line-sync edge, so a timebase error or a
// drifting sample clock moves nothing; the black and white levels are
// derived from the measured sync amplitude through the table 1 ratios, so
// the decoder is indifferent to gain and offset.
//
// Two front ends turn complex baseband into that composite: an FM
// discriminator (amateur FM ATV) and an envelope detector for the negative
// AM of broadcast systems. The envelope of a vestigial sideband signal is
// not exactly the envelope of double sideband; the quadrature component the
// vestige leaves is not corrected, which shows as a slight smear on the
// sharpest edges.
//
// WHERE IT STOPS
//
// Colour. The NTSC and PAL chroma subcarriers pass through as fine dot
// patterns on saturated areas; the decoder neither notches them nor decodes
// them. SECAM, the 819 and 405-line systems, and teletext are not handled.
// Sound is a separate FM carrier and is not this file's concern.
//
// IMAGE OUTPUT
//
// Frames are reported through ImageRow: an open event, one row per picture
// line as soon as it is decoded, and a close event. The first field fills the
// even rows and the second the odd ones. A frame whose second field never
// arrives is closed with the rows it has.
//
// ONE STREAM, ANY BLOCKING
//
// Every stage is a streaming filter or a state machine over absolute sample
// indices, so the rows produced do not depend on how the input was split.
//
// CLEAN ROOM
//
// No television decoder's source was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

#include "core/decode/image_sink.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

enum class AtvStandard : std::uint8_t {
    M525,   // system M, 525 lines, NTSC timing
    BG625,  // systems B and G, 625 lines, PAL timing
};

// The timing a standard fixes, in microseconds where a duration.
struct AtvTiming {
    double line_us = 0.0;          // H
    double sync_us = 0.0;          // d
    double equalising_us = 0.0;    // p
    double field_sync_us = 0.0;    // q
    double active_start_us = 0.0;  // b, from OH
    double front_porch_us = 0.0;   // c
    double pre_equalising_lines = 0.0;  // l, in H
    double field_sync_lines = 0.0;      // m
    double post_equalising_lines = 0.0; // n
    double field_blanking_lines = 0.0;  // j, without the trailing a
    double lines_per_field = 0.0;
    double sync_level = 0.0;       // with blanking 0 and peak white 100
    double black_level = 0.0;
    std::uint32_t rows_per_field = 0;
};

[[nodiscard]] AtvTiming atv_timing(AtvStandard standard);

// Where the first picture line of a field starts, in line periods after the
// field-sync edge OV. This file's choice, stated once so the transmitter and
// the receiver agree: the first line-grid position at or after j - l line
// periods from OV, the grid being OV itself in the first field and OV + H/2
// in the second.
[[nodiscard]] double atv_first_line_offset(AtvStandard standard, bool second_field);

struct AtvConfig {
    AtvStandard standard = AtvStandard::BG625;
    dsp::SampleRate sample_rate = 0;
    std::uint32_t width = 720;
    // True when sync tips are the most positive excursion of the input.
    bool inverted = false;
};

struct AtvStats {
    std::uint64_t line_syncs = 0;
    std::uint64_t equalising = 0;
    std::uint64_t field_syncs = 0;
    std::uint64_t fields = 0;
    std::uint64_t frames = 0;
    double sync_level = 0.0;
    double blanking_level = 0.0;
    double line_period_us = 0.0;  // measured
};

struct AtvCallbacks {
    std::function<void(const ImageOpen&)> on_open;
    std::function<void(const ImageRow&)> on_row;
    std::function<void(const ImageClose&)> on_close;
};

class AtvReceiver {
public:
    [[nodiscard]] static Expected<AtvReceiver> create(AtvConfig config, AtvCallbacks callbacks);

    // Demodulated composite video, any gain and offset.
    void push(dsp::ConstRealSpan video);

    [[nodiscard]] const AtvStats& stats() const { return stats_; }

private:
    struct Field {
        double ov = 0.0;      // field-sync edge, in samples
        bool second = false;
        std::uint32_t next_row = 0;
        double last_oh = 0.0; // line-sync edge of the last row rendered
    };

    AtvReceiver(AtvConfig config, AtvCallbacks callbacks);
    void on_sample(float raw);
    void on_pulse(double start, double end);
    void render_ready();
    [[nodiscard]] double edge_near(double nominal) const;
    [[nodiscard]] float at(double position) const;
    void close_image();

    AtvConfig config_;
    AtvCallbacks callbacks_;
    AtvTiming timing_;
    AtvStats stats_;
    double samples_per_us_ = 0.0;
    double line_samples_ = 0.0;

    // Raw samples by absolute index, kept back to the earliest unrendered line.
    std::vector<float> raw_;
    std::int64_t raw_start_ = 0;
    std::int64_t total_ = 0;

    // Sync path: moving average, slicer, pulse timer.
    std::size_t smooth_length_ = 1;
    std::vector<float> smooth_ring_;
    std::size_t smooth_pos_ = 0;
    std::size_t smooth_count_ = 0;
    double smooth_sum_ = 0.0;
    double previous_smoothed_ = 0.0;
    bool levels_known_ = false;
    std::vector<float> level_probe_;
    double sync_level_ = 0.0;
    double blank_level_ = 0.0;
    bool in_pulse_ = false;
    double pulse_start_ = 0.0;

    std::deque<double> line_edges_;  // recent line-sync leading edges
    std::optional<double> last_line_edge_;
    double last_field_sync_ = -1e18;
    std::optional<Field> field_;
    double next_gate_ = 0.0;            // sample count the next row waits for
    std::array<double, 2> first_line_{};  // atv_first_line_offset by field
    std::uint32_t image_id_ = 0;
    bool image_open_ = false;
    std::uint32_t rows_written_ = 0;
};

// Front ends. Both are streaming and chunk independent.

// FM discriminator: instantaneous frequency in hertz.
class AtvFmDemodulator {
public:
    explicit AtvFmDemodulator(dsp::SampleRate rate) : rate_(rate) {}
    void push(dsp::ConstComplexSpan in, std::vector<float>& out);

private:
    dsp::SampleRate rate_;
    dsp::Complex32 previous_{1.0F, 0.0F};
};

// Envelope detector for negative AM. Output is the envelope, so sync tips are
// the maximum: use AtvConfig::inverted = true behind it.
void atv_am_envelope(dsp::ConstComplexSpan in, std::vector<float>& out);

}  // namespace revenant::decode
