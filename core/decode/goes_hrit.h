// GOES-R HRIT/EMWIN: complex baseband to xRIT files and EMWIN text products.
//
// SPECIFICATION
//
// No GOES-R HRIT/EMWIN product specification was located: the NOAA OSPO
// HRIT/EMWIN page (www.ospo.noaa.gov/operations/goes/hrit/, read
// 2026-11-02) links CGMS and the report below as its specifications and no
// NOAA document. What is implemented rests on:
//
//   - CGMS LRIT/HRIT Global Specification issue 2.8 (core/decode/xrit.h) for
//     everything from the CADU to the file: section 8.3's CVCDU, 255-bit
//     randomiser and marker, 8.2's M_PDU, 6's transport file.
//   - Aerospace Corporation, "A Flexible Software Based EMWIN/HRIT Prototype
//     Solution for the GOES-R Transition", ATR-2010(5482)-1, 10 October 2009,
//     prepared for the GOES-R Program Office, fetched 2026-11-02 from
//     www.ospo.noaa.gov/operations/goes/hrit/docs/ATR-2010-5482.pdf. Section
//     1 and table 5: HRIT is BPSK at 927 ksym/s, carrying EMWIN in the same
//     stream. Table 8, for GOES LRIT: r = 1/2 K = 7, G1 1111001, G2 1011011,
//     no symbol inversion, no puncturing, RRC roll-off 0.5, RS(255,223)
//     depth 4, h(x) = 1 + x^3 + x^5 + x^7 + x^8. HRIT's own coding is not
//     tabulated there; the LRIT row is taken for it, and section 4.10.1 says
//     the three signals share the CCSDS processing.
//
// OBSERVED, NOT DOCUMENTED
//
// Two sample files from the HRIT broadcast, published on that OSPO page
// (A_WWUS53KDVN102359_C_KWIN_20260311000017_940099-1-SVSDVNIA.lrit and
// Z_QATA00KWBC110845_C_KWIN_20260311204545_011287-3-RADSMSVY.lrit,
// downloaded 2026-11-02), were read as bytes. What this decoder takes from
// them is labelled as observation per docs/clean-room.md:
//
//   - both carry header type 129, record length 14: four characters "NOAA",
//     then three 16-bit fields (9, 0, 0 in both) and one octet (0 in the
//     text file, 10 in the other). Read here as agency, product ID,
//     subproduct ID, parameter and a compression code, by position only;
//   - both are file type 2 with product ID 9, and are EMWIN products: the
//     first's data field is a plain text bulletin starting with an
//     abbreviated heading line ("WWUS53 KDVN 102359"), CR CR LF, then a
//     product identifier line; the second's data field starts "PK", a ZIP
//     archive, with code 10. So: product 9 is taken to be EMWIN, code 0 as
//     uncompressed and code 10 as ZIP, which this decoder does not open.
//
// WHERE IT STOPS
//
// At files, plus EMWIN text bulletins split into heading, identifier and
// body. Image files go to the ImageSink only when uncompressed 8-bit;
// docs/modes.md records that GOES HRIT image compression is unresolved
// between sources (Rice under header type 131, or JPEG 2000), and no
// document in hand settles it, so compressed images are files only.
//
// CLEAN ROOM
//
// No HRIT or EMWIN decoder was read: not goestools, SatDump, xrit-rx, the
// prototype's own source that the report describes, or any other.

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

inline constexpr double kGoesHritSymbolRate = 927000.0;
inline constexpr double kGoesHritRollOff = 0.5;

// Header type 129 as observed (see above).
struct NoaaHeader {
    std::string agency;
    std::uint16_t product_id = 0;
    std::uint16_t subproduct_id = 0;
    std::uint16_t parameter = 0;
    std::uint8_t compression = 0;
};
inline constexpr std::uint16_t kNoaaProductEmwin = 9;          // observed
inline constexpr std::uint8_t kNoaaCompressionZip = 10;        // observed
[[nodiscard]] std::optional<NoaaHeader> noaa_header(const XritFile& file);

struct EmwinText {
    std::string filename;  // the annotation
    std::string heading;   // first line, e.g. "WWUS53 KDVN 102359"
    std::string product;   // second non-empty line, e.g. "SVSDVN"
    std::string body;      // the whole bulletin, line ends as sent
};
// The bulletin, when `file` is an uncompressed EMWIN text product.
[[nodiscard]] std::optional<EmwinText> emwin_text(const XritFile& file);

struct GoesHritConfig {
    double sample_rate = 2.4e6;
    double symbol_rate = kGoesHritSymbolRate;
    double roll_off = kGoesHritRollOff;
    bool g2_inverted = false;
};

struct GoesHritEvents {
    std::function<void(const XritFile&)> on_file;
    std::function<void(const EmwinText&)> on_emwin_text;
    ImageSink image;
};

class GoesHritDecoder {
public:
    [[nodiscard]] static Expected<GoesHritDecoder> create(const GoesHritConfig& config, GoesHritEvents events);
    void push(std::span<const dsp::Complex32> samples);
    void finish();
    [[nodiscard]] const CcsdsDownlinkStats& stats() const { return link_->stats(); }
    [[nodiscard]] const XritTransport& transport() const { return state_->transport; }

private:
    struct State {
        GoesHritEvents events;
        XritTransport transport;
        std::vector<XritFile> files;
        std::uint32_t next_image = 1;
    };
    GoesHritDecoder() = default;
    std::unique_ptr<State> state_;
    std::unique_ptr<CcsdsDownlink> link_;
};

}  // namespace revenant::decode
