// A device's calibration, and where it is kept between sessions.
//
// Three settings per physical radio: its crystal's frequency error, whether
// the engine removes the zero-IF centre spike, and whether it corrects the
// I/Q imbalance that puts a mirror image of every signal on the other side of
// the centre. docs/calibration.md has what each does and what each costs.
//
// KEPT BY THE ENGINE, KEYED BY THE DEVICE'S SERIAL. The engine is what opens
// the radio, including when no client is connected at all: revenant-cli and a
// revenant-engine started with a URI on its command line both open a dongle
// with nobody watching, and a calibration held by the client would leave both
// uncalibrated. It is also the side the radio is plugged into. A client on
// another machine holding the correction for a dongle it cannot see would
// apply it to whatever that engine happens to open next.
//
// THE KEY IS THE BACKEND AND THE SERIAL, "rtlsdr:00000001". An index is not a
// key: rtlsdr://0 is whichever dongle enumerated first, and two dongles swap
// places across a replug. A serial is not unique either, and that is the
// field's real weakness: RTL-SDRs ship with "00000001" and most are never
// reprogrammed, so two stock dongles share one calibration. rtl_eeprom -s
// writes a distinct one; docs/calibration.md says so where an operator will
// read it.

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>

#include "core/error.h"
#include "core/source/capabilities.h"

namespace revenant::source {

struct DeviceCalibration {
    // The crystal's error in parts per billion, positive when it runs fast.
    // core/source/frequency_correction.h has the sign and the arithmetic.
    std::int64_t correction_ppb = 0;

    // Subtract a running mean of the stream: the zero-IF centre spike.
    bool dc_removal = false;

    // Correct the amplitude and phase mismatch between I and Q.
    bool iq_correction = false;

    [[nodiscard]] bool operator==(const DeviceCalibration&) const = default;
};

// Every device's calibration, by key. Ordered so the file comes out the same
// way every time it is written, and a diff of it shows what changed.
using CalibrationTable = std::map<std::string, DeviceCalibration, std::less<>>;

// "backend:serial", or empty for a source that carries no serial. A file and
// a synthetic scene have none: a recording is calibrated by whoever made it,
// and a scene has no crystal.
[[nodiscard]] std::string calibration_key(const SourceCapabilities& caps);

// Refused when it cannot be written as one field of one line: empty, or
// holding a tab, a line break or any other control character. A USB serial is
// printable ASCII in practice, and this says so rather than escaping.
[[nodiscard]] Status validate_calibration_key(std::string_view key);

// The file's grammar, one device a line:
//
//   # comment
//   rtlsdr:00000001<TAB>ppb=-1250<TAB>dc=on<TAB>iq=off
//
// A field this build does not know is skipped rather than refused, so a file
// written by a later engine does not cost an earlier one every device in it.
// A field it does know with a value it cannot read is refused, naming the
// line, because guessing would apply a correction nobody measured.
[[nodiscard]] Expected<CalibrationTable> parse_calibration_table(std::string_view text);
[[nodiscard]] std::string format_calibration_table(const CalibrationTable& table);

// A missing file is an empty table, which is every machine's first run.
[[nodiscard]] Expected<CalibrationTable> load_calibration_table(const std::filesystem::path& path);

// Written beside the target and renamed over it, so a crash half way through
// leaves the old file whole rather than a truncated one. The directory is
// created if it is missing.
[[nodiscard]] Status save_calibration_table(const std::filesystem::path& path,
                                            const CalibrationTable& table);

}  // namespace revenant::source
