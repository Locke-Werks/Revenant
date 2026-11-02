// JT4 A to G: channel coding, waveform and a receiver.
//
// SPECIFICATION
//
// A. Talbot G4JNT, "The JT4 Coding Process (Plain Text messages only)", July
// 2009, http://www.g4jnt.com/JT4_Coding_Process.pdf: the 72-bit message
// (core/decode/wsjt72.h), 31 zero tail bits, the K = 32 r = 1/2 code and
// bit-reversal interleaver shared with WSPR and JT9 (core/decode/wsjt_slow.h),
// a single zero bit put in front of the 206 interleaved bits, "Merge With
// Sync Vector" (Symbol = Sync + 2 Data over the printed 207-bit vector), and
// "Modulation": tone spacing 11025 / 2520 = 4.375 Hz times K = 1, 2, 4, 9,
// 18, 36, 72 for JT4A to JT4G, a symbol the reciprocal of 4.375 Hz, 207
// symbols. The document's last page lists the symbols for " G4JNT IO90IV",
// which the tests reproduce.
//
// GAPS
//
// The document gives no timing. The one-minute slot, the nominal start one
// second into it and the 0 to 3 s search window are NOT SPECIFIED by it. Its
// note that the conventional centre lies at 1270.46 Hz, tone 1.5, is
// recorded as jt4_conventional_tone0_hz but the receiver searches a band.
//
// The receiver is core/decode/wsjt_slow.h's front end, the same per-bit
// likelihood as WSPR, and core/decode/fano.h.

#pragma once

#include <span>
#include <vector>

#include "core/decode/wsjt72.h"
#include "core/decode/wsjt_slow.h"
#include "core/error.h"

namespace revenant::decode {

[[nodiscard]] std::span<const std::uint8_t> jt4_sync_vector();

// Submode 0 to 6 for A to G.
[[nodiscard]] const SlowProtocol& jt4_protocol(int submode = 0);

// "11025/1024 * 118 - 1.5 * 4.375 * K".
[[nodiscard]] double jt4_conventional_tone0_hz(int submode);

// The 207 channel symbols, 0 to 3.
[[nodiscard]] std::vector<int> jt4_channel_tones(const Wsjt72& payload);

struct Jt4Config {
    int submode = 0;
    SampleRate rate = 12000;
    double low_hz = 200.0;
    double high_hz = 2500.0;
    WsjtUtcOfSample utc_of_sample;
    // NOT SPECIFIED.
    double max_drift_hz = 0.0;
    int max_candidates = 40;
    double sync_threshold = 0.12;
    std::size_t fano_cycles_per_bit = 10000;
};

class Jt4Decoder {
public:
    [[nodiscard]] static Expected<Jt4Decoder> create(Jt4Config config);
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<Wsjt72Decode>& out);
    void decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                     std::vector<Wsjt72Decode>& out) const;

private:
    Jt4Decoder(Jt4Config config, SlowStream stream);

    Jt4Config config_;
    SlowStream stream_;
};

}  // namespace revenant::decode
