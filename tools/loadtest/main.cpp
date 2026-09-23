// revenant-loadtest: load an engine the way a live session does and report
// where the host's time went.
//
// WHY IT EXISTS
//
// After the playtest of 2026-09-23 the owner reported that decoding P25 off
// the dongle with signal identification busy made the engine "chug pretty
// hard" and IMBE decoding troublesome, and that the display and sigID were to
// be kept from contending with decoding. That report names a symptom and
// three suspects. This names the thread.
//
// WHAT IT RUNS, IN ONE PROCESS
//
// An engine configured as revenant-engine configures itself, a server on an
// ephemeral loopback port with a token of its own, and a client connected to
// it that does what revenant-ui does: subscribes to the spectrum, polls the
// detections four times a second, adds a P25 receiver, subscribes to its
// voice and to its p25p1 decoder. Nothing here touches another engine, a
// client's settings, a dongle or a sound card.
//
// WHAT IT COUNTS, over a window after a warm-up:
//
//   P25     LDUs the p25p1 decoder verified, and nine IMBE frames each. A
//           reference run (the same file unthrottled, --reference) gives the
//           LDUs the file offers per source second.
//   voice   the P25 receiver's voice stream as the client received it:
//           index gaps, and a playout at 8000 S/s held to wall time that
//           counts how often and for how long it would have run dry.
//   display spectrum rows received per wall second.
//   sigID   detector frames, decisions, tracks, labelled tracks, probes.
//   threads CPU per thread, by the names core/thread_role.h gives them.
//   engine  EngineLoad and ServerLoad deltas: time on the completion thread
//           in each kind of sink, time it waited on the GPU, time the
//           recording thread waited for a frame slot, and what a Paced
//           source lost.
//
// Run a file with flow=paced&pace=1 to make it behave as a radio, which loses
// what it cannot deliver, rather than as a file, which waits.

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/server.h"
#include "core/rpc/token.h"

using namespace revenant;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::string uri;
    std::uint32_t channels = 0;
    std::uint32_t taps = 17;
    std::size_t block = 65'536;
    std::uint32_t spectrum_points = 2048;
    std::uint32_t passband_points = 512;
    std::uint32_t probes = 4;
    double ring_seconds = 8.0;
    int gpu = -1;

    std::optional<std::int64_t> p25;
    std::vector<std::int64_t> nfm;
    bool detect = true;
    std::uint32_t every_nth = 1;
    std::uint32_t audio_ms = 200;

    double warmup = 5.0;
    double seconds = 30.0;
    std::string label = "run";
    bool reference = false;

    // Threads that do nothing but arithmetic at normal priority, standing in
    // for whatever else the machine is doing: a build, a test run, another
    // program. They are this process's own and named, so the report leaves
    // them out of the engine's figures.
    std::uint32_t burn = 0;

    // Confine this whole process to the top N logical processors, zero for no
    // confinement. What makes --burn usable on a machine somebody else is
    // using: the oversubscription stays on those N and the rest of the
    // machine is left alone.
    std::uint32_t cpus = 0;

    bool passband = false;
};

void print_usage()
{
    std::print(
        "revenant-loadtest: load an engine as a live session does and say where the time "
        "went\n"
        "\n"
        "Usage: revenant-loadtest <source-uri> [options]\n"
        "\n"
        "  --channels N          coarse channels, 0 for the engine's choice (0)\n"
        "  --block N             samples per block (65536, revenant-engine's)\n"
        "  --spectrum-points N   points per channel in the span transform (2048)\n"
        "  --probes N            probe receivers (4)\n"
        "  --p25 HZ              a p25p1 receiver this far from baseband DC, with its voice\n"
        "                        and its p25p1 decoder subscribed\n"
        "  --nfm HZ[,HZ...]      nfm receivers with audio subscribed, for extra load\n"
        "  --no-detect           do not poll detections, so no detector runs\n"
        "  --every-nth N         spectrum subscription stride (1)\n"
        "  --warmup S            seconds before the window opens (5)\n"
        "  --seconds S           the window (30)\n"
        "  --label TEXT          printed on every line of the report\n"
        "  --reference           run until the source ends and report the P25 LDUs per\n"
        "                        source second, which is what a window is scored against\n"
        "  --burn N              N threads of arithmetic at normal priority, standing in for\n"
        "                        a busy machine (0)\n"
        "  --passband            subscribe the P25 receiver's passband, as the fine-tuning\n"
        "                        display does\n"
        "  --cpus N              confine this process to the top N logical processors, so\n"
        "                        --burn loads those and leaves the rest of the machine alone\n"
        "  --gpu N               device index\n");
}

[[nodiscard]] Expected<std::int64_t> integer_of(std::string_view text)
{
    std::int64_t value = 0;
    const auto* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return fail(std::format("'{}' is not an integer", text));
    }
    return value;
}

[[nodiscard]] Expected<double> real_of(std::string_view text)
{
    try {
        std::size_t used = 0;
        const double value = std::stod(std::string(text), &used);
        if (used != text.size()) {
            return fail(std::format("'{}' is not a number", text));
        }
        return value;
    } catch (const std::exception&) {
        return fail(std::format("'{}' is not a number", text));
    }
}

[[nodiscard]] Expected<Options> parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view token = argv[i];
        const auto value = [&]() -> Expected<std::string_view> {
            if (i + 1 >= argc) {
                return fail(std::format("{} needs a value", token));
            }
            return std::string_view(argv[++i]);
        };
        const auto as_integer = [&]() -> Expected<std::int64_t> {
            auto text = value();
            if (!text) {
                return std::unexpected(text.error());
            }
            return integer_of(*text);
        };
        const auto as_real = [&]() -> Expected<double> {
            auto text = value();
            if (!text) {
                return std::unexpected(text.error());
            }
            return real_of(*text);
        };

        if (token == "--channels") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.channels = static_cast<std::uint32_t>(*got);
        } else if (token == "--block") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.block = static_cast<std::size_t>(*got);
        } else if (token == "--spectrum-points") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.spectrum_points = static_cast<std::uint32_t>(*got);
        } else if (token == "--probes") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.probes = static_cast<std::uint32_t>(*got);
        } else if (token == "--p25") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.p25 = *got;
        } else if (token == "--nfm") {
            auto text = value();
            if (!text) { return std::unexpected(text.error()); }
            std::string_view rest = *text;
            while (!rest.empty()) {
                const auto comma = rest.find(',');
                auto got = integer_of(rest.substr(0, comma));
                if (!got) { return std::unexpected(got.error()); }
                options.nfm.push_back(*got);
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
        } else if (token == "--no-detect") {
            options.detect = false;
        } else if (token == "--every-nth") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.every_nth = static_cast<std::uint32_t>(*got);
        } else if (token == "--warmup") {
            auto got = as_real();
            if (!got) { return std::unexpected(got.error()); }
            options.warmup = *got;
        } else if (token == "--seconds") {
            auto got = as_real();
            if (!got) { return std::unexpected(got.error()); }
            options.seconds = *got;
        } else if (token == "--label") {
            auto text = value();
            if (!text) { return std::unexpected(text.error()); }
            options.label = std::string(*text);
        } else if (token == "--reference") {
            options.reference = true;
        } else if (token == "--burn") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.burn = static_cast<std::uint32_t>(*got);
        } else if (token == "--passband") {
            options.passband = true;
        } else if (token == "--cpus") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.cpus = static_cast<std::uint32_t>(*got);
        } else if (token == "--gpu") {
            auto got = as_integer();
            if (!got) { return std::unexpected(got.error()); }
            options.gpu = static_cast<int>(*got);
        } else if (token.starts_with("--")) {
            return fail(std::format("unknown option {}", token));
        } else if (options.uri.empty()) {
            options.uri = std::string(token);
        } else {
            return fail(std::format("a second source URI '{}'", token));
        }
    }
    if (options.uri.empty()) {
        return fail("no source URI");
    }
    return options;
}

// ---------------------------------------------------------------------------
// Per-thread CPU
// ---------------------------------------------------------------------------

struct ThreadSample {
    std::string name;
    std::uint64_t cpu_100ns = 0;
};

[[nodiscard]] std::string narrow(const wchar_t* text)
{
    std::string out;
    for (const wchar_t* c = text; c != nullptr && *c != L'\0'; ++c) {
        out.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    return out;
}

// Every thread in this process, by id, with its name and its user plus kernel
// time so far.
[[nodiscard]] std::map<std::uint32_t, ThreadSample> sample_threads()
{
    std::map<std::uint32_t, ThreadSample> out;
#if defined(_WIN32)
    const DWORD self = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return out;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Thread32First(snapshot, &entry); more != FALSE;
         more = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr) {
            continue;
        }
        ThreadSample sample;
        FILETIME created{};
        FILETIME exited{};
        FILETIME kernel{};
        FILETIME user{};
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user) != FALSE) {
            const auto as64 = [](const FILETIME& time) {
                return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32U) |
                       time.dwLowDateTime;
            };
            sample.cpu_100ns = as64(kernel) + as64(user);
        }
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
            sample.name = narrow(description);
            LocalFree(description);
        }
        CloseHandle(thread);
        out.emplace(static_cast<std::uint32_t>(entry.th32ThreadID), std::move(sample));
    }
    CloseHandle(snapshot);
#endif
    return out;
}

// Groups by name, folding the numbered pool workers into one row and every
// unnamed thread into another, and returns CPU as a percentage of one core
// over the window.
[[nodiscard]] std::vector<std::pair<std::string, double>> cpu_by_name(
    const std::map<std::uint32_t, ThreadSample>& before,
    const std::map<std::uint32_t, ThreadSample>& after, double seconds, std::uint32_t main_id)
{
    std::map<std::string, std::uint64_t> grouped;
    for (const auto& [id, sample] : after) {
        std::uint64_t base = 0;
        if (auto found = before.find(id); found != before.end()) {
            base = found->second.cpu_100ns;
        }
        const std::uint64_t spent = sample.cpu_100ns >= base ? sample.cpu_100ns - base : 0;
        std::string name = sample.name;
        if (id == main_id) {
            name = "loadtest main";
        } else if (name.starts_with("revenant pool ")) {
            name = "revenant pool (all)";
        } else if (name == "loadtest burn") {
            name = "loadtest burn (all, not the engine)";
        } else if (name.empty()) {
            name = "unnamed (client loop, driver, runtime)";
        }
        grouped[name] += spent;
    }
    std::vector<std::pair<std::string, double>> out;
    for (const auto& [name, spent] : grouped) {
        out.emplace_back(name, 100.0 * static_cast<double>(spent) / 1e7 / seconds);
    }
    std::ranges::sort(out, [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

// ---------------------------------------------------------------------------
// What the client sees
// ---------------------------------------------------------------------------

struct VoiceArrival {
    Clock::time_point at;
    std::uint64_t index = 0;
    std::uint64_t frames = 0;
    std::uint64_t dropped_before = 0;
    bool voiced = false;
};

struct Observed {
    std::mutex lock;
    std::vector<Clock::time_point> rows;
    std::vector<std::uint64_t> row_sequences;
    std::vector<VoiceArrival> voice;
    std::map<std::string, std::uint64_t> decoded_kinds;
    std::vector<std::pair<Clock::time_point, std::string>> decoded;
    std::string voice_ended;
    std::string decoded_ended;
};

struct PollResult {
    std::uint32_t total = 0;
    std::uint32_t labelled = 0;
    std::uint64_t decisions = 0;
    double rpc_ms = 0.0;
};

struct VoiceScore {
    std::uint64_t chunks = 0;
    std::uint64_t frames = 0;
    std::uint64_t voiced_frames = 0;
    std::uint64_t gap_events = 0;
    std::uint64_t gap_frames = 0;
    std::uint64_t underruns = 0;
    double starved_ms = 0.0;
    double jitter_p50_ms = 0.0;
    double jitter_p99_ms = 0.0;
    double jitter_max_ms = 0.0;
};

// The voice stream against a listener: a playout at 8000 S/s that starts
// kLeadMs behind the first chunk, holds its playhead when it runs dry and
// resumes at the next arrival, which is what ui/audio's drift trim does on a
// starve. Arrival jitter is each chunk's arrival less the time its first
// sample was due, with the earliest taken as zero.
[[nodiscard]] VoiceScore score_voice(const std::vector<VoiceArrival>& arrivals)
{
    constexpr double kRate = 8000.0;
    constexpr double kLeadMs = 50.0;
    VoiceScore score;
    if (arrivals.empty()) {
        return score;
    }

    std::vector<double> lateness;
    lateness.reserve(arrivals.size());
    const Clock::time_point origin = arrivals.front().at;
    const std::uint64_t first_index = arrivals.front().index;

    std::uint64_t expected = arrivals.front().index;
    double playhead = static_cast<double>(first_index);
    double received_end = static_cast<double>(first_index);
    double clock_s = -kLeadMs / 1000.0;
    bool started = false;

    for (const VoiceArrival& arrival : arrivals) {
        ++score.chunks;
        score.frames += arrival.frames;
        if (arrival.voiced) {
            score.voiced_frames += arrival.frames;
        }
        if (arrival.index != expected || arrival.dropped_before != 0) {
            ++score.gap_events;
            score.gap_frames += arrival.index > expected ? arrival.index - expected : 0;
        }
        expected = arrival.index + arrival.frames;

        const double now_s = std::chrono::duration<double>(arrival.at - origin).count();
        const double due_s = static_cast<double>(arrival.index - first_index) / kRate;
        lateness.push_back(now_s - due_s);

        if (!started) {
            started = true;
            clock_s = now_s;
            playhead = static_cast<double>(arrival.index) - kLeadMs / 1000.0 * kRate;
        } else {
            // The playhead moves on from the last event to this arrival, and
            // stops at what had been received.
            const double would = playhead + (now_s - clock_s) * kRate;
            if (would > received_end) {
                ++score.underruns;
                score.starved_ms += (would - received_end) / kRate * 1000.0;
                playhead = received_end;
            } else {
                playhead = would;
            }
            clock_s = now_s;
        }
        received_end = std::max(received_end, static_cast<double>(expected));
    }

    const double floor = *std::ranges::min_element(lateness);
    for (double& value : lateness) {
        value = (value - floor) * 1000.0;
    }
    std::ranges::sort(lateness);
    const auto at = [&](double fraction) {
        const auto index = static_cast<std::size_t>(fraction * static_cast<double>(lateness.size() - 1));
        return lateness[index];
    };
    score.jitter_p50_ms = at(0.5);
    score.jitter_p99_ms = at(0.99);
    score.jitter_max_ms = lateness.back();
    return score;
}

[[nodiscard]] double ms(std::uint64_t ns) { return static_cast<double>(ns) / 1e6; }

[[nodiscard]] Status run(const Options& options)
{
#if defined(_WIN32)
    // Before anything starts a thread, so every thread inherits it.
    if (options.cpus != 0) {
        DWORD_PTR process_mask = 0;
        DWORD_PTR system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask) == FALSE) {
            return fail("could not read the process affinity");
        }
        DWORD_PTR chosen = 0;
        std::uint32_t taken = 0;
        for (int bit = static_cast<int>(sizeof(DWORD_PTR) * 8) - 1; bit >= 0 && taken < options.cpus;
             --bit) {
            const DWORD_PTR one = DWORD_PTR{1} << static_cast<unsigned>(bit);
            if ((system_mask & one) != 0) {
                chosen |= one;
                ++taken;
            }
        }
        if (taken < options.cpus || SetProcessAffinityMask(GetCurrentProcess(), chosen) == FALSE) {
            return fail(std::format("could not confine this process to {} processors", options.cpus));
        }
        std::println("{} confined to {} logical processors, mask 0x{:X}", options.label, taken,
                     static_cast<std::uint64_t>(chosen));
    }
#endif

    engine::EngineConfig config;
    config.gpu_index = options.gpu;
    config.ring_seconds = options.ring_seconds;
    config.channels = options.channels;
    config.taps_per_branch = options.taps;
    config.audio_rate = 48'000;
    config.block_samples = options.block;
    config.spectrum_transform = options.spectrum_points;
    config.passband_transform = options.passband_points;
    config.probe_receivers = options.spectrum_points != 0 ? options.probes : 0;

    auto created = engine::Engine::create(config);
    if (!created) {
        return std::unexpected(with_context(created.error(), "building the engine"));
    }
    engine::Engine& eng = **created;
    if (auto opened = eng.open_source(options.uri); !opened) {
        return std::unexpected(with_context(opened.error(), "opening the source"));
    }

    const engine::EngineInfo& info = eng.info();
    std::println("{} source rate {} S/s, grid {} channels, block {}, spectrum {} bins, flow {}",
                 options.label, info.source_rate, info.grid.channels, options.block,
                 info.spectrum.bins,
                 eng.source_capabilities().flow == source::FlowControl::Paced ? "paced"
                                                                               : "demand");

    // A token of this run's own, so nothing here reads the operator's.
    rpc::Token token{};
    std::mt19937_64 random(static_cast<std::uint64_t>(Clock::now().time_since_epoch().count()));
    for (auto& byte : token) {
        byte = static_cast<std::uint8_t>(random() & 0xFFU);
    }
    rpc::ServerOptions server_options;
    server_options.token.assign(token.begin(), token.end());
    auto served = rpc::Server::create(eng, server_options);
    if (!served) {
        return std::unexpected(with_context(served.error(), "starting the server"));
    }
    std::unique_ptr<rpc::Server> server = std::move(*served);

    auto connected = rpc::Client::connect("127.0.0.1", server->port(), token);
    if (!connected) {
        return std::unexpected(with_context(connected.error(), "connecting"));
    }
    std::unique_ptr<rpc::Client> client = std::move(*connected);

    auto observed = std::make_shared<Observed>();
    if (!options.reference) {
        if (auto subscribed = client->subscribe_spectrum(
                options.every_nth,
                [observed](const rpc::SpectrumFrame& frame) {
                    const std::scoped_lock held(observed->lock);
                    observed->rows.push_back(Clock::now());
                    observed->row_sequences.push_back(frame.sequence);
                });
            !subscribed) {
            return std::unexpected(with_context(subscribed.error(), "subscribing the spectrum"));
        }
    }

    std::optional<std::uint64_t> p25_vrx;
    if (options.p25.has_value()) {
        rpc::VrxParams params;
        params.center = *options.p25;
        params.bandwidth = 0;
        params.demod = rpc::Demod::P25p1;
        auto vrx = client->add_vrx(params);
        if (!vrx) {
            return std::unexpected(with_context(vrx.error(), "adding the P25 receiver"));
        }
        p25_vrx = *vrx;
        if (options.passband && !options.reference) {
            if (auto watched = client->subscribe_passband(*vrx, 1, [](const rpc::PassbandFrame&) {});
                !watched) {
                return std::unexpected(with_context(watched.error(), "subscribing the passband"));
            }
        }
        if (!options.reference) {
            auto granted = client->subscribe_audio(
                *vrx, options.audio_ms,
                [observed](const rpc::AudioChunk& chunk) {
                    const std::scoped_lock held(observed->lock);
                    observed->voice.push_back(VoiceArrival{
                        Clock::now(), chunk.sample_index,
                        chunk.channel_count == 0 ? 0 : chunk.samples.size() / chunk.channel_count,
                        chunk.frames_dropped_before, chunk.squelch_open});
                },
                [observed](const std::string& why) {
                    const std::scoped_lock held(observed->lock);
                    observed->voice_ended = why;
                });
            if (!granted) {
                return std::unexpected(with_context(granted.error(), "subscribing the voice"));
            }
        }
        auto decoding = client->subscribe_decoded(
            *vrx, "p25p1",
            [observed](const rpc::DecodedMessage& message) {
                const std::scoped_lock held(observed->lock);
                observed->decoded.emplace_back(Clock::now(), message.kind);
            },
            [observed](const std::string& why) {
                const std::scoped_lock held(observed->lock);
                observed->decoded_ended = why;
            });
        if (!decoding) {
            return std::unexpected(with_context(decoding.error(), "subscribing p25p1"));
        }
    }

    for (const std::int64_t offset : options.nfm) {
        rpc::VrxParams params;
        params.center = offset;
        params.demod = rpc::Demod::Nfm;
        auto vrx = client->add_vrx(params);
        if (!vrx) {
            return std::unexpected(with_context(vrx.error(), "adding an nfm receiver"));
        }
        auto granted = client->subscribe_audio(
            *vrx, options.audio_ms, [](const rpc::AudioChunk&) {}, [](const std::string&) {});
        if (!granted) {
            return std::unexpected(with_context(granted.error(), "subscribing nfm audio"));
        }
    }

    // The engine runs on a thread of its own, as revenant-engine's main does.
    Status run_outcome;
    std::thread runner([&] { run_outcome = eng.run(); });

    // The detections poll, at revenant-ui's 250 ms, on a thread of its own as
    // the UI's supervisor makes it.
    std::atomic<bool> polling{options.detect && !options.reference};
    std::mutex poll_lock;
    PollResult last_poll;
    std::vector<double> poll_ms;
    std::thread poller([&] {
        while (polling.load()) {
            const auto began = Clock::now();
            auto listed = client->detections(0.0, 0.0);
            const double took = std::chrono::duration<double, std::milli>(Clock::now() - began).count();
            if (listed) {
                PollResult result;
                result.total = listed->total;
                result.decisions = listed->decisions;
                result.rpc_ms = took;
                for (const rpc::Detection& detection : listed->detections) {
                    if (detection.label.kind != rpc::LabelKind::Unknown) {
                        ++result.labelled;
                    }
                }
                const std::scoped_lock held(poll_lock);
                last_poll = result;
                poll_ms.push_back(took);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    });

    const std::uint32_t main_id =
#if defined(_WIN32)
        static_cast<std::uint32_t>(GetCurrentThreadId());
#else
        0;
#endif

    std::atomic<bool> burning{true};
    std::vector<std::thread> burners;
    for (std::uint32_t i = 0; i < options.burn && !options.reference; ++i) {
        burners.emplace_back([&burning, i] {
#if defined(_WIN32)
            SetThreadDescription(GetCurrentThread(), L"loadtest burn");
#endif
            double x = 1.0 + static_cast<double>(i);
            volatile double kept = 0.0;
            while (burning.load(std::memory_order_relaxed)) {
                for (int k = 0; k < 100'000; ++k) {
                    x = std::sqrt(x * 1.0000001 + 0.5);
                }
                kept = x;
            }
            static_cast<void>(kept);
        });
    }

    const auto finish = [&]() -> Status {
        burning.store(false);
        for (std::thread& burner : burners) {
            burner.join();
        }
        polling.store(false);
        if (poller.joinable()) {
            poller.join();
        }
        static_cast<void>(eng.stop());
        if (runner.joinable()) {
            runner.join();
        }
        client.reset();
        server.reset();
        return {};
    };

    if (options.reference) {
        // Unthrottled to the end: the LDUs a quiet engine gets out of the
        // whole file, per source second. The first wait is for run() to have
        // started at all, which it does on the runner thread.
        while (!eng.running() && eng.source_stats().blocks_delivered == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        while (eng.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const source::SourceStats source = eng.source_stats();
        std::uint64_t ldus = 0;
        {
            const std::scoped_lock held(observed->lock);
            for (const auto& [at, kind] : observed->decoded) {
                ldus += (kind == "ldu1" || kind == "ldu2") ? 1 : 0;
            }
        }
        const double source_seconds =
            static_cast<double>(source.samples_delivered) / static_cast<double>(info.source_rate);
        std::println("{} reference: {} LDUs over {:.3f} source seconds, {:.4f} LDU/s, {:.2f} "
                     "IMBE frames/s",
                     options.label, ldus, source_seconds,
                     static_cast<double>(ldus) / source_seconds,
                     9.0 * static_cast<double>(ldus) / source_seconds);
        return finish();
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(options.warmup));

    const auto t0 = Clock::now();
    const auto threads0 = sample_threads();
    const engine::EngineLoad load0 = eng.load();
    const rpc::ServerLoad serve0 = server->load();
    const engine::ProbeStats probes0 = eng.probe_stats();
    const source::SourceStats source0 = eng.source_stats();

    std::this_thread::sleep_for(std::chrono::duration<double>(options.seconds));

    const auto t1 = Clock::now();
    const auto threads1 = sample_threads();
    const engine::EngineLoad load1 = eng.load();
    const rpc::ServerLoad serve1 = server->load();
    const engine::ProbeStats probes1 = eng.probe_stats();
    const source::SourceStats source1 = eng.source_stats();
    PollResult poll;
    std::vector<double> polls;
    {
        const std::scoped_lock held(poll_lock);
        poll = last_poll;
        polls = poll_ms;
    }

    const double wall = std::chrono::duration<double>(t1 - t0).count();
    const double source_seconds =
        static_cast<double>(source1.samples_delivered - source0.samples_delivered) /
        static_cast<double>(info.source_rate);

    std::uint64_t rows = 0;
    std::uint64_t ldus = 0;
    std::vector<VoiceArrival> voice;
    std::string voice_ended;
    {
        const std::scoped_lock held(observed->lock);
        for (const auto at : observed->rows) {
            rows += (at >= t0 && at < t1) ? 1 : 0;
        }
        for (const auto& [at, kind] : observed->decoded) {
            if (at >= t0 && at < t1 && (kind == "ldu1" || kind == "ldu2")) {
                ++ldus;
            }
        }
        for (const VoiceArrival& arrival : observed->voice) {
            if (arrival.at >= t0 && arrival.at < t1) {
                voice.push_back(arrival);
            }
        }
        voice_ended = observed->voice_ended;
    }
    const VoiceScore scored = score_voice(voice);

    const auto per_second = [&](std::uint64_t ns) { return ms(ns) / wall; };
    const std::string& tag = options.label;

    std::println("{} window {:.2f} s wall, {:.2f} s source, realtime {:.3f}", tag, wall,
                 source_seconds, source_seconds / wall);
    std::println("{} display rows/s {:.2f}", tag, static_cast<double>(rows) / wall);
    std::println("{} engine spectrum frames/s {:.2f}", tag,
                 static_cast<double>(load1.spectrum_frames - load0.spectrum_frames) / wall);
    if (p25_vrx.has_value()) {
        std::println("{} p25 LDUs {} ({:.3f}/source s), IMBE frames {}", tag, ldus,
                     static_cast<double>(ldus) / std::max(source_seconds, 1e-9), 9 * ldus);
        std::println("{} voice chunks {} frames {} voiced {} gaps {} gap_frames {} underruns {} "
                     "starved_ms {:.1f} jitter p50 {:.1f} p99 {:.1f} max {:.1f} ms{}",
                     tag, scored.chunks, scored.frames, scored.voiced_frames, scored.gap_events,
                     scored.gap_frames, scored.underruns, scored.starved_ms, scored.jitter_p50_ms,
                     scored.jitter_p99_ms, scored.jitter_max_ms,
                     voice_ended.empty() ? std::string{} : " ended: " + voice_ended);
    }
    std::println("{} source lost {} samples in {} overruns", tag,
                 source1.samples_lost - source0.samples_lost,
                 source1.overrun_events - source0.overrun_events);
    std::println("{} completion thread: handler {:.1f} ms/s (max {:.2f} ms), gpu wait {:.1f} "
                 "ms/s, audio sinks {:.1f} ms/s, spectrum sink {:.1f} ms/s, passband {:.1f} ms/s",
                 tag, per_second(load1.handler_ns - load0.handler_ns), ms(load1.handler_max_ns),
                 per_second(load1.gpu_wait_ns - load0.gpu_wait_ns),
                 per_second(load1.audio_sink_ns - load0.audio_sink_ns),
                 per_second(load1.spectrum_sink_ns - load0.spectrum_sink_ns),
                 per_second(load1.passband_sink_ns - load0.passband_sink_ns));
    std::println("{} recording thread: on_block {:.1f} ms/s, frame waits {} for {:.1f} ms/s", tag,
                 per_second(load1.record_ns - load0.record_ns),
                 load1.frame_stalls - load0.frame_stalls,
                 per_second(load1.frame_wait_ns - load0.frame_wait_ns));
    std::println("{} server: detector {} frames ({} shed) {:.1f} ms/s (max {:.2f} ms), tier two "
                 "{:.1f} ms/s, decode {:.1f} ms/s, voice {:.1f} ms/s, audio {:.1f} ms/s, "
                 "detect poll wait {:.1f} ms over {} polls",
                 tag, serve1.detector_frames - serve0.detector_frames,
                 serve1.detector_frames_shed - serve0.detector_frames_shed,
                 per_second(serve1.detector_ns - serve0.detector_ns), ms(serve1.detector_max_ns),
                 per_second(serve1.tier_two_ns - serve0.tier_two_ns),
                 per_second(serve1.decode_ns - serve0.decode_ns),
                 per_second(serve1.voice_ns - serve0.voice_ns),
                 per_second(serve1.audio_ns - serve0.audio_ns),
                 ms(serve1.detect_poll_wait_ns - serve0.detect_poll_wait_ns),
                 serve1.detect_polls - serve0.detect_polls);
    if (!polls.empty()) {
        std::ranges::sort(polls);
        std::println("{} detections: {} tracks, {} labelled, {} decisions; poll rpc p50 {:.1f} "
                     "max {:.1f} ms",
                     tag, poll.total, poll.labelled, poll.decisions, polls[polls.size() / 2],
                     polls.back());
    }
    std::println("{} probes: {} submitted, {} characterised, {:.1f} ms/s characterising, {} "
                 "receivers",
                 tag, probes1.submitted - probes0.submitted,
                 probes1.characterised - probes0.characterised,
                 (probes1.characterise_ms_total - probes0.characterise_ms_total) / wall,
                 probes1.receivers);
    for (const auto& [name, percent] : cpu_by_name(threads0, threads1, wall, main_id)) {
        if (percent >= 0.05) {
            std::println("{} cpu {:6.1f}%  {}", tag, percent, name);
        }
    }

    auto done = finish();
    if (!run_outcome && run_outcome.error().message.find("stopped") == std::string::npos) {
        std::println("{} engine run ended: {}", tag, run_outcome.error().message);
    }
    return done;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string_view first = argv[1];
    if (first == "-h" || first == "--help") {
        print_usage();
        return 0;
    }
    auto options = parse(argc, argv);
    if (!options) {
        std::println(stderr, "revenant-loadtest: {}", options.error().message);
        return 2;
    }
    if (auto status = run(*options); !status) {
        std::println(stderr, "revenant-loadtest: {}", status.error().message);
        return 1;
    }
    return 0;
}
