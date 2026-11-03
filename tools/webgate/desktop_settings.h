// What the desktop client has saved that the phone follows.
//
// The owner's decision of 2026-11-03: the phone has no radio settings of its
// own. Opening a radio uses the front end the desktop last opened, the
// engine-wide switches are the desktop's, and the detection confidence bar
// starts where the desktop's is. Everything here is read from the desktop's
// QSettings (org "Locke Werks", app "Revenant"), which on Windows is
// HKCU\Software\Locke Werks\Revenant\<group>, with the key names of
// ui/models/settings.h:
//
//   engine/lastSource         the URI the desktop last opened, front end
//                             parameters included, with what that radio was
//                             left at laid over it by the desktop's own
//                             restore_radio_uri: source/lastRadio names the
//                             radio and source/radios/<radio>/{sampleRate,
//                             centreHz,gain,directSampling} are the values
//   radio/impulseExcision     engine-wide, on or off
//   radio/combCancel          engine-wide, on or off
//   radio/spurCancel          engine-wide, on or off
//   detections/thresholdDb    engine-wide detection threshold
//   detections/confidenceBar  the desktop's own filter on the detections list
//   speech/transcribe         engine-wide speech to text switch
//
// A value the desktop has never written is absent here, and absent means
// nothing is sent: the desktop's rule for its engine-wide switches too, since
// a window that was never told anything leaves another client's setting alone.
//
// TWO USES.
//
//   Engine switches. When a phone's session starts, and so again after an
//   engine restart (which builds a new session), the gateway sends the ones
//   present, the way EngineLink does on every desktop connection.
//
//   The "desktop" event, which is how the phone learns the rest:
//
//     {"event":"desktop",
//      "confidenceBar":0.91,                  absent when unset
//      "radio":{"uri":"rtlsdr://0?agc=1&...",  absent when unset
//               "backend":"rtlsdr", "device":"0",
//               "params":{"agc":"1","bw":"3000000",...}}}
//
//   Sent on connect, on resume, and whenever the registry changes. The phone
//   applies the radio's parameters when it opens a radio on the same device,
//   web/src/ui/radio_panel.ts desktopOpenUri, and starts its confidence slider
//   at confidenceBar unless the phone's user has moved it.
//
// Only a desktop on this machine is followed, for the reason display_sync.h
// gives.

#pragma once

#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tools/webgate/display_sync.h"

namespace revenant::webgate {

struct DesktopSettings {
    std::optional<std::string> last_source;
    std::optional<bool> impulse_excision;
    std::optional<bool> comb_cancel;
    std::optional<bool> spur_cancel;
    std::optional<bool> transcribe;
    std::optional<double> detection_threshold_db;
    // Already through the desktop's restore rules (models/detector_settings.h).
    std::optional<double> confidence_bar;

    friend bool operator==(const DesktopSettings&, const DesktopSettings&) = default;
};

// One value by its group and name ("radio", "combCancel"); nullopt when absent.
using DesktopValueReader =
    std::function<std::optional<std::string>(std::string_view group, std::string_view name)>;

[[nodiscard]] DesktopSettings read_desktop_settings(const DesktopValueReader& read);

// HKCU\Software\Locke Werks\Revenant\<group>. Answers nothing off Windows.
[[nodiscard]] DesktopValueReader registry_desktop_reader();

// A source URI split the way the phone needs it. backend is the scheme and
// device the text between "://" and the query, an index ("0") or a serial.
struct ParsedSource {
    std::string uri;
    std::string backend;
    std::string device;
    std::vector<std::pair<std::string, std::string>> params;
};

[[nodiscard]] std::optional<ParsedSource> parse_source_uri(std::string_view uri);

// The "desktop" event for these settings; the same text for the same settings,
// so a caller compares strings to see a change.
[[nodiscard]] std::string desktop_event(const DesktopSettings& settings);

// The settings, re-read at most once an interval however often they are asked
// for. Any thread.
class DesktopSettingsSource {
public:
    using Clock = std::chrono::steady_clock;

    explicit DesktopSettingsSource(DesktopValueReader reader,
                                   std::function<Clock::time_point()> clock = {},
                                   std::chrono::milliseconds interval = std::chrono::seconds(2));

    [[nodiscard]] DesktopSettings current();

private:
    DesktopValueReader reader_;
    std::function<Clock::time_point()> clock_;
    std::chrono::milliseconds interval_;
    std::mutex mutex_;
    std::optional<Clock::time_point> read_at_;
    DesktopSettings settings_;
};

}  // namespace revenant::webgate
