// rpc_smoke_client: the other half of the two-process smoke test.
//
// WHY THIS PROGRAM EXISTS
//
// Every case in tests/rpc puts the engine, the server and the client in one
// address space. They talk over a real socket and share one heap, so nothing
// there has allocated on the static core's heap and freed on the UCRT's, which
// is the failure the engine/client process split exists to prevent. This is
// core/rpc/client.cpp compiled /MD against the dynamic vcpkg triplet, the way
// ui/ builds it, run in its own process against a revenant-engine built /MT
// from the root tree. scripts/two-process-smoke.ps1 starts the engine, reads
// the port it prints and runs this.
//
// It is not the Qt client. ui/main.cpp has no offscreen, timed-exit mode, and
// adding one is ui/'s change to make. What this proves is the part that does
// not need a window: a login, the source list, a spectrum subscription and a
// receiver's audio, each crossing from one runtime to the other.
//
// Exit 0 when every step completed, 1 with the step and the engine's words
// when one did not, 2 on a bad command line.

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/rpc/client.h"
#include "core/rpc/token.h"
#include "core/rpc/types.h"

namespace {

namespace rpc = revenant::rpc;
using namespace std::chrono_literals;

struct Options {
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;
    std::string token_file;
    std::uint64_t frames = 20;
    std::uint64_t chunks = 5;
    int timeout_seconds = 60;
};

template <typename T>
bool parse_number(std::string_view text, T& out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (i + 1 >= argc) {
            std::print(stderr, "rpc_smoke_client: {} needs a value\n", arg);
            return false;
        }
        const std::string_view value = argv[++i];
        bool ok = true;
        if (arg == "--address") {
            options.address = std::string(value);
        } else if (arg == "--port") {
            ok = parse_number(value, options.port);
        } else if (arg == "--token-file") {
            options.token_file = std::string(value);
        } else if (arg == "--frames") {
            ok = parse_number(value, options.frames);
        } else if (arg == "--chunks") {
            ok = parse_number(value, options.chunks);
        } else if (arg == "--timeout") {
            ok = parse_number(value, options.timeout_seconds);
        } else {
            std::print(stderr, "rpc_smoke_client: unknown option {}\n", arg);
            return false;
        }
        if (!ok) {
            std::print(stderr, "rpc_smoke_client: {} '{}' is not a number\n", arg, value);
            return false;
        }
    }
    if (options.port == 0 || options.token_file.empty()) {
        std::print(stderr,
                   "usage: rpc_smoke_client --port N --token-file PATH [--address A] "
                   "[--frames N] [--chunks N] [--timeout S]\n");
        return false;
    }
    return true;
}

int step_failed(std::string_view step, const revenant::Error& error) {
    std::print(stderr, "rpc_smoke_client: {} failed: {}\n", step, error.message);
    return 1;
}

// Waits on a counter the event loop thread advances. A plain poll rather than
// a condition variable: the callbacks run on the client's loop and must return
// quickly, and a sleep here costs the test a few milliseconds at most.
bool wait_for(const std::atomic<std::uint64_t>& counter, std::uint64_t target,
              std::chrono::steady_clock::time_point deadline) {
    while (counter.load() < target) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(10ms);
    }
    return true;
}

int run(const Options& options) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);

    auto token = rpc::load_token(options.token_file);
    if (!token) {
        return step_failed("reading the token", token.error());
    }

    auto client = rpc::Client::connect(options.address, options.port, *token);
    if (!client) {
        return step_failed("connect and login", client.error());
    }
    std::println("logged in to {}:{}", options.address, options.port);

    auto info = (*client)->info();
    if (!info) {
        return step_failed("info", info.error());
    }
    std::println("engine source rate {} S/s, channel rate {} S/s", info->source_rate, info->channel_rate);

    // Strings and vectors built by the /MD side out of a message the /MT side
    // wrote, and destroyed here. The allocation that would cross heaps if the
    // two were one process is exactly this.
    auto sources = (*client)->list_sources();
    if (!sources) {
        return step_failed("list_sources", sources.error());
    }
    std::println("{} source(s) listed", sources->size());

    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> bins{0};
    if (auto ok = (*client)->subscribe_spectrum(1, [&](const rpc::SpectrumFrame& frame) {
            bins.store(frame.power_db.size());
            frames.fetch_add(1);
        });
        !ok) {
        return step_failed("subscribe_spectrum", ok.error());
    }
    if (!wait_for(frames, options.frames, deadline)) {
        std::print(stderr, "rpc_smoke_client: {} of {} spectrum frames before the timeout\n", frames.load(),
                   options.frames);
        return 1;
    }
    std::println("{} spectrum frames of {} bins", frames.load(), bins.load());

    rpc::VrxParams params;
    params.center = 0;
    params.bandwidth = 12'000;
    params.demod = rpc::Demod::Nfm;
    auto vrx = (*client)->add_vrx(params);
    if (!vrx) {
        return step_failed("add_vrx", vrx.error());
    }

    std::atomic<std::uint64_t> chunks{0};
    std::atomic<std::uint64_t> samples{0};
    std::atomic<bool> ended{false};
    auto granted = (*client)->subscribe_audio(
        *vrx, 0,
        [&](const rpc::AudioChunk& chunk) {
            samples.fetch_add(chunk.samples.size());
            chunks.fetch_add(1);
        },
        [&](const std::string&) { ended.store(true); });
    if (!granted) {
        return step_failed("subscribe_audio", granted.error());
    }
    if (!wait_for(chunks, options.chunks, deadline)) {
        std::print(stderr, "rpc_smoke_client: {} of {} audio chunks before the timeout{}\n", chunks.load(),
                   options.chunks, ended.load() ? ", and the stream ended" : "");
        return 1;
    }
    std::println("{} audio chunks, {} samples, {} ms of buffer granted", chunks.load(), samples.load(), *granted);

    (*client)->unsubscribe_audio(*vrx);
    if (auto ok = (*client)->remove_vrx(*vrx); !ok) {
        return step_failed("remove_vrx", ok.error());
    }
    (*client)->unsubscribe_spectrum();

    // Destroying the client here, before main returns, is the teardown the
    // test is also about: every object it frees was allocated on this side.
    client->reset();
    std::println("two-process smoke: ok");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        return 2;
    }
    return run(options);
}
