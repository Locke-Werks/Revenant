// DRM30: complex baseband to the Fast Access Channel, the Service Description
// Channel and the Main Service Channel's streams.
//
// SPECIFICATION
//
// ETSI ES 201 980 V4.2.1 (2021-01), "Digital Radio Mondiale (DRM); System
// Specification", from www.etsi.org/deliver/etsi_es/201900_201999/201980/
// 04.02.01_60/, read 2026-11-02. Cited as "ES" with a clause or table. ETSI
// retains copyright; this file carries the clause-cited constants a receiver
// needs and no more of the document's tables than that. Where a table is
// derivable from a rule the document also states (the cell counts of tables
// 21, 25 and 41 to 44, the bit counts of Annex J), the rule is implemented and
// tests/decode/test_drm.cpp checks it regenerates the table.
//
// The PDF's two-column tables do not survive a layout-preserving text
// extraction: tables 62 to 65 (FAC cell positions) come out shifted by one or
// two symbol rows, and table 50's rows are misaligned. The positions here were
// read from a raw-order extraction instead, and the cell counts that follow
// from them match tables 25 and 41 to 44 exactly, which a shifted table does
// not (it puts FAC cells on gain references).
//
// WHAT IS DECODED
//
//   - Robustness modes A to D at 48 kS/s, every spectrum occupancy the mode
//     allows (ES 8.1 to 8.3): guard interval correlation for mode, timing and
//     fractional frequency; frequency references for the integer offset; time
//     references for the frame start; gain references for the channel
//     (ES 8.4). The FFT sizes 1152, 704 and 448 are not powers of two, so the
//     file carries its own mixed-radix FFT, textbook Cooley-Tukey.
//   - The FAC (ES 6.3, 7.5.3): 4-QAM, rate 3/5, CRC-8; channel and service
//     parameters.
//   - The SDC (ES 6.4, 7.5.2): 4- or 16-QAM, CRC-16, every data entity framed,
//     and types 0 (multiplex description), 1 (label), 5 (application
//     information), 8 (time and date) and 9 (audio information) parsed.
//   - The MSC (ES 6.2, 7.2 to 7.7): 16- or 64-QAM, equal or unequal
//     protection, short or long cell interleaving, multilevel decoding (each
//     level decoded independently, no iteration), energy dispersal, and the
//     multiplex frame cut into per-stream logical frames.
//   - Text messages (ES 6.5) from audio streams, and packet mode (ES 6.6)
//     packets and data units from data streams, as raw bytes: a Journaline or
//     MOT data group is what comes out, undecoded.
//
// WHERE IT STOPS
//
// Audio. DRM audio is AAC or xHE-AAC, both licensed (docs/modes.md), so an
// audio stream's logical frame is reported as bytes and its text message is
// extracted; the audio super frame inside it is not parsed or decoded.
// Robustness mode E (DRM+) needs a 100 kHz channel and so a higher sample
// rate than this receiver takes; not implemented. The alternative frequency,
// announcement and other SDC entity types are framed and reported raw.
//
// DEVIATIONS AND ENGINEERING CHOICES
//
//   - Timing and frequency are measured at acquisition and held, as in
//     core/decode/ofdm.h; a long capture with a drifting clock needs reset().
//   - The channel estimate is linear interpolation of the gain references in
//     time and then in frequency, not the Wiener filter a production receiver
//     would use.
//   - Until the first FAC with a good CRC, the cell map is built over the union
//     of every spectrum occupancy's carriers, without the edge boosts of
//     table 59; the FAC carriers sit inside every occupancy, so that is enough
//     to read it.
//
// CLEAN ROOM
//
// No DRM implementation was read: not Dream, not gr-drm, not any other. The
// FFT, Viterbi (core/decode/dv_codes.h) and the estimators are textbook.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::Complex32;
using dsp::ConstComplexSpan;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// Transmission structure, ES clause 8
// ---------------------------------------------------------------------------

inline constexpr SampleRate kDrmSampleRate = 48000;

enum class DrmMode : std::uint8_t { A = 0, B = 1, C = 2, D = 3 };

struct DrmModeParams {
    std::size_t tu = 0;  // useful part in samples at 48 kS/s, ES table 47
    std::size_t tg = 0;  // guard interval
    std::size_t ns = 0;  // symbols per transmission frame
    int x = 0, y = 0, k0 = 0;  // gain reference pattern, ES table 60
};

[[nodiscard]] DrmModeParams drm_mode_params(DrmMode mode);
[[nodiscard]] char drm_mode_letter(DrmMode mode);

// ES table 49. False where the table has a dash.
[[nodiscard]] bool drm_occupancy_valid(DrmMode mode, int occupancy);

enum class DrmCell : std::uint8_t { Unused, Frequency, Time, Gain, Fac, Sdc, Msc };

// One transmission super frame's cells (three transmission frames).
struct DrmStructure {
    DrmMode mode = DrmMode::A;
    int occupancy = 0;  // -1 for the acquisition union described in the header
    int kmin = 0;
    int kmax = 0;
    std::size_t ns = 0;

    [[nodiscard]] std::size_t carriers() const { return static_cast<std::size_t>(kmax - kmin + 1); }
    [[nodiscard]] std::size_t index(std::size_t frame, std::size_t symbol, int k) const {
        return (frame * ns + symbol) * carriers() + static_cast<std::size_t>(k - kmin);
    }

    std::vector<DrmCell> type;      // per cell
    std::vector<Complex32> pilot;   // reference value where type is a pilot

    // Cell indices in the order ES 8.5.2.2, 8.5.3.2 and 8.6.2 assign
    // constellation points: FAC per frame, SDC and MSC per super frame.
    std::array<std::vector<std::uint32_t>, 3> fac;
    std::vector<std::uint32_t> sdc;
    std::vector<std::uint32_t> msc;  // N_SFA cells

    [[nodiscard]] std::size_t n_mux() const { return msc.size() / 3; }  // ES 7.7
};

[[nodiscard]] Expected<DrmStructure> drm_structure(DrmMode mode, int occupancy);

// ---------------------------------------------------------------------------
// FFT for the sizes clause 8 needs at 48 kS/s
// ---------------------------------------------------------------------------

// Mixed-radix decimation in time for any size whose prime factors are small.
// Forward is sum x[n] exp(-j 2 pi k n / N), unscaled; inverse the conjugate.
class DrmFft {
   public:
    DrmFft() = default;
    explicit DrmFft(std::size_t n);
    void transform(std::vector<Complex32>& data, bool inverse) const;
    [[nodiscard]] std::size_t size() const { return n_; }

   private:
    void recurse(const Complex32* in, Complex32* out, std::size_t n, std::size_t stride,
                 const std::size_t* factors, bool inverse) const;
    std::size_t n_ = 0;
    std::vector<std::size_t> factors_;
    std::vector<Complex32> twiddle_;  // exp(-j 2 pi i / n_)
};

// ---------------------------------------------------------------------------
// Channel coding, ES clause 7
// ---------------------------------------------------------------------------

// ES 7.2.2: the energy dispersal PRBS, x^9 + x^5 + 1 from all ones.
[[nodiscard]] std::vector<std::uint8_t> drm_prbs(std::size_t length);

// ES Annex D: CRC over bits, MSB first, register initialised to ones, result
// inverted. `polynomial` is without the x^n term.
[[nodiscard]] std::uint32_t drm_crc(std::span<const std::uint8_t> bits, unsigned width,
                                    std::uint32_t polynomial);
inline constexpr std::uint32_t kDrmCrc8Poly = 0x1D;     // x^8+x^4+x^3+x^2+1
inline constexpr std::uint32_t kDrmCrc16Poly = 0x1021;  // x^16+x^12+x^5+1

// ES 7.3.3 and 7.6: the block interleaver permutation, y[i] = v[perm[i]].
[[nodiscard]] std::vector<std::uint32_t> drm_interleaver(std::size_t xin, unsigned t);

struct DrmRate {
    int rx = 1;
    int ry = 2;
};

// ES table 27 and table 28: puncture patterns as six rows of a period, row
// major, one byte per entry.
struct DrmPuncture {
    std::size_t period = 1;
    std::array<std::vector<std::uint8_t>, 6> rows;
};
[[nodiscard]] Expected<DrmPuncture> drm_puncture(DrmRate rate);
[[nodiscard]] DrmPuncture drm_tail_puncture(int rp);

enum class DrmQam : std::uint8_t { Qam4 = 1, Qam16 = 2, Qam64 = 3 };  // value is Pmax

// The layout of one multilevel-coded block, ES 7.2.1 and 7.3.1.
struct DrmCodingPlan {
    DrmQam qam = DrmQam::Qam4;
    std::size_t n1 = 0;  // cells in the higher protected part
    std::size_t n2 = 0;  // cells in the lower protected part, tail included
    std::array<DrmRate, 3> rate_a{};
    std::array<DrmRate, 3> rate_b{};
    std::array<std::size_t, 3> m1{};  // input bits per level, part A
    std::array<std::size_t, 3> m2{};  // part B
    std::array<int, 3> rp{};          // tail pattern index per level
    std::size_t l1 = 0;
    std::size_t l2 = 0;
    // The FAC punctures its tail with table 27 rather than table 28 (ES 7.3.2).
    bool fac_tail = false;
    [[nodiscard]] std::size_t levels() const { return static_cast<std::size_t>(qam); }
};

// EEP or UEP plan. For EEP pass n1 = 0.
[[nodiscard]] Expected<DrmCodingPlan> drm_plan(DrmQam qam, std::size_t n1, std::size_t n2,
                                               std::span<const DrmRate> rate_a,
                                               std::span<const DrmRate> rate_b);
// The FAC's plan, ES 7.2.1.2 and table 39.
[[nodiscard]] DrmCodingPlan drm_fac_plan();
// The SDC's plan for SDC mode 0 (16-QAM) or 1 (4-QAM), ES 7.5.2.
[[nodiscard]] Expected<DrmCodingPlan> drm_sdc_plan(std::size_t n_sdc, int sdc_mode);

// The MSC's per-level code rates for a protection level, ES tables 30 and 32,
// and RYlcm.
struct DrmMscRates {
    std::array<DrmRate, 3> rates{};
    int ry_lcm = 1;
};
[[nodiscard]] Expected<DrmMscRates> drm_msc_rates(DrmQam qam, int protection_level);

// ES 7.2.1.1: cells in part A for `part_a_bytes` bytes.
[[nodiscard]] std::size_t drm_msc_n1(DrmQam qam, int protection_level_a, std::size_t part_a_bytes);
[[nodiscard]] Expected<DrmCodingPlan> drm_msc_plan(DrmQam qam, int protection_a, int protection_b,
                                                   std::size_t part_a_bytes, std::size_t n_mux);

// ES 7.4: QAM point for bits, and per-bit soft values for a received point.
// `bits` are in level order (y0, y1, y2) for the I axis then the Q axis.
[[nodiscard]] Complex32 drm_qam_point(DrmQam qam, std::span<const std::uint8_t> i_bits,
                                      std::span<const std::uint8_t> q_bits);

// Encode L1 + L2 information bits (after energy dispersal) to N1 + N2 cells,
// ES 7.3 and 7.4.
[[nodiscard]] Expected<std::vector<Complex32>> drm_mlc_encode(const DrmCodingPlan& plan,
                                                              std::span<const std::uint8_t> bits);

// Decode N1 + N2 equalised cells with per-cell weights (|H|^2 or any value
// proportional to SNR) to L1 + L2 bits, still energy-dispersed.
[[nodiscard]] Expected<std::vector<std::uint8_t>> drm_mlc_decode(const DrmCodingPlan& plan,
                                                                 std::span<const Complex32> cells,
                                                                 std::span<const float> weights);

// ---------------------------------------------------------------------------
// FAC, ES 6.3
// ---------------------------------------------------------------------------

struct DrmFacChannel {
    bool enhancement = false;
    std::uint8_t identity = 0;   // ES 6.3.3
    bool rm = false;
    std::uint8_t occupancy = 0;
    bool short_interleave = false;  // interleaver depth flag: 1 = 400 ms
    std::uint8_t msc_mode = 0;      // 00 64-QAM, 11 16-QAM
    std::uint8_t sdc_mode = 0;      // 0 16-QAM, 1 4-QAM
    std::uint8_t services = 0;      // number of services code
    std::uint8_t reconfiguration = 0;
    bool toggle = false;
    bool rfu = false;
};

struct DrmFacService {
    std::uint32_t service_id = 0;
    std::uint8_t short_id = 0;
    bool audio_ca = false;
    std::uint8_t language = 0;
    bool data = false;
    std::uint8_t descriptor = 0;
    bool data_ca = false;
    std::uint8_t rfa = 0;
};

struct DrmFac {
    DrmFacChannel channel;
    DrmFacService service;
    bool crc_ok = false;
};

// 72 bits: 20 channel, 44 service, 8 CRC.
[[nodiscard]] std::vector<std::uint8_t> drm_fac_bits(const DrmFacChannel& c, const DrmFacService& s);
[[nodiscard]] DrmFac drm_parse_fac(std::span<const std::uint8_t> bits);

// ---------------------------------------------------------------------------
// SDC, ES 6.4
// ---------------------------------------------------------------------------

// ES table 21 by rule: data field bytes for an SDC block of `l_sdc` bits.
[[nodiscard]] std::size_t drm_sdc_data_bytes(std::size_t l_sdc);

// A block of l_sdc bits: AFS index, data field, CRC-16, zero padding.
[[nodiscard]] Expected<std::vector<std::uint8_t>> drm_sdc_bits(std::uint8_t afs_index,
                                                               std::span<const std::uint8_t> data_field,
                                                               std::size_t l_sdc);

struct DrmStreamDescription {
    std::uint16_t part_a = 0;  // bytes
    std::uint16_t part_b = 0;
};

struct DrmSdcEntity {
    std::uint8_t type = 0;
    bool version = false;
    std::uint8_t nibble = 0;           // the body's first four bits
    std::vector<std::uint8_t> body;    // the `length` whole bytes after them
};

struct DrmApplication {
    std::uint8_t short_id = 0;
    std::uint8_t stream_id = 0;
    bool packet_mode = false;
    bool data_units = false;
    std::uint8_t packet_id = 0;
    std::uint8_t domain = 0;
    std::uint8_t packet_length = 0;  // data field bytes
    std::vector<std::uint8_t> application_data;
};

struct DrmAudioInfo {
    std::uint8_t short_id = 0;
    std::uint8_t stream_id = 0;
    std::uint8_t coding = 0;  // 00 AAC, 11 xHE-AAC
    bool text = false;
};

struct DrmSdc {
    std::uint8_t afs_index = 0;
    bool crc_ok = false;
    std::vector<DrmSdcEntity> entities;

    // Parsed from the entities when present.
    std::optional<std::uint8_t> protection_a;
    std::optional<std::uint8_t> protection_b;
    std::vector<DrmStreamDescription> streams;     // type 0, version 0
    std::vector<std::pair<std::uint8_t, std::string>> labels;  // type 1: short id, UTF-8
    std::vector<DrmApplication> applications;      // type 5
    std::vector<DrmAudioInfo> audio;               // type 9
    // Type 8.
    std::optional<std::uint32_t> mjd;
    std::optional<std::uint8_t> utc_hours;
    std::optional<std::uint8_t> utc_minutes;
    std::optional<int> local_offset_half_hours;
};

[[nodiscard]] DrmSdc drm_parse_sdc(std::span<const std::uint8_t> bits);

// Data entity framing, ES 6.4.3: 7-bit length, version, 4-bit type, nibble, body.
[[nodiscard]] std::vector<std::uint8_t> drm_sdc_entity(std::uint8_t type, bool version,
                                                       std::uint8_t nibble,
                                                       std::span<const std::uint8_t> body);

// ---------------------------------------------------------------------------
// Streams, text messages and packets, ES 6.2, 6.5, 6.6
// ---------------------------------------------------------------------------

struct DrmStreamFrame {
    std::uint64_t multiplex_frame = 0;
    std::uint8_t stream = 0;
    std::vector<std::uint8_t> bytes;  // part A then part B
};

struct DrmTextMessage {
    std::uint8_t stream = 0;
    std::string text;
    bool toggle = false;
};

struct DrmPacket {
    std::uint8_t stream = 0;
    std::uint8_t packet_id = 0;
    bool first = false;
    bool last = false;
    std::uint8_t continuity = 0;
    bool crc_ok = false;
    std::vector<std::uint8_t> data;  // useful bytes, padding removed
};

struct DrmDataUnit {
    std::uint8_t stream = 0;
    std::uint8_t packet_id = 0;
    std::vector<std::uint8_t> bytes;  // a Journaline or MOT data group, raw
};

// Assembles text message segments from successive four-byte pieces.
class DrmTextAssembler {
   public:
    // Returns a message when a segment completes the last of one.
    std::optional<DrmTextMessage> push(std::uint8_t stream, std::span<const std::uint8_t, 4> piece);

   private:
    std::vector<std::uint8_t> segment_;
    bool in_segment_ = false;
    std::string message_;
    int expected_ = 0;
};

// The 4-byte pieces of a text message's segments, ES 6.5, for the transmitter.
[[nodiscard]] std::vector<std::array<std::uint8_t, 4>> drm_text_pieces(const std::string& text,
                                                                       bool toggle);

// One packet of a packet-mode stream: header, data field of `length` bytes,
// CRC, ES 6.6.1.
[[nodiscard]] std::vector<std::uint8_t> drm_packet(bool first, bool last, std::uint8_t packet_id,
                                                   std::uint8_t continuity,
                                                   std::span<const std::uint8_t> useful,
                                                   std::size_t length);

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct DrmConfig {
    SampleRate rate = kDrmSampleRate;

    // Normalised guard correlation a mode needs to be taken. NOT A SPECIFIED
    // VALUE.
    double acquisition_threshold = 0.35;

    // Integer carrier offsets searched either side of zero.
    int max_integer_offset = 12;
};

struct DrmSyncInfo {
    bool locked = false;
    DrmMode mode = DrmMode::A;
    double cfo_hz = 0.0;
    std::int64_t frame_start_sample = 0;
    double guard_metric = 0.0;
};

struct DrmOutput {
    std::vector<DrmFac> fac;
    std::vector<DrmSdc> sdc;
    std::vector<DrmStreamFrame> streams;
    std::vector<DrmTextMessage> texts;
    std::vector<DrmPacket> packets;
    std::vector<DrmDataUnit> data_units;
};

struct DrmStats {
    std::uint64_t frames = 0;
    std::uint64_t fac_ok = 0;
    std::uint64_t fac_bad = 0;
    std::uint64_t sdc_ok = 0;
    std::uint64_t sdc_bad = 0;
    std::uint64_t multiplex_frames = 0;
    double last_mer_db = 0.0;  // FAC cells, modulation error ratio
};

class DrmReceiver {
   public:
    [[nodiscard]] static Expected<DrmReceiver> create(const DrmConfig& config);

    // Complex baseband at 48 kS/s with the DRM reference frequency at 0 Hz.
    void process(ConstComplexSpan samples, DrmOutput& out);

    [[nodiscard]] const DrmSyncInfo& sync() const { return sync_; }
    [[nodiscard]] const DrmStats& stats() const { return stats_; }
    void reset();

   private:
    DrmReceiver() = default;

    struct Symbol {
        std::vector<Complex32> bins;  // kmin_all .. kmax_all
    };

    bool acquire();
    bool frame_sync();
    void demodulate_available();
    void process_frames(DrmOutput& out);
    void decode_frame(std::size_t frame_index, DrmOutput& out);
    void decode_super_frame(DrmOutput& out);
    void decode_msc(DrmOutput& out);
    void deliver(std::span<const std::uint8_t> mux, DrmOutput& out);
    [[nodiscard]] std::vector<Complex32> fft_at(std::int64_t start) const;
    void trim();
    void lose_lock(std::int64_t resume);

    DrmConfig config_{};
    int loss_ = 0;
    DrmSyncInfo sync_{};
    DrmStats stats_{};

    std::vector<Complex32> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t total_ = 0;
    std::int64_t next_attempt_ = 0;

    DrmModeParams params_{};
    DrmFft fft_{};
    double cfo_carriers_ = 0.0;   // fractional + integer
    int integer_offset_ = 0;
    std::size_t backoff_ = 0;
    std::int64_t symbol0_ = 0;    // prefix start of symbol 0 of the first frame
    bool frame_locked_ = false;
    std::uint64_t next_symbol_ = 0;
    int kmin_all_ = 0;
    int kmax_all_ = 0;

    std::deque<Symbol> symbols_;       // from frame first_frame_
    std::uint64_t first_frame_ = 0;    // frame number of symbols_.front()
    std::uint64_t next_frame_ = 0;     // next frame to decode

    std::optional<DrmStructure> probe_;
    std::optional<DrmStructure> structure_;
    std::optional<DrmFacChannel> channel_;
    std::optional<int> frame_in_super_;  // of next_frame_
    std::optional<DrmSdc> multiplex_;     // last SDC with a type 0 entity

    // Equalised cells and weights of the super frame being gathered.
    std::vector<Complex32> sf_cells_;
    std::vector<float> sf_weights_;
    std::size_t sf_frames_ = 0;

    // Received multiplex frames awaiting long deinterleaving, oldest first.
    std::deque<std::pair<std::vector<Complex32>, std::vector<float>>> mux_history_;
    std::uint64_t mux_count_ = 0;

    std::vector<DrmTextAssembler> text_;
    std::vector<std::vector<std::uint8_t>> units_;  // per stream * 4 + packet id
};

}  // namespace revenant::decode
