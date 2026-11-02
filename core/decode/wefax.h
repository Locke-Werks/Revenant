// HF radiofacsimile (WEFAX): receiver audio to charts.
//
// SPECIFICATION
//
// WMO-No. 386, "Manual on the Global Telecommunication System", Volume I,
// 2009 edition, Part III "Technical characteristics and specifications of
// the GTS", section 5, pages III-1 to III-4, read from the copy the US National
// Weather Service hosts at
// https://www.weather.gov/media/marine/WMO_386_Vol_I_2009_en.pdf on
// 2026-11-02. WMO stopped updating the manual on 2024-12-31. Sections used:
//
//   5.1.2  the index of cooperation, 576 or 288.
//   5.1.3.3  the dead sector, 4.5 % of the line, mostly white.
//   5.1.5  60, 90, 120 and 240 lines per minute.
//   5.2.1  the receiver starts on the IOC selection signal or on phasing.
//   5.2.2  IOC selection: alternating black and white for 5 to 10 s, at
//          300 Hz for IOC 576 and 675 Hz for IOC 288.
//   5.2.3  phasing: 30 s of alternating white and black at the line rate,
//          symmetrical or 5 % white and 95 % black, the leading edge of the
//          white at the start of the dead sector.
//   5.2.5  stop: 5 s of alternating black and white at 450 Hz, then 10 s of
//          black.
//   5.5.1  FM subcarrier: 1900 Hz centre, 1500 Hz black, 2300 Hz white.
//
// US National Weather Service, "Worldwide Marine Radiofacsimile Broadcast
// Schedules", 7 March 2025, https://www.weather.gov/media/marine/rfax.pdf,
// read 2026-11-02: the stations' RPM/IOC pairs (120/576 almost throughout),
// USB reception 1.9 kHz below the assigned frequency, and one station
// (DWD Hamburg) running white +425 Hz and black -425 Hz.
//
// Martin Bruchanov, "Image Communication on Short Waves" (see sstv.h),
// section 11.2.2: a line of IOC x pi pixels, 1810 at IOC 576.
//
// DEVIATIONS
//
// Polarity is a setting. WMO puts white at 2300 Hz; the handbook's section
// 11.2 reports CCITT T.2 and North American practice the other way up, and
// docs/modes.md records that a hardcoded choice turns half the world's charts
// into negatives.
//
// Pixels per line are IOC x pi rounded, the line read from the white leading
// edge of phasing, so the dead sector is the first 4.5 % of every row. A
// display that wants it gone crops it; the decoder does not guess where a
// station's dead sector really is.
//
// HOW IT RECEIVES
//
// The audio is mixed down by 1900 Hz, low-passed and decimated to about
// 12 kS/s, and the instantaneous frequency of what is left is the signal.
// Fixed 0.2 s blocks, counted from the start of the stream so the result does
// not depend on how the audio arrives, are scored for the 300, 675 and 450 Hz
// alternations by the share of their variance in that one Goertzel bin. Two
// seconds of 300 or 675 Hz is a start; its mean frequency is the tuning
// error. Once it ends, twelve seconds of phasing name the line rate (the
// shortest period that folds the phasing into a square) and a straight line
// through the white leading edges of every phasing line gives the line start
// and the sample clock ratio. Rows are read on that line from the first line
// that is not phasing, until the 450 Hz stop or a new start.
//
// CLEAN ROOM
//
// No fax decoder was read: not fldigi, not any other.

#pragma once

#include <complex>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/decode/image_sink.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// WMO-No. 386 III 5.5.1.
inline constexpr double kWefaxCentreHz = 1900.0;
inline constexpr double kWefaxDeviationHz = 400.0;
// 5.2.2 and 5.2.5.
inline constexpr double kWefaxStart576Hz = 300.0;
inline constexpr double kWefaxStart288Hz = 675.0;
inline constexpr double kWefaxStopHz = 450.0;
// 5.1.3.3.
inline constexpr double kWefaxDeadSector = 0.045;

// Handbook 11.2.2: IOC x pi pixels.
[[nodiscard]] std::uint32_t wefax_width(int ioc);

struct WefaxConfig {
    dsp::SampleRate rate = 48'000;
    // White at 1900 + 400 Hz when true, as WMO-No. 386 has it.
    bool white_high = true;
    // 0 to take the rate from phasing; otherwise 60, 90, 120 or 240.
    int lpm = 0;
};

struct WefaxStats {
    std::uint64_t starts = 0;            // start tones heard
    std::uint64_t phasing_failed = 0;    // starts with no phasing after them
    std::uint64_t images = 0;
    std::uint64_t images_complete = 0;   // closed by the stop signal
    std::uint64_t phasing_lines = 0;
    int last_ioc = 0;
    int last_lpm = 0;
    double last_offset_hz = 0.0;
    double last_period_ratio = 1.0;      // measured line period over nominal
};

class WefaxDecoder {
public:
    [[nodiscard]] static Expected<WefaxDecoder> create(const WefaxConfig& config);

    void process(std::span<const float> samples, const ImageSink& sink);
    // The stream has ended: an image still open closes incomplete.
    void flush(const ImageSink& sink);
    void reset(const ImageSink& sink);

    [[nodiscard]] const WefaxStats& stats() const { return stats_; }
    [[nodiscard]] bool image_open() const { return open_; }

private:
    enum class State { Idle, Phasing, Image };

    explicit WefaxDecoder(const WefaxConfig& config);

    void push(float x);
    void run(const ImageSink& sink);
    bool idle_step();
    bool phasing_step();
    bool image_step(const ImageSink& sink);
    void close(const ImageSink& sink, bool complete);
    void trim(double keep_from);

    [[nodiscard]] double end() const { return static_cast<double>(base_ + freq_.size()); }
    [[nodiscard]] double cumulative(double at) const;
    [[nodiscard]] double mean(double from, double to) const;
    [[nodiscard]] double tone_share(double from, double to, double hz) const;
    [[nodiscard]] double white_share(double from, double to) const;
    [[nodiscard]] double edge(double around, double reach) const;
    [[nodiscard]] double line_at(double k) const { return origin_ + k * period_; }
    [[nodiscard]] double seconds(double s) const { return s * work_rate_; }
    void refit();

    WefaxConfig config_;
    std::uint32_t decimation_ = 1;
    double work_rate_ = 0.0;

    std::vector<float> taps_;
    std::vector<std::complex<float>> history_;
    std::size_t history_at_ = 0;
    std::uint64_t input_count_ = 0;
    std::complex<double> phasor_{1.0, 0.0};
    std::complex<double> step_{1.0, 0.0};
    std::complex<float> previous_{0.0F, 0.0F};

    std::uint64_t base_ = 0;
    std::vector<float> freq_;
    std::vector<double> sum_;

    State state_ = State::Idle;
    std::uint64_t block_ = 0;          // next idle block, counted from the stream start
    int tone_blocks_ = 0;
    int tone_ioc_ = 0;
    double tone_sum_ = 0.0;
    double offset_hz_ = 0.0;
    double phasing_from_ = 0.0;
    int lpm_ = 0;
    int ioc_ = 0;
    std::uint32_t width_ = 0;
    double nominal_ = 0.0;             // line period, working samples
    double origin_ = 0.0;
    double period_ = 0.0;
    std::vector<std::pair<double, double>> edges_;   // (line, white leading edge)
    std::vector<double> template_;     // white share per bin of a phasing line
    double next_line_ = 0.0;
    double first_row_line_ = 0.0;
    int stop_lines_ = 0;
    bool open_ = false;
    std::uint32_t image_id_ = 0;
    std::uint32_t rows_ = 0;
    std::vector<std::uint8_t> row_;
    WefaxStats stats_;
};

}  // namespace revenant::decode
