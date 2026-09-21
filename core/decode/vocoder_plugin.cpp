// The host side of the vocoder plugin ABI.
//
// core/decode/vocoder_plugin.h states the policy: scanning cannot fail, a
// missing plugin is a reported condition and not an error, and the report is
// never silent. This file holds that up. Three things about the structure are
// not visible from the header.
//
// EVERY REFUSAL IS BUILT FROM WHAT THE HOST ALREADY KNOWS. No message in this
// file came across the ABI boundary. The plugin returns a number and the host
// writes the sentence, which is why a plugin cannot hand the operator a bare
// integer with no explanation and why a code this build has never heard of
// still produces a readable line with the filename in it. The only text that
// crosses is rv_vocoder_desc::name, which is a fixed array the host allocated
// and which is forced to terminate on arrival.
//
// THE DESCRIPTOR THAT GOES BACK IN IS THE ONE THAT CAME OUT. create() is
// handed the plugin's own bytes, unmodified, rather than a descriptor rebuilt
// from the VocoderFrame the caller matched on. VocoderFrame is a lossy view of
// rv_vocoder_desc: it drops the name and the reserved word and it maps an
// unrecognised kind to External. Rebuilding would hand the plugin a
// descriptor it never emitted and ask it to recognise it, which a strict
// plugin is entitled to refuse and a lax one is entitled to misread.
//
// A PLUGIN THAT LIES ABOUT HOW MUCH IT WROTE POISONS ITS OWN HANDLE. Detecting
// out_written past the end of the buffer is after the fact: if the write
// really happened, it happened before the host could look. What the host can
// still do is refuse the result and refuse every later call on that handle,
// so the fault surfaces as a decoder that stopped with a reason rather than as
// audio nobody can account for. Continuing to call a module that has already
// demonstrated it does not honour a capacity is the version of this that ends
// in a corrupted heap somewhere else entirely.

#include "core/decode/vocoder_plugin.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

#include "core/decode/vocoder_abi.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace revenant::decode {

// ---------------------------------------------------------------------------
// The loaded module
// ---------------------------------------------------------------------------

struct VocoderPluginModule {
    std::filesystem::path path;

#ifdef _WIN32
    HMODULE handle = nullptr;
#endif

    rv_vocoder_describe_fn describe = nullptr;
    rv_vocoder_create_fn create = nullptr;
    rv_vocoder_decode_fn decode = nullptr;
    rv_vocoder_reset_fn reset = nullptr;
    rv_vocoder_destroy_fn destroy = nullptr;

    // Exactly what the plugin enumerated, kept verbatim for create().
    std::vector<rv_vocoder_desc> descriptors;

    VocoderPluginModule() = default;

    VocoderPluginModule(const VocoderPluginModule&) = delete;
    VocoderPluginModule& operator=(const VocoderPluginModule&) = delete;

    ~VocoderPluginModule()
    {
#ifdef _WIN32
        if (handle != nullptr) {
            FreeLibrary(handle);
        }
#endif
    }
};

namespace {

// The descriptor layout is part of the ABI, so a change to it that did not
// also bump RV_VOCODER_ABI_VERSION would be a silent incompatibility. This is
// the cheapest place to make that impossible.
static_assert(sizeof(rv_vocoder_desc) == 56,
              "rv_vocoder_desc is part of the plugin ABI; changing its size is an ABI break "
              "and needs RV_VOCODER_ABI_VERSION raised with it");

// An unrecognised family is External rather than an error. The host has no
// standing to reject a codec it has never heard of: the offer is still
// matchable by its numbers, and name() still says what the plugin called it.
[[nodiscard]] VocoderKind kind_from_abi(std::uint32_t abi_kind) noexcept
{
    switch (abi_kind) {
        case RV_VOCODER_KIND_IMBE:
            return VocoderKind::Imbe;
        case RV_VOCODER_KIND_CODEC2:
            return VocoderKind::Codec2;
        default:
            return VocoderKind::External;
    }
}

// A switch WITH a default, deliberately, and docs/conventions.md asks that the
// default do something defensible. The set of return codes is open by design:
// a plugin built against a later header can return a number this build has
// never seen, and the defensible answer is to say so and print the number
// rather than to pretend it is one of the codes we know.
[[nodiscard]] std::string abi_status_text(std::int32_t code)
{
    switch (code) {
        case RV_VOCODER_OK:
            return "success";
        case RV_VOCODER_NO_MORE:
            return "no more entries";
        case RV_VOCODER_ERR_ARGUMENT:
            return "the plugin rejected the arguments, usually a descriptor size it does not accept";
        case RV_VOCODER_ERR_BIT_COUNT:
            return "the plugin says that is not the frame size this handle decodes";
        case RV_VOCODER_ERR_CAPACITY:
            return "the plugin says the output buffer is smaller than one frame";
        case RV_VOCODER_ERR_FRAME_REJECTED:
            return "the frame was not usable, which is an ordinary outcome on a weak signal";
        case RV_VOCODER_ERR_DEVICE:
            return "the hardware behind the plugin is absent, busy or stopped answering";
        case RV_VOCODER_ERR_UNSUPPORTED:
            return "the plugin cannot provide what was asked for";
        case RV_VOCODER_ERR_INTERNAL:
            return "the plugin reported an internal failure and said no more";
        default:
            return "a code this build of Revenant has no name for, which means the plugin was "
                   "built against a newer ABI than it claims";
    }
}

[[nodiscard]] std::string terminated_name(const rv_vocoder_desc& desc)
{
    // The plugin is required to terminate it. Doing it again here costs
    // nothing and is the difference between a mis-built plugin producing a
    // truncated name and it producing a read past the end of the struct.
    const char* const first = desc.name;
    const char* const last = first + RV_VOCODER_NAME_CAPACITY;
    const char* const nul = std::find(first, last, '\0');
    return std::string(first, static_cast<std::size_t>(nul - first));
}

[[nodiscard]] VocoderFrame frame_from_desc(const rv_vocoder_desc& desc) noexcept
{
    VocoderFrame frame;
    frame.kind = kind_from_abi(desc.kind);
    frame.bit_count = desc.bit_count;
    frame.pcm_frames = desc.pcm_frames;
    frame.sample_rate = desc.sample_rate;
    return frame;
}

#ifdef _WIN32

// A plugin enumerating without end would hang the scan, and the scan runs
// during startup. Sixty-four is far past anything a real plugin offers and far
// short of a loop nobody notices.
constexpr std::uint32_t kMaxOffersPerPlugin = 64;

// A descriptor with a zero in it cannot be decoded against and cannot be
// matched: an offer of zero bits would match a caller that asked for zero bits
// and then write zero samples for ever. Dropped and named rather than kept.
[[nodiscard]] std::string descriptor_complaint(const rv_vocoder_desc& desc)
{
    if (desc.bit_count == 0) {
        return "bit_count is 0";
    }
    if (desc.pcm_frames == 0) {
        return "pcm_frames is 0";
    }
    if (desc.sample_rate == 0) {
        return "sample_rate is 0";
    }
    if (desc.struct_size != sizeof(rv_vocoder_desc)) {
        return std::format("struct_size came back as {} and this build uses {}",
                           desc.struct_size,
                           sizeof(rv_vocoder_desc));
    }
    return {};
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// The adapter
// ---------------------------------------------------------------------------

class PluginVocoder final : public Vocoder {
public:
    PluginVocoder(std::shared_ptr<VocoderPluginModule> module,
                  const rv_vocoder_desc& descriptor,
                  rv_vocoder* handle)
        : module_(std::move(module)),
          handle_(handle),
          shape_(frame_from_desc(descriptor)),
          name_(std::format("{} from {}",
                            terminated_name(descriptor),
                            module_->path.filename().string()))
    {
    }

    ~PluginVocoder() override
    {
        if (handle_ != nullptr) {
            module_->destroy(handle_);
        }
    }

    [[nodiscard]] VocoderFrame shape() const noexcept override { return shape_; }

    [[nodiscard]] Status decode(std::span<const std::uint8_t> bits, std::span<float> out) override
    {
        if (!poison_.empty()) {
            return fail(poison_);
        }

        if (Status checked = check_vocoder_call(shape_, bits, out); !checked) {
            return checked;
        }

        // Clamped rather than rejected. A caller with a buffer larger than 4
        // gigasamples is not a caller this ABI needs to serve, and the clamped
        // number is still a capacity the host owns, so understating it is
        // safe in the direction that matters.
        const std::uint32_t capacity = out.size() > std::numeric_limits<std::uint32_t>::max()
                                           ? std::numeric_limits<std::uint32_t>::max()
                                           : static_cast<std::uint32_t>(out.size());

        std::uint32_t written = 0;
        const std::int32_t code = module_->decode(handle_,
                                                  bits.data(),
                                                  shape_.bit_count,
                                                  out.data(),
                                                  capacity,
                                                  &written);

        if (code != RV_VOCODER_OK) {
            return fail(std::format("{} returned {} from decode: {}",
                                    name_,
                                    code,
                                    abi_status_text(code)),
                        code);
        }

        if (written > capacity) {
            poison_ = std::format(
                "{} reported writing {} samples into a buffer of {} and is not being called "
                "again; the plugin does not honour out_capacity, so replace it or report it "
                "to its author",
                name_,
                written,
                capacity);
            return fail(poison_);
        }

        if (written != shape_.pcm_frames) {
            return fail(std::format(
                "{} reported success and wrote {} samples where one frame is {} ({}); a partial "
                "frame is not decodable, so the frame is dropped",
                name_,
                written,
                shape_.pcm_frames,
                describe_vocoder_frame(shape_)));
        }

        return {};
    }

    void reset() override
    {
        if (poison_.empty()) {
            module_->reset(handle_);
        }
    }

    [[nodiscard]] std::string_view name() const noexcept override { return name_; }

private:
    std::shared_ptr<VocoderPluginModule> module_;
    rv_vocoder* handle_ = nullptr;
    VocoderFrame shape_;
    std::string name_;

    // Empty until the plugin breaks the contract in a way that makes every
    // later call unsafe. Non-empty is both the flag and the message.
    std::string poison_;
};

#ifdef _WIN32

// One entry point, by name, so a missing symbol can be reported as the name
// rather than as "something was missing".
struct EntryPoint {
    const char* name;
    void** slot;
};

[[nodiscard]] long long last_os_error() noexcept
{
    return static_cast<long long>(GetLastError());
}

[[nodiscard]] std::string os_error_text(long long code)
{
    // std::system_category is used rather than FormatMessage by hand because
    // the standard library already owns the Win32 mapping and the result is
    // the same sentence the rest of the tree prints for a Win32 failure.
    return std::system_category().message(static_cast<int>(code));
}

[[nodiscard]] VocoderPluginReport load_one(const std::filesystem::path& path,
                                           std::shared_ptr<VocoderPluginModule>& out_module)
{
    VocoderPluginReport report;
    report.path = path;

    // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR is the point of the Ex form: a plugin's
    // own dependencies resolve from the plugin's directory and from the system
    // directories, and not from the process working directory, which is
    // whatever shell the operator happened to start Revenant from. The flags
    // require a fully qualified path, which is why the caller made it
    // absolute.
    const HMODULE handle = LoadLibraryExW(path.c_str(),
                                          nullptr,
                                          LOAD_LIBRARY_SEARCH_DEFAULT_DIRS
                                              | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (handle == nullptr) {
        report.refusal = PluginRefusal::LoadFailed;
        report.os_error = last_os_error();
        report.detail = std::format(
            "{} did not load: Windows error {}, {}. The usual cause is a dependency of the "
            "plugin that is not beside it; put its DLLs in the same directory",
            path.filename().string(),
            report.os_error,
            os_error_text(report.os_error));
        return report;
    }

    auto module = std::make_shared<VocoderPluginModule>();
    module->path = path;
    module->handle = handle;

    rv_vocoder_abi_version_fn version_fn = nullptr;

    // The slots are function pointers and the table holds void**, because a
    // table of six differently typed pointers cannot be written as a table at
    // all. Function pointer to object pointer is conditionally supported by
    // the standard and guaranteed by Win32, which is the platform this branch
    // is compiled for and the platform GetProcAddress exists on.
    const EntryPoint entries[] = {
        {"revenant_vocoder_abi_version", reinterpret_cast<void**>(&version_fn)},
        {"revenant_vocoder_describe", reinterpret_cast<void**>(&module->describe)},
        {"revenant_vocoder_create", reinterpret_cast<void**>(&module->create)},
        {"revenant_vocoder_decode", reinterpret_cast<void**>(&module->decode)},
        {"revenant_vocoder_reset", reinterpret_cast<void**>(&module->reset)},
        {"revenant_vocoder_destroy", reinterpret_cast<void**>(&module->destroy)},
    };

    for (const EntryPoint& entry : entries) {
        FARPROC symbol = GetProcAddress(handle, entry.name);
        if (symbol == nullptr) {
            report.refusal = PluginRefusal::MissingEntryPoint;
            report.os_error = last_os_error();
            report.detail = std::format(
                "{} does not export {}, so it is not a vocoder plugin of ABI version {}; a "
                "plugin must export all {} entry points in core/decode/vocoder_abi.h",
                path.filename().string(),
                entry.name,
                RV_VOCODER_ABI_VERSION,
                std::size(entries));
            return report;
        }
        *entry.slot = reinterpret_cast<void*>(symbol);
    }

    report.abi_version = version_fn();
    if (report.abi_version != RV_VOCODER_ABI_VERSION) {
        report.refusal = PluginRefusal::AbiVersionMismatch;
        report.detail = std::format(
            "{} reports vocoder ABI version {} and this build speaks {}, so it was not loaded; "
            "rebuild the plugin against core/decode/vocoder_abi.h from this version of Revenant",
            path.filename().string(),
            report.abi_version,
            RV_VOCODER_ABI_VERSION);
        return report;
    }

    std::string rejected;
    for (std::uint32_t index = 0; index < kMaxOffersPerPlugin; ++index) {
        rv_vocoder_desc desc{};
        desc.struct_size = static_cast<std::uint32_t>(sizeof(rv_vocoder_desc));

        const std::int32_t code = module->describe(index, &desc);
        if (code == RV_VOCODER_NO_MORE) {
            break;
        }
        if (code != RV_VOCODER_OK) {
            report.refusal = PluginRefusal::DescriptorRejected;
            report.detail = std::format(
                "{} returned {} from describe({}): {}. This build passes a {}-byte "
                "rv_vocoder_desc; a plugin built against an edited copy of the header will "
                "refuse it even when the ABI version matches",
                path.filename().string(),
                code,
                index,
                abi_status_text(code),
                sizeof(rv_vocoder_desc));
            return report;
        }

        if (const std::string complaint = descriptor_complaint(desc); !complaint.empty()) {
            if (!rejected.empty()) {
                rejected += ", ";
            }
            rejected += std::format("entry {} ({})", index, complaint);
            continue;
        }

        module->descriptors.push_back(desc);
        report.offers.push_back(VocoderOffer{frame_from_desc(desc), terminated_name(desc)});
    }

    if (module->descriptors.empty()) {
        report.refusal = PluginRefusal::OffersNothing;
        report.detail = rejected.empty()
                            ? std::format("{} loaded and enumerated no vocoder at all, so there "
                                          "is nothing to open; check the plugin's configuration",
                                          path.filename().string())
                            : std::format("{} loaded and every descriptor it offered was "
                                          "unusable: {}",
                                          path.filename().string(),
                                          rejected);
        return report;
    }

    report.loaded = true;
    report.detail = std::format("{} loaded, ABI {}, offering {}",
                                path.filename().string(),
                                report.abi_version,
                                report.offers.size());
    if (!rejected.empty()) {
        report.detail += std::format(", after dropping {}", rejected);
    }

    out_module = std::move(module);
    return report;
}

#endif  // _WIN32

}  // namespace

// ---------------------------------------------------------------------------
// PluginRefusal
// ---------------------------------------------------------------------------

std::string_view plugin_refusal_name(PluginRefusal refusal) noexcept
{
    // No default label, per docs/conventions.md.
    switch (refusal) {
        case PluginRefusal::None:
            return "none";
        case PluginRefusal::LoadFailed:
            return "load-failed";
        case PluginRefusal::MissingEntryPoint:
            return "missing-entry-point";
        case PluginRefusal::AbiVersionMismatch:
            return "abi-version-mismatch";
        case PluginRefusal::DescriptorRejected:
            return "descriptor-rejected";
        case PluginRefusal::OffersNothing:
            return "offers-nothing";
        case PluginRefusal::NotSupportedOnThisPlatform:
            return "not-supported-on-this-platform";
    }
    return "invalid";
}

// ---------------------------------------------------------------------------
// VocoderPluginSet
// ---------------------------------------------------------------------------

VocoderPluginSet::~VocoderPluginSet() = default;
VocoderPluginSet::VocoderPluginSet(VocoderPluginSet&&) noexcept = default;
VocoderPluginSet& VocoderPluginSet::operator=(VocoderPluginSet&&) noexcept = default;

std::vector<VocoderOffer> VocoderPluginSet::offers() const
{
    std::vector<VocoderOffer> all;
    all.reserve(offer_count_);
    for (const VocoderPluginReport& report : reports_) {
        if (!report.loaded) {
            continue;
        }
        all.insert(all.end(), report.offers.begin(), report.offers.end());
    }
    return all;
}

std::string VocoderPluginSet::status_line() const
{
    const std::string where = directory_.empty() ? std::string("<unknown>") : directory_.string();

    if (!directory_present_) {
        // Named as a refusal with a fix rather than as an absence, because an
        // operator who expected a dongle to work needs to know which directory
        // was looked in, and that directory is usually the answer.
        return std::format(
            "no vocoder: {} does not exist, so no plugin was scanned. Create it beside the "
            "executable and put a vocoder plugin DLL in it. Decoding, framing and metadata are "
            "unaffected; only voice audio needs a vocoder",
            where);
    }

    if (reports_.empty()) {
        return std::format(
            "no vocoder: {} exists and holds no .dll. Decoding, framing and metadata are "
            "unaffected; only voice audio needs a vocoder",
            where);
    }

    if (offer_count_ == 0) {
        std::string refusals;
        for (const VocoderPluginReport& report : reports_) {
            if (!refusals.empty()) {
                refusals += "; ";
            }
            refusals += report.detail;
        }
        return std::format("no vocoder: {} file(s) in {}, none usable. {}",
                           reports_.size(),
                           where,
                           refusals);
    }

    std::string loaded;
    for (const VocoderPluginReport& report : reports_) {
        if (!report.loaded) {
            continue;
        }
        for (const VocoderOffer& offer : report.offers) {
            if (!loaded.empty()) {
                loaded += "; ";
            }
            loaded += std::format("{} ({}) from {}",
                                  offer.name,
                                  describe_vocoder_frame(offer.frame),
                                  report.path.filename().string());
        }
    }

    const std::size_t refused = reports_.size()
                                - static_cast<std::size_t>(std::count_if(
                                    reports_.begin(), reports_.end(), [](const auto& r) {
                                        return r.loaded;
                                    }));

    std::string line = std::format("{} vocoder(s) from {}: {}", offer_count_, where, loaded);
    if (refused != 0) {
        line += std::format(". {} file(s) in the same directory were refused", refused);
    }
    return line;
}

Expected<std::unique_ptr<Vocoder>> VocoderPluginSet::open(const VocoderFrame& want) const
{
    for (const std::shared_ptr<VocoderPluginModule>& module : modules_) {
        for (const rv_vocoder_desc& desc : module->descriptors) {
            if (frame_from_desc(desc) != want) {
                continue;
            }

            rv_vocoder* const handle = module->create(&desc);
            if (handle == nullptr) {
                // Matched and still refused. That is a runtime condition, not
                // a configuration one, so the fix named here is a different
                // fix from the no-match case below.
                return fail(std::format(
                    "{} offers {} and refused to open it. A plugin that enumerates a vocoder and "
                    "then declines usually has no hardware attached, or has it open already in "
                    "another handle or another process",
                    module->path.filename().string(),
                    describe_vocoder_frame(want)));
            }

            return std::unique_ptr<Vocoder>(new PluginVocoder(module, desc, handle));
        }
    }

    std::string available;
    for (const VocoderOffer& offer : offers()) {
        if (!available.empty()) {
            available += "; ";
        }
        available += std::format("{} ({})", offer.name, describe_vocoder_frame(offer.frame));
    }
    if (available.empty()) {
        available = "nothing";
    }

    return fail(std::format(
        "no loaded vocoder provides {}. {} currently offers {}. This is not a fault in the "
        "signal: the mode still demodulates, frames and reports metadata, and only the voice "
        "audio needs a plugin that provides that frame shape",
        describe_vocoder_frame(want),
        directory_.empty() ? std::string("<unknown>") : directory_.string(),
        available));
}

std::filesystem::path default_vocoder_plugin_directory()
{
#ifdef _WIN32
    // Grown rather than assumed, because MAX_PATH stopped being the limit and
    // a truncated executable path would resolve to a directory that exists
    // somewhere else.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr,
                                                 buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            buffer.resize(written);
            break;
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }

    const std::filesystem::path executable(buffer);
    return executable.parent_path() / std::filesystem::path(kVocoderPluginDirectoryName);
#else
    return {};
#endif
}

VocoderPluginSet scan_vocoder_plugins(const VocoderPluginScanOptions& options)
{
    VocoderPluginSet set;
    set.directory_ = options.directory.value_or(default_vocoder_plugin_directory());

#ifndef _WIN32
    // The same shape as every other outcome: one report, one reason, and a
    // status line that says what an operator can do about it. A silent empty
    // set here would be indistinguishable from a machine with no plugins
    // installed, which is the confusion this whole file exists to prevent.
    VocoderPluginReport unsupported;
    unsupported.path = set.directory_;
    unsupported.refusal = PluginRefusal::NotSupportedOnThisPlatform;
    unsupported.detail =
        "vocoder plugins are loaded with LoadLibrary and this build is not for Windows; the "
        "dlopen path is not written yet, so no plugin can be loaded here";
    set.reports_.push_back(std::move(unsupported));
    return set;
#else
    std::error_code ec;
    if (set.directory_.empty() || !std::filesystem::is_directory(set.directory_, ec)) {
        return set;
    }
    set.directory_present_ = true;

    // Absolute because LoadLibraryExW's search flags require a fully qualified
    // path, and because a relative directory in configuration would otherwise
    // resolve against the working directory on every rescan.
    const std::filesystem::path root = std::filesystem::absolute(set.directory_, ec);
    if (ec) {
        set.directory_present_ = false;
        return set;
    }
    set.directory_ = root;

    std::vector<std::filesystem::path> candidates;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::string text = entry.path().extension().string();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (text == ".dll") {
            candidates.push_back(entry.path());
        }
    }

    // Sorted because directory order is whatever the filesystem feels like,
    // and both the report and the order open() matches in have to be the same
    // on two machines for a bug report about either to mean anything.
    std::sort(candidates.begin(), candidates.end());

    for (const std::filesystem::path& candidate : candidates) {
        std::shared_ptr<VocoderPluginModule> module;
        VocoderPluginReport report = load_one(candidate, module);
        if (module) {
            set.offer_count_ += module->descriptors.size();
            set.modules_.push_back(std::move(module));
        }
        set.reports_.push_back(std::move(report));
    }

    return set;
#endif
}

}  // namespace revenant::decode
