// Analog slow-scan television: receiver audio to images.
//
// SPECIFICATION
//
// JL Barber (N7CXI), "Proposal for SSTV Mode Specifications", presented at
// the Dayton SSTV forum, 20 May 2000, read from the copy DARC hosts at
// https://www.darc.de/fileadmin/filemounts/distrikte/q/ortsverbaende/01/images/SSTV/Dayton_Paper.pdf
// on 2026-11-02. The paper offers its specifications "for use or publication
// by anyone who finds them useful". Sections used:
//
//   "VIS Code and Robot calibration header": the 300 ms 1900 Hz leader, 10 ms
//   1200 Hz break, second leader, 30 ms 1200 Hz start bit, seven data bits
//   LSB first at 1100 Hz for 1 and 1300 Hz for 0, even parity, 1200 Hz stop
//   bit, and "all mode specifications begin immediately after the VIS stop
//   bit".
//   "SCOTTIE MODES": S1 60, S2 56, DX 76; the 9.0 ms starting sync on the
//   first line only; 1.5 ms separators; the sync between blue and red; scan
//   times 138.240, 88.064 and 345.6 ms; 320x256, green, blue, red.
//   "MARTIN MODES": M1 44, M2 40; 4.862 ms sync, 0.572 ms porch and
//   separators; scan times 146.432 and 73.216 ms; 320x256.
//   "ROBOT 36 COLOR" and "ROBOT 72 COLOR": VIS 8 and 12, 240 lines, 9.0 ms
//   sync, 3.0 ms 1500 Hz porch, Y scans of 88 and 138 ms, 4.5 ms separators
//   at 1500 Hz (even) and 2300 Hz (odd), 1.5 ms porches, chroma scans of 44
//   and 69 ms.
//   "WRASSE SC2-180": VIS 55, 5.5225 ms sync, 0.500 ms porch, red, green and
//   blue at 235.000 ms each, 256 lines.
//   "PASOKON P modes": P3 113, P5 114, P7 115; 640x496; scans of 133.333,
//   200.000 and 266.666 ms; syncs of 5.208, 7.813 and 10.417 ms; porches of
//   1.042, 1.563 and 2.083 ms, one before every colour and one after blue.
//   "PD Modes": VIS 93, 99, 95, 98, 96, 97 and 94 for PD50, 90, 120, 160,
//   180, 240 and 290; their resolutions and scan times; 20.000 ms sync and
//   2.080 ms porch; Y odd, R-Y and B-Y averaged over two lines, Y even.
//   Appendix A: 1500 + 3.1372549 * value.
//   Appendix B: the Y, R-Y, B-Y to RGB matrix, used as printed.
//
// The paper's layout puts each submode's first value on the line above its
// name; the values above were matched to submodes by the stated total
// transmission times, which every one reproduces.
//
// DEVIATIONS
//
// The paper gives no horizontal resolution for Robot 36 and 72. This decoder
// samples their Y scans at 320, the width of every other 240 and 256-line
// mode in the paper, and that is a receiver's choice, not part of the signal.
//
// HOW IT RECEIVES
//
// The audio is mixed down by 1900 Hz, low-passed to 1200 Hz either side and
// decimated to about 16 kS/s, and the instantaneous frequency of what is left
// is the whole of the signal. The VIS leader's measured frequency fixes the
// tuning error for the image that follows. Each line's sync is found by
// contrast against what surrounds it, a straight line is fitted through the
// syncs found so far, and pixels are read at the fitted positions: that fit
// is the slant correction, and it absorbs a sample clock that differs from
// the transmitter's.
//
// WHERE THIS STOPS
//
// FAX480 is the paper's "FAX480 mode" section: the 5 s header of 2.05 ms
// 2300 Hz and 1500 Hz tones, twenty phasing lines of 5.12 ms sync and white,
// then 480 lines of 5.12 ms sync and a 262.144 ms scan of 512 pixels. The
// header is recognised by its mean of 1900 Hz and by the frequency half a
// tone cycle apart being on opposite sides of it for a second at a time.
//
// SECOND SPECIFICATION
//
// Martin Bruchanov (OK2MNM), "Image Communication on Short Waves", the English
// edition published at https://www.sstv-handbook.com/download/sstv-handbook.pdf
// (2012 text, 2019 PDF build), read on 2026-11-02, free to read. Sections used:
//
//   4.2.5 "Amiga Video Transceiver" and table 4.6: no line sync; R, G, B
//   scans of 62.5, 125.0, 156.25 and 156.25 ms for AVT 24, 90, 94 and 188,
//   one 312.5 ms scan for AVT 125 BW; the narrow variant at 1700 Hz black and
//   2100 Hz white; the QRM variant sending odd lines first and even lines
//   second; the VIS sent three times; the digital header of 32 frames, each a
//   1900 Hz pulse then eight bits and the same eight inverted, 1600 Hz for 0
//   and 2200 Hz for 1 (1700 and 2100 narrow), at 2048/20 = 102.4 Bd; the
//   first three bits naming the mode (010, 011, 101) and the last five
//   counting 0 to 31, the image following the frame that reaches 31.
//   Figure 4.7 shows the pulse one bit long and the bits sent in the order
//   the text writes them, most significant first.
//   Chapter 5, note a: the four VIS codes per AVT mode are normal, narrow,
//   QRM, narrow and QRM. Note b: AVT 188 and Scottie DX share 76.
//   4.4.1 "MSCAN TV": TV-1 and TV-2 at the line timing of Wraase SC-180 and
//   Martin M1, the image in four quarter-frames sent top to bottom; chapter
//   5 gives VIS 104 and 105.
//   4.4.3 "Modes MP, MR, ML" and table 4.10: the 16-bit VIS, low byte 0x23,
//   each byte seven bits and an odd parity bit; MP as PD with a 9.0 ms sync
//   and 1.0 ms 1500 Hz gap; MR and ML as 4:2:2 with 0.1 ms gaps after Y,
//   R-Y and B-Y at the frequency of the last pixel before them.
//
// DEVIATIONS FROM THE HANDBOOK, AND WHERE IT DISAGREES WITH ITSELF
//
// AVT 24 is 128x120 here, as table 4.6 has it; chapter 5 lists 128x128.
// The 640x496 MMSSTV modes are ML180 to ML320 as table 4.10 names them;
// chapter 5 calls them MR180 to MR320 under the same VIS codes.
// MP73 is in chapter 5 (0x2523, 73 s, 210.526 lpm) but not in table 4.10;
// its 140.0 ms scans are those table 4.10 gives MP73-N, and they reproduce
// the 210.526 lpm.
// The order of rows inside an MSCAN quarter-frame is not stated. Rows q,
// q + 4, q + 8 and so on make quarter q here, the reading under which the
// handbook's "four bars" appear on a receiver that ignores the interlace.
// TV-2 uses Martin M1's period although chapter 5 lists 134.530 lpm
// against M1's 134.395; the section's text says they are the same.
//
// Not here: the narrowband MMSSTV modes (MP-N, MC-N), whose N-VIS bit order
// the handbook leaves to a figure reference it does not resolve; Proskan
// J120, for which the handbook gives a VIS code and a total time but no line
// structure. VIS 76 opens AVT 188 when an AVT digital header follows it and
// Scottie DX otherwise.
//
// AVT has no line sync to fit, so its slant correction comes from the
// header instead: a straight line through the frames found, against their
// count, gives both where the image starts and the sample clock ratio.
//
// CLEAN ROOM
//
// No SSTV implementation was read: not MMSSTV, QSSTV, slowrx or any other.
// The handbook's tables and figures were read; no code that accompanies it.

#pragma once

#include <complex>
#include <map>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/image_sink.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// Appendix A.
inline constexpr double kSstvBlackHz = 1500.0;
inline constexpr double kSstvWhiteHz = 2300.0;
inline constexpr double kSstvHzPerLevel = 3.1372549;
inline constexpr double kSstvSyncHz = 1200.0;
inline constexpr double kSstvLeaderHz = 1900.0;
// Handbook 4.2.5: the AVT digital header.
inline constexpr double kAvtBitMs = 1000.0 / 102.4;
inline constexpr int kAvtFrameBits = 17;   // the 1900 Hz pulse and sixteen bits
inline constexpr int kAvtFrames = 32;
inline constexpr double kAvtFrameMs = kAvtFrameBits * kAvtBitMs;
// Handbook 4.4.3: the low byte of every 16-bit VIS code.
inline constexpr int kVis16Low = 0x23;
// "VIS Code and Robot calibration header".
inline constexpr double kVisOneHz = 1100.0;
inline constexpr double kVisZeroHz = 1300.0;
inline constexpr double kVisBitMs = 30.0;
inline constexpr double kVisLeaderMs = 300.0;
inline constexpr double kVisBreakMs = 10.0;

enum class SstvColour : std::uint8_t {
    Rgb,    // channels 0, 1, 2 are R, G, B
    Ycc,    // channels 0, 1, 2 are Y, R-Y, B-Y; chroma is shared by every line of a period
    Gray,   // channel 0
};

// One piece of a period. A tone holds `hz`; a scan reads `channel` for line
// `line` of the period and has no frequency of its own.
struct SstvSegment {
    double ms = 0.0;
    double hz = 0.0;
    int channel = -1;
    int line = 0;

    [[nodiscard]] bool scan() const { return channel >= 0; }
};

// A mode is a period repeated: one line for most modes, two for Robot 36 and
// the PD modes. `lead` is what comes once before the first period, Scottie's
// starting sync. `sync_offset_ms` is where in the period the sync to track
// begins.
struct SstvMode {
    std::string_view name;
    int vis = -1;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int lines_per_period = 1;
    SstvColour colour = SstvColour::Rgb;
    std::vector<SstvSegment> lead;
    std::vector<SstvSegment> period;
    double sync_offset_ms = 0.0;
    double sync_ms = 0.0;

    // Periods sent before the first row of the image, FAX480's phasing.
    int skip_periods = 0;

    // Opened by the fax start tone and phasing rather than a VIS code.
    bool fax_start = false;

    // Black and white, 1500 and 2300 Hz except the AVT narrow variants.
    double black_hz = 1500.0;
    double white_hz = 2300.0;

    // Transmitted lines are sent as `interlace` fields, field f carrying rows
    // f, f + interlace, ... in turn: 2 for the AVT QRM variants, 4 for MSCAN.
    int interlace = 1;

    // AVT: no line sync; the VIS three times, then the digital header, whose
    // first three bits are `avt_family`. sync_ms is 0.
    bool avt_header = false;
    int avt_family = 0;

    // A 16-bit VIS code: `vis` holds both bytes, low byte 0x23.
    [[nodiscard]] bool vis16() const { return vis > 0x7F; }
    [[nodiscard]] std::uint32_t row_of(std::uint32_t line) const {
        if (interlace <= 1) {
            return line;
        }
        const std::uint32_t k = static_cast<std::uint32_t>(interlace);
        const std::uint32_t per = (height + k - 1) / k;
        return (line / per) + k * (line % per);
    }

    [[nodiscard]] double period_ms() const;
    [[nodiscard]] double lead_ms() const;
    [[nodiscard]] std::uint32_t periods() const {
        return static_cast<std::uint32_t>(skip_periods) +
               (height + static_cast<std::uint32_t>(lines_per_period) - 1) /
                   static_cast<std::uint32_t>(lines_per_period);
    }
    [[nodiscard]] double line_rate_hz() const {
        return static_cast<double>(lines_per_period) * 1000.0 / period_ms();
    }
};

[[nodiscard]] std::span<const SstvMode> sstv_modes();
[[nodiscard]] const SstvMode* sstv_mode_by_vis(int vis);
[[nodiscard]] const SstvMode* sstv_mode_by_name(std::string_view name);

// Appendix B, both directions, values 0 to 255.
struct SstvRgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};
struct SstvYcc {
    double y = 0.0;
    double ry = 0.0;
    double by = 0.0;
};
[[nodiscard]] SstvYcc sstv_rgb_to_ycc(double r, double g, double b);
[[nodiscard]] SstvRgb sstv_ycc_to_rgb(double y, double ry, double by);

struct SstvConfig {
    dsp::SampleRate rate = 48'000;
};

struct SstvStats {
    std::uint64_t vis_codes = 0;          // VIS codes that checked, known or not
    std::uint64_t unknown_vis = 0;        // of those, codes no mode here carries
    std::uint64_t images = 0;             // images opened
    std::uint64_t images_complete = 0;
    std::uint64_t syncs_found = 0;
    std::uint64_t syncs_missed = 0;
    int last_vis = -1;
    double last_offset_hz = 0.0;          // tuning error measured on the last leader
    double last_period_ratio = 1.0;       // fitted line period over nominal
};

class SstvDecoder {
public:
    [[nodiscard]] static Expected<SstvDecoder> create(const SstvConfig& config);

    // Every row the samples complete goes to `sink`, with open and close
    // around each image.
    void process(std::span<const float> samples, const ImageSink& sink);

    // The stream has ended: rows still recoverable go out, and an image still
    // open closes incomplete.
    void flush(const ImageSink& sink);

    // Forget the transmitter. An image still open closes incomplete.
    void reset(const ImageSink& sink);

    [[nodiscard]] const SstvStats& stats() const { return stats_; }
    [[nodiscard]] bool image_open() const { return image_.has_value(); }

private:
    struct Image {
        const SstvMode* mode = nullptr;
        double start = 0.0;          // working-rate position of period 0
        double nominal = 0.0;        // nominal period, working samples
        double offset_hz = 0.0;
        std::uint32_t next_search = 0;
        std::uint32_t next_emit = 0;
        std::uint32_t misses_in_row = 0;
        std::vector<std::pair<double, double>> syncs;   // (period, start of period)
        double fit_start = 0.0;
        double fit_period = 0.0;
    };

    explicit SstvDecoder(const SstvConfig& config);

    void push(float x);
    void run(const ImageSink& sink, bool at_end);
    bool hunt(const ImageSink& sink);
    bool step_image(const ImageSink& sink, bool at_end);
    void emit_period(std::uint32_t period, const ImageSink& sink);
    void open_on(const ImageSink& sink, const SstvMode& mode);
    void row_on(const ImageSink& sink, std::uint32_t row, std::span<const std::uint8_t> bytes);
    void release(const ImageSink& sink, bool all);
    void refit();
    void finish(const ImageSink& sink, bool complete);
    void trim(double keep_from);

    [[nodiscard]] double end() const { return static_cast<double>(base_ + freq_.size()); }
    [[nodiscard]] double cumulative(double at) const;
    [[nodiscard]] double mean_deviation(double from, double to) const;
    bool fax_header(double at);
    // The AVT header after a VIS ending at `after`: true with the image start
    // and period ratio when frames of `family` were found.
    bool find_avt_header(double after, double offset_hz, const SstvMode& mode, double& start,
                    double& ratio) const;
    [[nodiscard]] double mean(double from, double to) const;
    [[nodiscard]] double ms(double milliseconds) const { return milliseconds * work_rate_ / 1000.0; }
    [[nodiscard]] double fit(double period) const;

    SstvConfig config_;
    std::uint32_t decimation_ = 1;
    double work_rate_ = 0.0;

    // Front end.
    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;
    std::size_t history_at_ = 0;
    std::uint64_t input_count_ = 0;
    std::complex<double> phasor_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    std::complex<float> previous_{0.0F, 0.0F};

    // Instantaneous frequency at the working rate, from index base_, and its
    // running sum from the same index.
    std::uint64_t base_ = 0;
    std::vector<float> freq_;
    std::vector<double> sum_;     // sum_[i] = sum of freq_[0..i)
    std::vector<double> dev_sum_; // the same for (f[n] - 1900) * (f[n - 2.05 ms] - 1900)

    double hunt_at_ = 0.0;
    std::optional<Image> image_;
    std::vector<std::vector<std::vector<std::uint8_t>>> scans_;   // [channel][line][pixel]
    std::vector<std::uint8_t> row_;
    std::uint32_t image_id_ = 0;
    std::uint32_t rows_sent_ = 0;
    std::uint32_t next_row_ = 0;
    std::map<std::uint32_t, std::vector<std::uint8_t>> held_;   // rows waiting on one above
    SstvStats stats_;
};

}  // namespace revenant::decode
