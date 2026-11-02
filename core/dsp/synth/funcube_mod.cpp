#include "core/dsp/synth/funcube_mod.h"

#include <algorithm>

namespace revenant::siggen {

namespace {

class BitWriter {
public:
    explicit BitWriter(std::vector<std::uint8_t>& octets) : octets_(octets) {}
    void put(std::uint32_t value, unsigned bits) {
        for (unsigned i = 0; i < bits; ++i, ++pos_) {
            const auto b = static_cast<std::uint8_t>((value >> (bits - 1U - i)) & 1U);
            octets_[pos_ / 8] = static_cast<std::uint8_t>(octets_[pos_ / 8] | (b << (7U - (pos_ % 8U))));
        }
    }

private:
    std::vector<std::uint8_t>& octets_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<std::uint8_t> funcube_symbols(std::span<const std::vector<std::uint8_t>> frames, bool differential) {
    std::vector<std::uint8_t> out;
    std::uint8_t level = 0;
    for (const auto& frame : frames) {
        for (const auto b : decode::funcube_encode_block(frame)) {
            if (differential) {
                level ^= b;
                out.push_back(level);
            } else {
                out.push_back(b);
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> funcube_build_frame(std::uint8_t satellite_id, std::uint8_t frame_type,
                                              const decode::FuncubeRealTime& t,
                                              std::span<const std::uint8_t> payload) {
    std::vector<std::uint8_t> f(decode::kFuncubeFrameOctets, 0);
    BitWriter w(f);
    w.put(satellite_id, 2);
    w.put(frame_type, 6);
    for (const auto v : t.photo_voltage) {
        w.put(v, 16);
    }
    w.put(t.total_photo_current, 16);
    w.put(t.battery_voltage, 16);
    w.put(t.total_system_current, 16);
    w.put(t.reboot_count, 16);
    w.put(t.eps_software_errors, 16);
    for (const auto v : t.boost_converter_temp) {
        w.put(v, 8);
    }
    w.put(t.battery_temp, 8);
    w.put(t.latch_up_5v1, 8);
    w.put(t.latch_up_3v3_1, 8);
    w.put(t.reset_cause, 8);
    w.put(t.power_point_tracking_mode, 8);
    for (const auto v : t.bob) {
        w.put(v, 10);
    }
    for (const auto v : t.rf) {
        w.put(v, 8);
    }
    for (const auto v : t.pa) {
        w.put(v, 8);
    }
    for (const auto v : t.antenna_temp) {
        w.put(v, 8);
    }
    for (const auto v : t.antenna_deployed) {
        w.put(v ? 1U : 0U, 1);
    }
    w.put(t.sequence_number, 24);
    w.put(t.dtmf_command_count, 6);
    w.put(t.dtmf_last_command, 5);
    w.put(t.dtmf_command_success ? 1U : 0U, 1);
    for (const auto v : t.data_valid) {
        w.put(v ? 1U : 0U, 1);
    }
    w.put(t.in_eclipse ? 1U : 0U, 1);
    w.put(t.in_safe_mode ? 1U : 0U, 1);
    w.put(t.hardware_abf ? 1U : 0U, 1);
    w.put(t.software_abf ? 1U : 0U, 1);
    w.put(t.deployment_wait ? 1U : 0U, 1);
    std::copy_n(payload.begin(), std::min<std::size_t>(payload.size(), 200), f.begin() + 56);
    return f;
}

}  // namespace revenant::siggen
