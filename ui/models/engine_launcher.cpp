#include "models/engine_launcher.h"

#include <cstdio>
#include <string>
#include <utility>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstddef>
#include <vector>

namespace revenant::ui {
namespace {

// How long the probe waits for a connect to complete. A listening socket on
// loopback completes one in well under a millisecond, whatever the process
// behind it is doing, because the kernel finishes the handshake into the
// listen backlog without asking it. A port with nothing on it is the slow
// answer on Windows: the refusal is retried and takes about two seconds to
// come back. So a short wait with no answer IS the answer, and the person
// starting the window does not sit through the retries.
constexpr long kProbeWaitMicroseconds = 250'000;

// How many of the engine's last stderr lines are kept for the exit sentence.
// engine_exit_sentence looks back for the line that begins with the program's
// name, and the fatal paths in tools/engined/main.cpp write at most two lines
// after it.
constexpr std::size_t kTailLines = 8;

// A line longer than this with no newline is kept as a line of its own rather
// than grown without bound. The engine writes nothing that long; the cap is
// for a stream that is not the engine's text at all.
constexpr std::size_t kLongestLine = 8192;

[[nodiscard]] std::string narrow(const std::wstring& wide)
{
    if (wide.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                        length, nullptr, nullptr);
    return out;
}

[[nodiscard]] std::string win32_message(DWORD code)
{
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring wide = length > 0 && text != nullptr ? std::wstring(text, length) : std::wstring();
    if (text != nullptr) {
        LocalFree(text);
    }
    while (!wide.empty() && (wide.back() == L'\n' || wide.back() == L'\r' || wide.back() == L'.')) {
        wide.pop_back();
    }
    char number[32] = {};
    std::snprintf(number, sizeof number, " (error %lu)", static_cast<unsigned long>(code));
    return narrow(wide) + number;
}

// One argument quoted the way CommandLineToArgvW and the CRT read it back:
// backslashes are literal except before a quote, where they escape in pairs.
// The install directory is under Program Files, which has a space in it, so
// this is the ordinary case and not a corner.
[[nodiscard]] std::wstring quote_argument(const std::wstring& argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring out = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            // Doubled, so the closing quote below is not escaped by them.
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        out.push_back(*it);
    }
    out.push_back(L'"');
    return out;
}

// Closes a handle that may be null, and says so at the call site.
void close_handle(void*& handle)
{
    if (handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle));
        handle = nullptr;
    }
}

}  // namespace

bool loopback_port_answers(std::uint16_t port)
{
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }

    bool answered = false;
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (probe != INVALID_SOCKET) {
        u_long nonblocking = 1;
        ioctlsocket(probe, FIONBIO, &nonblocking);

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(probe, reinterpret_cast<const sockaddr*>(&to), sizeof to) == 0) {
            answered = true;
        } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set writable;
            FD_ZERO(&writable);
            FD_SET(probe, &writable);
            timeval wait{};
            wait.tv_usec = kProbeWaitMicroseconds;
            // Writable means connected; a refusal shows in the except set,
            // which is not asked for, because a refusal and a timeout are the
            // same answer here.
            answered = select(0, nullptr, &writable, nullptr, &wait) > 0 &&
                       FD_ISSET(probe, &writable) != 0;
        }
        closesocket(probe);
    }

    WSACleanup();
    return answered;
}

EngineLauncher::~EngineLauncher()
{
    if (job_ != nullptr) {
        // TerminateJobObject first, and then the close that would have done
        // the same. KILL_ON_JOB_CLOSE fires on the LAST handle to the job, and
        // this process's is the only one it hands out, but anything that has
        // opened the job since, a debugger or Process Explorer, holds another.
        // The explicit terminate is the ordinary exit; the flag is for the
        // exit that runs no destructor.
        TerminateJobObject(static_cast<HANDLE>(job_), 1);
        close_handle(job_);
    }

    // The engine is gone or going, so its end of the pipe closes and the
    // reader's ReadFile returns. The reader then waits on the process handle,
    // which is why that handle is closed only after the join.
    if (reader_.joinable()) {
        reader_.join();
    }
    close_handle(process_);
    close_handle(stderr_read_);
}

std::string EngineLauncher::start(const std::filesystem::path& exe, std::uint16_t port,
                                  const std::filesystem::path& token_file)
{
    // THE JOB FIRST, AND NO ENGINE WITHOUT IT. An engine this window could not
    // tie to itself would outlive a crash and hold the radio, and the next
    // launch would find the port answering and connect to an orphan nobody
    // started on purpose. Refusing to start is the better failure: the window
    // says why, and the person can start an engine by hand.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return "could not create the Job object that stops the engine with this window: " +
               win32_message(GetLastError());
    }
    job_ = job;

    // KILL_ON_JOB_CLOSE is the owner's rule. DIE_ON_UNHANDLED_EXCEPTION is
    // for the engine's own crash: without it Windows Error Reporting holds a
    // crashed process open behind a dialog, and this engine has no window for
    // that dialog to belong to, so the person sees an engine that is "still
    // starting" for as long as the dialog waits. With it the process ends with
    // the exception code, which engine_exit_sentence says in hex.
    //
    // NO BREAKAWAY FLAG, which is what keeps the engine in. Without
    // JOB_OBJECT_LIMIT_BREAKAWAY_OK a process in this job cannot create a
    // child outside it, even by asking with CREATE_BREAKAWAY_FROM_JOB.
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits) ==
        0) {
        const std::string reason = win32_message(GetLastError());
        close_handle(job_);
        return "could not set the Job object to stop the engine with this window: " + reason;
    }

    // The engine's stderr, read by drain(). The write end is inherited and the
    // read end is not, or the engine would hold the read end open too and the
    // pipe would never report its end.
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (CreatePipe(&read, &write, &inheritable, 0) == 0) {
        const std::string reason = win32_message(GetLastError());
        close_handle(job_);
        return "could not make a pipe for the engine's stderr: " + reason;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    // stdin and stdout go to NUL. The engine prints a status line a second on
    // stdout, which nobody here reads, and a pipe nobody drains fills and then
    // blocks the engine's next print.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0,
                             nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        const std::string reason = win32_message(GetLastError());
        CloseHandle(read);
        CloseHandle(write);
        close_handle(job_);
        return "could not open NUL for the engine's stdout: " + reason;
    }

    // IN THE JOB FROM ITS FIRST INSTRUCTION, through PROC_THREAD_ATTRIBUTE_JOB_LIST,
    // rather than created suspended and assigned after. The two end the same
    // way when both calls succeed. The difference is the failure between
    // them: a process created and then refused by AssignProcessToJobObject is
    // a process that exists outside the job and has to be found and killed,
    // where a JOB_LIST that cannot be honoured fails CreateProcess and nothing
    // exists at all. Windows 10 1607 and later, which Windows 11 is.
    //
    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST alongside it, so the engine inherits
    // the two handles it is given and nothing else this process happens to
    // hold as inheritable. The job's handle is not inheritable in any case;
    // an engine holding one would keep its own job alive past this window.
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
    std::vector<std::byte> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    HANDLE inherited[] = {nul, write};
    HANDLE jobs[] = {job};
    const bool attributes_ready =
        InitializeProcThreadAttributeList(attributes, 2, 0, &attribute_bytes) != 0 &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                  sizeof inherited, nullptr, nullptr) != 0 &&
        UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs,
                                  sizeof jobs, nullptr, nullptr) != 0;
    if (!attributes_ready) {
        const std::string reason = win32_message(GetLastError());
        CloseHandle(nul);
        CloseHandle(read);
        CloseHandle(write);
        close_handle(job_);
        return "could not prepare the engine's process attributes: " + reason;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = nul;
    startup.StartupInfo.hStdError = write;
    startup.lpAttributeList = attributes;

    // --pace 1 because the engine's default, 0, runs a synthetic scene as fast
    // as the GPU allows, which is right for a benchmark and wrong for a window
    // somebody is watching. A live radio sets its own pace and a recording the
    // window opens plays at 1 regardless, so this only changes the synthetic
    // source, and makes it what the person picking it expects.
    std::wstring command = quote_argument(exe.wstring());
    command += L" --no-source --pace 1 --port ";
    command += std::to_wstring(port);
    if (!token_file.empty()) {
        command += L" --token-file ";
        command += quote_argument(token_file.wstring());
    }

    // CREATE_NO_WINDOW: the engine is a console program and the window is
    // what the person opened. Without it a console appears beside the window
    // and closing that console kills the engine, which reads as the radio
    // dying for no reason.
    //
    // The engine's own directory as its working directory, so a relative path
    // it resolves means the same thing whether the window was started from
    // the Start Menu or from a shell somewhere else.
    PROCESS_INFORMATION process{};
    const std::wstring directory = exe.parent_path().wstring();
    const BOOL created =
        CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                       EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
                       directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo,
                       &process);
    const DWORD create_error = created != 0 ? 0 : GetLastError();

    DeleteProcThreadAttributeList(attributes);

    // This process's copies of the child's ends. The write end in particular:
    // while this process holds it the pipe never ends, and drain() would wait
    // on a dead engine forever.
    CloseHandle(write);
    CloseHandle(nul);

    if (created == 0) {
        CloseHandle(read);
        close_handle(job_);
        return "could not start " + narrow(exe.wstring()) + ": " + win32_message(create_error);
    }

    CloseHandle(process.hThread);
    process_ = process.hProcess;
    pid_ = process.dwProcessId;
    stderr_read_ = read;

    reader_ = std::thread([this] { drain(); });
    return {};
}

std::optional<EngineLauncher::Exit> EngineLauncher::exited() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return exit_;
}

void EngineLauncher::drain()
{
    const auto keep = [this](std::string line) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Passed on to this process's stderr as well. A GUI-subsystem window
        // started from the Start Menu has nowhere for it to go and the write
        // fails quietly; started from a shell or a script, it puts the
        // engine's warnings beside the window's own.
        std::fprintf(stderr, "[engine] %s\n", line.c_str());
        if (line.empty()) {
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        tail_.push_back(std::move(line));
        if (tail_.size() > kTailLines) {
            tail_.erase(tail_.begin());
        }
    };

    std::string partial;
    char buffer[4096];
    for (;;) {
        DWORD got = 0;
        if (ReadFile(static_cast<HANDLE>(stderr_read_), buffer, sizeof buffer, &got, nullptr) ==
                0 ||
            got == 0) {
            break;
        }
        partial.append(buffer, got);
        for (std::size_t newline = partial.find('\n'); newline != std::string::npos;
             newline = partial.find('\n')) {
            keep(partial.substr(0, newline));
            partial.erase(0, newline + 1);
        }
        if (partial.size() > kLongestLine) {
            keep(std::exchange(partial, std::string()));
        }
    }
    if (!partial.empty()) {
        keep(std::move(partial));
    }

    // The pipe ends when the engine's last handle to it closes, which is its
    // exit in every case this engine can reach. The wait makes the exit code
    // a real one rather than STILL_ACTIVE read a moment too early.
    WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(process_), &code);

    const std::lock_guard<std::mutex> lock(mutex_);
    exit_ = Exit{static_cast<std::uint32_t>(code), tail_};
}

}  // namespace revenant::ui

#else  // _WIN32

// The client is built for Windows. These keep the file compiling elsewhere
// and start nothing, so a build on another platform behaves as the client did
// before it started engines: it waits for one.
namespace revenant::ui {

bool loopback_port_answers(std::uint16_t) { return false; }

EngineLauncher::~EngineLauncher() = default;

std::string EngineLauncher::start(const std::filesystem::path&, std::uint16_t,
                                  const std::filesystem::path&)
{
    return "starting the engine from the window is implemented on Windows only";
}

std::optional<EngineLauncher::Exit> EngineLauncher::exited() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return exit_;
}

void EngineLauncher::drain() {}

}  // namespace revenant::ui

#endif  // _WIN32
