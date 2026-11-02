// GOES HRIT/EMWIN, core/decode/goes_hrit.h, against core/dsp/synth/
// goes_hrit_mod.h: the header type 129 layout as observed in NOAA's sample
// files, EMWIN text bulletins, a ZIP-coded EMWIN product passed through as a
// file, an uncompressed image to the ImageSink, an LNB offset with drift and
// noise, and any blocking.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/decode/goes_hrit.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/goes_hrit_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

const std::string kBulletin =
    "WWUS99 KXYZ 021200\r\r\nSPSXYZ\r\r\n\r\r\nSpecial Weather Statement\r\r\nTest bulletin body.\r\r\n$$\r\r\n";

struct Fixture {
    std::vector<std::uint8_t> text = siggen::emwin_text_file("A_WWUS99KXYZ021200_C_KWIN_TEST.lrit", kBulletin);
    std::vector<std::uint8_t> zip_file = [] {
        NoaaHeader n;
        n.agency = "NOAA";
        n.product_id = kNoaaProductEmwin;
        n.compression = kNoaaCompressionZip;
        const std::string name = "Z_TEST.lrit";
        return siggen::xrit_file_bytes(2, {{4, std::vector<std::uint8_t>(name.begin(), name.end())},
                                           siggen::noaa_record(n)},
                                       {'P', 'K', 3, 4, 0, 0});
    }();
    std::vector<std::uint8_t> pixels = [] {
        std::vector<std::uint8_t> p(200 * 60);
        for (std::size_t i = 0; i < p.size(); ++i) {
            p[i] = static_cast<std::uint8_t>(i * 31 + i / 200);
        }
        return p;
    }();
    std::vector<std::uint8_t> image = siggen::goes_image_file("IMG_TEST.lrit", 200, 60, pixels);
    std::vector<siggen::XritSend> sends() const {
        return {{20, siggen::xrit_packets(640, 1, text)},
                {20, siggen::xrit_packets(641, 2, zip_file)},
                {0, siggen::xrit_packets(1, 3, image)}};
    }
};

struct Got {
    std::vector<XritFile> files;
    std::vector<EmwinText> texts;
    std::size_t rows = 0;
};

Got run(const std::vector<dsp::Complex32>& s, std::size_t chunk, CcsdsDownlinkStats* st = nullptr) {
    Got g;
    GoesHritEvents ev;
    ev.on_file = [&](const XritFile& f) { g.files.push_back(f); };
    ev.on_emwin_text = [&](const EmwinText& t) { g.texts.push_back(t); };
    ev.image.on_row = [&](const ImageRow&) { ++g.rows; };
    auto d = GoesHritDecoder::create({}, std::move(ev)).value();
    for (std::size_t i = 0; i < s.size(); i += chunk) {
        d.push(std::span(s).subspan(i, std::min(chunk, s.size() - i)));
    }
    d.finish();
    if (st != nullptr) {
        *st = d.stats();
    }
    return g;
}

}  // namespace

TEST_CASE("NOAA header 129 parses the 14-octet layout seen in the sample files", "[decode][goes_hrit]") {
    // The record as it appears at offset 0x52 of the published text sample.
    const std::vector<std::uint8_t> rec = {0x81, 0x00, 0x0E, 'N', 'O', 'A', 'A', 0x00,
                                           0x09, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::vector<std::uint8_t> file = {0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00, 16 + 14,
                                      0, 0, 0, 0, 0, 0, 0, 8};
    file.insert(file.end(), rec.begin(), rec.end());
    file.push_back('X');
    const auto f = parse_xrit_file(file).value();
    const auto n = noaa_header(f).value();
    CHECK(n.agency == "NOAA");
    CHECK(n.product_id == 9);
    CHECK(n.compression == 0);
    CHECK(f.data.size() == 1);
}

TEST_CASE("GOES HRIT files, EMWIN text and images round trip over BPSK", "[decode][goes_hrit]") {
    Fixture fx;
    for (const double phase : {0.1, 0.1 + 3.14159}) {
        auto spec = siggen::goes_hrit_bpsk_spec();
        spec.initial_phase = phase;
        spec.carrier_offset_hz = 60000.0;
        spec.timing_offset_samples = 0.45;
        const auto s = siggen::xrit_bpsk_baseband(spec, fx.sends()).value();
        const auto g = run(s, 1 << 16);
        REQUIRE(g.files.size() == 3);
        REQUIRE(g.texts.size() == 1);
        CHECK(g.texts[0].heading == "WWUS99 KXYZ 021200");
        CHECK(g.texts[0].product == "SPSXYZ");
        CHECK(g.texts[0].body == kBulletin);
        CHECK(g.texts[0].filename == "A_WWUS99KXYZ021200_C_KWIN_TEST.lrit");
        CHECK(noaa_header(g.files[1]).value().compression == kNoaaCompressionZip);
        CHECK(g.files[2].data == fx.pixels);
        CHECK(g.rows == 60);
    }
}

TEST_CASE("GOES HRIT decodes the same in any blocking", "[decode][goes_hrit]") {
    Fixture fx;
    auto s = siggen::xrit_bpsk_baseband(siggen::goes_hrit_bpsk_spec(), fx.sends()).value();
    REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(5.0, 463500.0 * 223 / 255), 2400000, 8)
                .has_value());
    const auto a = run(s, s.size());
    const auto b = run(s, 7919);
    const auto c = run(s, 255);
    REQUIRE(a.files.size() == 3);
    REQUIRE(b.files.size() == 3);
    REQUIRE(c.files.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        CHECK(a.files[i].data == b.files[i].data);
        CHECK(a.files[i].data == c.files[i].data);
    }
}

TEST_CASE("GOES HRIT with LNB offset, drift and noise", "[decode][goes_hrit]") {
    Fixture fx;
    for (const double eb_n0 : {3.0, 4.0, 5.0, 7.0}) {
        auto spec = siggen::goes_hrit_bpsk_spec();
        spec.initial_phase = 2.5;
        spec.clock_error = 40e-6;
        auto s = siggen::xrit_bpsk_baseband(spec, fx.sends()).value();
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = 45000;
        fc.drift_hz_per_second = 200.0;
        REQUIRE(siggen::apply_frequency_offset(s, fc, 2400000, 4).has_value());
        // 927 ksym/s at r = 1/2 is 463.5 kbit/s of CADU, 223/255 of it data.
        REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(eb_n0, 463500.0 * 223.0 / 255.0), 2400000,
                                 static_cast<std::uint64_t>(eb_n0 * 10))
                    .has_value());
        CcsdsDownlinkStats st;
        const auto g = run(s, 1 << 15, &st);
        const auto good = static_cast<std::size_t>(
            std::count_if(g.files.begin(), g.files.end(), [](const XritFile& f) { return f.crc_ok; }));
        std::printf("GOES HRIT BPSK Eb/N0 %.1f dB, 45 kHz at 200 Hz/s: %zu frames, %zu RS symbols corrected, "
                    "%zu/3 files, %zu EMWIN texts\n",
                    eb_n0, st.frames, st.rs_corrected, good, g.texts.size());
        if (eb_n0 >= 4.0) {
            CHECK(good == 3);
            CHECK(g.texts.size() == 1);
        }
    }
}
