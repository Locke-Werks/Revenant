// Floating-point discipline first, for the reason sweep.cpp gives.
#include "core/dsp/reference_fp.h"

#include "tools/bench/cw_engine.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <print>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/dsp/synth/channel.h"
#include "core/dsp/synth/cw_mod.h"
#include "core/engine/engine.h"
#include "core/rpc/decoders.h"
#include "tools/bench/mode_support.h"

namespace revenant::bench {

static_assert(dsp::kReferenceFpDisciplineApplied,
              "tools/bench/cw_engine.cpp must include core/dsp/reference_fp.h first");

namespace {

// 96 kS/s, the KF4FIC recordings' rate, over 16 channels: 6 kHz apart at
// 12 kS/s each, so a sideband receiver's 2.4 kHz filter a pitch off the
// carrier fits one channel whole. 16 is what the engine chooses for those
// recordings when a centre is given, docs/recordings.md.
constexpr dsp::SampleRate kFileRate = 96'000;
constexpr std::uint32_t kChannels = 16;
constexpr dsp::Hertz kCentreHz = 14'000'000;

// The carrier sits 1500 Hz above channel 0's centre, so every receiver has a
// residual for its fine stage to mix out and none straddles two channels.
constexpr dsp::Hertz kCarrierOffsetHz = 1'500;

constexpr dsp::SampleRate kAudioRate = 48'000;

// A cw receiver's sidetone, VrxParams' default.
constexpr dsp::Hertz kCwPitchHz = 700;

// cw-wide's filter, signed hertz from the receiver's centre. The tone sits at
// 700 + (carrier - centre), so 300..1200 Hz of tone is -400..+500 Hz of
// carrier offset; 100 Hz more either side keeps each pitch clear of an edge.
constexpr dsp::Hertz kWideLowHz = -500;
constexpr dsp::Hertz kWideHighHz = 600;

// Noise before the keyer's own half second of lead-in and after its one
// second of tail, so every receiver hears two seconds of band before the
// first element and three and a half after the last.
constexpr double kExtraLeadSeconds = 1.5;
constexpr double kExtraTailSeconds = 2.5;

// A labelled line is scored against this pitch when it is within this many
// hertz of where the tone landed.
constexpr double kNearHz = 60.0;

constexpr std::string_view kPool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

std::string random_text(std::uint64_t characters, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> pick(0, kPool.size() - 1);
    std::uniform_int_distribution<int> word(2, 6);
    std::string text;
    int left = word(rng);
    while (text.size() < characters) {
        if (left == 0) {
            text.push_back(' ');
            left = word(rng);
            continue;
        }
        text.push_back(kPool[pick(rng)]);
        --left;
    }
    while (!text.empty() && text.back() == ' ') {
        text.pop_back();
    }
    return text;
}

struct Tuning {
    engine::Demod demod = engine::Demod::Usb;
    dsp::Hertz offset_from_carrier = 0;
    dsp::Hertz low = 0;
    dsp::Hertz high = 0;
};

Expected<Tuning> tuning_for(const std::string& receiver, std::int64_t pitch) {
    Tuning out;
    if (receiver == "usb") {
        out.demod = engine::Demod::Usb;
        out.offset_from_carrier = -pitch;
    } else if (receiver == "lsb") {
        out.demod = engine::Demod::Lsb;
        out.offset_from_carrier = pitch;
    } else if (receiver == "cw" || receiver == "cw-wide") {
        out.demod = engine::Demod::Cw;
        out.offset_from_carrier = kCwPitchHz - pitch;
        if (receiver == "cw-wide") {
            out.low = kWideLowHz;
            out.high = kWideHighHz;
        }
    } else {
        return fail(std::format("'{}' is not a receiver this grid knows: cw, cw-wide, usb, lsb",
                                receiver));
    }
    return out;
}

struct Chunk {
    dsp::SampleIndex start = 0;
    std::vector<float> samples;
};

struct Received {
    std::size_t cell = 0;
    std::string mode;
    std::vector<Chunk> chunks;
};

// The transmission as IQ at kFileRate with the carrier at kCarrierOffsetHz,
// noise over the whole of it.
Expected<std::vector<dsp::Complex32>> render_file(const std::string& text, double wpm,
                                                  double snr_db, double jitter,
                                                  std::uint64_t seed) {
    siggen::CwModConfig mod;
    mod.rate = kFileRate;
    mod.tone_hz = kCarrierOffsetHz;
    mod.wpm = wpm;
    mod.jitter = jitter;
    mod.seed = siggen::derive_seed(seed, 1);
    auto keyed = siggen::cw_render_analytic(mod, text);
    if (!keyed) {
        return std::unexpected(keyed.error());
    }
    const double power = siggen::mean_power(*keyed);
    auto noise = siggen::awgn_power_for(siggen::NoiseLevel::snr_in_2500_hz_db(snr_db), power,
                                        kFileRate);
    if (!noise) {
        return std::unexpected(noise.error());
    }
    const auto lead = static_cast<std::size_t>(kExtraLeadSeconds * kFileRate);
    const auto tail = static_cast<std::size_t>(kExtraTailSeconds * kFileRate);
    std::vector<dsp::Complex32> all(lead, dsp::Complex32{});
    all.insert(all.end(), keyed->begin(), keyed->end());
    all.insert(all.end(), tail, dsp::Complex32{});
    if (auto added = siggen::add_awgn_at_power(all, *noise, siggen::derive_seed(seed, 2));
        !added) {
        return std::unexpected(added.error());
    }
    // Headroom: the file is float, but the engine's converters and meters
    // read full scale as 1.
    for (dsp::Complex32& x : all) {
        x *= 0.1F;
    }
    return all;
}

Status write_cf32(const std::filesystem::path& path, std::span<const dsp::Complex32> samples) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(std::format("cannot write '{}'", path.string()));
    }
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(dsp::Complex32)));
    if (!out) {
        return fail(std::format("short write to '{}'", path.string()));
    }
    return {};
}

struct Scored {
    std::string near_text;
    std::uint64_t elsewhere = 0;
    double wpm_sum = 0.0;
    std::uint64_t wpm_lines = 0;
};

const rpc::DecodedField* field(const rpc::DecodedMessage& message, std::string_view key) {
    for (const rpc::DecodedField& f : message.fields) {
        if (f.key == key) {
            return &f;
        }
    }
    return nullptr;
}

Expected<Scored> decode_and_score(const Received& received, std::int64_t pitch) {
    const rpc::DecoderSpec* spec = rpc::find_decoder("cw");
    if (spec == nullptr) {
        return fail("the decoder registry has no \"cw\"");
    }
    auto made = spec->make(rpc::DecoderBuild{kAudioRate, received.mode});
    if (!made) {
        return std::unexpected(made.error());
    }
    rpc::ChunkDecoder& decoder = **made;
    std::vector<rpc::DecodedMessage> messages;
    for (const Chunk& chunk : received.chunks) {
        rpc::DecoderChunk in;
        in.samples = chunk.samples;
        in.channels = 1;
        in.rate = kAudioRate;
        in.start = chunk.start;
        if (auto consumed = decoder.consume(in, messages); !consumed) {
            return std::unexpected(consumed.error());
        }
    }
    decoder.flush(messages);

    struct Line {
        std::int64_t began = 0;
        std::string text;
    };
    std::vector<Line> near;
    Scored out;
    for (const rpc::DecodedMessage& message : messages) {
        bool is_near = true;
        if (const rpc::DecodedField* heard = field(message, "pitch_hz");
            heard != nullptr && heard->real() != nullptr) {
            is_near = std::abs(*heard->real() - static_cast<double>(pitch)) <= kNearHz;
        }
        if (!is_near) {
            out.elsewhere += message.text.size();
            continue;
        }
        std::int64_t began = 0;
        if (const rpc::DecodedField* at = field(message, "began_sample");
            at != nullptr && at->integer() != nullptr) {
            began = *at->integer();
        }
        near.push_back(Line{began, message.text});
        if (const rpc::DecodedField* speed = field(message, "wpm");
            speed != nullptr && speed->real() != nullptr && *speed->real() > 0.0) {
            out.wpm_sum += *speed->real();
            ++out.wpm_lines;
        }
    }
    std::stable_sort(near.begin(), near.end(),
                     [](const Line& a, const Line& b) { return a.began < b.began; });
    for (const Line& line : near) {
        if (!out.near_text.empty()) {
            out.near_text.push_back(' ');
        }
        out.near_text += line.text;
    }
    return out;
}

}  // namespace

Expected<CwEngineReport> run_cw_engine(const CwEngineConfig& config) {
    if (config.trials == 0 || config.characters == 0) {
        return fail("cw-engine needs at least one trial of at least one character");
    }
    const auto started = std::chrono::steady_clock::now();

    CwEngineReport report;
    std::map<std::tuple<std::string, std::int64_t, double, double>, std::size_t> index;
    for (const std::string& receiver : config.receivers) {
        for (const std::int64_t pitch : config.pitch_hz) {
            if (auto ok = tuning_for(receiver, pitch); !ok) {
                return std::unexpected(ok.error());
            }
            for (const double wpm : config.wpm) {
                for (const double snr : config.snr_db) {
                    index[{receiver, pitch, wpm, snr}] = report.cells.size();
                    CwEngineCell cell;
                    cell.receiver = receiver;
                    cell.pitch_hz = pitch;
                    cell.wpm = wpm;
                    cell.snr_db = snr;
                    report.cells.push_back(std::move(cell));
                }
            }
        }
    }

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        std::format("revenant-cw-engine-{}.cf32",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::string uri = std::format("file:///{}?rate={}&format=cf32&center={}",
                                        path.generic_string(), kFileRate, kCentreHz);
    // The trial file goes on every way out, a refusal included: the first
    // run of this left a 24 MB file behind when a receiver was refused.
    struct RemoveOnExit {
        const std::filesystem::path& path;
        ~RemoveOnExit() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } remove_on_exit{path};
    const unsigned threads =
        config.threads != 0 ? config.threads : std::max(1U, std::thread::hardware_concurrency());

    std::uint64_t file = 0;
    for (const double wpm : config.wpm) {
        for (const double snr : config.snr_db) {
            for (std::uint64_t trial = 0; trial < config.trials; ++trial) {
                ++file;
                const std::uint64_t seed = siggen::derive_seed(config.seed, file);
                const std::string text = random_text(config.characters, seed);
                auto iq = render_file(text, wpm, snr, config.jitter, seed);
                if (!iq) {
                    return std::unexpected(with_context(iq.error(), "rendering a trial"));
                }
                if (auto written = write_cf32(path, *iq); !written) {
                    return std::unexpected(written.error());
                }
                iq->clear();
                iq->shrink_to_fit();

                engine::EngineConfig engine_config;
                engine_config.gpu_index = config.gpu_index;
                engine_config.channels = kChannels;
                engine_config.audio_rate = kAudioRate;
                engine_config.pace = 0.0;
                auto made = engine::Engine::create(engine_config);
                if (!made) {
                    return std::unexpected(with_context(made.error(), "creating the engine"));
                }
                engine::Engine& eng = **made;
                if (auto opened = eng.open_source(uri); !opened) {
                    return std::unexpected(with_context(opened.error(), "opening the trial"));
                }
                report.device = eng.info().device.name;

                std::mutex lock;
                std::vector<Received> received;
                std::vector<std::int64_t> pitch_of;
                std::vector<engine::VrxId> ids;
                for (const std::string& receiver : config.receivers) {
                    for (const std::int64_t pitch : config.pitch_hz) {
                        const Tuning tuning = *tuning_for(receiver, pitch);
                        engine::VrxParams params;
                        params.center = kCarrierOffsetHz + tuning.offset_from_carrier;
                        params.demod = tuning.demod;
                        params.bandwidth = 0;
                        params.passband_low = tuning.low;
                        params.passband_high = tuning.high;
                        params.cw_pitch = kCwPitchHz;
                        params.audio_rate = kAudioRate;
                        auto added = eng.add_vrx(params);
                        if (!added) {
                            return std::unexpected(with_context(
                                added.error(),
                                std::format("adding the {} receiver for {} Hz", receiver, pitch)));
                        }
                        ids.push_back(*added);
                        Received r;
                        r.cell = index[{receiver, pitch, wpm, snr}];
                        r.mode = engine::demod_name(tuning.demod);
                        received.push_back(std::move(r));
                        pitch_of.push_back(pitch);
                    }
                }
                for (std::size_t k = 0; k < ids.size(); ++k) {
                    auto attached = eng.attach_audio_sink(
                        ids[k], [&lock, &received, k](const engine::AudioChunk& chunk) -> Status {
                            const std::lock_guard<std::mutex> guard(lock);
                            received[k].chunks.push_back(
                                Chunk{chunk.start, std::vector<float>(chunk.samples.begin(),
                                                                      chunk.samples.end())});
                            return {};
                        });
                    if (!attached) {
                        return std::unexpected(attached.error());
                    }
                }
                if (auto ran = eng.run(); !ran) {
                    return std::unexpected(with_context(ran.error(), "running the trial"));
                }
                made->reset();

                // The decoders run on the host, one receiver to a thread.
                std::vector<Expected<Scored>> scored(received.size(), Scored{});
                std::vector<std::thread> pool;
                std::atomic<std::size_t> next{0};
                for (unsigned t = 0; t < std::min<std::size_t>(threads, received.size()); ++t) {
                    pool.emplace_back([&] {
                        for (std::size_t k = next.fetch_add(1); k < received.size();
                             k = next.fetch_add(1)) {
                            scored[k] = decode_and_score(received[k], pitch_of[k]);
                        }
                    });
                }
                for (std::thread& worker : pool) {
                    worker.join();
                }
                for (std::size_t k = 0; k < received.size(); ++k) {
                    if (!scored[k]) {
                        return std::unexpected(with_context(scored[k].error(), "decoding"));
                    }
                    CwEngineCell& cell = report.cells[received[k].cell];
                    const std::size_t distance =
                        std::min(detail::edit_distance(text, scored[k]->near_text), text.size());
                    cell.sent += text.size();
                    cell.errors += distance;
                    cell.elsewhere += scored[k]->elsewhere;
                    cell.wpm_sum += scored[k]->wpm_sum;
                    cell.wpm_lines += scored[k]->wpm_lines;
                    if (trial == 0) {
                        cell.first_sent = text;
                        cell.first_got = scored[k]->near_text;
                    }
                }
            }
        }
    }
    report.wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return report;
}

namespace {

struct FileReceiver {
    std::string spec;
    std::string mode;
    std::unique_ptr<rpc::ChunkDecoder> decoder;
    std::vector<rpc::DecodedMessage> messages;
};

void print_lines(std::size_t number, FileReceiver& receiver) {
    for (const rpc::DecodedMessage& message : receiver.messages) {
        const auto real = [&](std::string_view key) {
            const rpc::DecodedField* f = field(message, key);
            return f != nullptr && f->real() != nullptr ? *f->real() : 0.0;
        };
        const rpc::DecodedField* stream = field(message, "stream");
        const double seconds = message.sample_rate == 0
                                   ? 0.0
                                   : static_cast<double>(message.end_sample) /
                                         static_cast<double>(message.sample_rate);
        std::println("{:8.2f}s  vrx {:2}  stream {:3}  {:7.1f} Hz  {:4.1f} WPM  {}", seconds, number,
                     stream != nullptr && stream->integer() != nullptr ? *stream->integer() : 0,
                     real("pitch_hz"), real("wpm"), message.text);
    }
    receiver.messages.clear();
}

}  // namespace

Status run_cw_file(const CwFileConfig& config) {
    engine::EngineConfig engine_config;
    engine_config.gpu_index = config.gpu_index;
    engine_config.channels = config.channels;
    engine_config.audio_rate = kAudioRate;
    engine_config.pace = 0.0;
    auto made = engine::Engine::create(engine_config);
    if (!made) {
        return std::unexpected(with_context(made.error(), "creating the engine"));
    }
    engine::Engine& eng = **made;
    if (auto opened = eng.open_source(config.uri); !opened) {
        return std::unexpected(with_context(opened.error(), std::format("opening '{}'", config.uri)));
    }

    std::vector<std::unique_ptr<FileReceiver>> receivers;
    std::vector<engine::VrxId> ids;
    for (const std::string& spec : config.receivers) {
        std::vector<std::string> parts;
        std::string_view rest = spec;
        while (!rest.empty()) {
            const std::size_t colon = rest.find(':');
            parts.emplace_back(rest.substr(0, colon));
            rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        }
        if (parts.size() != 2 && parts.size() != 4) {
            return fail(std::format("'{}' is not offset:mode or offset:mode:low:high", spec));
        }
        const auto number = [&](const std::string& text) -> Expected<std::int64_t> {
            std::string_view digits = text;
            if (digits.starts_with('+')) {
                digits.remove_prefix(1);
            }
            std::int64_t value = 0;
            const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value);
            if (digits.empty() || result.ec != std::errc{} ||
                result.ptr != digits.data() + digits.size()) {
                return fail(std::format("'{}' in '{}' is not a whole number of hertz", text, spec));
            }
            return value;
        };
        auto offset = number(parts[0]);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        engine::VrxParams params;
        params.center = *offset;
        params.bandwidth = 0;
        params.audio_rate = kAudioRate;
        params.cw_pitch = kCwPitchHz;
        if (parts[1] == "cw") {
            params.demod = engine::Demod::Cw;
        } else if (parts[1] == "usb") {
            params.demod = engine::Demod::Usb;
        } else if (parts[1] == "lsb") {
            params.demod = engine::Demod::Lsb;
        } else {
            return fail(std::format("'{}' in '{}' is not cw, usb or lsb", parts[1], spec));
        }
        if (parts.size() == 4) {
            auto low = number(parts[2]);
            auto high = number(parts[3]);
            if (!low || !high) {
                return fail(std::format("'{}' has an edge that is not a number", spec));
            }
            params.passband_low = *low;
            params.passband_high = *high;
        }
        auto added = eng.add_vrx(params);
        if (!added) {
            return std::unexpected(with_context(added.error(), std::format("adding '{}'", spec)));
        }
        ids.push_back(*added);
        auto receiver = std::make_unique<FileReceiver>();
        receiver->spec = spec;
        receiver->mode = parts[1];
        receivers.push_back(std::move(receiver));
    }

    std::mutex lock;
    std::atomic<bool> past{false};
    std::string fault;
    for (std::size_t k = 0; k < ids.size(); ++k) {
        FileReceiver* receiver = receivers[k].get();
        auto attached = eng.attach_audio_sink(
            ids[k], [&, receiver, k](const engine::AudioChunk& chunk) -> Status {
                const std::lock_guard<std::mutex> guard(lock);
                if (config.seconds > 0.0 &&
                    static_cast<double>(chunk.start) / static_cast<double>(chunk.rate) > config.seconds) {
                    past.store(true);
                    return {};
                }
                if (!receiver->decoder) {
                    auto built = rpc::find_decoder("cw")->make(rpc::DecoderBuild{chunk.rate, receiver->mode});
                    if (!built) {
                        fault = built.error().message;
                        return {};
                    }
                    receiver->decoder = std::move(*built);
                }
                rpc::DecoderChunk in;
                in.samples = chunk.samples;
                in.channels = chunk.channels;
                in.rate = chunk.rate;
                in.start = chunk.start;
                if (auto consumed = receiver->decoder->consume(in, receiver->messages); !consumed) {
                    fault = consumed.error().message;
                }
                print_lines(k + 1, *receiver);
                return {};
            });
        if (!attached) {
            return std::unexpected(attached.error());
        }
    }
    for (std::size_t k = 0; k < receivers.size(); ++k) {
        std::println("vrx {:2}  {}", k + 1, receivers[k]->spec);
    }

    std::atomic<bool> finished{false};
    std::thread watcher([&] {
        while (!finished.load()) {
            if (past.load()) {
                static_cast<void>(eng.stop());
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    Status ran = eng.run();
    finished.store(true);
    watcher.join();
    if (!ran) {
        return std::unexpected(with_context(ran.error(), "the run"));
    }
    made->reset();
    for (std::size_t k = 0; k < receivers.size(); ++k) {
        if (receivers[k]->decoder) {
            receivers[k]->decoder->flush(receivers[k]->messages);
            print_lines(k + 1, *receivers[k]);
        }
    }
    if (!fault.empty()) {
        return fail(fault);
    }
    return {};
}

std::string cw_engine_tables(const CwEngineConfig& config, const CwEngineReport& report) {
    std::string out;
    for (const std::string& receiver : config.receivers) {
        for (const double wpm : config.wpm) {
            out += std::format("\n{} receiver, {:g} WPM: character error rate\n\n", receiver, wpm);
            out += "| pitch |";
            for (const double snr : config.snr_db) {
                out += std::format(" {:g} dB |", snr);
            }
            out += "\n| --- |";
            for (std::size_t i = 0; i < config.snr_db.size(); ++i) {
                out += " --- |";
            }
            out += "\n";
            for (const std::int64_t pitch : config.pitch_hz) {
                out += std::format("| {} Hz |", pitch);
                for (const double snr : config.snr_db) {
                    for (const CwEngineCell& cell : report.cells) {
                        if (cell.receiver == receiver && cell.pitch_hz == pitch &&
                            cell.wpm == wpm && cell.snr_db == snr) {
                            out += std::format(" {:.3f} |", cell.error_rate());
                        }
                    }
                }
                out += "\n";
            }
        }
    }
    return out;
}

std::string cw_engine_json(const CwEngineReport& report) {
    const auto escape = [](std::string_view text) {
        std::string s;
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                s.push_back('\\');
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                s += std::format("\\u{:04x}", static_cast<unsigned>(c));
                continue;
            }
            s.push_back(c);
        }
        return s;
    };
    std::string out = std::format("{{\n  \"device\": \"{}\",\n  \"wall_seconds\": {:.1f},\n  \"cells\": [\n",
                                  escape(report.device), report.wall_seconds);
    for (std::size_t i = 0; i < report.cells.size(); ++i) {
        const CwEngineCell& c = report.cells[i];
        out += std::format(
            "    {{\"receiver\": \"{}\", \"pitch_hz\": {}, \"wpm\": {:g}, \"snr_db\": {:g}, "
            "\"sent\": {}, \"errors\": {}, \"cer\": {:.4f}, \"elsewhere\": {}, \"mean_wpm\": {:.2f}, "
            "\"first_sent\": \"{}\", \"first_got\": \"{}\"}}{}\n",
            c.receiver, c.pitch_hz, c.wpm, c.snr_db, c.sent, c.errors, c.error_rate(), c.elsewhere,
            c.wpm_lines == 0 ? 0.0 : c.wpm_sum / static_cast<double>(c.wpm_lines),
            escape(c.first_sent), escape(c.first_got), i + 1 == report.cells.size() ? "" : ",");
    }
    out += "  ]\n}\n";
    return out;
}

}  // namespace revenant::bench
