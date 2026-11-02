// Baseline sequential JPEG: compressed bytes to pixels.
//
// SPECIFICATION
//
// ITU-T Recommendation T.81 (09/92), ISO/IEC 10918-1, "Information
// technology - Digital compression and coding of continuous-tone still
// images - Requirements and guidelines", read from the copy the W3C hosts at
// https://www.w3.org/Graphics/JPEG/itu-t81.pdf on 2026-11-02. Sections used:
//
//   A.1.1  component dimensions, ceil(X * Hi / Hmax) by ceil(Y * Vi / Vmax).
//   A.2.2, A.2.3  non-interleaved scans in each component's own block raster,
//          interleaved MCUs of Hi x Vi blocks per component in scan order.
//   A.3.1  the level shift; A.3.3 the IDCT, used as its equation states;
//          A.3.4 dequantization; A.3.6 the zig-zag order (figure A.6).
//   B.1.1.2 to B.2.5  markers, the frame and scan headers, DQT, DHT, DRI.
//   C.2  Huffman code generation (figures C.1 and C.2).
//   F.2.2.1 to F.2.2.5  DECODE with MINCODE, MAXCODE and VALPTR (figures F.15
//          and F.16), RECEIVE, EXTEND (figure F.12), the AC decoding loop and
//          NEXTBIT with its 0xFF00 stuffing.
//   B.2.4.4, E.2.4  a restart interval sets every DC prediction to zero.
//   F.2.1.3.1  each DC coefficient is its decoded difference plus PRED.
//
// ITU-T Recommendation T.871 (05/2011), "JPEG File Interchange Format", read
// from https://www.itu.int/rec/T-REC-T.871-201105-I on 2026-11-02, clause 7:
// the YCbCr to RGB conversion applied when a frame has three components.
//
// SCOPE AND DEVIATIONS
//
// Baseline and extended sequential Huffman frames (SOF0, SOF1) at 8-bit
// precision, one to four components, any sampling factors 1 to 4, any
// number of scans, restart intervals. Not progressive, lossless,
// hierarchical or arithmetic coded; a DNL marker is refused.
//
// T.81 leaves upsampling to the application. Subsampled chroma is replicated
// here, which is the simplest choice and needs no siting convention.
//
// A decoder that meets a corrupt entropy segment does not stop: the blocks it
// could not read stay at their prediction and the image is returned with
// `damaged` set, because a picture with a smeared band is more use to a radio
// receiver than none.
//
// The entry point for scans without markers, decode_entropy, exists for
// formats that send fixed tables out of band: SSDV and DAB MOT SlideShow
// headers carry their tables in a specification rather than in the stream.
//
// CLEAN ROOM
//
// No JPEG implementation was read: not libjpeg, stb_image or any other.

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "core/error.h"

namespace revenant::decode {

// B.2.4.2: BITS, the number of codes of each length 1 to 16, and HUFFVAL.
struct JpegHuffmanSpec {
    std::array<std::uint8_t, 16> bits{};
    std::vector<std::uint8_t> values;
};

// B.2.4.1: 64 elements in zig-zag order.
struct JpegQuantSpec {
    std::array<std::uint16_t, 64> q{};
};

struct JpegComponent {
    std::uint8_t id = 0;
    std::uint8_t h = 1;
    std::uint8_t v = 1;
    std::uint8_t tq = 0;   // quantization table
    std::uint8_t td = 0;   // DC Huffman table, for the scan that carries it
    std::uint8_t ta = 0;   // AC Huffman table
};

// Everything the markers would have said, for decode_entropy.
struct JpegTables {
    std::array<JpegQuantSpec, 4> quant{};
    std::array<JpegHuffmanSpec, 4> dc{};
    std::array<JpegHuffmanSpec, 4> ac{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<JpegComponent> components;
    std::uint32_t restart_interval = 0;
};

struct JpegImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // 1 for Gray8, 3 for Rgb888 (T.871 clause 7 applied); a two or four
    // component frame comes out as its first component.
    int channels = 1;
    std::vector<std::uint8_t> pixels;
    // An entropy segment ran short or held an invalid code.
    bool damaged = false;
};

// A whole interchange-format stream, SOI to EOI.
[[nodiscard]] Expected<JpegImage> jpeg_decode(std::span<const std::uint8_t> bytes);

// One scan over every component in `tables`, in their order, without
// markers: interleaved when there is more than one. `byte_stuffed` false
// reads 0xFF as an ordinary byte, as SSDV's wiki says its scan data is sent.
[[nodiscard]] Expected<JpegImage> jpeg_decode_entropy(const JpegTables& tables,
                                                      std::span<const std::uint8_t> scan,
                                                      bool byte_stuffed);

}  // namespace revenant::decode
