// DAB (Eureka-147): complex baseband at 2.048 MS/s to the Fast Information
// Channel, the ensemble's configuration and labels, and the raw bytes of
// every Main Service Channel sub-channel, with packet mode data groups and
// MOT objects on top.
//
// SPECIFICATION
//
// ETSI EN 300 401 V2.1.1 (2017-01), "Radio Broadcasting Systems; Digital
// Audio Broadcasting (DAB) to mobile, portable and fixed receivers", free
// from etsi.org and copyright ETSI. Cited below as "EN 300 401" with a
// clause, table or figure. Every constant in dab.cpp carries the clause it
// came from; nothing was taken from anywhere else.
//
// ETSI EN 301 234 V2.1.1 (2006-05), "Multimedia Object Transfer (MOT)
// protocol", cited as "EN 301 234", for the segmentation header and the
// header core and extension of clauses 5.1.1, 6.1 and 6.2.
//
// ETSI TS 101 499 V3.1.1 (2015-01), "MOT SlideShow; User Application
// Specification", cited as "TS 101 499", clause 6.1: SlideShow is MOT header
// mode, one object per image, carried in packet mode or X-PAD.
//
// WHAT IS DECODED
//
//   - transmission mode I, the only mode V2.1.1 defines (table 22): the null symbol and phase reference symbol of clause
//     14.3, D-QPSK of 14.5 and 14.7, the frequency interleaver of 14.6.1;
//   - the FIC: the block partitioning of 14.4.1.1, the rate 1/3 punctured
//     code of 11.2.1 through the clause 11.1.1 mother code, the energy
//     dispersal of 10.2 and the FIB CRC of 5.2.1;
//   - FIGs 0/0 (ensemble), 0/1 (sub-channel organisation), 0/2 (services and
//     components), 0/3 (packet mode components), 1/0, 1/1 and 1/5 (labels);
//   - every long form (EEP, 11.3.2) sub-channel the FIC describes: the
//     block partitioning of 14.4.2.1, the CIF of clause 13, the time
//     de-interleaver of clause 12, the set A and set B profiles of tables 18
//     and 20, and the energy dispersal of 10.3, to logical frame bytes;
//   - packet mode, 5.3.2 to 5.3.4: packets checked against their CRC and
//     reassembled into MSC data groups per address, checked against theirs;
//   - MOT header mode, EN 301 234 5.1 and 6: header and body segments
//     reassembled by TransportId into objects carrying ContentName, the
//     content type pair and the body bytes.
//
// WHERE IT STOPS, AND WHY
//
//   - Audio. MPEG Layer II (the "DAB" row of docs/modes.md) is ISO/IEC
//     11172-3, purchasable and not held; DAB+ is HE-AAC under a licensing
//     pool. Stream audio sub-channels are delivered as raw logical frames.
//   - Short form (UEP) sub-channels are parsed out of FIG 0/1 and reported
//     with their table index, but not decoded: their size and profile come
//     from tables 8 and 15, which exist only for Layer II audio, the one
//     thing that could not be decoded afterwards anyway.
//   - The Reed-Solomon outer code of 5.3.5 for packet mode is not applied.
//     Clause 5.3.5.0 makes it transparent by design: the packets go out
//     unaltered and the parity follows in its own FEC packets, which this
//     receiver discards as packets of an address nobody asked for.
//   - SlideShow images arrive as MOT objects carrying JPEG or PNG bytes (TS
//     101 499 clause 6.2.3). Turning them into ImageRow pixels needs a JPEG
//     and a PNG decoder and the tree has neither; see core/decode/dab.cpp.
//   - Journaline, ETSI TS 102 979, is not decoded.
//   - Transmitter Identification Information in the null symbol (14.8).
//
// SYNCHRONISATION, WHICH IS DAB'S OWN
//
// core/decode/ofdm.h finds symbols from the cyclic prefix and fixes the
// channel from scattered pilots. DAB has no pilots: it is differential, and
// the frame is marked by a null symbol and a phase reference symbol. So this
// receiver does its own:
//
//   1. The null symbol is the 2656-sample window of least energy in a frame
//      length of signal (14.3.1: s(t) is zero there).
//   2. The fractional carrier offset is the angle of the guard interval's
//      correlation with the symbol tail it copies, summed over all 76
//      symbols (van de Beek, Sandell and Borjesson, IEEE Trans. Signal
//      Processing 45(7), 1997, as in ofdm.h).
//   3. The integer offset is the shift that best matches the differential
//      phase between adjacent carriers of the received phase reference
//      symbol to the transmitted one (14.3.2). The product of adjacent
//      carriers is unaffected by a timing offset short of a carrier's worth
//      of phase slope, which the null symbol has already guaranteed.
//   4. Fine timing is the first strong path of the channel impulse
//      response, the inverse transform of the received phase reference
//      symbol divided by the transmitted one.
//
// Steps 2 and 4 are repeated on every frame, so a slow clock or frequency
// drift is followed; the null is checked on every frame and three frames
// without one drop the lock.
//
// ONE STREAM, ANY BLOCKING
//
// Every decision is made on a whole frame held in the buffer and addressed by
// absolute sample index, and acquisition is tried only at windows anchored to
// absolute positions, so the output does not depend on how the stream was
// split. tests/decode/test_dab.cpp holds it to that.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The round trip in test_dab.cpp against
// core/dsp/synth/dab_mod.h, written from the same clauses, stands in its
// place, with the standard's own printed values (table 12's PRBS, table 25's
// interleaver) checked directly.
//
// CLEAN ROOM
//
// No DAB, MOT or SlideShow implementation was read: not welle.io, dablin,
// qt-dab or any other. See docs/clean-room.md.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// ---------------------------------------------------------------------------
// Transmission mode I, EN 300 401 table 22. Times in the elementary period
// T = 1/2 048 000 s, which is one sample at the rate this receiver takes.
// ---------------------------------------------------------------------------

inline constexpr dsp::SampleRate kDabSampleRate = 2'048'000;
inline constexpr std::size_t kDabSymbols = 76;          // L, null excluded
inline constexpr std::size_t kDabCarriers = 1536;       // K
inline constexpr std::size_t kDabFrameSamples = 196'608; // T_F, 96 ms
inline constexpr std::size_t kDabNullSamples = 2656;    // T_NULL
inline constexpr std::size_t kDabSymbolSamples = 2552;  // T_S
inline constexpr std::size_t kDabFftSize = 2048;        // T_U
inline constexpr std::size_t kDabGuardSamples = 504;    // Delta

// Bits per OFDM symbol, 2K (clause 14.5).
inline constexpr std::size_t kDabBitsPerSymbol = 2 * kDabCarriers;

// Clause 5.1 table 1 and figure 5: 12 FIBs and 4 CIFs per frame; the FIC is
// symbols 2 to 4 and the MSC symbols 5 to 76.
inline constexpr std::size_t kDabFibsPerFrame = 12;
inline constexpr std::size_t kDabCifsPerFrame = 4;
inline constexpr std::size_t kDabFicSymbols = 3;
inline constexpr std::size_t kDabFibBytes = 32;         // 5.2.1: 256 bits
inline constexpr std::size_t kDabFicCodewordBits = 2304; // 11.2.1
inline constexpr std::size_t kDabCifBits = 55'296;       // clause 13
inline constexpr std::size_t kDabCuBits = 64;            // clause 13
inline constexpr std::size_t kDabCusPerCif = 864;        // 6.2.1

// ---------------------------------------------------------------------------
// The standard's fixed sequences. Exposed for the transmitter and the tests.
// ---------------------------------------------------------------------------

// Phase reference symbol value on carrier k, -768..768, k = 0 giving zero
// (clause 14.3.2, tables 23 and 24).
[[nodiscard]] dsp::Complex32 dab_prs_value(int carrier);

// F(n) of clause 14.6.1: the carrier, -768..768 without 0, that QPSK symbol
// n of an OFDM symbol is carried on.
[[nodiscard]] const std::array<int, kDabCarriers>& dab_frequency_interleaver();

// The first `length` bits of the clause 10.1 PRBS, x^9 + x^5 + 1 from all
// ones, one bit per byte.
[[nodiscard]] std::vector<std::uint8_t> dab_prbs(std::size_t length);

// The clause 11.1.1 mother code: rate 1/4, constraint length 7, octal 133,
// 171, 145, 133. Generators in the dv_codes.h convention, D^0 in bit 0.
inline constexpr std::uint32_t kDabCodeMemory = 6;
inline constexpr std::array<std::uint32_t, 4> kDabCodeGenerators{0x6D, 0x4F, 0x53, 0x6D};

// One run of 128-bit blocks punctured with one index (clause 11.1.2).
struct DabPunctureRun {
    std::size_t blocks = 0;
    unsigned pi = 0;  // 1..24, table 13
};

// The table 13 vector for an index, 32 entries of 0 or 1.
[[nodiscard]] std::array<std::uint8_t, 32> dab_puncture_vector(unsigned pi);

// Encodes I information bits through the mother code and the puncturing
// runs, appending the 12 tail bits of V_T. The runs must cover 4I/128 blocks.
[[nodiscard]] Expected<std::vector<std::uint8_t>> dab_convolutional_encode(
    std::span<const std::uint8_t> bits, std::span<const DabPunctureRun> runs);

// Soft decodes the same shape; `soft` in the SoftSpan convention of
// dv_codes.h (positive is a zero). Returns I bits.
[[nodiscard]] Expected<std::vector<std::uint8_t>> dab_convolutional_decode(
    std::span<const float> soft, std::size_t information_bits,
    std::span<const DabPunctureRun> runs);

// The FIC's runs: clause 11.2.1, 21 blocks at PI 16 and 3 at PI 15.
[[nodiscard]] std::vector<DabPunctureRun> dab_fic_runs();

// Clause 12 table 21: the codeword index r' that bit i of CIF r carries is
// r - dab_time_interleave_delay(i mod 16).
[[nodiscard]] unsigned dab_time_interleave_delay(std::size_t i);

// The CRC every DAB structure uses: x^16 + x^12 + x^5 + 1, preset to ones,
// complemented, MSb first (clauses 5.2.1, 5.3.2.3, 5.3.3.4 and annex E).
[[nodiscard]] std::uint16_t dab_crc16(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// Sub-channel protection
// ---------------------------------------------------------------------------

enum class DabProtection : std::uint8_t {
    // Long form option 000, table 18: protection levels 1-A to 4-A.
    EepA,
    // Long form option 001, table 20: 1-B to 4-B.
    EepB,
    // Short form: table 8 index, not decoded (see WHERE IT STOPS).
    Uep,
};

struct DabSubchannel {
    unsigned id = 0;             // SubChId, 0..63
    unsigned start_cu = 0;       // 0..863
    unsigned size_cu = 0;        // long form only; zero for UEP
    DabProtection protection = DabProtection::EepA;
    unsigned level = 1;          // 1..4, long form only
    unsigned uep_index = 0;      // short form only
    // Packet mode FEC scheme from FIG 0/14 is not parsed; see WHERE IT STOPS.
};

// The logical frame length I in bits and the puncturing runs of an EEP
// sub-channel (tables 17 to 20). Fails for a UEP sub-channel, a size that is
// not a multiple of the profile's CU unit, or a level out of range.
struct DabEepProfile {
    std::size_t information_bits = 0;
    std::vector<DabPunctureRun> runs;
    unsigned bitrate_kbps = 0;
};
[[nodiscard]] Expected<DabEepProfile> dab_eep_profile(const DabSubchannel& subchannel);

// ---------------------------------------------------------------------------
// The ensemble as the FIC describes it
// ---------------------------------------------------------------------------

struct DabLabel {
    unsigned charset = 0;
    // The sixteen character bytes as sent, and the eight the character flag
    // field marks for a short label (clause 5.2.2.2, figure 8).
    std::array<std::uint8_t, 16> bytes{};
    std::uint16_t short_flags = 0;
    // The bytes rendered for display: printable ASCII kept, trailing spaces
    // trimmed, anything else '?'. A charset table (TS 101 756 table 1) is not
    // held; the ASCII range is common to EBU Latin and UTF-8.
    std::string text;
};

struct DabComponent {
    // TMId, clause 6.3.1: 0 stream audio, 1 stream data, 3 packet data.
    unsigned transport = 0;
    unsigned type = 0;           // ASCTy or DSCTy
    unsigned subchannel = 0;     // stream modes
    unsigned scid = 0;           // packet mode
    bool primary = false;
    bool ca = false;
};

struct DabService {
    std::uint32_t sid = 0;
    bool data_sid = false;       // 32-bit SId (P/D = 1)
    std::vector<DabComponent> components;
    std::optional<DabLabel> label;
};

// FIG 0/3: a packet mode component's sub-channel and address.
struct DabPacketComponent {
    unsigned scid = 0;
    unsigned subchannel = 0;
    unsigned address = 0;
    unsigned dscty = 0;
    bool data_groups = true;
};

struct DabEnsemble {
    std::optional<std::uint16_t> eid;
    std::optional<DabLabel> label;
    std::optional<unsigned> cif_count;  // last FIG 0/0, 0..4999
    bool alarm = false;
    std::map<unsigned, DabSubchannel> subchannels;
    std::map<std::uint32_t, DabService> services;
    std::map<unsigned, DabPacketComponent> packet_components;  // by SCId
};

// Parses the 30 data bytes of one FIB whose CRC has already passed and
// merges what it carries into `ensemble`. Only current configuration (C/N =
// 0) FIGs are applied. Returns the number of FIGs recognised.
std::size_t dab_parse_fib(std::span<const std::uint8_t> fib, DabEnsemble& ensemble);

// ---------------------------------------------------------------------------
// Packet mode and MOT
// ---------------------------------------------------------------------------

struct DabDataGroup {
    unsigned subchannel = 0;
    unsigned address = 0;
    unsigned type = 0;
    unsigned continuity = 0;
    unsigned repetition = 0;
    bool has_segment = false;
    bool last = false;
    unsigned segment = 0;
    std::optional<std::uint16_t> transport_id;
    std::vector<std::uint8_t> end_user_address;
    std::vector<std::uint8_t> data;  // the data group data field
};

// Packet level reassembly for one sub-channel (clauses 5.3.2 to 5.3.4).
class DabPacketAssembler {
public:
    explicit DabPacketAssembler(unsigned subchannel) : subchannel_(subchannel) {}

    // Feeds one logical frame; every completed data group with a good CRC is
    // appended to `out`. Packets with a bad CRC break the series they were in.
    void push(std::span<const std::uint8_t> frame, std::vector<DabDataGroup>& out);

    [[nodiscard]] std::size_t packets_ok() const { return packets_ok_; }
    [[nodiscard]] std::size_t packets_bad() const { return packets_bad_; }
    [[nodiscard]] std::size_t groups_bad() const { return groups_bad_; }

private:
    struct Series {
        std::vector<std::uint8_t> bytes;
        unsigned next_continuity = 0;
        bool open = false;
    };
    unsigned subchannel_;
    std::map<unsigned, Series> series_;
    std::size_t packets_ok_ = 0;
    std::size_t packets_bad_ = 0;
    std::size_t groups_bad_ = 0;
};

// Parses a whole MSC data group (header, session header, data field, CRC);
// nullopt if it is malformed or its CRC fails.
[[nodiscard]] std::optional<DabDataGroup> dab_parse_data_group(std::span<const std::uint8_t> bytes,
                                                               unsigned subchannel,
                                                               unsigned address);

struct MotObject {
    std::uint16_t transport_id = 0;
    unsigned content_type = 0;
    unsigned content_subtype = 0;
    std::string content_name;
    std::size_t body_size = 0;         // BodySize from the header core
    std::vector<std::uint8_t> header;  // the whole header, core included
    std::vector<std::uint8_t> body;
};

// MOT header mode reassembly, EN 301 234 clauses 5.1 and 6: data group types
// 3 (header) and 4 (unscrambled body), segments keyed by TransportId. An
// object is emitted once its header and every body segment are in and the
// body length matches BodySize (clause 6.1).
class MotAssembler {
public:
    void push(const DabDataGroup& group, std::vector<MotObject>& out);

private:
    struct Entity {
        std::map<unsigned, std::vector<std::uint8_t>> segments;
        std::optional<unsigned> last;
    };
    struct Pending {
        Entity header;
        Entity body;
        bool emitted = false;
    };
    static std::optional<std::vector<std::uint8_t>> complete(const Entity& entity);
    std::map<std::uint16_t, Pending> pending_;
};

// Header core and the ContentName parameter of a reassembled MOT header.
// Fails if the header is shorter than its core or HeaderSize says.
[[nodiscard]] Expected<MotObject> mot_parse_header(std::span<const std::uint8_t> header);

// ---------------------------------------------------------------------------
// The receiver
// ---------------------------------------------------------------------------

struct DabReceiverConfig {
    // Carrier offsets searched either side of zero, in carrier spacings of
    // 1 kHz. Ten covers a 50 ppm tuner at 200 MHz.
    int max_integer_cfo = 10;

    // Ratio of null-window energy to mean frame energy above which a frame is
    // taken not to start there.
    double null_threshold = 0.5;

    // Samples before the end of the guard the FFT window starts, so an early
    // path or a timing error lands in the guard rather than the next symbol.
    std::size_t window_backoff = 48;

    // Sub-channels to decode. Empty decodes every long form sub-channel the
    // FIC describes.
    std::vector<unsigned> subchannels;
};

struct DabSync {
    bool locked = false;
    std::int64_t frame_start = 0;     // absolute sample of the last null
    double cfo_carriers = 0.0;
    double cfo_hz = 0.0;
    double null_ratio = 0.0;
    double prs_metric = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t fibs_ok = 0;
    std::uint64_t fibs_bad = 0;
};

struct DabLogicalFrame {
    unsigned subchannel = 0;
    std::uint64_t cif = 0;  // CIFs since lock at which this codeword began
    std::vector<std::uint8_t> bytes;
};

struct DabCallbacks {
    std::function<void(std::span<const std::uint8_t> fib, bool crc_ok)> on_fib;
    std::function<void(const DabLogicalFrame&)> on_frame;
    std::function<void(const DabDataGroup&)> on_data_group;
    std::function<void(const MotObject&)> on_mot_object;
};

class DabReceiver {
public:
    [[nodiscard]] static Expected<DabReceiver> create(DabReceiverConfig config,
                                                      DabCallbacks callbacks = {});

    void push(dsp::ConstComplexSpan samples);

    [[nodiscard]] const DabSync& sync() const { return sync_; }
    [[nodiscard]] const DabEnsemble& ensemble() const { return ensemble_; }

    void reset();

private:
    struct SubchannelState {
        DabSubchannel config;
        DabEepProfile profile;
        std::vector<std::vector<float>> history;  // ring of 16 CIFs
        std::uint64_t filled = 0;
        std::optional<DabPacketAssembler> packets;
        MotAssembler mot;
    };

    explicit DabReceiver(DabReceiverConfig config, DabCallbacks callbacks);

    bool try_acquire();
    bool process_frame(bool acquiring);
    void demodulate(std::int64_t frame_start, std::vector<float>& soft);
    void fft_symbol(std::int64_t start, std::vector<dsp::Complex32>& bins) const;
    double fractional_cfo(std::int64_t frame_start) const;
    int integer_cfo(std::int64_t frame_start, double& metric) const;
    int timing_offset(std::int64_t frame_start) const;
    double null_ratio(std::int64_t frame_start) const;
    void decode_fic(std::span<const float> soft);
    void decode_msc(std::span<const float> soft);
    void sync_subchannels();
    [[nodiscard]] dsp::Complex32 sample(std::int64_t index) const;
    void trim();

    DabReceiverConfig config_;
    DabCallbacks callbacks_;
    DabSync sync_;
    DabEnsemble ensemble_;

    std::vector<dsp::Complex32> buffer_;
    std::int64_t buffer_start_ = 0;
    std::int64_t total_ = 0;
    std::int64_t next_attempt_ = 0;
    unsigned missed_nulls_ = 0;

    std::uint64_t cif_ = 0;
    std::map<unsigned, SubchannelState> subchannel_state_;
    std::vector<float> soft_;
};

}  // namespace revenant::decode
