// GEO-KOMPSAT-2A LRIT: complex baseband to LRIT files.
//
// SPECIFICATION
//
// NMSC/KMA, "GK2A LRIT Mission Specification Document", issue 1.0, 31 May
// 2019, fetched 2026-11-02 from nmsc.kma.go.kr/resources/homepage/pdf/
// GK2A_LRIT_Mission_Specification_Document_v1.0.pdf. Cited as "GK2A". With
// the CGMS LRIT/HRIT Global Specification 2.8 it builds on (core/decode/
// xrit.h).
//
//   - Section 9, table 9.1: 1692.14 MHz, BPSK NRZ-L, root raised cosine
//     roll-off 0.5, r = 1/2 K = 7 convolutional code "except symbol inversion
//     on output path of G2", RS(255,223) depth 4, 1024-octet CADU, 64 kbit/s
//     information rate, which the table's note defines as the CADU rate
//     before the convolutional code: 128 ksym/s. (docs/modes.md says about
//     146 ksym/s; the document says 128.)
//   - Section 8: M_PDU of 2 + 884 octets, AOS frame, 892-octet VCDU, 128
//     check octets, marker 0x1ACFFC1D. The randomiser is not restated there
//     and is CGMS 8.3.2.2's 255-bit sequence.
//   - Section 6: transport file and source packets per CGMS, APID and
//     sequence flags in table 6.4, CRC g(x) = x^16 + x^12 + x^5 + 1.
//   - Section 4.4.9, table 4.10: header type 128, image segment
//     identification: segment sequence number (1 octet), total segments (1),
//     line number of the segment's first line (2).
//   - Section 5: image files are compressed (5.1, lossless, citing ISO/IEC
//     15444-1) and may be DES encrypted (5.2).
//
// The G2 phrase is read as "no inversion": GK2A follows the basic code
// "except" the inversion, and ATR-2010(5482)-1 table 8 lists the GOES LRIT
// code, which GK-2A's mirrors, with "symbol inversion: none". `g2_inverted`
// flips it if a recording says otherwise.
//
// WHERE IT STOPS
//
// At files. Uncompressed 8-bit image files (compression flag 0) go to the
// ImageSink, one image per file; GK-2A's own image files are JPEG 2000 and
// are delivered as files only, because no JPEG 2000 decoder is in the tree.
// Encrypted files are delivered undecrypted, flagged by XritFile::encrypted.
//
// CLEAN ROOM
//
// No GK-2A or xRIT decoder was read: not xrit-rx, goestools, SatDump or any
// other.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "core/decode/ccsds_downlink.h"
#include "core/decode/image_sink.h"
#include "core/decode/xrit.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr double kGk2aLritSymbolRate = 128000.0;
inline constexpr double kGk2aLritRollOff = 0.5;

// GK2A table 4.10.
struct Gk2aSegment {
    std::uint8_t sequence = 0;
    std::uint8_t total = 0;
    std::uint16_t first_line = 0;
};
[[nodiscard]] std::optional<Gk2aSegment> gk2a_segment(const XritFile& file);

struct Gk2aLritConfig {
    double sample_rate = 512000.0;
    double symbol_rate = kGk2aLritSymbolRate;
    bool g2_inverted = false;
};

struct Gk2aLritEvents {
    std::function<void(const XritFile&)> on_file;
    ImageSink image;
};

class Gk2aLritDecoder {
public:
    [[nodiscard]] static Expected<Gk2aLritDecoder> create(const Gk2aLritConfig& config, Gk2aLritEvents events);
    void push(std::span<const dsp::Complex32> samples);
    void finish();
    [[nodiscard]] const CcsdsDownlinkStats& stats() const { return link_->stats(); }
    [[nodiscard]] const XritTransport& transport() const { return state_->transport; }

private:
    struct State {
        Gk2aLritEvents events;
        XritTransport transport;
        std::vector<XritFile> files;
        std::uint32_t next_image = 1;
    };
    Gk2aLritDecoder() = default;
    std::unique_ptr<State> state_;
    std::unique_ptr<CcsdsDownlink> link_;
};

// Sends an uncompressed 8-bit image file to `sink`, one image; false when the
// file is not one.
bool xrit_emit_image(const XritFile& file, const ImageSink& sink, std::uint32_t image_id, const std::string& label);

}  // namespace revenant::decode
