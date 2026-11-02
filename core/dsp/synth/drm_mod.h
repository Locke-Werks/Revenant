// DRM30 transmitter for the round trip in tests/decode/test_drm.cpp.
//
// SPECIFICATION
//
// ETSI ES 201 980 V4.2.1, the clauses core/decode/drm.h cites: the FAC and
// SDC of 6.3 and 6.4, the multiplex of 6.2, energy dispersal, multilevel
// coding, cell interleaving and mapping of clause 7, and the OFDM super frame
// of clause 8 with every pilot. The signal is complex baseband at 48 kS/s with
// the reference frequency at 0 Hz.
//
// No deviation from the document in what is sent. The audio streams carry
// whatever bytes the caller supplies: there is no AAC encoder here, by the
// same licensing reasoning as the receiver.

#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/drm.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::siggen {

using dsp::Complex32;

struct DrmTxConfig {
    decode::DrmMode mode = decode::DrmMode::B;
    int occupancy = 3;
    decode::DrmQam msc = decode::DrmQam::Qam64;
    bool long_interleave = false;
    int sdc_mode = 1;  // 0 16-QAM, 1 4-QAM
    int protection_a = 0;
    int protection_b = 1;
    std::vector<decode::DrmStreamDescription> streams;
    decode::DrmFacService service;
    std::uint8_t services_code = 0b0100;  // one audio service
    std::uint8_t afs_index = 0;
    double amplitude = 1.0;
};

struct DrmTxSuperFrame {
    // The SDC data field, at most drm_sdc_data_bytes() bytes.
    std::vector<std::uint8_t> sdc;
    // Per multiplex frame, per stream: the logical frame, part A then part B.
    std::array<std::vector<std::vector<std::uint8_t>>, 3> logical;
};

class DrmModulator {
   public:
    [[nodiscard]] static Expected<DrmModulator> create(const DrmTxConfig& config);

    // Appends one transmission super frame of baseband.
    [[nodiscard]] Status super_frame(const DrmTxSuperFrame& frame, std::vector<Complex32>& out);

    [[nodiscard]] const decode::DrmStructure& structure() const { return structure_; }
    [[nodiscard]] std::size_t sdc_data_bytes() const;
    [[nodiscard]] std::size_t multiplex_bytes() const;  // floor((L1 + L2) / 8)

   private:
    DrmModulator() = default;
    DrmTxConfig config_{};
    decode::DrmStructure structure_{};
    decode::DrmCodingPlan msc_plan_{};
    decode::DrmCodingPlan sdc_plan_{};
    decode::DrmFft fft_{};
    std::deque<std::vector<Complex32>> history_;  // long interleaving, newest last
};

// SDC data entity helpers, ES 6.4.3.
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_multiplex(int protection_a, int protection_b,
                                                          std::span<const decode::DrmStreamDescription> s);
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_label(std::uint8_t short_id, std::string_view label);
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_time(std::uint32_t mjd, std::uint8_t hours,
                                                     std::uint8_t minutes);
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_audio(std::uint8_t short_id, std::uint8_t stream,
                                                      bool text);
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_packet_app(std::uint8_t short_id, std::uint8_t stream,
                                                           std::uint8_t packet_id, bool data_units,
                                                           std::uint8_t packet_length);

}  // namespace revenant::siggen
