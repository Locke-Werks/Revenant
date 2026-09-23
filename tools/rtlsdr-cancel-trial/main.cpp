// Counts how often librtlsdr's cancel goes wrong, and how often that kills the
// process, over as many cancels as it is asked for.
//
// WHY IT EXISTS. rtlsdr_read_async sometimes returns -5,
// LIBUSB_ERROR_NOT_FOUND, when it is cancelled, where a clean cancel returns
// 0. When it does, it has freed a bulk transfer libusb still holds, and libusb
// touches the freed memory shortly after: in windows_iocp_thread, or in
// add_to_flying_list under the next control transfer. core/source/rtlsdr_source.cpp
// (run_usb) has the measurement that found it. This tool is the instrument for
// deciding which librtlsdr and libusb to ship, so it has to give the same
// answer to the same question against any build of either.
//
// WHAT IT DOES. The pattern the engine uses for every control call on a
// streaming dongle, and nothing else: stream with rtlsdr_read_async on its own
// thread, cancel it, join, make one control call, flush the device buffer,
// start streaming again. It calls librtlsdr's public API only, so it links
// whatever librtlsdr the build hands it; tools/rtlsdr-cancel-trial/CMakeLists.txt
// says how to point it at a vcpkg install tree other than the project's.
//
// Two processes, because a crash is one of the things being counted. The
// parent starts children, each of which runs a slice of the cancels and
// reports every one on a line of its own, flushed, so a child that dies still
// leaves the count of what it did. The parent adds them up and reads each
// child's exit code: an NTSTATUS such as 0xC0000005 or 0xC0000374 is a crash.
//
// WHAT A CHILD DOES AFTER A BAD CANCEL. By default it does what the engine now
// does: the stream is not restarted, the device is closed, and the child ends.
// rtlsdr_close makes control transfers of its own, so a use after free that
// the -5 left behind can still land there, and a crash that follows is
// counted against the -5 that preceded it. --after-error continue restarts
// the stream on top of it instead, which is what the engine did before
// 2026-09-23 and is the harsher test.
//
// UNDER PAGE HEAP. Without it a freed transfer is usually still readable and
// the process often survives; that is the rate an operator meets. With it,
// enable full page heap on this executable (gflags /p /enable
// rtlsdr-cancel-trial.exe /full, elevated) and every access to a freed
// transfer faults at once, so the crash count approaches the -5 count.
// Nothing in the report can tell which of the two was measured, so say so in
// --label.
//
// It also times each cancel, from rtlsdr_cancel_async being accepted to
// rtlsdr_read_async returning, because that is the part of every control
// call's pause the library decides.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <sddl.h>

#include <rtl-sdr.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    R"(rtlsdr-cancel-trial: count bad cancels and crashes in librtlsdr

usage:
  rtlsdr-cancel-trial [options]

options:
  --cancels N         total cancels to make across all children (default 3000)
  --per-child N       cancels per child process (default 300)
  --device N          librtlsdr device index (default 0)
  --step-ms N         streaming time before a cancel is (round % 7) * N ms (default 20)
  --transfers N       transfers handed to rtlsdr_read_async (default 16)
  --transfer-bytes N  bytes per transfer, a multiple of 512 (default 65536)
  --after-error M     end (default) or continue, after a cancel returns an error
  --timeout-s N       a child that runs longer than this is killed and counted (default 600)
  --label TEXT        copied into the summary so runs can be told apart

The defaults are the engine's: sixteen 64 KiB transfers, and the spacing of the
probe in tests/engine/test_rtlsdr_source.cpp that reproduced the fault.

exit codes:
  0  the run completed, whatever it counted
  1  bad arguments
  2  the dongle could not be opened, or another process held Revenant's
     machine-wide RTL-SDR lock for a minute
  3  a child could not be started
)";

struct Options {
    int cancels = 3000;
    int per_child = 300;
    int device = 0;
    int step_ms = 20;
    int transfers = 16;
    int transfer_bytes = 65'536;
    bool continue_after_error = false;
    int timeout_s = 600;
    std::string label;
    bool child = false;
};

[[nodiscard]] std::optional<Options> parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const auto number = [&](int& into) {
            const char* text = value();
            if (text == nullptr) {
                return false;
            }
            into = std::atoi(text);
            return true;
        };
        bool ok = true;
        if (arg == "--child") {
            options.child = true;
        } else if (arg == "--cancels") {
            ok = number(options.cancels);
        } else if (arg == "--per-child") {
            ok = number(options.per_child);
        } else if (arg == "--device") {
            ok = number(options.device);
        } else if (arg == "--step-ms") {
            ok = number(options.step_ms);
        } else if (arg == "--transfers") {
            ok = number(options.transfers);
        } else if (arg == "--transfer-bytes") {
            ok = number(options.transfer_bytes);
        } else if (arg == "--timeout-s") {
            ok = number(options.timeout_s);
        } else if (arg == "--after-error") {
            const char* text = value();
            ok = text != nullptr &&
                 (std::string_view(text) == "end" || std::string_view(text) == "continue");
            options.continue_after_error = ok && std::string_view(text) == "continue";
        } else if (arg == "--label") {
            const char* text = value();
            ok = text != nullptr;
            if (ok) {
                options.label = text;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    if (options.cancels < 1 || options.per_child < 1 || options.transfers < 1 ||
        options.transfer_bytes < 512 || options.transfer_bytes % 512 != 0 || options.step_ms < 0 ||
        options.timeout_s < 1) {
        return std::nullopt;
    }
    return options;
}

// ---------------------------------------------------------------------------
// The child: the engine's pause, over and over
// ---------------------------------------------------------------------------

// Kept as short as the engine's own callback. The RTL-SDR lane found that
// holding the callback after a cancel made the fault more frequent, so a
// heavier callback here would measure a different thing.
void on_samples(unsigned char* buffer, std::uint32_t length, void* context) {
    auto* bytes = static_cast<std::atomic<std::uint64_t>*>(context);
    bytes->fetch_add(length, std::memory_order_relaxed);
    if (length > 0) {
        // Touch the buffer, as a consumer would, so a transfer whose buffer
        // was freed under it is read here rather than never.
        static volatile unsigned char sink;
        sink = buffer[length - 1];
    }
}

// One control call of the three kinds the engine makes, retried as the engine
// retries, because with librtlsdr v2.0.2 the first control transfer after a
// cancel failed with LIBUSB_ERROR_PIPE more often than not. Returns how many
// attempts it took, or kControlAttempts + 1 when none succeeded, which is what
// the report counts: whether the first transfer after a cancel goes through is
// a property of the cancel.
constexpr int kControlAttempts = 4;

int control_call(rtlsdr_dev_t* device, int round) {
    for (int attempt = 1; attempt <= kControlAttempts; ++attempt) {
        int rc = 0;
        switch (round % 3) {
            case 0:
                rc = rtlsdr_set_center_freq(device, round % 2 == 0 ? 96'500'000 : 98'100'000);
                break;
            case 1:
                rc = rtlsdr_set_tuner_gain_mode(device, 1);
                if (rc == 0) {
                    rc = rtlsdr_set_tuner_gain(device, round % 2 == 0 ? 97 : 297);
                }
                break;
            default: rc = rtlsdr_set_tuner_gain_mode(device, round % 2 == 0 ? 0 : 1); break;
        }
        if (rc == 0) {
            return attempt;
        }
    }
    return kControlAttempts + 1;
}

int run_child(const Options& options) {
    rtlsdr_dev_t* device = nullptr;
    if (const int rc = rtlsdr_open(&device, static_cast<std::uint32_t>(options.device));
        rc != 0 || device == nullptr) {
        std::println("open {}", rc);
        std::fflush(stdout);
        return 2;
    }
    static_cast<void>(rtlsdr_set_sample_rate(device, 2'400'000));
    static_cast<void>(rtlsdr_set_center_freq(device, 98'100'000));
    static_cast<void>(rtlsdr_set_tuner_gain_mode(device, 1));
    static_cast<void>(rtlsdr_set_tuner_gain(device, 197));
    static_cast<void>(rtlsdr_reset_buffer(device));

    std::atomic<std::uint64_t> bytes{0};
    for (int round = 0; round < options.cancels; ++round) {
        std::atomic<bool> done{false};
        std::atomic<int> read_rc{0};
        std::thread usb([&] {
            read_rc = rtlsdr_read_async(device, &on_samples, &bytes,
                                        static_cast<std::uint32_t>(options.transfers),
                                        static_cast<std::uint32_t>(options.transfer_bytes));
            done = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(options.step_ms * (round % 7)));

        // The engine's cancel: retried every 2 ms until accepted, because
        // rtlsdr_cancel_async refuses with -2 until read_async is running, and
        // then not again. See stop_transfers_locked in the backend.
        auto accepted = std::chrono::steady_clock::now();
        while (!done.load()) {
            if (rtlsdr_cancel_async(device) == 0) {
                accepted = std::chrono::steady_clock::now();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        usb.join();

        // From the cancel being accepted to read_async having returned, which
        // is the part of a control call's pause that belongs to librtlsdr.
        const auto stopping_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - accepted)
                                     .count();
        const int rc = read_rc.load();
        std::println("cancel {} {}", rc, stopping_us);
        std::fflush(stdout);

        if (rc != 0 && !options.continue_after_error) {
            break;
        }

        std::println("control {}", control_call(device, round));
        std::fflush(stdout);
        static_cast<void>(rtlsdr_reset_buffer(device));
    }

    std::println("closing {}", bytes.load());
    std::fflush(stdout);
    rtlsdr_close(device);
    std::println("end");
    std::fflush(stdout);
    return 0;
}

// ---------------------------------------------------------------------------
// The parent: children, counts and exit codes
// ---------------------------------------------------------------------------

struct ChildResult {
    int cancels = 0;
    std::map<int, int> by_rc;
    std::vector<long long> stopping_us;
    std::map<int, int> control_attempts;
    int last_rc = 0;
    bool opened = true;
    bool reached_close = false;
    bool ended = false;
    DWORD exit_code = 0;
    bool timed_out = false;
};

[[nodiscard]] std::optional<ChildResult> run_one_child(const Options& options, int cancels) {
    char self[MAX_PATH];
    if (GetModuleFileNameA(nullptr, self, MAX_PATH) == 0) {
        return std::nullopt;
    }
    std::string command = std::format(
        "\"{}\" --child --device {} --cancels {} --step-ms {} --transfers {} --transfer-bytes {} "
        "--after-error {}",
        self, options.device, cancels, options.step_ms, options.transfers, options.transfer_bytes,
        options.continue_after_error ? "continue" : "end");

    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &inherit, 0)) {
        return std::nullopt;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &process)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return std::nullopt;
    }
    CloseHandle(write_end);
    CloseHandle(process.hThread);

    ChildResult result;
    std::thread reader([&] {
        std::string line;
        char chunk[4096];
        DWORD got = 0;
        while (ReadFile(read_end, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
            for (DWORD i = 0; i < got; ++i) {
                if (chunk[i] == '\r') {
                    continue;
                }
                if (chunk[i] != '\n') {
                    line.push_back(chunk[i]);
                    continue;
                }
                if (line.starts_with("cancel ")) {
                    char* after = nullptr;
                    const int rc = static_cast<int>(std::strtol(line.c_str() + 7, &after, 10));
                    ++result.cancels;
                    ++result.by_rc[rc];
                    result.last_rc = rc;
                    if (after != nullptr && *after == ' ') {
                        result.stopping_us.push_back(std::strtoll(after + 1, nullptr, 10));
                    }
                } else if (line.starts_with("control ")) {
                    ++result.control_attempts[std::atoi(line.c_str() + 8)];
                } else if (line.starts_with("open ")) {
                    result.opened = false;
                } else if (line.starts_with("closing ")) {
                    result.reached_close = true;
                } else if (line == "end") {
                    result.ended = true;
                }
                line.clear();
            }
        }
    });

    const DWORD waited =
        WaitForSingleObject(process.hProcess, static_cast<DWORD>(options.timeout_s) * 1000U);
    if (waited == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateProcess(process.hProcess, 0xDEAD);
        WaitForSingleObject(process.hProcess, INFINITE);
    }
    GetExitCodeProcess(process.hProcess, &result.exit_code);
    CloseHandle(process.hProcess);
    reader.join();
    CloseHandle(read_end);
    return result;
}

[[nodiscard]] std::string_view status_name(DWORD code) {
    switch (code) {
        case 0xC0000005: return "access violation";
        case 0xC0000374: return "heap corruption";
        case 0xC0000409: return "stack buffer overrun / fail fast";
        case 0xC0000602: return "fail fast";
        case 0x80000003: return "breakpoint";
        default: return "other";
    }
}

// The machine-wide lock every Revenant process takes before opening the dongle,
// core/source/rtlsdr_lock.h. Taken here by name rather than through
// revenant_core so this tool still links nothing of Revenant's, and held by the
// parent for the whole run: the children are the parent's own, and CI and the
// engine must not open the radio between two of them. The descriptor is
// core/source/device_lock.cpp's, so a mutex created here is one a service in
// session 0 can still open and release.
class RadioLock {
public:
    explicit RadioLock(DWORD wait_ms) {
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x00100001;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1,
                &descriptor, nullptr)) {
            return;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
        mutex_ = CreateMutexExW(&attributes, L"Global\\Revenant.RtlSdr", 0,
                                SYNCHRONIZE | MUTEX_MODIFY_STATE);
        LocalFree(descriptor);
        if (mutex_ == nullptr) {
            return;
        }
        const DWORD waited = WaitForSingleObject(mutex_, wait_ms);
        held_ = waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED;
    }
    ~RadioLock() {
        if (held_) {
            ReleaseMutex(mutex_);
        }
        if (mutex_ != nullptr) {
            CloseHandle(mutex_);
        }
    }
    RadioLock(const RadioLock&) = delete;
    RadioLock& operator=(const RadioLock&) = delete;

    [[nodiscard]] bool held() const { return held_; }

private:
    HANDLE mutex_ = nullptr;
    bool held_ = false;
};

int run_parent(const Options& options) {
    const RadioLock lock(60'000);
    if (!lock.held()) {
        std::println(stderr,
                     "another process has held Global\\Revenant.RtlSdr for a minute; "
                     "not opening the dongle under it");
        return 2;
    }

    struct Crash {
        int child;
        DWORD code;
        int cancels_in_child;
        int last_rc;
        bool in_close;
    };

    int total = 0;
    int children = 0;
    int refused_opens = 0;
    int hangs = 0;
    std::map<int, int> by_rc;
    std::vector<Crash> crashes;
    int errors_survived = 0;
    std::vector<long long> stopping_us;
    std::map<int, int> control_attempts;

    std::println(stderr,
                 "rtlsdr-cancel-trial: {} cancels, {} per child, {} x {} B transfers, "
                 "step {} ms, after an error: {}",
                 options.cancels, options.per_child, options.transfers, options.transfer_bytes,
                 options.step_ms, options.continue_after_error ? "continue" : "end");

    while (total < options.cancels) {
        const int slice = std::min(options.per_child, options.cancels - total);
        auto result = run_one_child(options, slice);
        if (!result) {
            std::println(stderr, "could not start a child process: error {}", GetLastError());
            return 3;
        }
        ++children;
        if (!result->opened) {
            // Held by something else, or gone. A few in a row is somebody
            // else's process; more is the device, and counting on is pointless.
            if (++refused_opens >= 5) {
                std::println(stderr, "the dongle refused five opens in a row; stopping");
                return 2;
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        refused_opens = 0;

        total += result->cancels;
        for (const auto& [rc, count] : result->by_rc) {
            by_rc[rc] += count;
        }
        stopping_us.insert(stopping_us.end(), result->stopping_us.begin(),
                           result->stopping_us.end());
        for (const auto& [attempts, count] : result->control_attempts) {
            control_attempts[attempts] += count;
        }
        const bool crashed =
            (result->exit_code & 0xC0000000U) == 0xC0000000U || result->exit_code == 0x80000003U;
        if (result->timed_out) {
            ++hangs;
        } else if (crashed) {
            crashes.push_back(Crash{children, result->exit_code, result->cancels, result->last_rc,
                                    result->reached_close && !result->ended});
        } else if (result->last_rc != 0) {
            ++errors_survived;
        }
        std::println(stderr, "child {:>3}: {:>4} cancels, last rc {:>3}, exit 0x{:08X}{}{}",
                     children, result->cancels, result->last_rc, result->exit_code,
                     crashed ? " CRASH" : "", result->timed_out ? " HUNG" : "");
        if (result->cancels == 0 && !crashed && !result->timed_out) {
            std::println(stderr, "a child made no cancels and did not crash; stopping");
            break;
        }
    }

    int bad = 0;
    for (const auto& [rc, count] : by_rc) {
        if (rc != 0) {
            bad += count;
        }
    }
    const auto count_of = [&by_rc](int rc) { return by_rc.contains(rc) ? by_rc.at(rc) : 0; };
    const int after_error = static_cast<int>(std::count_if(
        crashes.begin(), crashes.end(), [](const Crash& c) { return c.last_rc != 0; }));

    std::println("label            {}", options.label.empty() ? "(none)" : options.label);
    std::println("cancels          {}", total);
    std::println("returned 0       {}", count_of(0));
    std::println("returned -5      {}", count_of(-5));
    std::println("returned other   {}", bad - count_of(-5));
    for (const auto& [rc, count] : by_rc) {
        if (rc != 0 && rc != -5) {
            std::println("  rc {:>4}        {}", rc, count);
        }
    }
    // An element of the sorted list rather than an interpolation between two,
    // so every figure printed is a time that was measured.
    std::ranges::sort(stopping_us);
    const auto percentile = [&stopping_us](double p) -> double {
        if (stopping_us.empty()) {
            return 0.0;
        }
        const auto rank = static_cast<std::size_t>(p * static_cast<double>(stopping_us.size() - 1));
        return static_cast<double>(stopping_us[rank]) / 1000.0;
    };
    std::println("stopping, ms     median {:.1f}, 90th {:.1f}, max {:.1f}", percentile(0.5),
                 percentile(0.9), percentile(1.0));
    int controls = 0;
    for (const auto& [attempts, count] : control_attempts) {
        controls += count;
    }
    const auto attempts_of = [&control_attempts](int n) {
        return control_attempts.contains(n) ? control_attempts.at(n) : 0;
    };
    std::println("control calls    {}, first attempt {}, second {}, later {}, never {}", controls,
                 attempts_of(1), attempts_of(2), attempts_of(3) + attempts_of(kControlAttempts),
                 attempts_of(kControlAttempts + 1));
    std::println("children         {}", children);
    std::println("crashes          {}", crashes.size());
    std::println("  after an error {}", after_error);
    std::println("  after rc 0     {}", crashes.size() - static_cast<std::size_t>(after_error));
    std::println("errors survived  {}", errors_survived);
    std::println("hangs            {}", hangs);
    for (const Crash& crash : crashes) {
        std::println("  child {} died 0x{:08X} ({}) after {} cancels, last rc {}{}", crash.child,
                     crash.code, status_name(crash.code), crash.cancels_in_child, crash.last_rc,
                     crash.in_close ? ", inside rtlsdr_close" : "");
    }
    std::println(
        "SUMMARY {} cancels={} rc0={} rc-5={} rcother={} crashes={} crashes_after_error={} "
        "hangs={}",
        options.label.empty() ? "-" : options.label, total, count_of(0), count_of(-5),
        bad - count_of(-5), crashes.size(), after_error, hangs);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse(argc, argv);
    if (!options) {
        std::print(stderr, "{}", kUsage);
        return 1;
    }
    return options->child ? run_child(*options) : run_parent(*options);
}
