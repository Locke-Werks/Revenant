// AAC encoding for the HLS "keep listening" path, through the Media Foundation
// AAC encoder MFT.
//
// The Media Foundation headers are kept out of this header on purpose: they
// pull in <windows.h>, whose DELETE macro breaks kj::HttpMethod in anything
// that includes http.h afterwards. Everything MF lives behind the pimpl in
// audio_hls.cpp.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::webgate {

// Mono AAC-LC at 64 kbit/s, raw access units (MF_MT_AAC_PAYLOAD_TYPE 0), which
// is what an mp4 sample wants: no ADTS header to strip.
//
// Not thread safe. Use it from one thread; it initialises COM and Media
// Foundation on the calling thread.
class AacEncoder {
public:
    // The MFT offers 44100 and 48000 Hz; any other rate is an error rather
    // than a silent resample, because a wrong rate here plays at the wrong
    // pitch.
    [[nodiscard]] static Expected<std::unique_ptr<AacEncoder>> create(std::uint32_t sample_rate);

    ~AacEncoder();
    AacEncoder(const AacEncoder&) = delete;
    AacEncoder& operator=(const AacEncoder&) = delete;

    // Any chunk size. Samples are mono floats in [-1, 1]; out-of-range values
    // are clipped. Frames are cut as enough samples arrive.
    [[nodiscard]] Status push(std::span<const float> samples);

    // Access units finished since the last call. Each covers 1024 samples.
    [[nodiscard]] std::vector<std::vector<std::uint8_t>> take_access_units();

    // The two-byte AudioSpecificConfig the esds box carries.
    [[nodiscard]] std::span<const std::uint8_t> audio_specific_config() const;

    [[nodiscard]] std::uint32_t sample_rate() const;

private:
    struct Impl;
    explicit AacEncoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace revenant::webgate
