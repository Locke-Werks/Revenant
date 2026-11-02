// VDL Mode 4, the aeronautical VHF data link: an FM receiver's discriminator
// audio to link layer bursts.
//
// SPECIFICATION
//
// ETSI EN 301 842-1 V1.4.1 (2015-04), "VHF air-ground and air-air Digital
// Link (VDL) Mode 4 radio equipment; Technical characteristics and methods
// of measurement for ground-based equipment; Part 1: Physical layer", read
// 2026-11-02 from
// https://www.etsi.org/deliver/etsi_en/301800_301899/30184201/01.04.01_60/en_30184201v010401p.pdf.
// ETSI retains copyright; the clauses are cited and only the constants they
// state are written here. Clauses used: 3.1, the definitions of "data rate"
// (19 200 bit/s), "VDL Mode 4 burst" (source address, burst ID, information,
// slot reservation and FCS, bracketed by opening and closing flags and
// preceded by a preamble) and "frame"; 5.2, the modulation (GFSK, two tones,
// NRZI with the tone toggled for a zero, the 24-bit training sequence
// 0101...0101, modulation index 0.25 and BT 0.28); 6.1.1 (112.000 to 136.975
// MHz in 25 kHz channels); and clause 9's test signal (symbol rate 19 200,
// deviation 2 400 Hz).
//
// DEVIATIONS, AND WHAT THE DOCUMENT DOES NOT SAY
//
// EN 301 842-1 is the physical layer only. It names flags and an FCS and
// does not give the flag's value, the stuffing rule, the FCS generator, the
// bit order within an octet or the layout of a burst's fields; ICAO's VDL
// Mode 4 SARPs and technical manual, which do, and ETSI EN 301 842-2 were not
// read. What this takes for them is HDLC as core/decode/ais.h already states
// it (flag 01111110, a zero after five ones, the 16-bit FCS complemented and
// sent low octet first, each octet least significant bit first), because the
// burst the clause 3.1 definition describes is an HDLC-bounded frame and the
// training sequence and NRZI of clause 5.2 are AIS's to the bit. That is an
// assumption, not a citation, and it has not been checked against a real
// transmitter. AisStats::fcs_uncomplemented is kept so a capture can say
// whether the complement is right. Bursts come out as octets with nothing
// parsed: the field layout is not in the document held.
//
// HOW IT RECOVERS BITS
//
// It is core/decode/ais.h's burst engine at 19 200 bit/s with a 16-slot
// frame bound: eight slicing phases, the least-squares fit of training
// sequence and start flag per burst, and HDLC deframing per phase. That
// file's header explains each choice. One differs: no boxcar ahead of the
// slicer. At BT 0.28 and index 0.25 the transmit filter alone nearly closes
// the eye, and integrating over half a bit or a whole one lost more bursts,
// measured in tests/decode/test_vdl4.cpp.
//
// HOW WELL. Not well enough. A discriminator and a slicer on this waveform
// lose a tenth of the bursts at 40 dB in 2500 Hz and nearly all at 30,
// where clause 6.1.4's -98 dBm for a BER of 1e-4 sits at roughly 32 dB in
// 2500 Hz for a receiver of 10 dB noise figure. Closing that gap wants a
// coherent detector or sequence estimation across the Gaussian's memory,
// which this is not. It decodes strong local signals.
//
// WHAT THIS TAKES AS INPUT
//
// Discriminator audio from an nfm receiver at no less than kVdl4MinimumRate,
// three samples a bit, with a passband of plus and minus kVdl4PassbandHz.
// 12.5 kHz was tried and was no better.
//
// CLEAN ROOM
//
// No VDL Mode 4 implementation was read.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/decode/ais.h"
#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode {

// Clause 3.1, "data rate", and clause 9's test signal.
inline constexpr double kVdl4BitRate = 19200.0;

// Clause 5.2: modulation index 0.25, so the peak deviation is an eighth of
// the bit rate, 2 400 Hz, the figure clause 9's test signal states.
inline constexpr double kVdl4ModulationIndex = 0.25;
inline constexpr double kVdl4DeviationHz = kVdl4ModulationIndex * kVdl4BitRate / 2.0;

// Clause 5.2: BT 0.28.
inline constexpr double kVdl4Bt = 0.28;

// Clause 5.2: 24 bits, 0101 ... 0101.
inline constexpr std::size_t kVdl4TrainingBits = 24;

// Clause 6.2's maximum burst: 16 slots, 212.08 ms at 19 200 symbol/s, 4072
// bits. The frame bound is that, rounded up to a whole slot of 256.
inline constexpr std::size_t kVdl4MaximumFrameBits = 16 * 256;

// Clause 6.1.1.
inline constexpr double kVdl4LowestHz = 112'000'000.0;
inline constexpr double kVdl4HighestHz = 136'975'000.0;

// Three samples to a bit.
inline constexpr SampleRate kVdl4MinimumRate = 57600;

// The receiver's passband either side of the channel centre. See WHAT THIS
// TAKES AS INPUT.
inline constexpr double kVdl4PassbandHz = 9'000.0;

struct Vdl4Burst {
    // Between the flags, FCS removed, as received.
    std::vector<std::uint8_t> octets;
    SampleIndex first_sample = 0;
    SampleIndex last_sample = 0;
    std::uint8_t phase = 0;
};

struct Vdl4Config {
    SampleRate rate = 96000;
    double sync_threshold = 0.7;
    // No boxcar: one tap. Measured best; see tests/decode/test_vdl4.cpp.
    double integrate_bits = 0.0;
};

class Vdl4Decoder {
public:
    [[nodiscard]] static Expected<Vdl4Decoder> create(const Vdl4Config& config);

    // As AisDecoder::process: state carries across calls, so the blocking
    // of the audio changes nothing.
    void process(ConstRealSpan audio, std::vector<Vdl4Burst>& out);

    [[nodiscard]] const AisStats& stats() const { return engine_.stats(); }
    void reset() { engine_.reset(); }

private:
    explicit Vdl4Decoder(AisDecoder engine) : engine_(std::move(engine)) {}
    AisDecoder engine_;
    std::vector<AisMessage> scratch_;
};

}  // namespace revenant::decode
