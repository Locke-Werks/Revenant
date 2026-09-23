// DMR: complex baseband to bursts, and the framing and metadata the air
// interface carries in clear. Voice is not decoded.
//
// SPECIFICATION
//
// Implements ETSI TS 102 361-1 V2.7.1 (2026-05), "DMR Air Interface (AI)
// protocol": clause 4.2 (the TDMA burst and frame structure), clause 5.1.2
// (the voice superframe), clause 6 (the burst formats and the CACH), clause 7
// (Link Control, the embedded signalling, Short LC in the CACH and the CSBK),
// clause 8.2.1 (the data header blocks), clause 9 (the PDUs and their
// information elements), clause 10.2 (the 4FSK modulation), Annex B (the FEC
// and CRC codes, core/decode/dmr_codes.h) and Annex E (the transmit bit
// order). What the Full LC, the CSBK and the Short LC carry is ETSI TS 102
// 361-2 V2.5.1 (2023-05) clause 7 and its Annex B opcode lists. Both were
// fetched from etsi.org's deliver repository on 2026-09-23 and every clause,
// table and figure number below is the document's own.
//
// WHY THIS IS HERE AT ALL
//
// docs/modes.md excluded DMR on 2026-09-21 over Motorola's US8306071, live
// until 2027-02-19 and declared essential to TS 102 361-1, whose claim 1
// compares a received synchronisation pattern against known patterns. On
// 2026-09-23 the owner put DMR back in scope and accepted that risk; the
// decision and the query behind the exclusion are both kept in
// docs/modes.md. AMBE+2 voice stays out because no document for it exists.
//
// WHERE THIS STOPS
//
// At the 216 vocoder socket bits of each voice burst, which are handed out
// and not interpreted. It does not decrypt and has nothing to decrypt with:
// the Privacy bit of the Service Options and a PI header are reported, and
// the payload is the same channel bits either way, per docs/modes.md.
// Data continuation blocks, rate 3/4 trellis data and the MBC continuation
// blocks are reported by their Data Type and not decoded; the reverse
// channel burst is recognised by its sync and not decoded.
//
// POLARITY IS TAKEN AS GIVEN
//
// Table 9.2's note says each voice sync is the symbol-wise complement of the
// matching data sync, so a correlator reads the kind of a burst from the sign
// of its score. That is only possible with the discriminator's polarity
// known, because an inverted receiver sees every voice sync as a data sync.
// So this decoder reads the sign as the burst kind and takes the polarity of
// its input as Table 10.3 states it: a positive deviation is a positive
// frequency. DmrConfig::invert is for a receiver that delivers the
// conjugate. P25 and D-STAR fit their polarity per frame because their sync
// words have no complement with a different meaning.
//
// CLEAN ROOM
//
// No DMR implementation was read. Every constant names its clause, table or
// figure, and where a number is an engineering choice the comment says so.
//
// NOTHING HERE IS BIT EXACT AGAINST A GPU TWIN
//
// Host code with no kernel behind it. The transmitter in
// core/dsp/synth/dmr_mod.h, written from the same clauses, and the round
// trips in tests/decode/test_dmr.cpp stand in its place, as for the other
// digital voice modes.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/decode/dmr_codes.h"
#include "core/decode/dv_phy.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

using dsp::ConstComplexSpan;
using dsp::SampleRate;

// ---------------------------------------------------------------------------
// The physical layer, clause 10
// ---------------------------------------------------------------------------

// Clause 10.2.1: "The modulation sends 4 800 symbols/s with each symbol
// conveying 2 bits of information."
inline constexpr double kDmrSymbolRate = 4800.0;

// Clause 10.2.2.1 and Table 10.3: deviation index 0,27 gives 1,944 kHz at the
// outer symbols, so one symbol unit is 648 Hz. Table 10.3 maps dibits to
// symbols as Table 9-1 of TIA-102.BAAA-A does, indexed by the dibit as an
// integer: kDmrDibitToSymbol[0b01] is +3.
inline constexpr double kDmrDeviationPerSymbolUnitHz = 648.0;
inline constexpr int kDmrDibitToSymbol[4] = {+1, +3, -1, -3};

// Clause 10.2.2.2, formula 14: the square root raised cosine filter, flat to
// 1920 Hz, |cos(pi f / 1920)| from there to 2880 Hz and zero above. The
// transmitter and the receiver each carry one, so their cascade is the
// Nyquist raised cosine the clause splits.
inline constexpr double kDmrRrcFlatEdgeHz = 1920.0;
inline constexpr double kDmrRrcStopHz = 2880.0;

// |F(f)| of formula 14, for design_from_response.
[[nodiscard]] double dmr_rrc_response(double hertz, const void* context);

// ---------------------------------------------------------------------------
// The burst, clauses 4.2.2, 6 and Annex E
// ---------------------------------------------------------------------------

// Clause 4.2.2: 264 bits in 27,5 ms, which is 132 symbols; the outbound
// channel fills the other 2,5 ms with the 24-bit CACH and the inbound leaves
// it as guard time. So a slot is 144 symbols, 30 ms, and a TDMA frame 288.
inline constexpr std::size_t kDmrBurstBits = 264;
inline constexpr std::size_t kDmrBurstSymbols = kDmrBurstBits / 2;
inline constexpr std::size_t kDmrCachSymbols = kDmrCachBits / 2;
inline constexpr std::size_t kDmrSlotSymbols = kDmrBurstSymbols + kDmrCachSymbols;

// Figures 6.2 to 6.5 and Annex E: symbols L66 to L1 and R1 to R66, L66 first.
// The 48-bit centre field, sync or embedded signalling, is L12 to R12, so it
// starts 54 symbols in; a voice burst carries 108 vocoder bits either side of
// it and a data burst 98 information bits and 10 slot type bits.
inline constexpr std::size_t kDmrCentreBits = 48;
inline constexpr std::size_t kDmrCentreFirstBit = 108;
inline constexpr std::size_t kDmrCentreFirstSymbol = kDmrCentreFirstBit / 2;
inline constexpr std::size_t kDmrVoiceBits = 216;
inline constexpr std::size_t kDmrInfoHalfBits = 98;
inline constexpr std::size_t kDmrSlotTypeHalfBits = 10;
inline constexpr std::size_t kDmrSyncSymbols = kDmrCentreBits / 2;

// Figure 6.4 and Table E.6: in a voice burst the centre field is 8 EMB bits,
// the 32 embedded signalling bits and the other 8 EMB bits.
inline constexpr std::size_t kDmrEmbHalfBits = 8;

// Clause 5.1.2.1: six bursts, A to F, 360 ms.
inline constexpr std::size_t kDmrSuperframeBursts = 6;

// Table 9.2. Every one is made of the nibbles 5, 7, D and F, so every symbol
// is +3 or -3, and each voice sync is the symbol-wise complement of the data
// sync beside it, the RC sync of the reserved pattern.
enum class DmrSyncType : std::uint8_t {
    BsVoice,
    BsData,
    MsVoice,
    MsData,
    MsReverseChannel,
    DirectVoiceSlot1,
    DirectDataSlot1,
    DirectVoiceSlot2,
    DirectDataSlot2,
    Reserved,
};

inline constexpr std::uint64_t kDmrSyncBsVoice = 0x755FD7DF75F7ULL;
inline constexpr std::uint64_t kDmrSyncBsData = 0xDFF57D75DF5DULL;
inline constexpr std::uint64_t kDmrSyncMsVoice = 0x7F7D5DD57DFDULL;
inline constexpr std::uint64_t kDmrSyncMsData = 0xD5D7F77FD757ULL;
inline constexpr std::uint64_t kDmrSyncMsReverseChannel = 0x77D55F7DFD77ULL;
inline constexpr std::uint64_t kDmrSyncDirectVoiceSlot1 = 0x5D577F7757FFULL;
inline constexpr std::uint64_t kDmrSyncDirectDataSlot1 = 0xF7FDD5DDFD55ULL;
inline constexpr std::uint64_t kDmrSyncDirectVoiceSlot2 = 0x7DFFD5F55D5FULL;
inline constexpr std::uint64_t kDmrSyncDirectDataSlot2 = 0xD7557F5FF7F5ULL;
inline constexpr std::uint64_t kDmrSyncReserved = 0xDD7FF5D757DDULL;

[[nodiscard]] std::uint64_t dmr_sync_bits(DmrSyncType type);
[[nodiscard]] std::string_view dmr_sync_name(DmrSyncType type);
[[nodiscard]] bool dmr_sync_is_voice(DmrSyncType type);
[[nodiscard]] bool dmr_sync_is_data(DmrSyncType type);
[[nodiscard]] bool dmr_sync_is_base_station(DmrSyncType type);

// The TDMA direct mode slot a sync names, or 0 for one that names none.
[[nodiscard]] std::uint8_t dmr_sync_slot(DmrSyncType type);

// A sync pattern as 24 soft symbol values, Table 9.2 through Table 10.3.
[[nodiscard]] std::array<float, kDmrSyncSymbols> dmr_sync_pattern(DmrSyncType type);

// ---------------------------------------------------------------------------
// Information elements, clause 9.3
// ---------------------------------------------------------------------------

// Table 9.22.
enum class DmrDataType : std::uint8_t {
    PiHeader = 0b0000,
    VoiceLcHeader = 0b0001,
    TerminatorWithLc = 0b0010,
    Csbk = 0b0011,
    MbcHeader = 0b0100,
    MbcContinuation = 0b0101,
    DataHeader = 0b0110,
    RateHalfData = 0b0111,
    RateThreeQuarterData = 0b1000,
    Idle = 0b1001,
    RateOneData = 0b1010,
    UnifiedSingleBlockData = 0b1011,
};

// Table 9.22's name for a 4-bit Data Type, "reserved" for 1100 to 1111.
[[nodiscard]] std::string_view dmr_data_type_name(std::uint8_t data_type);

// Table 9.20.
inline constexpr std::uint8_t kDmrLcssSingle = 0b00;
inline constexpr std::uint8_t kDmrLcssFirst = 0b01;
inline constexpr std::uint8_t kDmrLcssLast = 0b10;
inline constexpr std::uint8_t kDmrLcssContinuation = 0b11;

// Table 9.21: FID 0 is the standardised feature set of TS 102 361-2.
inline constexpr std::uint8_t kDmrStandardFid = 0x00;

// TS 102 361-2 Table B.1, the Full Link Control opcodes.
inline constexpr std::uint8_t kDmrFlcoGroupVoice = 0b000000;
inline constexpr std::uint8_t kDmrFlcoUnitToUnitVoice = 0b000011;
inline constexpr std::uint8_t kDmrFlcoTalkerAliasHeader = 0b000100;
inline constexpr std::uint8_t kDmrFlcoTalkerAliasBlock1 = 0b000101;
inline constexpr std::uint8_t kDmrFlcoTalkerAliasBlock2 = 0b000110;
inline constexpr std::uint8_t kDmrFlcoTalkerAliasBlock3 = 0b000111;
inline constexpr std::uint8_t kDmrFlcoGpsInfo = 0b001000;

// TS 102 361-2 Table B.2, the CSBK opcodes.
inline constexpr std::uint8_t kDmrCsbkoUnitToUnitVoiceRequest = 0b000100;
inline constexpr std::uint8_t kDmrCsbkoUnitToUnitAnswerResponse = 0b000101;
inline constexpr std::uint8_t kDmrCsbkoChannelTiming = 0b000111;
inline constexpr std::uint8_t kDmrCsbkoNegativeAcknowledge = 0b100110;
inline constexpr std::uint8_t kDmrCsbkoBsOutboundActivation = 0b111000;
inline constexpr std::uint8_t kDmrCsbkoPreamble = 0b111101;

// TS 102 361-2 Table B.3, the Short LC opcodes.
inline constexpr std::uint8_t kDmrSlcoNull = 0b0000;
inline constexpr std::uint8_t kDmrSlcoActivityUpdate = 0b0001;

// Table 9.30, the Data Packet Format values, and Table 9.31's proprietary SAP.
inline constexpr std::uint8_t kDmrDpfUnifiedDataTransport = 0b0000;
inline constexpr std::uint8_t kDmrDpfResponse = 0b0001;
inline constexpr std::uint8_t kDmrDpfUnconfirmed = 0b0010;
inline constexpr std::uint8_t kDmrDpfConfirmed = 0b0011;
inline constexpr std::uint8_t kDmrDpfShortDataDefined = 0b1101;
inline constexpr std::uint8_t kDmrDpfShortDataRawOrStatus = 0b1110;
inline constexpr std::uint8_t kDmrDpfProprietary = 0b1111;
inline constexpr std::uint8_t kDmrSapProprietary = 0b1001;

[[nodiscard]] std::string_view dmr_flco_name(std::uint8_t fid, std::uint8_t flco);
[[nodiscard]] std::string_view dmr_csbko_name(std::uint8_t fid, std::uint8_t csbko);
[[nodiscard]] std::string_view dmr_dpf_name(std::uint8_t dpf);
[[nodiscard]] std::string_view dmr_sap_name(std::uint8_t sap);

// TS 102 361-2 clause 7.2.1, Table 7.11, bit 7 first.
struct DmrServiceOptions {
    std::uint8_t raw = 0;
    bool emergency = false;
    // "Privacy is not defined in the present document": the bit says a
    // privacy service is in use and nothing more, which is exactly what is
    // reported.
    bool privacy = false;
    bool broadcast = false;
    bool open_voice_call_mode = false;
    std::uint8_t priority = 0;
};

[[nodiscard]] DmrServiceOptions dmr_parse_service_options(std::uint8_t raw);

// ---------------------------------------------------------------------------
// What comes out
// ---------------------------------------------------------------------------

// Where a Full LC arrived from.
enum class DmrLcCarrier : std::uint8_t { VoiceHeader, Terminator, Embedded };

// Clause 7.1, figure 7.1, and TS 102 361-2 clause 7.1.1.
struct DmrFullLc {
    std::array<std::uint8_t, 9> octets{};
    DmrLcCarrier carrier = DmrLcCarrier::VoiceHeader;

    bool protect_flag = false;
    std::uint8_t flco = 0;
    std::uint8_t fid = 0;

    // Parsed only for FID 0 and the two voice channel user opcodes, TS
    // 102 361-2 Tables 7.1 and 7.2: Service Options in octet 2, the group or
    // target address in octets 3 to 5, the source in 6 to 8. A
    // manufacturer's FID redefines the rest (Table 9.21's note), so it is
    // left as octets.
    std::optional<DmrServiceOptions> service_options;
    bool group = false;
    std::optional<std::uint32_t> destination;
    std::optional<std::uint32_t> source;

    // Octets the Reed-Solomon code changed, or bits the embedded BPTC did.
    std::uint32_t corrected = 0;
};

[[nodiscard]] DmrFullLc dmr_parse_full_lc(std::span<const std::uint8_t, 9> octets,
                                          DmrLcCarrier carrier);

// Clause 7.2, figure 7.8, and TS 102 361-2 clause 7.1.2.
struct DmrCsbk {
    std::array<std::uint8_t, 10> octets{};

    // Clause 7.4: an MBC header has the CSBK structure, and this is one.
    bool mbc_header = false;

    bool last_block = false;
    bool protect_flag = false;
    std::uint8_t opcode = 0;
    std::uint8_t fid = 0;

    // The addresses and options of the FID 0 opcodes that carry them,
    // TS 102 361-2 Tables 7.5a to 7.7. Channel Timing carries leader and
    // source identifiers of its own shape and is left as octets.
    std::optional<DmrServiceOptions> service_options;
    std::optional<std::uint32_t> target;
    std::optional<std::uint32_t> source;

    // Table 7.7, the Preamble CSBK: data or CSBK follows, a group or an
    // individual target, and the CSBK Blocks to Follow.
    std::optional<bool> preamble_data_follows;
    std::optional<bool> group;
    std::optional<std::uint8_t> blocks_to_follow;
};

[[nodiscard]] DmrCsbk dmr_parse_csbk(std::span<const std::uint8_t, 10> octets, bool mbc_header);

// Clause 8.2.1, figures 8.3 to 8.10: the fields every first header block has
// in the same place, and the second block of a proprietary packet.
struct DmrDataHeader {
    std::array<std::uint8_t, 10> octets{};

    // Figure 8.6, the block that follows a first header whose SAP is 1001.
    // It has no addresses, and carries the manufacturer's ID in octet 1.
    bool proprietary_second = false;
    std::optional<std::uint8_t> manufacturer_id;

    std::uint8_t format = 0;  // DPF, Table 9.30
    std::uint8_t sap = 0;     // Table 9.31
    bool group = false;       // Table 9.28
    bool response_requested = false;
    std::optional<std::uint32_t> destination;
    std::optional<std::uint32_t> source;

    // Figures 8.3 to 8.5: octet 8's low seven bits for the response,
    // unconfirmed and confirmed formats.
    std::optional<std::uint8_t> blocks_to_follow;
};

[[nodiscard]] DmrDataHeader dmr_parse_data_header(std::span<const std::uint8_t, 10> octets,
                                                  bool proprietary_second);

// Clause 7.1, figure 7.2, and TS 102 361-2 clause 7.1.3.
struct DmrShortLc {
    std::uint8_t slco = 0;
    std::uint32_t data = 0;  // the 24 data bits

    // TS 102 361-2 Table 7.10, the Activity Update: each slot's activity ID
    // and the B.3.7 hash of its destination.
    std::optional<std::array<std::uint8_t, 2>> activity;
    std::optional<std::array<std::uint8_t, 2>> hashed_address;

    std::uint32_t corrected = 0;
};

// Clause 9.1.4, Table 9.5, read through Hamming (7,4).
struct DmrCach {
    bool access_busy = false;   // Table 9.23
    std::uint8_t channel = 0;   // Table 9.24: the following outbound burst is 1 or 2
    std::uint8_t lcss = 0;
    std::uint32_t tact_corrected = 0;
    std::array<std::uint8_t, kDmrCachPayloadBits> payload{};
};

// Clause 9.1.2, Table 9.3, read through the quadratic residue code.
struct DmrEmb {
    std::uint8_t colour_code = 0;
    bool pi = false;
    std::uint8_t lcss = 0;
    std::uint32_t corrected = 0;
};

// One burst of one slot.
struct DmrBurst {
    // The input sample at which the burst's first symbol, L66, is centred,
    // counted from the start of the stream since create() or reset(), with
    // the receive filter's delay taken off.
    std::size_t first_sample = 0;

    // The timeslot: 1 or 2 where the CACH's TDMA Channel bit or a direct mode
    // sync names it, and 0 where nothing on the air does, which is an MS or
    // simplex transmission that has not been joined to a timeslot yet.
    std::uint8_t slot = 0;

    // The sync pattern in the centre field, if it carried one.
    std::optional<DmrSyncType> sync;

    // For a voice burst, 1 to 6 for A to F. 0 for anything else.
    std::uint8_t voice_burst = 0;

    // From the slot type on a data burst and from the EMB on voice bursts B
    // to F. Burst A carries none.
    std::optional<std::uint8_t> colour_code;

    // A data burst's Data Type, when its slot type decoded within the three
    // errors Golay (20,8) corrects.
    std::optional<std::uint8_t> data_type;
    std::uint32_t slot_type_corrected = 0;

    std::optional<DmrEmb> emb;
    std::optional<DmrCach> cach;

    // A Full LC from a voice LC header, a terminator, or the four embedded
    // fragments completed on this burst.
    std::optional<DmrFullLc> full_lc;
    std::optional<DmrCsbk> csbk;
    std::optional<DmrDataHeader> data_header;

    // Completed by the CACH in front of this burst.
    std::optional<DmrShortLc> short_lc;

    // A PI header, Table 6.1: privacy is in use on this call. What it carries
    // is not defined in TS 102 361-1 and is not interpreted.
    bool pi_header = false;

    // A BPTC-protected payload whose CRC or Reed-Solomon code failed, so none
    // of the optionals above it were filled in.
    bool payload_failed = false;
    std::uint32_t bptc_corrected = 0;

    // The 216 vocoder socket bits of a voice burst, VS(215) first, or the
    // 196 information bits of a data burst, TX(195) first.
    std::vector<std::uint8_t> payload;

    // The sync's centred correlation, and what fitting its 24 known symbols
    // said about the carrier. A burst with no sync carries its slot's last.
    double sync_score = 0.0;
    double carrier_offset_hz = 0.0;
    double deviation_ratio = 1.0;
};

struct DmrConfig {
    SampleRate rate = 48000;

    // Centred correlation a sync must reach to start framing, anywhere in
    // the stream. An engineering choice, measured on Table 9.2's patterns:
    // one of the 24 symbols flipped scores 0.920 and two 0.845, and each
    // pattern slid across 2000 random surroundings of its own length either
    // side reached 0.858 at worst, so 0.9 admits one wrong symbol and no
    // sidelobe.
    double sync_threshold = 0.9;

    // The same where the framing says a burst starts, which only has to tell
    // the patterns apart. Distinct patterns in Table 9.2 correlate at 0.167
    // at most, and a random centre field at about 0.2 per standard deviation,
    // so 0.7 is 3.5 of those. Inside a voice superframe, where bursts B to F
    // carry no sync, sync_threshold applies instead.
    double tracking_threshold = 0.7;

    // Taps in the receive filter. Odd. 121 at 48 kHz spans twelve symbols,
    // the span core/decode/p25p1.h settled on for the same symbol rate.
    std::size_t filter_taps = 121;

    // Negate the discriminator, for a receiver that delivers the conjugate.
    // See the top of this file.
    bool invert = false;
};

// The best sync in a run of soft symbols, for a caller that wants to know
// whether DMR is there at all and does not want bursts.
struct DmrSyncScore {
    // The largest centred correlation of any Table 9.2 pattern, in [0, 1].
    double score = 0.0;
    std::optional<DmrSyncType> type;

    // Where the best one starts: a symbol index over symbols, an input
    // sample over baseband.
    std::size_t position = 0;

    // Syncs that reached the threshold, and how many of those sat a whole
    // number of 144-symbol slots from the best one, within a symbol, which
    // is what a DMR channel does and noise does not.
    std::size_t hits = 0;
    std::size_t hits_on_slot_grid = 0;
};

// Over symbols in units where a Table 10.3 symbol is +3, +1, -1 or -3.
[[nodiscard]] DmrSyncScore dmr_sync_score(std::span<const float> symbols,
                                          double threshold = DmrConfig{}.sync_threshold);

// Over complex baseband at `rate`, through Dmr's own front end and search:
// the signal-labelling hook this was written for has samples and no symbols.
[[nodiscard]] Expected<DmrSyncScore> dmr_sync_score(ConstComplexSpan baseband, SampleRate rate);

// ---------------------------------------------------------------------------
// Burst codecs, exposed so the transmitter and the tests share them
// ---------------------------------------------------------------------------

// Clause 9.1.3: CC and Data Type through Golay (20,8), CC first.
[[nodiscard]] std::array<std::uint8_t, 2 * kDmrSlotTypeHalfBits> dmr_slot_type_bits(
    std::uint8_t colour_code, std::uint8_t data_type);

// Clause 9.1.2: CC, PI and LCSS through the quadratic residue code.
[[nodiscard]] std::array<std::uint8_t, 2 * kDmrEmbHalfBits> dmr_emb_bits(std::uint8_t colour_code,
                                                                         bool pi, std::uint8_t lcss);

// The 96 bits a data burst's BPTC carries for a Data Type, from its octets:
// Figure 7.3, 7.4 and 7.9 and clause 8.2.1. For a voice LC header and a
// terminator, nine LC octets and the Reed-Solomon parity; for the others ten
// octets and the CRC-CCITT; each CRC with its Table B.21 mask.
[[nodiscard]] Expected<std::array<std::uint8_t, kDmrBptcInformationBits>> dmr_data_block_bits(
    DmrDataType data_type, std::span<const std::uint8_t> octets);

// Table D.2, the Idle message's 96 information bits.
[[nodiscard]] std::array<std::uint8_t, kDmrBptcInformationBits> dmr_idle_bits();

// ---------------------------------------------------------------------------
// The decoder
// ---------------------------------------------------------------------------

// WHY THIS DECODER HAS NO SymbolSync
//
// The other digital voice decoders recover symbols with dv_phy.h's
// SymbolSync, a square-law estimator averaged over windows of 32 symbols.
// On a continuous signal that is right. DMR's inbound and direct mode
// channels are not continuous: a burst is 27,5 ms and the carrier is off for
// the rest of its 60 ms frame, so most windows hold noise alone, and the
// estimate those windows give pulls the symbol phase somewhere random before
// the next burst arrives. Measured with it on 2026-09-23, a direct mode call
// at 35 dB in 2500 Hz came back with 12 of its 18 voice bursts exact, one of
// them read a whole symbol late.
//
// A TDMA burst carries its own timing instead: the sync marks the burst
// centre (clause 4.3), so this decoder correlates the sync against the
// filtered discriminator output at every sample, takes the peak to a
// fraction of a sample, and reads the burst's 132 symbols at that phase. A
// burst with no sync, B to F of a voice superframe, is read at the phase its
// slot's last sync gave, 60 ms per burst on; clause 10.1.4's clock drift
// limit moves that by well under a hundredth of a symbol across a
// superframe.
class Dmr {
   public:
    [[nodiscard]] static Expected<Dmr> create(const DmrConfig& config);

    // Consumes complex baseband centred on the carrier and appends every
    // burst completed by it, in stream order. State carries across calls, so
    // a stream gives the same bursts however it is split.
    [[nodiscard]] Status process(ConstComplexSpan samples, std::vector<DmrBurst>& out);

    void reset();

   private:
    friend Expected<DmrSyncScore> dmr_sync_score(ConstComplexSpan baseband, SampleRate rate);

    Dmr() = default;

    // One timeslot's own state. Two of them, told apart by the parity of the
    // slot's position on the 144-symbol grid, so the two TDMA channels are
    // decoded independently of each other.
    struct Lane {
        std::uint8_t slot = 0;

        bool voice = false;
        std::uint8_t letter = 0;  // 0 to 5, A to F, of the last voice burst

        std::array<std::uint8_t, kDmrEmbeddedBits> embedded{};
        std::size_t fragments = 0;

        bool proprietary_expected = false;

        LevelFit fit{};
        bool have_fit = false;
        double sync_score = 0.0;
    };

    // The burst grid: when a slot's first symbol is due, from the last sync
    // found, and how many slots on the grid have been read since.
    struct Grid {
        double anchor = 0.0;  // stream sample of a burst's L66
        std::size_t next = 0;
        std::size_t lane_of_anchor = 0;
        std::size_t misses = 0;
        bool base_station = false;

        // The CACH short LC being gathered, which belongs to the channel and
        // not to either slot.
        std::array<std::uint8_t, kDmrShortLcBits> short_lc{};
        std::size_t short_lc_fragments = 0;
    };

    struct SyncFound {
        DmrSyncType type = DmrSyncType::BsVoice;
        double score = 0.0;
        double time = 0.0;  // stream sample of the sync's first symbol
    };

    // The filtered discriminator at a fractional stream sample, by cubic
    // interpolation, and `count` symbols from `time` on.
    [[nodiscard]] double value_at(double time) const;
    void symbols_at(double time, std::span<float> out) const;

    // The best pattern at one sample, signed as Table 9.2's note reads it.
    [[nodiscard]] std::optional<SyncFound> score_at(double time) const;

    // The peak of the sync correlation within `reach` samples either side of
    // `time`, refined between samples, if it reaches `threshold`.
    [[nodiscard]] std::optional<SyncFound> peak_near(double time, std::size_t reach,
                                                     double threshold) const;

    void read_bits(double time, std::size_t symbols, const LevelFit& fit,
                   std::vector<std::uint8_t>& bits) const;

    // Reads the grid slot whose first symbol is due at `start` for `lane`.
    // True when it was a burst of DMR, a sync or a voice burst whose EMB
    // decoded.
    bool read_slot(double start, std::size_t lane, std::vector<DmrBurst>& out);
    void read_cach(double start, const LevelFit& fit, DmrBurst& burst);
    void read_data(const std::vector<std::uint8_t>& bits, DmrBurst& burst, Lane& lane);
    void start_grid(double start);

    // Keeps the best sync seen, and every one past the threshold when
    // collect_hits_ is set, for dmr_sync_score.
    void note_sync(const SyncFound& found);

    DmrConfig config_{};
    FmDiscriminator discriminator_{};
    RealFir filter_{};
    double samples_per_symbol_ = 0.0;
    double filter_delay_ = 0.0;

    // The filtered discriminator in symbol units, with the front trimmed as
    // the stream goes by and how much has been trimmed, so a stream sample
    // index is trimmed_ plus an index here.
    std::vector<float> shaped_;
    std::size_t trimmed_ = 0;

    // The next stream sample the search tries as a sync's first symbol.
    std::size_t search_ = 0;

    std::optional<Grid> grid_;
    std::array<Lane, 2> lanes_{};

    DmrSyncScore seen_{};
    double best_time_ = 0.0;
    bool collect_hits_ = false;
    std::vector<double> hits_;

    std::vector<float> discriminated_;
    std::vector<float> filtered_;
};

}  // namespace revenant::decode
