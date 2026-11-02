// GK-2A LRIT, core/decode/gk2a_lrit.h, and the CGMS transport of
// core/decode/xrit.h, against core/dsp/synth/gk2a_lrit_mod.h: files split
// across source packets and frames, the segment header, the ImageSink, a
// lost packet, BPSK with an LNB offset, drift and noise, and any blocking.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/decode/gk2a_lrit.h"
#include "core/decode/xrit.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/gk2a_lrit_mod.h"

using namespace revenant;
using namespace revenant::decode;

namespace {

std::vector<std::uint8_t> picture(std::uint16_t w, std::uint16_t h, unsigned seed) {
    std::vector<std::uint8_t> p(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < p.size(); ++i) {
        p[i] = static_cast<std::uint8_t>((i * 7 + seed * 13 + (i / w) * 3) & 0xFFU);
    }
    return p;
}

SpacePacket as_packet(const std::vector<std::uint8_t>& octets) {
    return {parse_space_packet_header(octets).value(), octets};
}

struct Fixture {
    std::vector<std::uint8_t> image1 = picture(120, 90, 1);
    std::vector<std::uint8_t> image2 = picture(120, 90, 2);
    std::vector<std::uint8_t> file1 =
        siggen::gk2a_image_file("IMG_FD_001_IR105_20261102_000000_01.lrit", 120, 90, image1, {1, 2, 1});
    std::vector<std::uint8_t> file2 =
        siggen::gk2a_image_file("IMG_FD_001_IR105_20261102_000000_02.lrit", 120, 90, image2, {2, 2, 91});
    std::string text = "GK2A dissemination schedule\r\n0000 FD\r\n";
    std::vector<std::uint8_t> file3 = siggen::gk2a_text_file("ADD_ANT_001_20261102_000000_01.lrit", text);
    std::vector<siggen::XritSend> sends() const {
        auto p1 = siggen::xrit_packets(5, 30, file1, 0);
        const auto p2 = siggen::xrit_packets(5, 31, file2, static_cast<std::uint16_t>(p1.size()));
        p1.insert(p1.end(), p2.begin(), p2.end());
        return {{0, p1}, {4, siggen::xrit_packets(128, 255, file3)}};
    }
};

}  // namespace

TEST_CASE("xRIT transport reassembles files and checks every segment's CRC", "[decode][xrit]") {
    Fixture fx;
    const auto packets = siggen::xrit_packets(5, 30, fx.file1);
    REQUIRE(packets.size() == 2);  // 10 + 10859 octets over 8190-octet blocks
    XritTransport t;
    std::vector<XritFile> out;
    for (const auto& p : packets) {
        t.push(0, as_packet(p), out);
    }
    REQUIRE(out.size() == 1);
    const auto& f = out[0];
    CHECK(f.crc_ok);
    CHECK(f.apid == 5);
    CHECK(f.file_counter == 30);
    CHECK(f.file_type == 0);
    CHECK(f.data == fx.image1);
    CHECK(f.annotation() == "IMG_FD_001_IR105_20261102_000000_01.lrit");
    const auto is = f.image_structure().value();
    CHECK(is.columns == 120);
    CHECK(is.lines == 90);
    CHECK(is.bits_per_pixel == 8);
    const auto seg = gk2a_segment(f).value();
    CHECK(seg.sequence == 1);
    CHECK(seg.total == 2);
    CHECK_FALSE(f.encrypted());

    // A corrupted octet fails that segment's CRC; the file is still
    // delivered, flagged.
    auto bad = packets;
    bad[1][100] ^= 0x40U;
    out.clear();
    for (const auto& p : bad) {
        t.push(0, as_packet(p), out);
    }
    REQUIRE(out.size() == 1);
    CHECK_FALSE(out[0].crc_ok);

    // A missing continuation drops the file.
    out.clear();
    std::vector<std::uint8_t> big = fx.file1;
    big.insert(big.end(), fx.file2.begin(), fx.file2.end());
    const auto many = siggen::xrit_packets(7, 2, big);
    REQUIRE(many.size() == 3);
    t.push(0, as_packet(many[0]), out);
    t.push(0, as_packet(many[2]), out);
    CHECK(out.empty());
    CHECK(t.dropped() >= 1);
}

TEST_CASE("GK-2A LRIT files and images round trip over BPSK", "[decode][gk2a]") {
    Fixture fx;
    for (const double phase : {0.4, 0.4 + 3.14159}) {
        auto spec = siggen::gk2a_bpsk_spec();
        spec.initial_phase = phase;
        spec.carrier_offset_hz = 9000.0;  // LNB error
        spec.timing_offset_samples = 0.6;
        const auto s = siggen::xrit_bpsk_baseband(spec, fx.sends()).value();
        std::vector<XritFile> files;
        std::vector<ImageRow> rows;
        std::vector<ImageOpen> opens;
        Gk2aLritEvents ev;
        ev.on_file = [&](const XritFile& f) { files.push_back(f); };
        ev.image.on_open = [&](const ImageOpen& o) { opens.push_back(o); };
        ev.image.on_row = [&](const ImageRow& r) { rows.push_back(r); };
        auto d = Gk2aLritDecoder::create({}, std::move(ev)).value();
        d.push(s);
        d.finish();
        REQUIRE(files.size() == 3);
        CHECK(files[0].data == fx.image1);
        CHECK(files[1].data == fx.image2);
        CHECK(gk2a_segment(files[1]).value().first_line == 91);
        CHECK(files[2].file_type == 2);
        CHECK(std::string(files[2].data.begin(), files[2].data.end()) == fx.text);
        CHECK(files[2].virtual_channel == 4);
        REQUIRE(opens.size() == 2);
        CHECK(opens[0].width == 120);
        REQUIRE(rows.size() == 180);
        CHECK(std::equal(rows[0].bytes.begin(), rows[0].bytes.end(), fx.image1.begin()));
    }
}

TEST_CASE("GK-2A LRIT decodes the same in any blocking", "[decode][gk2a]") {
    Fixture fx;
    auto spec = siggen::gk2a_bpsk_spec();
    auto s = siggen::xrit_bpsk_baseband(spec, fx.sends()).value();
    REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(5.0, 64000.0 * 223 / 255), 512000, 3).has_value());
    auto run = [&](std::size_t chunk) {
        std::vector<std::vector<std::uint8_t>> got;
        Gk2aLritEvents ev;
        ev.on_file = [&](const XritFile& f) { got.push_back(f.data); };
        auto d = Gk2aLritDecoder::create({}, std::move(ev)).value();
        for (std::size_t i = 0; i < s.size(); i += chunk) {
            d.push(std::span(s).subspan(i, std::min(chunk, s.size() - i)));
        }
        d.finish();
        return got;
    };
    const auto a = run(s.size());
    CHECK(a.size() == 3);
    CHECK(run(4099) == a);
    CHECK(run(61) == a);
}

TEST_CASE("GK-2A LRIT with offset, drift and noise", "[decode][gk2a]") {
    Fixture fx;
    for (const double eb_n0 : {3.0, 4.0, 5.0, 7.0}) {
        auto spec = siggen::gk2a_bpsk_spec();
        spec.initial_phase = 1.3;
        spec.clock_error = 50e-6;
        auto s = siggen::xrit_bpsk_baseband(spec, fx.sends()).value();
        // A warming LNB: 12 kHz off and drifting 30 Hz/s.
        siggen::FrequencyConfig fc;
        fc.doppler_shift_hz = -12000;
        fc.drift_hz_per_second = 30.0;
        REQUIRE(siggen::apply_frequency_offset(s, fc, 512000, 2).has_value());
        REQUIRE(siggen::add_awgn(s, siggen::NoiseLevel::eb_over_n0_db(eb_n0, 64000.0 * 223.0 / 255.0), 512000,
                                 static_cast<std::uint64_t>(eb_n0 * 10))
                    .has_value());
        std::size_t good = 0;
        Gk2aLritEvents ev;
        ev.on_file = [&](const XritFile& f) { good += f.crc_ok ? 1 : 0; };
        auto d = Gk2aLritDecoder::create({}, std::move(ev)).value();
        d.push(s);
        d.finish();
        std::printf("GK-2A LRIT BPSK Eb/N0 %.1f dB, -12 kHz at 30 Hz/s: %zu frames, %zu RS symbols corrected, "
                    "%zu/3 files\n",
                    eb_n0, d.stats().frames, d.stats().rs_corrected, good);
        if (eb_n0 >= 4.0) {
            CHECK(good == 3);
        }
    }
}
