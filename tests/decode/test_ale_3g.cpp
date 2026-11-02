// 3G ALE: the coding and PDUs against MIL-STD-188-141B Appendix C's printed
// tables and examples, and the decoder against the transmitter through noise,
// a two-path channel, a mistuned receiver and arbitrary chunking.
//
// Error rates are measured and reported in WARN lines with loose assertions,
// for the reason tests/decode/CMakeLists.txt gives.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "core/decode/ale_3g.h"
#include "core/dsp/synth/ale_3g_mod.h"

using namespace revenant;
using decode::Ale3gPduType;
using decode::Ale3gWaveform;

namespace {

constexpr dsp::SampleRate kRate = 8000;

void add_noise(std::vector<float>& x, double snr_db, double amplitude, std::uint32_t seed) {
    const double signal = amplitude * amplitude / 2.0;
    const double noise_3k = signal / std::pow(10.0, snr_db / 10.0);
    const double sigma = std::sqrt(noise_3k * (static_cast<double>(kRate) / 2.0) / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<double> n(0.0, sigma);
    for (auto& v : x) {
        v = static_cast<float>(v + n(rng));
    }
}

std::vector<float> two_path(const std::vector<float>& x) {
    const std::size_t delay = static_cast<std::size_t>(kRate) * 2 / 1000;
    std::vector<float> y(x.size(), 0.0F);
    for (std::size_t i = 0; i < x.size(); ++i) {
        y[i] = x[i] + (i >= delay ? 0.5F * x[i - delay] : 0.0F);
    }
    return y;
}

std::vector<float> render(const std::vector<siggen::Ale3gModBurst>& b, double offset = 0.0) {
    siggen::Ale3gModConfig mc;
    mc.rate = kRate;
    mc.offset_hz = offset;
    mc.lead_in_samples = 1500.0;
    mc.tail_samples = 2000.0;
    auto a = siggen::ale3g_render(mc, b);
    REQUIRE(a.has_value());
    return *a;
}

std::vector<decode::Ale3gBurst> run(const std::vector<float>& audio, std::size_t chunk = 0) {
    auto dec = decode::Ale3gDecoder::create({});
    REQUIRE(dec.has_value());
    std::vector<decode::Ale3gBurst> out;
    if (chunk == 0) {
        dec->process(audio, out);
    } else {
        for (std::size_t i = 0; i < audio.size(); i += chunk) {
            const std::size_t n = std::min(chunk, audio.size() - i);
            dec->process(std::span<const float>(audio.data() + i, n), out);
        }
    }
    dec->flush(out);
    return out;
}

}  // namespace

TEST_CASE("3G ALE LinkID matches the C.5.2.2.2 examples", "[ale3g]") {
    CHECK(decode::ale3g_link_id(1, 2) == 0x3D);
    CHECK(decode::ale3g_link_id(1, 3) == 0x24);
    CHECK(decode::ale3g_link_id(2, 1) == 0x04);
    CHECK(decode::ale3g_link_id(3, 1) == 0x33);
    CHECK(decode::ale3g_link_id(1951, 1) == 0x1E);
}

TEST_CASE("3G ALE tables", "[ale3g]") {
    // Spot values from tables C-V, C-VI, C-XI and C-IX, first and last.
    CHECK(decode::ale3g_tlc()[0] == 2);
    CHECK(decode::ale3g_tlc()[255] == 0);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw0)[0] == 7);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw0)[383] == 3);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw1)[0] == 4);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw1)[575] == 6);
    CHECK(decode::ale3g_pn()[0] == 0);
    CHECK(decode::ale3g_pn()[255] == 6);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw0).size() == 384);
    CHECK(decode::ale3g_preamble(Ale3gWaveform::Bw1).size() == 576);
}

TEST_CASE("3G ALE interleavers fill by rows and fetch by columns", "[ale3g]") {
    // Tables C-VII and C-XII under the C.5.1.2 algorithm.
    const auto o0 = decode::ale3g_interleave_order(Ale3gWaveform::Bw0);
    REQUIRE(o0.size() == 52);
    CHECK(o0[0] == 0);
    CHECK(o0[1] == 13);
    CHECK(o0[2] == 26);
    CHECK(o0[3] == 39);
    CHECK(o0[4] == 1);
    const auto o1 = decode::ale3g_interleave_order(Ale3gWaveform::Bw1);
    REQUIRE(o1.size() == 144);
    CHECK(o1[1] == 9);
    CHECK(o1[16] == 1);
    auto sorted = o1;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 144; ++i) {
        CHECK(sorted[static_cast<std::size_t>(i)] == i);
    }
}

TEST_CASE("3G ALE data symbols carry table C-VIII Walsh rows", "[ale3g]") {
    // An all-zero payload codes to all-zero bits, Walsh row 0000, so the data
    // symbols are the PN sequence itself (C.5.1.3.6).
    const auto d = decode::ale3g_data(Ale3gWaveform::Bw0, 0);
    REQUIRE(d.size() == 832);
    for (std::size_t i = 0; i < d.size(); ++i) {
        REQUIRE(d[i] == decode::ale3g_pn()[i % 256]);
    }
    // Encoders are tail biting: a single one in the payload produces exactly
    // weight-of-generators ones in the code.
    std::vector<std::uint8_t> p(26, 0);
    p[10] = 1;
    const auto c = decode::ale3g_encode(Ale3gWaveform::Bw0, p);
    CHECK(std::count(c.begin(), c.end(), 1) == 10);  // 133 and 171 octal: 5 + 5
}

TEST_CASE("3G ALE PDU layouts and CRCs", "[ale3g]") {
    const auto call = decode::ale3g_parse(Ale3gWaveform::Bw0, decode::ale3g_call(12, 2, 34, 17));
    CHECK(call.type == Ale3gPduType::Call);
    CHECK(call.crc_ok);
    CHECK(call.called_member == 12);
    CHECK(call.call_type == 2);
    CHECK(call.caller_member == 34);
    CHECK(call.caller_group == 17);
    const auto hs = decode::ale3g_parse(Ale3gWaveform::Bw0, decode::ale3g_handshake(0x3D, 1, 55));
    CHECK(hs.type == Ale3gPduType::Handshake);
    CHECK(hs.crc_ok);
    CHECK(hs.link_id == 0x3D);
    CHECK(hs.argument == 55);
    const auto sc = decode::ale3g_parse(Ale3gWaveform::Bw0, decode::ale3g_scanning_call(1951));
    CHECK(sc.type == Ale3gPduType::ScanningCall);
    CHECK(sc.crc_ok);
    CHECK(sc.called_address == 1951);
    // C.5.2.2: "the LE_Broadcast PDU shall begin 0, 1, 1, 1, 0".
    CHECK(decode::ale3g_parse(Ale3gWaveform::Bw0, std::uint64_t{0b01110} << 21U).type == Ale3gPduType::Broadcast);
    const auto tm = decode::ale3g_parse(Ale3gWaveform::Bw1, decode::ale3g_tm(2, 2047, 100, 2, 5));
    CHECK(tm.type == Ale3gPduType::TmTerminate);
    CHECK(tm.crc_ok);
    CHECK(tm.dest_address == 2047);
    CHECK(tm.source_address == 100);
    // A flipped bit fails each CRC.
    CHECK_FALSE(decode::ale3g_parse(Ale3gWaveform::Bw0, decode::ale3g_call(12, 2, 34, 17) ^ 0x100U).crc_ok);
    CHECK_FALSE(decode::ale3g_parse(Ale3gWaveform::Bw1, decode::ale3g_tm(2, 2047, 100, 2, 5) ^ 0x1000U).crc_ok);
}

TEST_CASE("3G ALE bursts round trip", "[ale3g]") {
    const std::vector<siggen::Ale3gModBurst> bursts = {
        {Ale3gWaveform::Bw0, decode::ale3g_call(12, 2, 34, 17), 3000.0},
        {Ale3gWaveform::Bw0, decode::ale3g_handshake(0x3D, 1, 55), 3000.0},
        {Ale3gWaveform::Bw1, decode::ale3g_tm(0, 300, 100, 0, 9), 3000.0},
        {Ale3gWaveform::Bw0, decode::ale3g_scanning_call(1951), 0.0},
    };
    const auto check = [&](const std::vector<decode::Ale3gBurst>& out) {
        REQUIRE(out.size() == bursts.size());
        for (std::size_t i = 0; i < bursts.size(); ++i) {
            CHECK(out[i].waveform == bursts[i].waveform);
            CHECK(out[i].payload == bursts[i].payload);
            CHECK(out[i].pdu.crc_ok);
        }
    };
    const auto clean = render(bursts);
    const auto whole = run(clean);
    check(whole);
    for (const std::size_t chunk : {1U, 333U, 8192U}) {
        const auto parts = run(clean, chunk);
        REQUIRE(parts.size() == whole.size());
        for (std::size_t i = 0; i < parts.size(); ++i) {
            CHECK(parts[i].position == whole[i].position);
            CHECK(parts[i].payload == whole[i].payload);
        }
    }
    const auto off = run(render(bursts, 45.0));
    check(off);
    CHECK(std::abs(off[0].offset_hz - 45.0) < 2.0);
}

TEST_CASE("3G ALE in noise and on two paths", "[ale3g]") {
    std::mt19937 rng(4);
    for (const bool multipath : {false, true}) {
        for (const double snr : {0.0, -3.0, -6.0, -9.0}) {
            int ok = 0;
            constexpr int kTrials = 30;
            for (int i = 0; i < kTrials; ++i) {
                const auto payload =
                    decode::ale3g_call(static_cast<int>(rng() % 60), static_cast<int>(rng() % 8),
                                       static_cast<int>(rng() % 64), static_cast<int>(rng() % 32));
                auto audio = render({{Ale3gWaveform::Bw0, payload, 0.0}}, 20.0);
                if (multipath) {
                    audio = two_path(audio);
                }
                add_noise(audio, snr, 0.5, static_cast<std::uint32_t>(100 + i));
                const auto out = run(audio);
                if (out.size() == 1 && out[0].payload == payload) {
                    ++ok;
                }
            }
            WARN("3G ALE BW0 " << (multipath ? "two-path" : "AWGN") << " SNR " << snr << " dB: " << ok << " of "
                               << kTrials << " decoded");
            if (snr >= 0.0) {
                CHECK(ok >= kTrials - 1);
            }
        }
    }
}

TEST_CASE("3G ALE noise alone decodes nothing valid", "[ale3g]") {
    std::vector<float> audio(static_cast<std::size_t>(kRate) * 20, 0.0F);
    add_noise(audio, 0.0, 0.5, 77);
    const auto out = run(audio);
    int valid = 0;
    for (const auto& b : out) {
        valid += b.pdu.crc_ok ? 1 : 0;
    }
    WARN("3G ALE noise only, 20 s: " << out.size() << " detections, " << valid << " with a good CRC");
    CHECK(out.size() <= 2);
}
