// revenant-engine: the engine, headless, with the Cap'n Proto session in
// front of it.
//
// revenant-cli owns a source and a fixed set of receivers for the length of
// one command. This owns a source and serves whoever connects, which is what
// core/engine/engine.h means when it says the GUI is one client among
// several. Receivers are not configured here at all: a client adds them over
// the session, and this program never learns what they were for.
//
// Five things in here are decisions rather than transcription.
//
// THE TOKEN IS RESOLVED BEFORE THE ENGINE IS BUILT, AND NEVER PRINTED
//
// A client holds nothing until it has passed Authenticator.login, so this
// program has to have the token before it can serve anything. It is resolved
// first, ahead of Engine::create, so that a missing folder or a token file
// somebody else can read is reported before a radio is opened and then shut
// again. The PATH is printed in the startup block and the token is not:
// stdout here is scrollback, a supervisor's log and CI output at once, and a
// secret written to any of those has left the machine. There is deliberately
// no --print-token; `type` on the file is the way to read it.
//
// THE PORT IS PRINTED AND FLUSHED, AND THAT IS NOT DECORATION
//
// ServerOptions::port defaults to zero, meaning bind whatever is free, so
// that two engines on one machine do not collide. An ephemeral port is then
// unknowable to whoever wants to connect unless this says what it was. stdout
// is block buffered when it is a pipe, which is precisely the case where a
// supervisor is reading for that line, so the line is flushed rather than
// left to arrive whenever the buffer happens to fill.
//
// THE SOURCE IS OPENED BEFORE THE SERVER IS CREATED
//
// Engine::set_spectrum_sink is refused until the source is open, because the
// spectrum stage is built with the rest of the coarse chain. Server::create
// installs that sink. Doing it the other way round works, since the server
// retries the install at subscribeSpectrum, but it means the first client to
// subscribe pays for a failure that was avoidable here.
//
// TEARDOWN ORDER IS server.h's, SPELLED OUT RATHER THAN DERIVED
//
// The server holds the engine's spectrum sink and a thread that fans frames
// out to subscribers, so the engine has to outlive it. stop() is therefore
// called explicitly: stop the engine, stop the server, then destroy either.
// Leaving that to the order two locals were declared in is correct today and
// is one edit away from not being.
//
// THE CONSOLE HANDLER IS tools/cli/main.cpp's, FOR ITS REASONS
//
// Windows terminates the process the moment a close, logoff or shutdown
// handler returns, and this one holds a radio open. So those three block
// until teardown says it has finished; Ctrl-C and Ctrl-Break let the process
// keep running and return at once. The lock around the engine pointer is what
// makes the handler's injected thread safe against the main thread destroying
// the engine underneath it, because unregistering a handler does not wait for
// one that is already inside.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "core/dsp/types.h"
#include "core/engine/engine.h"
#include "core/error.h"
#include "core/rpc/server.h"
#include "core/source/capabilities.h"
#include "core/source/registry.h"

namespace {

using revenant::Error;
using revenant::Expected;
using revenant::Status;
using revenant::fail;
using revenant::with_context;

namespace dsp = revenant::dsp;
namespace engine = revenant::engine;
namespace rpc = revenant::rpc;
namespace source = revenant::source;

using dsp::Hertz;
using dsp::SampleRate;

// One grammar for a frequency, in one place. core/source/registry.h owns the
// text layer of a source for the reason it gives there, and --audio-rate here
// has to accept what a URI's rate= accepts.
using source::parse_frequency;

// ---------------------------------------------------------------------------
// Numbers in and numbers out
// ---------------------------------------------------------------------------

[[nodiscard]] Expected<double> parse_real(std::string_view text, std::string_view what)
{
    double value = 0.0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    // from_chars rather than strtod: strtod honours the C locale, so on a
    // machine set to a comma decimal separator "0.5" would parse as 0.
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("{} '{}' is not a number", what, text));
    }
    return value;
}

[[nodiscard]] Expected<std::int64_t> parse_integer(std::string_view text, std::string_view what)
{
    std::int64_t value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return fail(std::format("{} '{}' is not a whole number", what, text));
    }
    return value;
}

// Range-checked at the point of parsing rather than where the value is used.
// A port of 70000 silently becoming 4464 is a server listening somewhere
// nobody asked for, and the only place that can still be reported as a usage
// error is here.
[[nodiscard]] Expected<std::int64_t> parse_bounded(std::string_view text, std::string_view what,
                                                   std::int64_t low, std::int64_t high)
{
    auto value = parse_integer(text, what);
    if (!value) {
        return value;
    }
    if (*value < low || *value > high) {
        return fail(std::format("{} {} is outside {} to {}", what, *value, low, high));
    }
    return *value;
}

// Display only. Every decision is made on the integer.
[[nodiscard]] std::string format_hz(double hertz)
{
    if (std::abs(hertz) >= 1'000'000.0) {
        return std::format("{:.6f} MHz", hertz / 1e6);
    }
    if (std::abs(hertz) >= 1'000.0) {
        return std::format("{:.3f} kHz", hertz / 1e3);
    }
    return std::format("{:.3f} Hz", hertz);
}

// ---------------------------------------------------------------------------
// The command line
// ---------------------------------------------------------------------------

struct Options {
    std::string uri;

    std::string bind = "127.0.0.1";

    // Zero binds an ephemeral port, which is the default because two engines
    // on one machine is the ordinary case and a fixed default port makes the
    // second one fail. The bound port is printed either way.
    std::uint16_t port = 0;

    // Empty asks core/rpc/token.h for %LOCALAPPDATA%\Revenant\rpc-token.
    // Named when the engine runs as a service, where that path resolves
    // inside C:\Windows\System32\config\systemprofile and the operator's
    // client cannot read it.
    std::string token_file;

    // Mint over whatever is there, rather than loading it.
    bool new_token = false;

    // Zero is "work it out from the source rate", which is what the engine
    // does with EngineConfig::channels at zero. See default_channel_count in
    // core/engine/engine.cpp: 64 is a good grid at 20 MS/s and an unusable
    // one at 2.4 MS/s, where it leaves a broadcast FM receiver clamped to a
    // fraction of the passband it asked for.
    std::uint32_t channels = 0;
    std::uint32_t taps_per_branch = 17;
    SampleRate audio_rate = 48'000;
    int gpu = -1;
    double ring_seconds = 8.0;
    std::size_t block_samples = 65'536;
    double pace = 0.0;

    // Points per coarse channel in the full-span transform, or zero for no
    // spectrum stage at all. On by default here and off by default in the
    // engine, because a client that connects to this is a display: the whole
    // reason it is a separate process is that it draws.
    std::uint32_t spectrum_points = 2048;

    // Points in a per-receiver passband transform, or zero for no passband
    // stage. On by default here for the same reason the spectrum is: a
    // client connecting to this is a display, and the passband is the
    // surface a filter is dragged over. It costs nothing until a client
    // subscribes, because the stage is per receiver and opt in.
    std::uint32_t passband_points = 512;

    std::optional<float> spectrum_floor_db;
    std::optional<float> spectrum_ceiling_db;

    // Seconds of source time. Zero serves until the source ends or Ctrl-C.
    double duration = 0.0;

    bool list = false;
    bool quiet = false;
    bool help = false;
    long status_ms = 2000;
};

void print_usage()
{
    std::print(
        "revenant-engine: serve one engine over Cap'n Proto\n"
        "\n"
        "Usage: revenant-engine <source-uri> [options]\n"
        "\n"
        "Receivers are not configured here. A client adds, retunes and removes\n"
        "them over the session, and subscribes to the full-span spectrum; see\n"
        "core/rpc/revenant.capnp for what the session offers.\n"
        "\n"
        "Serving:\n"
        "  --bind <address>    Interface to listen on, default 127.0.0.1. A client has\n"
        "                      to present the token below, but this wire is plaintext,\n"
        "                      so a token crossing a network is readable and replayable\n"
        "                      by anything on the path. Off loopback still means a\n"
        "                      tunnel; binding elsewhere prints a warning saying so.\n"
        "  --port <n>          Port, default 0, which binds whatever is free. The bound\n"
        "                      port is printed on startup either way, because an\n"
        "                      ephemeral one is otherwise unknowable to a client.\n"
        "  --token-file <path> Where the pre-shared token lives. Default is\n"
        "                      %LOCALAPPDATA%\\Revenant\\rpc-token, which is minted on\n"
        "                      first run readable only by you and SYSTEM. Name a path\n"
        "                      when this runs as a service, because LocalSystem's local\n"
        "                      app data is not somewhere your client can read.\n"
        "                      The path is printed on startup and the token never is;\n"
        "                      run `type` on the file when you need the bytes.\n"
        "  --new-token         Mint a fresh token over the existing file.\n"
        "                      ROTATING DOES NOT DISCONNECT ANYBODY. A session already\n"
        "                      granted is a capability and capabilities do not\n"
        "                      re-check, so rotating because a token leaked means\n"
        "                      restarting the engine to kill the sessions the leak\n"
        "                      already bought.\n"
        "\n"
        "Engine:\n"
        "  --channels <n>      Channelizer channel count, a power of two. The default\n"
        "                      is chosen from the source rate so that one channel can\n"
        "                      carry a 200 kHz broadcast FM receiver wherever it\n"
        "                      lands: 64 at 20 MS/s, 8 at 2.4 MS/s. Naming a count\n"
        "                      pins it. More channels is a finer waterfall and a\n"
        "                      narrower widest receiver, and the two trade directly.\n"
        "  --taps <n>          Taps per polyphase branch, default 17.\n"
        "  --audio-rate <hz>   Default 48000. A receiver may ask for its own.\n"
        "  --gpu <n>           Device index, default -1, which honours\n"
        "                      REVENANT_GPU_INDEX. See revenant-devices.\n"
        "  --ring-seconds <x>  Capture history the device ring holds, default 8.\n"
        "                      Shrunk to fit the device; what was achieved is printed.\n"
        "  --block-samples <n> Samples uploaded per submission, default 65536. Smaller\n"
        "                      is more spectrum frames a second and more submissions.\n"
        "  --pace <x>          Deliver at x times realtime, default 0 for unthrottled.\n"
        "                      A live radio sets its own rate and ignores this. A file\n"
        "                      or synthetic source served to a live display wants 1.\n"
        "\n"
        "Spectrum:\n"
        "  --spectrum <n>      Points per coarse channel in the full-span transform,\n"
        "                      default 2048, a power of two. The frame is then\n"
        "                      channels*n/2 bins wide.\n"
        "  --no-spectrum       Build no spectrum stage. subscribeSpectrum then fails\n"
        "                      with the engine's reason rather than returning a\n"
        "                      subscription that never produces a frame.\n"
        "  --passband <n>      Points in a per-receiver passband transform, default\n"
        "                      512, a power of two. Costs nothing until a client\n"
        "                      subscribes to one: the stage is per receiver and\n"
        "                      opt in.\n"
        "  --no-passband       Build no passband stage. subscribePassband then fails\n"
        "                      with the engine's reason and the detail display in a\n"
        "                      client has nothing to draw.\n"
        "  --spectrum-floor <dbfs>\n"
        "  --spectrum-ceiling <dbfs>\n"
        "                      Hold one or both ends of the colour map still. Both\n"
        "                      track the signal otherwise, which is right almost\n"
        "                      always; pin them when two captures have to be compared.\n"
        "\n"
        "Run:\n"
        "  --duration <sec>    Stop after this many seconds of source time. Accepts a\n"
        "                      fraction. Omitted serves until the source ends or\n"
        "                      Ctrl-C.\n"
        "  --status-ms <n>     Status interval, default 2000. One line per interval\n"
        "                      rather than a line redrawn in place, because this\n"
        "                      program's stdout is usually a log.\n"
        "  --quiet             No periodic status line.\n"
        "  --list              List sources and exit.\n"
        "  -h, --help          This.\n"
        "\n"
        "Exit codes: 0 finished cleanly, 1 an error, 2 bad usage.\n"
        "\n"
        "Examples:\n"
        "  revenant-engine \"synthetic:wideband?rate=2400000&emitters=8&seed=4242\" \\\n"
        "      --pace 1\n"
        "  revenant-engine \"rtlsdr://0?freq=162.550M&rate=2400000\" --port 47000\n");
}

[[nodiscard]] Expected<Options> parse_options(int argc, char** argv)
{
    Options options;

    const auto value_of = [&](int& i, std::string_view name, std::string_view inline_value,
                              bool has_inline) -> Expected<std::string> {
        if (has_inline) {
            return std::string(inline_value);
        }
        if (i + 1 >= argc) {
            return fail(std::format("{} needs a value", name));
        }
        ++i;
        return std::string(argv[i]);
    };

    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};

        std::string_view inline_value;
        bool has_inline = false;
        if (arg.starts_with("--")) {
            if (const auto equals = arg.find('='); equals != std::string_view::npos) {
                inline_value = arg.substr(equals + 1);
                has_inline = true;
                arg = arg.substr(0, equals);
            }
        }

        if (arg == "-h" || arg == "--help") {
            options.help = true;
            return options;
        }
        if (arg == "--list") {
            options.list = true;
            continue;
        }
        if (arg == "--quiet") {
            options.quiet = true;
            continue;
        }
        if (arg == "--no-spectrum") {
            options.spectrum_points = 0;
            continue;
        }

        if (arg == "--bind") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (text->empty()) {
                return fail("--bind needs an address");
            }
            options.bind = *text;
            continue;
        }

        if (arg == "--port") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 0, 65'535);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.port = static_cast<std::uint16_t>(*number);
            continue;
        }

        if (arg == "--token-file") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            if (text->empty()) {
                return fail("--token-file needs a path");
            }
            options.token_file = *text;
            continue;
        }

        if (arg == "--new-token") {
            options.new_token = true;
            continue;
        }

        if (arg == "--channels") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 2, 65'536);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.channels = static_cast<std::uint32_t>(*number);
            continue;
        }

        if (arg == "--taps") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 1, 1'024);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.taps_per_branch = static_cast<std::uint32_t>(*number);
            continue;
        }

        if (arg == "--audio-rate") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto rate = parse_frequency(*text, "--audio-rate");
            if (!rate) {
                return std::unexpected(rate.error());
            }
            if (*rate <= 0) {
                return fail("--audio-rate must be above zero");
            }
            options.audio_rate = *rate;
            continue;
        }

        if (arg == "--gpu") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, -1, 255);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.gpu = static_cast<int>(*number);
            continue;
        }

        if (arg == "--ring-seconds") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (*number <= 0.0) {
                return fail("--ring-seconds must be above zero");
            }
            options.ring_seconds = *number;
            continue;
        }

        if (arg == "--block-samples") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 256, 1 << 24);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.block_samples = static_cast<std::size_t>(*number);
            continue;
        }

        if (arg == "--pace") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (*number < 0.0) {
                return fail("--pace cannot be negative. Zero is unthrottled");
            }
            options.pace = *number;
            continue;
        }

        if (arg == "--spectrum") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 2, 65'536);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.spectrum_points = static_cast<std::uint32_t>(*number);
            continue;
        }

        if (arg == "--passband") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 2, 8'192);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.passband_points = static_cast<std::uint32_t>(*number);
            continue;
        }

        if (arg == "--no-passband") {
            options.passband_points = 0;
            continue;
        }

        if (arg == "--spectrum-floor" || arg == "--spectrum-ceiling") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (arg == "--spectrum-floor") {
                options.spectrum_floor_db = static_cast<float>(*number);
            } else {
                options.spectrum_ceiling_db = static_cast<float>(*number);
            }
            continue;
        }

        if (arg == "--duration") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_real(*text, arg);
            if (!number) {
                return std::unexpected(number.error());
            }
            if (*number <= 0.0) {
                return fail("--duration must be above zero");
            }
            options.duration = *number;
            continue;
        }

        if (arg == "--status-ms") {
            auto text = value_of(i, arg, inline_value, has_inline);
            if (!text) {
                return std::unexpected(text.error());
            }
            auto number = parse_bounded(*text, arg, 10, 3'600'000);
            if (!number) {
                return std::unexpected(number.error());
            }
            options.status_ms = static_cast<long>(*number);
            continue;
        }

        if (arg.starts_with("-")) {
            return fail(std::format("unknown option '{}'", arg));
        }
        if (!options.uri.empty()) {
            return fail(std::format("'{}' is a second source URI; this serves one engine",
                                    arg));
        }
        options.uri = std::string(arg);
    }

    return options;
}

// ---------------------------------------------------------------------------
// Ctrl-C
// ---------------------------------------------------------------------------

// tools/cli/main.cpp's handler, and its reasoning holds unchanged here: the
// handler runs on a thread the OS injects, so the engine is reached under a
// lock and nothing else is touched. Engine::stop() is safe from any thread.
//
// THE LOCK IS NOT DECORATION. Unregistering a handler does not wait for one
// already running, so without it the injected thread can load the engine
// pointer, be descheduled while the main thread finishes tearing down, and
// then make a virtual call through a freed vtable.
std::mutex g_engine_lock;
engine::Engine* g_engine = nullptr;

// Set whenever this process asked the engine to stop, by Ctrl-C or by
// --duration. The graph reports its own cancellation as a failed run, which is
// right for it and wrong here: a stop we asked for and got is a clean finish.
std::atomic<bool> g_stop_requested{false};

// Signalled once the server is stopped and the device is released.
//
// CTRL_C_EVENT and CTRL_BREAK_EVENT let the process keep running, so the
// handler returns and the main thread unwinds normally. The other three do
// not: Windows terminates the process the moment a close, logoff or shutdown
// handler returns, which would leave the radio open and every connected
// client's socket to be reaped by the OS. Windows allows about five seconds
// for a close before killing the process anyway, so the wait is bounded below
// that.
HANDLE g_teardown_done = nullptr;

constexpr DWORD kTeardownWaitMs = 4000;

BOOL WINAPI console_handler(DWORD event)
{
    bool terminating = false;
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT: break;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT: terminating = true; break;
        default: return FALSE;
    }

    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        if (g_engine == nullptr) {
            return FALSE;
        }
        g_stop_requested.store(true, std::memory_order_release);
        static_cast<void>(g_engine->stop());
    }

    if (terminating && g_teardown_done != nullptr) {
        static_cast<void>(WaitForSingleObject(g_teardown_done, kTeardownWaitMs));
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// What gets printed before anything is served
// ---------------------------------------------------------------------------

void print_engine_block(const engine::Engine& eng)
{
    const engine::EngineInfo& info = eng.info();
    const source::SourceCapabilities& caps = eng.source_capabilities();

    std::println("");
    std::println("device      {}", info.device.describe());
    std::println("source      {}", caps.uri);
    std::println("            {}, {} S/s", caps.display_name, info.source_rate);
    if (info.source_center != 0) {
        std::println("            baseband DC at {}",
                     format_hz(static_cast<double>(info.source_center)));
    }
    std::println("grid        M={} D={} taps/branch={}", info.grid.channels,
                 info.grid.decimation, info.grid.taps_per_branch);
    std::println("            channel {} S/s, spacing {}", info.channel_rate,
                 format_hz(static_cast<double>(info.channel_spacing)));

    // The number an operator needs before they click on anything, and the
    // one the grid line does not say. A receiver landing anywhere is
    // guaranteed one spacing; one landing on a channel centre gets the whole
    // channel rate. Anything wider is clamped, and on an FM mode a clamp is
    // not a narrower receiver but a broken one.
    std::println("            widest receiver {} anywhere, {} on a channel centre",
                 format_hz(static_cast<double>(info.channel_spacing)),
                 format_hz(static_cast<double>(info.channel_rate)));

    const std::uint32_t would_choose = engine::default_channel_count(info.source_rate);
    if (info.grid.channels > would_choose &&
        info.channel_spacing < engine::kWidestReceiverHz) {
        // The block above is buffered and this is not, so it is flushed
        // first: a warning that appears above the grid line it is about
        // reads as being about something else.
        std::fflush(stdout);
        std::println(
            stderr,
            "warning: a {} channel grid on a {} S/s source leaves one channel {} wide, so a "
            "receiver asking for more than that is clamped. A {} Hz broadcast FM receiver is "
            "the case this bites, and on the FM modes the engine now REFUSES the placement "
            "rather than narrowing it: a discriminator fed a truncated signal produces the "
            "wrong audio rather than narrow audio, at full strength. So clicking a broadcast "
            "station on this grid will not open a receiver at all. --channels {} is what "
            "this source rate chooses on its own{}",
            info.grid.channels, info.source_rate,
            format_hz(static_cast<double>(info.channel_spacing)), engine::kWidestReceiverHz,
            would_choose,
            // THE LAST SENTENCE USED TO READ AS THOUGH THE OPERATOR CHOSE THE
            // COUNT, and since the resolution request started widening the grid
            // that is often false: an HF recording opens on 256 channels
            // nobody typed. Offering --channels 8 as the fix there is offering
            // to undo a choice the engine made for a stated reason, which is
            // the coarse grid the request exists to avoid.
            //
            // The clamp reason is what says which happened, and it is now
            // printed in the block above. This points at it rather than
            // restating it, because the engine's own sentence names the signal
            // width and the basis and this warning has neither.
            info.ring.clamped && !info.ring.clamp_reason.empty()
                ? ", and the line under the ring above says whether that is what "
                  "happened or whether the source asked for this grid."
                : ".");
    }

    if (info.spectrum.enabled()) {
        std::println("spectrum    {} channels x {} points, {} bins across the span",
                     info.spectrum.channels, info.spectrum.transform, info.spectrum.bins);
        std::println("            {} per bin, bin zero at {}",
                     format_hz(info.spectrum.bin_width_hz()),
                     format_hz(info.spectrum.bin_zero_hz()));
    } else {
        std::println("spectrum    none. subscribeSpectrum will be refused");
    }

    std::println("ring        {} samples, {:.2f} s retained", info.ring.capacity_samples,
                 info.ring.seconds_retained);

    // WHAT THE ENGINE BUILT THAT NOBODY ASKED FOR, and until now this program
    // dropped it.
    //
    // EngineInfo::ring.clamp_reason is the one field in EngineInfo that can
    // carry a sentence, which is why a reduced channel count, a reduced block
    // size and a widened grid all ride out in it. It is not ring trivia,
    // despite the field name, and printing the two ring numbers above without
    // it left an operator looking at a geometry they did not choose with
    // nothing on screen saying why.
    //
    // It bites hardest on the widening the resolution request now does: an HF
    // recording opens on 256 channels where the rate alone would have picked 8,
    // and the sentence explaining that is the only thing between the operator
    // and the warning below, which reads as though they chose it.
    if (info.ring.clamped && !info.ring.clamp_reason.empty()) {
        std::println("            {}", info.ring.clamp_reason);
    }
}

[[nodiscard]] Status list_sources()
{
    auto described = source::describe_sources();
    if (!described) {
        return std::unexpected(with_context(described.error(), "listing sources"));
    }

    for (const source::SourceCapabilities& caps : *described) {
        std::println("{}", caps.uri);
        std::println("  {}  ({} backend)", caps.display_name, caps.backend);
        if (!caps.available()) {
            std::println("  UNAVAILABLE     {}", caps.unavailable);
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Serving
// ---------------------------------------------------------------------------

// Loopback in the only two spellings a bind address can have here, because
// the warning has to fire for a LAN address and must not fire for the
// default. parseAddress accepts a hostname too, so this is a check on what
// was asked for rather than on what was resolved; anything it does not
// recognise gets the warning, which is the safe direction.
[[nodiscard]] bool is_loopback(std::string_view address)
{
    return address == "127.0.0.1" || address == "::1" || address == "localhost";
}

// The token, before the engine is built, so a token problem is reported
// before a device is opened.
//
// The path is printed by the caller and the token never is. stdout here is
// scrollback, a supervisor's log and CI output all at once, and a secret
// written to any of those has left the machine. An operator who needs the
// bytes runs `type` on the file, which is why there is no --print-token.
[[nodiscard]] Expected<std::pair<std::string, rpc::Token>> resolve_token(const Options& options)
{
    std::string path = options.token_file;
    if (path.empty()) {
        auto resolved = rpc::default_token_path();
        if (!resolved) {
            return std::unexpected(resolved.error());
        }
        path = std::move(*resolved);
    }

    auto token = options.new_token ? rpc::rotate_token(path) : rpc::load_or_mint_token(path);
    if (!token) {
        return std::unexpected(token.error());
    }
    return std::pair<std::string, rpc::Token>{std::move(path), *token};
}

[[nodiscard]] Status serve(const Options& options)
{
    auto token = resolve_token(options);
    if (!token) {
        return std::unexpected(with_context(token.error(), "preparing the RPC token"));
    }

    engine::EngineConfig config;
    config.gpu_index = options.gpu;
    config.ring_seconds = options.ring_seconds;
    config.channels = options.channels;
    config.taps_per_branch = options.taps_per_branch;
    config.audio_rate = options.audio_rate;
    config.block_samples = options.block_samples;
    config.pace = options.pace;
    config.spectrum_transform = options.spectrum_points;
    config.passband_transform = options.passband_points;
    config.spectrum_floor_db = options.spectrum_floor_db;
    config.spectrum_ceiling_db = options.spectrum_ceiling_db;

    auto created = engine::Engine::create(config);
    if (!created) {
        return std::unexpected(with_context(created.error(), "building the engine"));
    }
    engine::Engine& eng = **created;

    // Before the server, per the note at the top: the spectrum stage is built
    // when the source is opened, and Server::create installs the sink.
    if (auto opened = eng.open_source(options.uri); !opened) {
        return std::unexpected(with_context(opened.error(), "opening the source"));
    }

    print_engine_block(eng);

    // stdout is block buffered when it is a pipe and stderr never is, so a
    // warning written next would otherwise appear above the block it is
    // about. Both of them are read together by whoever is looking at a
    // console or a log.
    std::fflush(stdout);

    // A DEMAND SOURCE WITH NO PACE AND A SOURCE THAT CANNOT KEEP UP LOOK
    // IDENTICAL FROM OUTSIDE, AND THIS IS THE ONLY PLACE THAT KNOWS WHICH
    // WAS ASKED FOR.
    //
    // Unthrottled is the right default for a recorder and for every test:
    // there is no throttle anywhere, only the graph's blocking reserve, and
    // a capture retired as fast as the GPU manages is what an offline run
    // wants. It is the wrong default the moment somebody is listening,
    // because the source then delivers at whatever rate it can reach and a
    // listener hears that rate. On this host a synthetic scene at 20 MS/s
    // reaches about a fifth of realtime, and what a client sees is an audio
    // queue that keeps running dry: true about the queue and pointing at
    // the wrong component.
    //
    // Said here rather than refused, because a headless recording is a real
    // and common use of this program and it wants exactly this setting.
    // EngineInfo::realtimeFactor and sourcePacedBy are the same fact on the
    // wire, for the operator who is looking at a GUI rather than at this.
    if (eng.source_capabilities().flow == source::FlowControl::Demand && options.pace == 0.0) {
        std::println(stderr,
                     "warning: '{}' is a demand source and --pace is 0, so it is asked to "
                     "deliver as fast as the machine retires it rather than on a clock. "
                     "Nothing downstream paces it, so a client listening to this hears "
                     "whatever rate the source reaches, and a source that cannot reach "
                     "realtime is indistinguishable from one deliberately running flat out. "
                     "Pass --pace 1 to serve a live display; EngineInfo.realtimeFactor is "
                     "what a client reads to tell the two apart.",
                     eng.source_capabilities().uri);
    }

    rpc::ServerOptions server_options;
    server_options.bind_address = options.bind;
    server_options.port = options.port;
    server_options.token.assign(token->second.begin(), token->second.end());

    auto server = rpc::Server::create(eng, server_options);
    if (!server) {
        return std::unexpected(with_context(server.error(), "starting the RPC server"));
    }

    std::println("");
    std::println("token file      {}", token->first);

    // One line on stderr and no second flag. Two flags that have to agree are
    // two flags that get out of sync, and the token does not make this wire
    // safe to put on a network: it is plaintext, so anything on the path can
    // read the token as it goes by and replay it.
    if (!is_loopback(options.bind)) {
        std::println(stderr,
                     "warning: bound to {}, which is not loopback. This wire is not "
                     "encrypted, so the token crosses in the clear and anything on the path "
                     "can read it and reuse it. Put a tunnel in front of this.",
                     options.bind);
    }

    // The one line a supervisor parses, and the reason for the flush. See the
    // note at the top of the file.
    std::println("listening on {}:{}", options.bind, (*server)->port());
    std::fflush(stdout);

    // Manual reset, so a handler arriving after teardown returns at once
    // rather than waiting for something that already happened.
    g_teardown_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        g_engine = &eng;
    }
    if (SetConsoleCtrlHandler(console_handler, TRUE) == 0) {
        std::println(stderr, "warning: the Ctrl-C handler could not be installed, so an "
                             "interrupt will leave the device open");
    }

    std::atomic<bool> finished{false};
    const auto wall_start = std::chrono::steady_clock::now();

    // One thread for both jobs, as in revenant-cli. --duration is enforced
    // against the source's own delivered sample count rather than a wall
    // clock, because a capture replayed faster than realtime has to stop
    // after the requested amount of capture, not of an afternoon.
    std::thread monitor([&] {
        constexpr auto kPoll = std::chrono::milliseconds(10);
        auto last_line = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds(options.status_ms);

        // The measured half of the warning above, said once. The config
        // warning fires on a setting and this one fires on the outcome, so
        // a source that was expected to keep up and does not gets a line
        // even when nobody thought --pace was worth passing.
        //
        // Not before five seconds of wall clock: the factor is a lifetime
        // mean and the first second of any run includes opening a device,
        // designing a prototype and filling a ring, so an early reading is
        // low on a source that is fine.
        // Said once PER SOURCE and not once per process. An operator who
        // changed radios because the first one could not keep up would
        // otherwise get no warning about the second, which is the reading they
        // changed radios to obtain. The epoch is the same counter the serve
        // loop below uses to tell a client's close from a stream that ended.
        bool behind_reported = false;
        std::uint64_t reported_for_epoch = eng.info().source_epoch;
        constexpr double kBehind = 0.9;
        constexpr double kSettleSeconds = 5.0;

        while (!finished.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(kPoll);

            const source::SourceStats stats = eng.source_stats();
            const SampleRate rate = eng.info().source_rate;
            if (const std::uint64_t epoch = eng.info().source_epoch; epoch != reported_for_epoch) {
                reported_for_epoch = epoch;
                behind_reported = false;
            }
            const double source_seconds =
                rate > 0 ? static_cast<double>(stats.samples_delivered) /
                               static_cast<double>(rate)
                         : 0.0;

            if (options.duration > 0.0 && source_seconds >= options.duration &&
                !g_stop_requested.exchange(true, std::memory_order_acq_rel)) {
                static_cast<void>(eng.stop());
            }

            const engine::SourcePacing pacing = eng.source_pacing();
            if (!behind_reported && pacing.demand && pacing.paced_by == 0.0 &&
                pacing.elapsed_seconds >= kSettleSeconds &&
                pacing.realtime_factor > 0.0 && pacing.realtime_factor < kBehind) {
                behind_reported = true;
                std::println(
                    stderr,
                    "warning: this source is delivering {:.2f}x realtime unthrottled, so it "
                    "cannot keep up and is not being held back. Audio to a listening client "
                    "arrives in fragments and the client sees that as a starving queue, which "
                    "is the wrong component. Lower the rate, or accept that this is an "
                    "offline run. EngineInfo.realtimeFactor carries the same number.",
                    pacing.realtime_factor);
                std::fflush(stderr);
            }

            const auto now = std::chrono::steady_clock::now();
            if (options.quiet || now - last_line < interval) {
                continue;
            }
            last_line = now;

            // pacing.realtime_factor rather than source_seconds over this
            // process's wall clock.
            //
            // Both counters this thread reads restart with the source, because
            // Session.closeSource made a process outlive the thing it was
            // serving: samples_delivered is per-source and so is the engine's
            // pacing window, while wall_start is once per process. Dividing one
            // by the other reported x0.38 on a dongle holding realtime
            // perfectly, the ratio climbing towards 1 as the new source's
            // seconds caught up with the old one's, which reads as a source
            // that is behind and recovering. The engine already computes the
            // honest number over the window it belongs to; this prints that.
            std::string line = std::format(
                "{:8.2f}s  x{:6.2f} | frames {} sent, {} dropped | {} vrx", source_seconds,
                pacing.realtime_factor, (*server)->frames_sent(), (*server)->frames_dropped(),
                eng.vrx_ids().size());
            if (stats.samples_lost != 0) {
                line += std::format(" | lost {}", stats.samples_lost);
            }
            std::println("{}", line);
            std::fflush(stdout);
        }
    });

    // ONE SOURCE PER run(), AND THIS PROGRAM NOW SERVES MORE THAN ONE
    //
    // run() returns when the stream ends, and since Session.closeSource landed
    // that has three causes rather than one. Two of them are this process
    // finishing and one of them is an operator changing radios, so a single
    // call would exit the program the first time somebody picked a different
    // dongle in the client.
    //
    //   The source ran out, or --duration was reached, or Ctrl-C. Exit, which
    //   is what every script and every test that drives this program expects.
    //
    //   A client called closeSource. Wait for it to open another.
    //
    //   A client called closeSource and then openSource, fast enough that both
    //   happened before this loop looked. There is a source open and it is not
    //   the one that just ended.
    //
    // THE EPOCH IS WHAT SEPARATES THE LAST TWO FROM THE FIRST, and has_source
    // on its own cannot. Reading only has_source would see the third case as a
    // source that is open and running, conclude the stream ended by itself and
    // exit, and changing radios is precisely the sequence that produces it.
    // EngineInfo::source_epoch counts opens, so a number that moved means a
    // client intervened however quickly it did so.
    //
    // A refused run() is not special-cased, because it does not have to be: if
    // the source went between the has_source() below and the call, the engine
    // refuses with "before a source is open" and this loop finds has_source
    // false and goes back to waiting, which is the answer either way.
    constexpr auto kSourceWaitPoll = std::chrono::milliseconds(20);
    Status ran;
    for (;;) {
        if (g_stop_requested.load(std::memory_order_acquire)) {
            break;
        }
        if (!eng.has_source()) {
            std::this_thread::sleep_for(kSourceWaitPoll);
            continue;
        }

        const std::uint64_t epoch = eng.info().source_epoch;
        ran = eng.run();

        if (g_stop_requested.load(std::memory_order_acquire)) {
            break;
        }
        if (!eng.has_source() || eng.info().source_epoch != epoch) {
            continue;
        }
        break;
    }

    finished.store(true, std::memory_order_release);
    monitor.join();

    // Unregistering does not wait for a handler already inside stop(), so the
    // lock is what makes the engine safe to destroy after this returns.
    static_cast<void>(SetConsoleCtrlHandler(console_handler, FALSE));
    {
        const std::lock_guard<std::mutex> guard(g_engine_lock);
        g_engine = nullptr;
    }

    // Before the engine goes, and explicitly rather than by destructor order.
    // The server holds the engine's spectrum sink and a thread that touches
    // subscriptions; server.h requires the engine to outlive it.
    (*server)->stop();

    if (g_teardown_done != nullptr) {
        static_cast<void>(SetEvent(g_teardown_done));
    }

    const double wall_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - wall_start).count();
    const source::SourceStats stats = eng.source_stats();

    // The wall clock is this process's and the sample counts are the last
    // source's, so the line says which is which rather than reading as one
    // measurement. After a closeSource and an openSource they are genuinely
    // different spans, and a run that served two radios for a minute each
    // would otherwise report two minutes' worth of one of them. Nothing keeps
    // a lifetime sample count across sources, and inventing one here to make
    // the sentence tidier would be a number no other surface reports.
    std::println("");
    std::println("served {:.2f} s of wall clock; the source open at the end delivered {} blocks, "
                 "{} samples",
                 wall_seconds, stats.blocks_delivered, stats.samples_delivered);
    std::println("spectrum   {} frames sent, {} dropped", (*server)->frames_sent(),
                 (*server)->frames_dropped());
    if (stats.overrun_events != 0) {
        std::println("OVERRUN    {} events, {} samples lost", stats.overrun_events,
                     stats.samples_lost);
    }

    // A stop we asked for and got is a clean finish. Anything else is not, and
    // the difference is the exact message the graph reports when it cancels
    // itself: run() still stops the source, flushes the graph and checks the
    // scheduler after cancellation, and any of those can fail for a real
    // reason. Swallowing every error behind the latch would turn a device lost
    // to a driver reset into "stopped at the requested 30 s" and exit 0.
    const bool asked_to_stop = g_stop_requested.load(std::memory_order_acquire);
    const bool is_cancellation =
        !ran && ran.error().message.find("the engine was stopped") != std::string::npos;
    if (!ran && !(asked_to_stop && is_cancellation)) {
        return std::unexpected(with_context(ran.error(), "running the engine"));
    }
    return {};
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        print_usage();
        return 2;
    }

    auto options = parse_options(argc, argv);
    if (!options) {
        std::println(stderr, "revenant-engine: {}", options.error().message);
        std::println(stderr, "Try revenant-engine --help.");
        return 2;
    }

    if (options->help) {
        print_usage();
        return 0;
    }

    if (options->list) {
        if (auto listed = list_sources(); !listed) {
            std::println(stderr, "revenant-engine: {}", listed.error().message);
            return 1;
        }
        return 0;
    }

    if (options->uri.empty()) {
        std::println(stderr,
                     "revenant-engine: no source URI. Run --list to see what is available.");
        return 2;
    }
    if (options->spectrum_floor_db.has_value() && options->spectrum_ceiling_db.has_value() &&
        *options->spectrum_ceiling_db <= *options->spectrum_floor_db) {
        std::println(stderr,
                     "revenant-engine: --spectrum-ceiling {:g} is not above --spectrum-floor "
                     "{:g}, so the colour map has no range.",
                     *options->spectrum_ceiling_db, *options->spectrum_floor_db);
        return 2;
    }
    if (options->spectrum_points == 0 &&
        (options->spectrum_floor_db.has_value() || options->spectrum_ceiling_db.has_value())) {
        std::println(stderr,
                     "revenant-engine: --spectrum-floor and --spectrum-ceiling set the colour "
                     "map of a spectrum stage that --no-spectrum turned off.");
        return 2;
    }

    if (auto served = serve(*options); !served) {
        std::println(stderr, "");
        std::println(stderr, "revenant-engine: {}", served.error().message);
        if (served.error().code != 0) {
            std::println(stderr, "  code {}", served.error().code);
        }
        return 1;
    }

    return 0;
}
