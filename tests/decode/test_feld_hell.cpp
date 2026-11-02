// Feld-Hell: the page's timing, then the receiver against the transmitter
// in slots. Document: core/decode/feld_hell.h.

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "core/decode/feld_hell.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/feld_hell_mod.h"
#include "core/dsp/synth/rds_mod.h"

using namespace revenant;

namespace {

constexpr dsp::SampleRate kRate = 8000;

double mean_power(const std::vector<float>& x) {
    double sum = 0.0;
    for (const float v : x) {
        sum += static_cast<double>(v) * v;
    }
    return x.empty() ? 0.0 : sum / static_cast<double>(x.size());
}

// Random columns obeying "The Two Pixel Rule": no key-down run shorter than
// two slots inside a column. Two blank columns first, so the receiver's peak
// has a reference before the first dot matters only from column two on.
std::vector<std::uint16_t> random_columns(std::size_t count, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::vector<std::uint16_t> out(2, 0);
    for (std::size_t c = 0; c < count; ++c) {
        std::uint16_t col = 0;
        int slot = 1;  // the bottom and top slots stay blank, as in the font
        while (slot < 13) {
            const int run = 2 + static_cast<int>(rng() % 3);
            const bool on = (rng() % 3) != 0;
            for (int i = 0; i < run && slot < 13; ++i, ++slot) {
                if (on) {
                    col = static_cast<std::uint16_t>(col | (1U << slot));
                }
            }
            slot += 1 + static_cast<int>(rng() % 2);  // a gap
        }
        out.push_back(col);
    }
    return out;
}

// The receiver decides each slot a column late, so the transmission is
// followed by two columns of silence to carry the last one out.
siggen::FeldHellModConfig tail_config() {
    siggen::FeldHellModConfig c;
    c.tail_samples = 2.0 * 14.0 * kRate / decode::kFeldHellSlotRate;
    return c;
}

std::vector<decode::FeldHellColumn> receive(const std::vector<float>& audio, std::size_t chunk = 0) {
    auto dec = decode::FeldHellDecoder::create(decode::FeldHellConfig{});
    REQUIRE(dec.has_value());
    std::vector<decode::FeldHellColumn> out;
    if (chunk == 0) {
        dec->process(audio, out);
    } else {
        for (std::size_t at = 0; at < audio.size(); at += chunk) {
            dec->process(std::span<const float>(audio.data() + at, std::min(chunk, audio.size() - at)),
                         out);
        }
    }
    return out;
}

double slot_error_rate(const std::vector<std::uint16_t>& tx,
                       const std::vector<decode::FeldHellColumn>& rx) {
    std::size_t errors = 0;
    std::size_t slots = 0;
    for (std::size_t c = 2; c < tx.size(); ++c) {
        const std::uint16_t got = c < rx.size() ? rx[c].bits : 0;
        errors += static_cast<std::size_t>(std::popcount(static_cast<unsigned>(got ^ tx[c])));
        slots += decode::kFeldHellHalfPixelsPerColumn;
    }
    return static_cast<double>(errors) / static_cast<double>(slots);
}

}  // namespace

TEST_CASE("Feld-Hell timing is the page's", "[feld_hell]") {
    // "150 characters are transmitted every minute. Each character takes
    // 400ms", seven columns a character, "exactly 17.5 columns per second".
    CHECK(decode::kFeldHellColumnRate == 17.5);
    CHECK(std::abs(1.0 / decode::kFeldHellPixelRate - 0.008163) < 1e-6);
    CHECK(std::abs(7.0 / decode::kFeldHellColumnRate - 0.4) < 1e-12);
}

TEST_CASE("Feld-Hell columns round trip, bottom slot first", "[feld_hell]") {
    const auto tx = random_columns(300, 1);
    auto audio = siggen::feld_hell_render(tail_config(), tx);
    REQUIRE(audio.has_value());
    const auto rx = receive(*audio);
    REQUIRE(rx.size() + 1 >= tx.size());
    CHECK(slot_error_rate(tx, rx) == 0.0);

    // A one-pixel dot in slots 1 and 2, near the bottom, reads in bits 1 and 2.
    std::vector<std::uint16_t> one = {0, 0, 0b0000'0000'0000'0110, 0, 0};
    auto a = siggen::feld_hell_render(tail_config(), one);
    REQUIRE(a.has_value());
    const auto r = receive(*a);
    REQUIRE(r.size() >= 3);
    CHECK(r[2].bits == 0b0110);
}

TEST_CASE("Feld-Hell output does not depend on chunking", "[feld_hell]") {
    const auto tx = random_columns(80, 2);
    auto audio = siggen::feld_hell_render(tail_config(), tx);
    REQUIRE(audio.has_value());
    REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                  siggen::NoiseLevel::snr_in_2500_hz_db(3.0), kRate, 9)
                .has_value());
    const auto whole = receive(*audio);
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{97}, std::size_t{4000}}) {
        const auto parts = receive(*audio, chunk);
        REQUIRE(parts.size() == whole.size());
        for (std::size_t i = 0; i < whole.size(); ++i) {
            CHECK(parts[i].bits == whole[i].bits);
            CHECK(parts[i].position == whole[i].position);
        }
    }
}

TEST_CASE("Feld-Hell slot errors through noise", "[feld_hell][noise]") {
    const auto tx = random_columns(400, 3);
    // Limits of 1 are measurement points.
    const std::pair<double, double> points[] = {
        {20.0, 0.0}, {10.0, 0.0}, {6.0, 0.001}, {3.0, 1.0}, {0.0, 1.0}, {-3.0, 1.0}, {-6.0, 1.0}};
    for (const auto& [snr, limit] : points) {
        auto audio = siggen::feld_hell_render(tail_config(), tx);
        REQUIRE(audio.has_value());
        REQUIRE(siggen::add_real_awgn(*audio, mean_power(*audio),
                                      siggen::NoiseLevel::snr_in_2500_hz_db(snr), kRate, 11)
                    .has_value());
        const double ser = slot_error_rate(tx, receive(*audio));
        std::printf("Feld-Hell at %.1f dB in 2500 Hz: slot error rate %.4f over %zu slots\n", snr,
                    ser, (tx.size() - 2) * 14);
        CHECK(ser <= limit);
    }
}
