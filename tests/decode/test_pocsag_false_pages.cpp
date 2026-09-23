// POCSAG: pages nobody sent, from noise alone and from a real transmitter at
// low signal to noise.
//
// Through the wire, at 6 and 4 dB, the POCSAG decoders reported pages for
// addresses nobody sent, several with no uncorrectable codeword. This file
// reproduces that at decode level and holds the decoder to what ITU-R
// M.584-2 lets it refuse; core/decode/pocsag.h has the two rules and their
// clauses. Each case decodes the same audio with an address correction
// budget of two, what the decoder allowed before, and with the default of
// one, so the log shows what each rule buys and what the budget costs.
//
// WHAT IT WAS, AND WHAT EACH RULE BOUGHT
//
// Measured 2026-09-23 with this file's seeds: 240 pages sent at 1200 bit/s
// per point, false pages the 1200 bit/s decoder reported, how many of them
// had no uncorrectable codeword, and the pages that came through whole.
//
//     dB in 2500 Hz   before            sync rule only    both rules       whole, before / after
//     12              0                 0                 0                240 / 240
//     8               3, 3 clean        3                 0                131 / 113
//     6               150, 67 clean     148               3, 1 clean       0 / 0
//     5               131, 34 clean     109               7, 0 clean       0 / 0
//     4               37, 7 clean       21                0                0 / 0
//
// Per page sent, 0.625 false pages at 6 dB became 0.0125, and 0.546 at 5 dB
// became 0.029. Most of what was refused had two bits corrected in its
// address, and many were an idle codeword read one code word over: their
// identities sit around 2007664, the idle codeword's own. What is left has
// five or more errors in a codeword and lands within one bit of another,
// which the code cannot tell from a real one. The cost is at 8 dB, 18 of
// 240 pages whose address had exactly two bits wrong, and none at 12 dB.
// The 512 and 2400 bit/s decoders reported nothing from this audio before
// or after. Noise alone, 40 hours: 8.1 pages an hour before, none after,
// with either rule on its own also giving none.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <print>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "core/decode/dv_phy.h"
#include "core/decode/pocsag.h"
#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/fsk_mod.h"

using namespace revenant;

namespace {

constexpr std::array<double, 3> kRates = {decode::kPocsag512, decode::kPocsag1200,
                                          decode::kPocsag2400};

// The budget the decoder had before, and the default now.
constexpr std::array<int, 2> kBudgets = {2, decode::PocsagConfig{}.address_correction_budget};

// Hours of noise the noise-only case decodes, from REVENANT_POCSAG_NOISE_HOURS
// when set, for a long measurement run.
double noise_hours(double fallback) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): read once, at test start.
    const char* text = std::getenv("REVENANT_POCSAG_NOISE_HOURS");
    if (text == nullptr) {
        return fallback;
    }
    const double value = std::atof(text);
    return value > 0.0 ? value : fallback;
}

double channel_response(double hertz, const void*) {
    return std::abs(hertz) <= 7000.0 ? 1.0 : 0.0;
}

struct Sent {
    std::uint32_t identity;
    std::string text;
};

std::vector<Sent> pages_for(std::uint64_t seed, std::size_t count) {
    std::vector<Sent> sent;
    std::mt19937_64 engine(seed);
    for (std::size_t i = 0; i < count; ++i) {
        std::string text;
        for (int k = 0; k < 40; ++k) {
            text.push_back(static_cast<char>(' ' + engine() % 94));
        }
        sent.push_back({static_cast<std::uint32_t>(engine() & 0x1FFFFFU), text});
    }
    return sent;
}

std::vector<std::uint8_t> bits_for(const std::vector<Sent>& sent) {
    std::vector<siggen::PocsagPageSpec> specs;
    for (const Sent& s : sent) {
        siggen::PocsagPageSpec spec;
        spec.identity = s.identity;
        spec.function = decode::kPocsagFunctionAlphanumeric;
        auto bits = siggen::pocsag_alphanumeric_bits(s.text);
        REQUIRE(bits.has_value());
        spec.message_bits = *bits;
        specs.push_back(spec);
    }
    return siggen::pocsag_bits(specs);
}

decode::PocsagDecoder make_decoder(double bit_rate, dsp::SampleRate rate, int budget) {
    decode::PocsagConfig config;
    config.rate = rate;
    config.bit_rate = bit_rate;
    config.address_correction_budget = budget;
    auto decoder = decode::PocsagDecoder::create(config);
    REQUIRE(decoder.has_value());
    return std::move(*decoder);
}

std::string describe(const decode::PocsagPage& page) {
    return std::format("id {} fn {} corrected {} uncorrectable {} words {} at {}", page.identity,
                       page.function, page.corrected_bits, page.uncorrectable_codewords,
                       page.message_bits.size() / 20, page.position);
}

}  // namespace

TEST_CASE("POCSAG holds a batch found without a preamble until the next one", "[decode][pocsag]") {
    // Clean audio, so only the rules are under test. One short page in frame
    // 0 fits in the first batch; three long ones run to several.
    const std::vector<Sent> short_page = {{1234560, "Short one."}};
    const std::vector<Sent> two_batches = pages_for(0x9A6E'0000'0000'0200ULL, 3);

    const auto decode_bits = [](std::span<const std::uint8_t> bits,
                                decode::PocsagStats* stats = nullptr) {
        auto audio = siggen::pocsag_render_audio(siggen::PocsagModConfig{}, bits);
        REQUIRE(audio.has_value());
        decode::PocsagDecoder decoder =
            make_decoder(decode::kPocsag1200, decode::PocsagConfig{}.rate,
                         decode::PocsagConfig{}.address_correction_budget);
        std::vector<decode::PocsagPage> pages;
        decoder.process(*audio, pages);
        decoder.flush(pages);
        if (stats != nullptr) {
            *stats = decoder.stats();
        }
        return pages;
    };

    for (const auto* sent : {&short_page, &two_batches}) {
        const std::vector<std::uint8_t> bits = bits_for(*sent);
        INFO(sent->size() << " pages, " << bits.size() << " bits with the preamble");
        // Clause 1.1's preamble in front: taken at once.
        CHECK(decode_bits(bits).size() == sent->size());

        // The same transmission joined after its preamble, the way clause 2.3
        // describes a receiver that "commences receiving after the preamble
        // has been completed". The first batch waits for the second.
        const std::span<const std::uint8_t> late =
            std::span(bits).subspan(decode::kPocsagPreambleBits);
        decode::PocsagStats stats;
        const auto pages = decode_bits(late, &stats);
        const std::size_t batches = (late.size() / decode::kPocsagCodewordBits) /
                                    (decode::kPocsagCodewordsPerBatch + 1);
        INFO(batches << " batches");
        CHECK((batches == 1) == (sent == &short_page));
        if (batches == 1) {
            CHECK(pages.empty());
            CHECK(stats.batches_unconfirmed == 1);
            CHECK(stats.pages_unconfirmed == sent->size());
        } else {
            CHECK(pages.size() == sent->size());
            CHECK(stats.pages_unconfirmed == 0);
        }
    }
}

TEST_CASE("POCSAG refuses an address codeword corrected past its budget", "[decode][pocsag]") {
    // Clause 1.4's code corrects two bits; an address is taken with one.
    const std::vector<Sent> sent = {{1234567, "Short one."}};
    const std::vector<std::uint8_t> clean = bits_for(sent);
    // The address codeword follows the preamble, the synchronization
    // codeword and the frames before its own, clause 1.2.
    const std::size_t address = decode::kPocsagPreambleBits + decode::kPocsagCodewordBits +
                                decode::kPocsagCodewordBits * 2 * (1234567U & 7U);
    for (const int flipped : {0, 1, 2}) {
        std::vector<std::uint8_t> bits = clean;
        for (int i = 0; i < flipped; ++i) {
            bits[address + 3 + 7 * static_cast<std::size_t>(i)] ^= 1U;
        }
        auto audio = siggen::pocsag_render_audio(siggen::PocsagModConfig{}, bits);
        REQUIRE(audio.has_value());
        decode::PocsagDecoder decoder =
            make_decoder(decode::kPocsag1200, decode::PocsagConfig{}.rate,
                         decode::PocsagConfig{}.address_correction_budget);
        std::vector<decode::PocsagPage> pages;
        decoder.process(*audio, pages);
        decoder.flush(pages);
        INFO(flipped << " bits of the address codeword flipped");
        if (flipped <= decode::PocsagConfig{}.address_correction_budget) {
            REQUIRE(pages.size() == 1);
            CHECK(pages[0].identity == 1234567U);
            CHECK(pages[0].corrected_bits == flipped);
        } else {
            CHECK(pages.empty());
            CHECK(decoder.stats().addresses_refused == 1);
        }
    }
}

TEST_CASE("POCSAG reports no pages from noise alone", "[decode][pocsag]") {
    // Receiver audio with no signal in the channel: the FM discriminator of
    // complex noise. 16000 S/s rather than the 48000 the other cases use,
    // because what matters is the bits the decoder's slicer makes of it and
    // an hour of noise is 57.6 million samples at this rate. The channel is
    // the whole 16 kHz, a little wider than the 14 kHz the other cases
    // filter to.
    //
    // Measured 2026-09-23 over 40 hours, seed below: with the rules as they
    // were, 38 pages at 512 bit/s, 75 at 1200 and 211 at 2400, 8.1 an hour,
    // from 17, 37 and 94 batches synchronised on noise. With them, none.
    constexpr dsp::SampleRate kRate = 16'000;
    constexpr std::uint64_t kSeed = 0x9A6E'0000'0000'0001ULL;
    const double hours = noise_hours(0.5);
    const auto total = static_cast<std::uint64_t>(hours * 3600.0 * static_cast<double>(kRate));

    std::vector<decode::PocsagDecoder> decoders;
    for (const int budget : kBudgets) {
        for (const double bit_rate : kRates) {
            decoders.push_back(make_decoder(bit_rate, kRate, budget));
        }
    }
    std::vector<std::vector<decode::PocsagPage>> pages(decoders.size());

    auto discriminator = decode::FmDiscriminator::create(kRate);
    REQUIRE(discriminator.has_value());
    constexpr std::size_t kChunk = 1U << 20U;
    std::vector<dsp::Complex32> noise(kChunk);
    std::vector<float> audio(kChunk);
    std::uint64_t done = 0;
    for (std::uint64_t chunk = 0; done < total; ++chunk) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, total - done));
        noise.assign(count, dsp::Complex32{});
        audio.resize(count);
        REQUIRE(siggen::add_awgn_at_power(noise, 1.0, siggen::derive_seed(kSeed, chunk))
                    .has_value());
        REQUIRE(discriminator->process(noise, audio).has_value());
        for (std::size_t d = 0; d < decoders.size(); ++d) {
            decoders[d].process(audio, pages[d]);
        }
        done += count;
    }

    for (std::size_t b = 0; b < kBudgets.size(); ++b) {
        std::size_t all_pages = 0;
        for (std::size_t r = 0; r < kRates.size(); ++r) {
            const std::size_t d = b * kRates.size() + r;
            decoders[d].flush(pages[d]);
            const decode::PocsagStats& stats = decoders[d].stats();
            std::println(
                "test_pocsag_false_pages noise {:.2f} h, budget {}, {} bit/s: {} pages; {} "
                "batches synchronised, {} of them unconfirmed and {} pages dropped with them; {} "
                "addresses refused",
                hours, kBudgets[b], kRates[r], pages[d].size(), stats.batches,
                stats.batches_unconfirmed, stats.pages_unconfirmed, stats.addresses_refused);
            for (const decode::PocsagPage& page : pages[d]) {
                std::println("test_pocsag_false_pages   {}", describe(page));
            }
            all_pages += pages[d].size();
        }
        std::println("test_pocsag_false_pages noise, budget {}: {:.2f} pages per hour", kBudgets[b],
                     static_cast<double>(all_pages) / hours);
        CHECK(all_pages == 0);
    }
}

TEST_CASE("POCSAG pages at low signal to noise are the pages sent", "[decode][pocsag]") {
    // The FM receiver path of test_pocsag.cpp's measured case: the RF signal
    // with noise added across the whole capture, a 7 kHz channel filter and
    // the discriminator, with two seconds of noise either side of the
    // transmission, as a receiver has. Every rate's decoder reads the same
    // audio, as the wire adapter's do. Six transmissions of 40 alphanumeric
    // pages of 40 characters at 1200 bit/s per point.
    constexpr dsp::SampleRate kRate = 48'000;
    constexpr std::size_t kPagesPerRun = 40;
    constexpr std::size_t kRuns = 6;
    const double points[] = {12.0, 8.0, 6.0, 5.0, 4.0};

    auto taps = decode::design_from_response(kRate, 127, channel_response, nullptr);
    REQUIRE(taps.has_value());

    for (const double snr : points) {
        struct Tally {
            std::size_t good = 0;
            std::size_t false_at_rate = 0;
            std::size_t false_clean = 0;
            std::size_t false_other_rates = 0;
            std::size_t refused = 0;
            std::size_t unconfirmed = 0;
        };
        std::array<Tally, kBudgets.size()> tallies{};
        std::size_t sent_total = 0;

        for (std::size_t run = 0; run < kRuns; ++run) {
            const std::vector<Sent> sent = pages_for(0x9A6E'0000'0000'0100ULL + run, kPagesPerRun);
            sent_total += sent.size();
            siggen::PocsagModConfig mod;
            mod.rate = kRate;
            auto rf = siggen::pocsag_render_baseband(mod, bits_for(sent));
            REQUIRE(rf.has_value());

            const std::size_t quiet = 2 * static_cast<std::size_t>(kRate);
            std::vector<dsp::Complex32> all(quiet, dsp::Complex32{});
            all.insert(all.end(), rf->begin(), rf->end());
            all.insert(all.end(), quiet, dsp::Complex32{});
            // The carrier has unit power, so this is the noise that puts it
            // `snr` above the noise in 2500 Hz.
            const double noise_power =
                std::pow(10.0, -snr / 10.0) * static_cast<double>(kRate) / 2500.0;
            REQUIRE(siggen::add_awgn_at_power(all, noise_power, 0x5CA7ULL + run).has_value());

            std::vector<dsp::Complex32> filtered(all.size());
            REQUIRE(decode::filter_complex(all, *taps, filtered).has_value());
            std::vector<float> audio(all.size());
            REQUIRE(decode::fm_discriminate(filtered, audio, kRate).has_value());

            std::set<std::uint32_t> identities;
            for (const Sent& s : sent) {
                identities.insert(s.identity);
            }
            for (std::size_t b = 0; b < kBudgets.size(); ++b) {
                Tally& tally = tallies[b];
                for (const double bit_rate : kRates) {
                    decode::PocsagDecoder decoder = make_decoder(bit_rate, kRate, kBudgets[b]);
                    std::vector<decode::PocsagPage> got;
                    decoder.process(audio, got);
                    decoder.flush(got);
                    const bool at_rate = bit_rate == decode::kPocsag1200;
                    if (at_rate) {
                        tally.refused += decoder.stats().addresses_refused;
                        tally.unconfirmed += decoder.stats().pages_unconfirmed;
                    }
                    for (const decode::PocsagPage& page : got) {
                        if (identities.contains(page.identity) &&
                            page.function == decode::kPocsagFunctionAlphanumeric) {
                            continue;
                        }
                        if (!at_rate) {
                            ++tally.false_other_rates;
                            continue;
                        }
                        ++tally.false_at_rate;
                        tally.false_clean += page.uncorrectable_codewords == 0 ? 1U : 0U;
                        std::println("test_pocsag_false_pages {} dB run {} budget {} false: {}",
                                     snr, run, kBudgets[b], describe(page));
                    }
                    if (!at_rate) {
                        continue;
                    }
                    for (const Sent& s : sent) {
                        for (const decode::PocsagPage& page : got) {
                            if (page.identity == s.identity && page.text == s.text) {
                                ++tally.good;
                                break;
                            }
                        }
                    }
                }
            }
        }
        for (std::size_t b = 0; b < kBudgets.size(); ++b) {
            const Tally& tally = tallies[b];
            std::println(
                "test_pocsag_false_pages {} dB/2500 Hz, budget {}: {} of {} pages whole; at "
                "1200 bit/s {} false pages ({:.4f} per page sent), {} of them with no "
                "uncorrectable codeword, {} addresses refused, {} unconfirmed pages dropped; {} "
                "false at 512 and 2400",
                snr, kBudgets[b], tally.good, sent_total, tally.false_at_rate,
                static_cast<double>(tally.false_at_rate) / static_cast<double>(sent_total),
                tally.false_clean, tally.refused, tally.unconfirmed, tally.false_other_rates);
        }
        const Tally& now = tallies.back();
        CHECK(now.false_other_rates == 0);
        if (snr >= 8.0) {
            CHECK(now.false_at_rate == 0);
        } else {
            // What is left below 8 dB is a word with five or more errors
            // landing within one bit of another code word, which no budget
            // short of refusing every correction can tell from a real one;
            // the header of this file has the figures.
            CHECK(static_cast<double>(now.false_at_rate) <= 0.05 * static_cast<double>(sent_total));
        }
        if (snr >= 12.0) {
            CHECK(now.good == sent_total);
        }
    }
}
