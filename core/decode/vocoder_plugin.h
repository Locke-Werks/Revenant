// Loading vocoder plugins, and saying out loud when there are none.
//
// core/decode/vocoder_abi.h is the contract a plugin implements and the
// reasoning behind its shape. This header is the host side: find the DLLs,
// check them, refuse the ones that fail, and present whatever survives as an
// ordinary Vocoder to the rest of the tree.
//
// A MISSING OR REFUSED PLUGIN IS NOT AN ERROR
//
// It is the normal case. Nobody has a dongle plugged in most of the time, and
// a mode whose voice payload needs one still demodulates, still frames, still
// recovers talkgroup and unit identifiers and still reports everything it can
// see. Losing all of that because the audio half is unavailable would be the
// worst possible trade.
//
// So scanning cannot fail. scan_vocoder_plugins returns a set, always, and
// the set describes what happened. What it must never do is come back empty
// and quiet: a decoder that is not locked has to look different from a signal
// that is not there, and a mode with no vocoder has to look different from a
// mode with nothing to say. status_line() is that difference, it is never
// empty, and a caller that has one is expected to put it where the operator
// can read it, the same way the RDS decoder reports its lock state whether it
// has one or not.
//
// WHAT THE DIRECTORY IS
//
// "vocoders", beside the executable, overridden by configuration. Every .dll
// in it is a candidate.
//
// That directory is as trusted as the executable next to it, and there is no
// way to make it less so. LoadLibrary runs a DLL's entry point before a single
// byte of it can be inspected, so any check this code performs happens after
// the file has already had its turn. Nothing here should be read as
// sandboxing: it validates a contract, it does not contain a hostile file.
// The mitigation that is available is applied, which is loading with the
// search path pinned to the plugin's own directory and the system directories,
// so a plugin's dependency cannot be satisfied by whatever happens to sit in
// the working directory.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/vocoder.h"
#include "core/error.h"

namespace revenant::decode {

// One loaded DLL: its handle, its entry points and what it enumerated. Opaque
// here because nothing outside the loader has any business with a function
// pointer table, and because naming it in this header would drag <windows.h>
// in behind it.
struct VocoderPluginModule;

// The subdirectory name looked for beside the executable.
inline constexpr std::string_view kVocoderPluginDirectoryName = "vocoders";

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

// Why a candidate file did not become a usable plugin.
//
// One enumerator per cause rather than a bare bool, because these are told
// apart by the person holding the dongle and not by this code: "the DLL would
// not load" sends them to a missing dependency, "the ABI version is wrong"
// sends them to the plugin's author, and "it offers nothing" sends them to
// their own configuration. A single "failed" would send them nowhere.
enum class PluginRefusal : std::uint8_t {
    None,
    LoadFailed,
    MissingEntryPoint,
    AbiVersionMismatch,
    DescriptorRejected,
    OffersNothing,
    NotSupportedOnThisPlatform,
};

[[nodiscard]] std::string_view plugin_refusal_name(PluginRefusal refusal) noexcept;

// One thing a plugin says it can decode.
struct VocoderOffer {
    VocoderFrame frame;
    std::string name;
};

// What happened to one candidate file. Produced for every .dll in the
// directory, loaded or not, because "this file was looked at and here is why
// it is not in the list" is the report the operator needs and the one a scan
// that only lists successes cannot give.
struct VocoderPluginReport {
    std::filesystem::path path;
    bool loaded = false;
    PluginRefusal refusal = PluginRefusal::None;

    // What the plugin reported from revenant_vocoder_abi_version, when it got
    // that far. Zero means it did not.
    std::uint32_t abi_version = 0;

    // GetLastError() where the operating system had an opinion, zero
    // otherwise. Kept separate from the message so a caller can log the raw
    // number without parsing prose.
    long long os_error = 0;

    // One sentence naming the number, the cause and the fix. Always set,
    // including on success, where it says what was loaded.
    std::string detail;

    std::vector<VocoderOffer> offers;
};

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------

struct VocoderPluginScanOptions {
    // Configuration's override. Empty means "vocoders" beside the executable.
    std::optional<std::filesystem::path> directory;
};

// Loaded modules, and the vocoders they can open.
//
// The set owns every loaded module. A Vocoder handed out by open() keeps its
// own module alive, which is the one place in this file that needs shared
// ownership. docs/conventions.md asks for a justification naming the second
// owner and why its lifetime cannot be ordered, so: the second owner is each
// live decoder, and its lifetime genuinely cannot be ordered
// against the set's. A decoder is handed to a receiver that outlives the call
// that made it, the set is rebuilt whenever the operator rescans the
// directory, and FreeLibrary on a module with a live handle in it unmaps the
// code the next decode() call is about to enter. Ordering that by hand means
// one missed path on shutdown and a crash with no stack.
class VocoderPluginSet {
public:
    VocoderPluginSet() = default;
    ~VocoderPluginSet();

    VocoderPluginSet(const VocoderPluginSet&) = delete;
    VocoderPluginSet& operator=(const VocoderPluginSet&) = delete;
    VocoderPluginSet(VocoderPluginSet&&) noexcept;
    VocoderPluginSet& operator=(VocoderPluginSet&&) noexcept;

    // The directory that was scanned, whether or not it exists.
    [[nodiscard]] const std::filesystem::path& directory() const noexcept { return directory_; }
    [[nodiscard]] bool directory_present() const noexcept { return directory_present_; }

    // Every candidate file, in the order they were scanned.
    [[nodiscard]] std::span<const VocoderPluginReport> reports() const noexcept { return reports_; }

    // Everything that loaded, flattened across plugins.
    [[nodiscard]] std::vector<VocoderOffer> offers() const;

    [[nodiscard]] std::size_t offer_count() const noexcept { return offer_count_; }
    [[nodiscard]] bool empty() const noexcept { return offer_count_ == 0; }

    // The line an operator reads. Never empty. Says either what is loaded or,
    // in the same amount of detail, why nothing is.
    [[nodiscard]] std::string status_line() const;

    // Open a decoder matching want exactly on all four fields.
    //
    // Exact, not nearest. A vocoder with the right family and the wrong frame
    // size produces confident audio out of misaligned bits, which sounds like
    // a bad signal rather than like a configuration mistake, and no amount of
    // listening localises it.
    //
    // The failure names what was asked for, what is available and where to put
    // a plugin that would satisfy it.
    [[nodiscard]] Expected<std::unique_ptr<Vocoder>> open(const VocoderFrame& want) const;

private:
    friend VocoderPluginSet scan_vocoder_plugins(const VocoderPluginScanOptions& options);

    std::filesystem::path directory_;
    bool directory_present_ = false;
    std::vector<VocoderPluginReport> reports_;
    std::vector<std::shared_ptr<VocoderPluginModule>> modules_;
    std::size_t offer_count_ = 0;
};

// Scan the directory and load what passes. Cannot fail: everything that could
// go wrong is a reported condition, because none of it stops the mode above
// from working.
[[nodiscard]] VocoderPluginSet scan_vocoder_plugins(const VocoderPluginScanOptions& options);

// "vocoders" beside the running executable. Empty when the executable's own
// path cannot be determined, which the scan then reports rather than guessing
// a relative path that would resolve against whatever the working directory
// happens to be.
[[nodiscard]] std::filesystem::path default_vocoder_plugin_directory();

}  // namespace revenant::decode
