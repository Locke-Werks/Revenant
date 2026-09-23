#include "core/source/calibration.h"

#include <charconv>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "core/source/frequency_correction.h"

namespace revenant::source {
namespace {

constexpr std::string_view kHeader =
    "# Revenant device calibration, written by the engine. One device a line:\n"
    "# its key, then ppb= (crystal error, parts per billion, positive when it\n"
    "# runs fast), dc= and iq= (on or off), separated by tabs.\n"
    "# docs/calibration.md describes each setting.\n";

[[nodiscard]] std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> out;
    std::size_t from = 0;
    for (;;) {
        const std::size_t at = text.find(separator, from);
        if (at == std::string_view::npos) {
            out.push_back(text.substr(from));
            return out;
        }
        out.push_back(text.substr(from, at - from));
        from = at + 1;
    }
}

[[nodiscard]] Expected<bool> on_off(std::string_view value, std::string_view field,
                                    std::size_t line) {
    if (value == "on") {
        return true;
    }
    if (value == "off") {
        return false;
    }
    return fail(std::format("calibration line {}: {}='{}' is neither on nor off", line, field,
                            value));
}

}  // namespace

std::string calibration_key(const SourceCapabilities& caps) {
    if (caps.serial.empty() || caps.backend.empty()) {
        return {};
    }
    return caps.backend + ":" + caps.serial;
}

Status validate_calibration_key(std::string_view key) {
    if (key.empty()) {
        return fail("a calibration key cannot be empty");
    }
    for (const char c : key) {
        const auto code = static_cast<unsigned char>(c);
        if (code < 0x20 || code == 0x7F) {
            return fail(std::format(
                "the calibration key '{}' holds a control character, so it cannot be one field "
                "of one line in the calibration file. A USB serial is printable text; "
                "rtl_eeprom -s writes a new one.",
                key));
        }
    }
    return {};
}

Expected<CalibrationTable> parse_calibration_table(std::string_view text) {
    CalibrationTable table;
    std::size_t line_number = 0;
    for (std::string_view line : split(text, '\n')) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }

        const std::vector<std::string_view> fields = split(line, '\t');
        const std::string_view key = fields.front();
        if (auto ok = validate_calibration_key(key); !ok) {
            return std::unexpected(
                with_context(ok.error(), std::format("calibration line {}", line_number)));
        }
        if (table.contains(key)) {
            return fail(std::format(
                "calibration line {}: '{}' appears twice, and which of the two to believe is "
                "not something to guess",
                line_number, key));
        }

        DeviceCalibration entry;
        for (std::size_t i = 1; i < fields.size(); ++i) {
            const std::string_view field = fields[i];
            const std::size_t equals = field.find('=');
            if (equals == std::string_view::npos) {
                return fail(std::format("calibration line {}: '{}' is not name=value",
                                        line_number, field));
            }
            const std::string_view name = field.substr(0, equals);
            const std::string_view value = field.substr(equals + 1);

            if (name == "ppb") {
                std::int64_t ppb = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), ppb);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                    return fail(std::format("calibration line {}: ppb='{}' is not a whole number",
                                            line_number, value));
                }
                if (!correction_in_range(ppb)) {
                    return fail(std::format(
                        "calibration line {}: ppb={} is past the {} ppb either way that a "
                        "crystal error can be",
                        line_number, ppb, kMaxCorrectionPpb));
                }
                entry.correction_ppb = ppb;
            } else if (name == "dc") {
                auto on = on_off(value, name, line_number);
                if (!on) {
                    return std::unexpected(on.error());
                }
                entry.dc_removal = *on;
            } else if (name == "iq") {
                auto on = on_off(value, name, line_number);
                if (!on) {
                    return std::unexpected(on.error());
                }
                entry.iq_correction = *on;
            }
            // Anything else is a later engine's field. Skipped, see the header.
        }
        table.emplace(std::string(key), entry);
    }
    return table;
}

std::string format_calibration_table(const CalibrationTable& table) {
    std::string out(kHeader);
    for (const auto& [key, entry] : table) {
        out += std::format("{}\tppb={}\tdc={}\tiq={}\n", key, entry.correction_ppb,
                           entry.dc_removal ? "on" : "off", entry.iq_correction ? "on" : "off");
    }
    return out;
}

Expected<CalibrationTable> load_calibration_table(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return fail(std::format("could not look for the calibration file {}: {}",
                                    path.string(), error.message()),
                        error.value());
        }
        return CalibrationTable{};
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(std::format("could not open the calibration file {}", path.string()));
    }
    std::ostringstream contents;
    contents << in.rdbuf();

    auto table = parse_calibration_table(contents.str());
    if (!table) {
        return std::unexpected(with_context(table.error(), path.string()));
    }
    return table;
}

Status save_calibration_table(const std::filesystem::path& path, const CalibrationTable& table) {
    for (const auto& [key, entry] : table) {
        if (auto ok = validate_calibration_key(key); !ok) {
            return ok;
        }
        if (!correction_in_range(entry.correction_ppb)) {
            return fail(std::format("'{}' carries a correction of {} ppb, past the {} ppb a "
                                    "crystal error can be",
                                    key, entry.correction_ppb, kMaxCorrectionPpb));
        }
    }

    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return fail(std::format("could not create {} for the calibration file: {}",
                                    path.parent_path().string(), error.message()),
                        error.value());
        }
    }

    std::filesystem::path staged = path;
    staged += ".partial";
    {
        std::ofstream out(staged, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(std::format("could not write {}", staged.string()));
        }
        const std::string text = format_calibration_table(table);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            return fail(std::format("writing {} failed part way", staged.string()));
        }
    }

    // MSVC's rename is MoveFileExW with MOVEFILE_REPLACE_EXISTING, so an
    // existing file is replaced in one step rather than removed first.
    std::filesystem::rename(staged, path, error);
    if (error) {
        const std::string why = error.message();
        const int code = error.value();
        std::filesystem::remove(staged, error);
        return fail(std::format("could not move the new calibration file into place at {}: {}",
                                path.string(), why),
                    code);
    }
    return {};
}

}  // namespace revenant::source
