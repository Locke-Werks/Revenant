#include "core/characterise/catalogue.h"

#include <algorithm>
#include <cmath>

namespace revenant::characterise {
namespace {

using F = ModulationFamily;

// The catalogue.
//
// Every figure is from the docs/modes.md row the citation names, and that
// row names the standard it came from. Nothing here was taken out of an
// implementation, and nothing here is remembered: where docs/modes.md
// states a bit rate rather than a symbol rate, the row below carries the
// symbol rate only where the two are the same by the modulation's own
// definition, which for a two-level scheme they are.
constexpr ProtocolRow kCatalogue[] = {
    // ----------------------------------------------------------------
    // Four-level FSK at 4800 symbols a second. Five systems, one physical
    // layer, and this block is the reason match_protocols returns a list.
    // ----------------------------------------------------------------
    {"DMR Tier I/II/III", F::Fsk, 4, 0, 4800.0, 12500.0, 0.0,
     "ETSI TS 102 361-1 through -4, via the DMR row of docs/modes.md"},
    {"NXDN at 12.5 kHz", F::Fsk, 4, 0, 4800.0, 12500.0, 0.0,
     "NXDN TS 1-A v1.3 (November 2011), via the NXDN row of docs/modes.md"},
    {"P25 Phase 1 C4FM", F::Fsk, 4, 0, 4800.0, 12500.0, 0.0,
     "TIA-102.BAAA-A, via the P25 Phase 1 row of docs/modes.md"},
    {"M17", F::Fsk, 4, 0, 4800.0, 9000.0, 0.0,
     "M17 Protocol Specification Part I, via the M17 row of docs/modes.md"},
    {"Yaesu System Fusion (C4FM)", F::Fsk, 4, 0, 4800.0, 12500.0, 0.0,
     "Yaesu Amateur Radio Digital Standards rev 1.02, NOT OBTAINED: the "
     "document endpoint returned HTTP 500 from two network paths on 2026-09-20, "
     "per the open-documents table of docs/modes.md. The physical layer is "
     "shared with the rest of this block, which is why the row stands"},

    // ----------------------------------------------------------------
    // Four-level FSK at 2400, which is the 6.25 kHz half of the same
    // family. A measured bandwidth is what separates this block from the
    // one above when the symbol rate is ambiguous.
    // ----------------------------------------------------------------
    {"NXDN at 6.25 kHz", F::Fsk, 4, 0, 2400.0, 6250.0, 0.0,
     "NXDN TS 1-A v1.3 (November 2011), via the NXDN row of docs/modes.md"},
    {"dPMR446 and licensed dPMR modes 2/3", F::Fsk, 4, 0, 2400.0, 6250.0, 0.0,
     "ETSI TS 102 490 and TS 102 658, via the dPMR row of docs/modes.md"},

    // ----------------------------------------------------------------
    // Two-level, in symbol-rate order.
    // ----------------------------------------------------------------
    {"RTTY (ITA2)", F::Fsk, 2, 0, 45.45, 300.0, 0.0,
     "ITU-T Recommendation S.1 for the alphabet; the 45.45 baud and 170 Hz "
     "shift convention is practice rather than a standard and docs/modes.md's "
     "RTTY row cites it as such"},
    {"NAVTEX, SITOR-B and HF DSC", F::Fsk, 2, 0, 100.0, 500.0, 0.0,
     "ITU-R M.540-2 (06/1990) for NAVTEX, M.625-4 (03/2012) for SITOR and "
     "M.493-16 (12/2023) for DSC, via the maritime rows of docs/modes.md. One "
     "row because all three are 100 baud with a 170 Hz shift and nothing this "
     "stage measures tells them apart"},
    {"POCSAG at 512 bit/s", F::Fsk, 2, 0, 512.0, 25000.0, 0.0,
     "ITU-R M.584-2 (11/1997), via the POCSAG row of docs/modes.md"},
    {"POCSAG at 1200 bit/s", F::Fsk, 2, 0, 1200.0, 25000.0, 0.0,
     "ITU-R M.584-2 (11/1997), via the POCSAG row of docs/modes.md"},
    {"AX.25 at 1200 baud (Bell 202 AFSK)", F::Fsk, 2, 0, 1200.0, 12500.0, 0.0,
     "AX.25 v2.2 (TAPR/ARRL, July 1998) for the link layer; the modem has no "
     "standards document and docs/modes.md's AX.25 row cites it to Bell 202"},
    {"POCSAG at 2400 bit/s", F::Fsk, 2, 0, 2400.0, 25000.0, 0.0,
     "ITU-R M.584-2 (11/1997), via the POCSAG row of docs/modes.md"},
    {"D-STAR DV", F::Fsk, 2, 0, 4800.0, 6250.0, 0.0,
     "JARL Digitalization Technology Standard for D-STAR, via the D-STAR DV "
     "row of docs/modes.md. GMSK, so two levels at 4800 bit/s"},
    {"AIS", F::Fsk, 2, 0, 9600.0, 25000.0, 0.0,
     "ITU-R M.1371-6 (02/2026), via the AIS row of docs/modes.md. GMSK at "
     "BT 0.4, so two levels at 9600 bit/s"},

    // ----------------------------------------------------------------
    // The rest of the FSK tone counts.
    // ----------------------------------------------------------------
    {"2G ALE", F::Fsk, 8, 0, 125.0, 3000.0, 0.0,
     "MIL-STD-188-141D Appendix A, via the 2G ALE row of docs/modes.md. Eight "
     "tones on 250 Hz spacing from 750 to 2500 Hz"},
    {"ERMES", F::Fsk, 4, 0, 3125.0, 25000.0, 0.0,
     "ETSI ETS 300 133-4, via the ERMES row of docs/modes.md. 4-PAM on FM at "
     "3125 baud"},
    {"FLEX at 1600 sym/s", F::Fsk, 2, 0, 1600.0, 25000.0, 0.0,
     "ARIB STD-T43, NOT OBTAINED: docs/modes.md's FLEX row records that no "
     "English edition was located and that Motorola never published its own"},
    {"FLEX at 3200 sym/s (4-level)", F::Fsk, 4, 0, 3200.0, 25000.0, 0.0,
     "ARIB STD-T43, NOT OBTAINED: docs/modes.md's FLEX row records that no "
     "English edition was located and that Motorola never published its own"},

    // ----------------------------------------------------------------
    // Linear modulations.
    // ----------------------------------------------------------------
    {"PSK31", F::Psk, 0, 2, 31.25, 100.0, 0.0,
     "G3PLX in RadCom December 1998 and January 1999, reprinted free by the "
     "ARRL with the full varicode table, via the PSK31 row of docs/modes.md"},
    {"STANAG 4529", F::Psk, 0, 0, 1200.0, 1240.0, 0.0,
     "STANAG 4529 Edition 1 (1995), via the STANAG 4529 row of "
     "docs/modes.md. A parameter variant of 4285; the constellation runs from "
     "BPSK to 8PSK, so the order is left unstated here rather than guessed"},
    {"MIL-STD-188-110 serial tone", F::Psk, 0, 0, 2400.0, 3000.0, 0.0,
     "MIL-STD-188-110C Change 1 and -110D, via the serial tone row of "
     "docs/modes.md. BPSK through 64-QAM on an 1800 Hz subcarrier, so the "
     "order is not fixed by the waveform family"},
    {"STANAG 4539", F::Psk, 0, 0, 2400.0, 3000.0, 0.0,
     "MIL-STD-188-110B Appendix C, which docs/modes.md's STANAG 4539 row "
     "names as specifying the identical waveform and being the free document "
     "of the two"},
    {"STANAG 4415", F::Psk, 0, 2, 2400.0, 3000.0, 0.0,
     "The 75 bit/s low-SNR mode of MIL-STD-188-110B and -110C, via the "
     "STANAG 4415 row of docs/modes.md"},
    {"TETRA V+D and Direct Mode", F::Psk, 0, 4, 18000.0, 25000.0, 0.0,
     "ETSI EN 300 392-2 and the EN 300 396 series, via the TETRA row of "
     "docs/modes.md. pi/4-DQPSK at 18000 sym/s"},

    // ----------------------------------------------------------------
    // Multicarrier. One row, and core/characterise/catalogue.h says why.
    // ----------------------------------------------------------------
    {"DAB Mode I", F::Ofdm, 0, 0, 0.0, 1536000.0, 0.001,
     "ETSI EN 300 401 V2.1.1, via the DAB row of docs/modes.md: 1536 carriers "
     "on 1 kHz spacing, which fixes the useful symbol at 1 ms, inside a "
     "1246 us total symbol. The subcarriers are pi/4-DQPSK and the order is "
     "left unstated in this row, because nothing in core/characterise reads a "
     "subcarrier constellation"},

    // ----------------------------------------------------------------
    // Analogue, which the characteriser reaches often and docs/modes.md
    // does not table because its subject is digital modes.
    // ----------------------------------------------------------------
    {"FM broadcast", F::AnalogueFm, 0, 0, 0.0, 200000.0, 0.0,
     "EN 50067:1998 clause 1.3 for the 75 kHz peak deviation of the "
     "multiplex, cited in core/dsp/synth/rds_mod.h; the 200 kHz channel "
     "spacing is the ITU Region 2 raster and is stated here from general "
     "engineering knowledge rather than from a document anybody opened"},
    {"Narrowband FM land mobile", F::AnalogueFm, 0, 0, 0.0, 12500.0, 0.0,
     "core/dsp/synth/modulators.h's NfmParams, which states 5 kHz of "
     "deviation as the land mobile figure 25 kHz spacing was built around and "
     "2.5 kHz as the narrowband refarming figure"},
};

[[nodiscard]] bool rate_matches(double row_value, double measured, double tolerance) {
    if (row_value <= 0.0) {
        return true;
    }
    if (measured <= 0.0) {
        // The row states a figure and nothing measured it, so the numbers
        // do not support this row. Reporting it anyway is how a candidate
        // list turns into a guess.
        return false;
    }
    return std::abs(measured - row_value) <= tolerance * row_value;
}

}  // namespace

std::string_view modulation_family_name(ModulationFamily family)
{
    switch (family) {
        case ModulationFamily::Unknown:
            return "unknown";
        case ModulationFamily::Unmodulated:
            return "unmodulated carrier";
        case ModulationFamily::AnalogueFm:
            return "analogue FM";
        case ModulationFamily::Fsk:
            return "FSK";
        case ModulationFamily::Psk:
            return "PSK";
        case ModulationFamily::Ofdm:
            return "OFDM";
    }
    return "unknown";
}

std::span<const ProtocolRow> protocol_catalogue()
{
    return std::span<const ProtocolRow>(kCatalogue);
}

std::vector<ProtocolCandidate> match_protocols(const ProtocolQuery& query)
{
    std::vector<ProtocolCandidate> candidates;
    if (query.family == ModulationFamily::Unknown) {
        return candidates;
    }

    for (const ProtocolRow& row : kCatalogue) {
        if (row.family != query.family) {
            continue;
        }
        if (row.tone_count != 0 && query.tone_count != 0 &&
            row.tone_count != query.tone_count) {
            continue;
        }
        if (row.tone_count != 0 && query.tone_count == 0) {
            continue;
        }
        if (row.psk_order != 0 && query.psk_order != 0 && row.psk_order != query.psk_order) {
            continue;
        }
        if (!rate_matches(row.symbol_rate_hz, query.symbol_rate_hz, query.rate_tolerance)) {
            continue;
        }
        if (!rate_matches(row.ofdm_symbol_seconds, query.ofdm_symbol_seconds,
                          query.rate_tolerance)) {
            continue;
        }
        if (row.channel_spacing_hz > 0.0 && query.bandwidth_hz > 0.0 &&
            query.bandwidth_factor > 1.0) {
            const double low = row.channel_spacing_hz / query.bandwidth_factor;
            const double high = row.channel_spacing_hz * query.bandwidth_factor;
            if (query.bandwidth_hz < low || query.bandwidth_hz > high) {
                continue;
            }
        }

        ProtocolCandidate candidate;
        candidate.row = row;
        if (row.symbol_rate_hz > 0.0 && query.symbol_rate_hz > 0.0) {
            candidate.symbol_rate_error_percent =
                100.0 * std::abs(query.symbol_rate_hz - row.symbol_rate_hz) / row.symbol_rate_hz;
        }
        candidates.push_back(candidate);
    }

    // Nearest first, then by name so the order is stable rather than
    // whatever the table happened to be written in. Two rows sharing a
    // symbol rate is the normal case in this catalogue, not the exception.
    std::sort(candidates.begin(), candidates.end(),
              [](const ProtocolCandidate& left, const ProtocolCandidate& right) {
                  if (left.symbol_rate_error_percent != right.symbol_rate_error_percent) {
                      return left.symbol_rate_error_percent < right.symbol_rate_error_percent;
                  }
                  return left.row.name < right.row.name;
              });
    return candidates;
}

std::string summarise_candidates(std::span<const ProtocolCandidate> candidates)
{
    std::string out;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) {
            out += index + 1 == candidates.size() ? " or " : ", ";
        }
        out += candidates[index].row.name;
    }
    return out;
}

}  // namespace revenant::characterise
