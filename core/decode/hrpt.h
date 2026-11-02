// NOAA HRPT: complex baseband to minor frames and five AVHRR images.
//
// SPECIFICATION
//
// NOAA KLM User's Guide, April 2014 revision, section 4.1 "HRPT System", from
// the NCEI podguides PDF "2.1 Section 4.0 Real Time Data Systems for Local
// Users". A US Government work. Cited below as "KLM".
//
//   - Table 4.1.2-2: 665,400 bit/s split phase, data "0" +68 degrees then -68,
//     "1" -68 then +68 (NOAA-N and N' use 67); 10-bit words MSB first; 11,090
//     words a minor frame, 6 minor frames a second, 3 to a major frame.
//   - Table 4.1.3.1-1: words 1-6 frame sync, 7-8 ID, 9-12 time code, 13-22
//     telemetry, 23-52 calibration target view, 53-102 space data, 103 sync
//     data, 104-623 TIP or AIP data, 624-750 spare, 751-10,990 earth data as
//     channels 1 to 5 interleaved for 2048 samples, 10,991-11,090 auxiliary
//     sync. Word 7: bit 1 AVHRR sync, bits 2-3 minor frame number, bits 4-7
//     spacecraft address, bit 8 resync, bit 9 PN input, bit 10 channel 3A.
//     Word 9 bits 1-9 day count; words 10 bits 4-10, 11 and 12 the
//     millisecond of day.
//   - Note 1 of that table: the frame sync is the first 60 bits of the 63-bit
//     PN generator x^6+x^5+x^2+x+1 started all ones. The generator is built
//     here from the polynomial, and tests/decode/test_hrpt.cpp checks it
//     against the six words printed in the table. The note does not say
//     Fibonacci or Galois; the Galois form shifting left is the one that
//     reproduces the printed words, which is how it was chosen.
//
// Deviations and engineering choices, the reader's and not the document's:
//
//   - The residual carrier (cos 68 degrees of the amplitude) is tracked by a
//     frequency-aided phase-locked loop; the Manchester data averages out of
//     its error over every bit.
//   - A matched filter over one split-phase bit turns each bit into a pulse
//     peaking mid-bit, and Gardner timing (core/dsp/gardner.h) runs on that
//     at the bit rate. Gardner on the half-bit chips was tried first and
//     settled on the chip edges in noise.
//   - Sync is accepted at 6 or fewer of 60 bits wrong, either polarity, and
//     held across frames while the next frame's sync stays within 15.
//
// WHERE IT STOPS
//
// At the 10-bit words. TIP, AIP (AMSU) and the auxiliary sync PN are carried
// in the frame event undecoded; the 1023-bit PN of notes 4 and 6 is not
// checked.
//
// CLEAN ROOM
//
// No HRPT decoder was read: not SatDump, LeanHRPT or any other.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "core/decode/image_sink.h"
#include "core/dsp/gardner.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// Table 4.1.2-2.
inline constexpr double kHrptBitRate = 665400.0;
inline constexpr std::size_t kHrptWordBits = 10;
inline constexpr std::size_t kHrptFrameWords = 11090;
inline constexpr double kHrptFrameRate = 6.0;
inline constexpr double kHrptPhaseDegKlm = 68.0;

// Table 4.1.3.1-1, zero-based word offsets.
inline constexpr std::size_t kHrptSyncWords = 6;
inline constexpr std::size_t kHrptIdWord = 6;
inline constexpr std::size_t kHrptTimeWord = 8;
inline constexpr std::size_t kHrptTipWord = 103;
inline constexpr std::size_t kHrptEarthWord = 750;
inline constexpr std::size_t kHrptEarthSamples = 2048;
inline constexpr std::size_t kHrptChannels = 5;
inline constexpr std::size_t kHrptAuxSyncWord = 10990;

// The 60-bit frame sync, first bit in bit 59.
[[nodiscard]] std::uint64_t hrpt_frame_sync();

struct HrptFrame {
    std::uint64_t index = 0;            // frames decoded since create
    double sample = 0.0;                // input sample of the sync's first chip
    std::uint32_t sync_errors = 0;      // of 60
    bool inverted = false;
    std::uint8_t minor_frame = 0;       // 1..3, word 7 bits 2-3; 0 is GAC
    std::uint8_t spacecraft = 0;        // word 7 bits 4-7
    bool channel_3a = false;            // word 7 bit 10
    std::uint16_t day = 0;              // word 9 bits 1-9
    std::uint32_t msec = 0;             // words 10-12
    std::array<std::uint16_t, kHrptFrameWords> words{};
};

struct HrptConfig {
    // At least two samples a chip, 2 x 665.4 kHz.
    double sample_rate = 2661600.0;
    // Lost frames, held by the flywheel, before the images are closed.
    std::size_t max_missed_frames = 6;
};

struct HrptEvents {
    // Five Gray8 images, AVHRR channels 1 to 5, 2048 wide, 8 MSBs.
    ImageSink image;
    std::function<void(const HrptFrame&)> on_frame;
};

class HrptDecoder {
public:
    [[nodiscard]] static Expected<HrptDecoder> create(const HrptConfig& config,
                                                     HrptEvents events);

    void push(std::span<const dsp::Complex32> samples);
    void finish();

    // Carrier frequency the loop is tracking, Hz.
    [[nodiscard]] double carrier_hz() const;

private:
    HrptDecoder() = default;

    void on_bit(std::uint8_t bit, double position);
    void emit_frame();
    void close_images(bool complete);

    HrptConfig config_{};
    HrptEvents events_{};

    // Carrier loop.
    double theta_ = 0.0;
    double freq_ = 0.0;
    double kp_ = 0.0, ki_ = 0.0, kf_ = 0.0;
    double lock_ = 0.0;
    dsp::Complex32 prev_{1.0F, 0.0F};

    // Bit matched filter and timing.
    std::vector<float> chip_hist_;
    std::size_t chip_pos_ = 0;
    std::vector<dsp::GardnerTiming> timing_;  // one; vector for default ctor
    std::vector<dsp::Complex32> scratch_;


    // Framing.
    std::uint64_t shift_ = 0;
    bool in_frame_ = false;
    bool inverted_ = false;
    std::size_t frame_bits_ = 0;
    std::uint32_t sync_errors_ = 0;
    double frame_sample_ = 0.0;
    std::array<std::uint16_t, kHrptFrameWords> words_{};
    std::uint64_t frames_ = 0;
    std::size_t missed_ = 0;

    // Images.
    bool images_open_ = false;
    std::uint32_t next_image_id_ = 1;
    std::array<std::uint32_t, kHrptChannels> image_ids_{};
    std::uint32_t rows_ = 0;
};

}  // namespace revenant::decode
