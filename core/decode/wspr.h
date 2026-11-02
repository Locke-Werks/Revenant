// WSPR: the 50-bit beacon message, its coding, and a receiver.
//
// SPECIFICATION
//
// A. Talbot G4JNT, "The WSPR Coding Process", June 2009,
// http://www.g4jnt.com/WSPR_Coding_Process.pdf. Every section is used:
// "Source Coding" for the callsign, locator and power fields and the
// integers N and M; "Bit Packing" for the 50 bits and the 31 zero tail;
// "Convolutional Encoding" for the K = 32, r = 1/2 code with taps 0xF2D05351
// and 0xE4613C47; "Interleaving" for the 8-bit bit-reversal shuffle; "Merge
// With Sync Vector" for the 162-bit vector and Symbol = Sync + 2 Data; and
// "Modulation" for 4-FSK at 12000/8192 Hz spacing, one symbol per 8192
// samples at 12 kHz, 162 symbols, 110.6 s.
//
// DEVIATIONS AND GAPS
//
// The document's sync vector prints "0.0,0" near its end; read as three
// values, which is the only reading that makes 162. The document covers
// type 1 messages only ("special cases ... are not covered here"), so a
// decode whose power field does not mark type 1 (M mod 128 below 64) is
// dropped, not guessed at.
//
// The document gives no transmission timing. The two-minute slot and the
// nominal start one second into it are NOT SPECIFIED by it; they are the
// convention WSPR stations keep, used here to cut slots and as the zero of
// the reported DT. Signals starting anywhere from 0 to 4 s in are searched.
//
// The receiver is core/decode/wsjt_slow.h's front end, a non-coherent bit
// likelihood per data symbol from its two candidate tones given the known
// sync bit, and core/decode/fano.h. None of that is in the document.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/decode/wsjt_slow.h"
#include "core/error.h"

namespace revenant::decode {

inline constexpr int kWsprBits = 50;
using WsprPayload = std::array<std::uint8_t, kWsprBits>;

struct WsprMessage {
    std::string call;
    std::string grid;
    int power_dbm = 0;
    // "Illegal values are labelled when decoded": a power not ending in 0, 3
    // or 7.
    bool odd_power = false;
    std::string text;  // "K1ABC FN42 37"
};

// "Source Coding" and "Bit Packing".
[[nodiscard]] Expected<WsprPayload> wspr_pack(std::string_view call, std::string_view grid, int power_dbm);
[[nodiscard]] std::optional<WsprMessage> wspr_unpack(const WsprPayload& bits);

// "Merge With Sync Vector": the 162 sync bits.
[[nodiscard]] std::span<const std::uint8_t> wspr_sync_vector();

// The whole chain to channel symbols 0 to 3.
[[nodiscard]] std::vector<int> wspr_channel_tones(const WsprPayload& bits);

[[nodiscard]] const SlowProtocol& wspr_protocol();

struct WsprConfig {
    SampleRate rate = 12000;
    // WSPR sub-band audio, tone 0. NOT SPECIFIED by the document.
    double low_hz = 1350.0;
    double high_hz = 1650.0;
    WsjtUtcOfSample utc_of_sample;
    // NOT SPECIFIED. Drift searched, Hz over the transmission.
    double max_drift_hz = 4.0;
    int max_candidates = 40;
    double sync_threshold = 0.12;
    std::size_t fano_cycles_per_bit = 10000;
};

struct WsprDecode {
    double slot_utc = 0.0;
    SampleIndex slot_start_sample = 0;
    double frequency_hz = 0.0;  // tone 0, mid transmission
    double dt_seconds = 0.0;    // against the nominal 1 s
    double drift_hz = 0.0;
    double snr_db = 0.0;
    WsprMessage message;
};

class WsprDecoder {
public:
    [[nodiscard]] static Expected<WsprDecoder> create(WsprConfig config);

    // Appends what every slot completed by `audio` decoded to. Independent of
    // how the audio is split into calls.
    [[nodiscard]] Status process(ConstRealSpan audio, std::vector<WsprDecode>& out);

    // One slot of 12 kHz audio starting on its boundary.
    void decode_slot(std::span<const float> slot_12k, double slot_utc, SampleIndex slot_start,
                     std::vector<WsprDecode>& out) const;

private:
    WsprDecoder(WsprConfig config, SlowStream stream);

    WsprConfig config_;
    SlowStream stream_;
};

}  // namespace revenant::decode
