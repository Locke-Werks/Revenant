// GNSS spread-spectrum front end: ranging codes, acquisition, tracking, and
// the per-code-period prompt stream every navigation-message decoder reads.
//
// SPECIFICATION
//
// The ranging codes are built from the generators the interface documents
// state, and every one is checked in tests/decode/test_gnss_acq.cpp against
// the first chips those documents print:
//
//   - GPS C/A, "IS-GPS-200 Revision N" (2022-08-01), free from gps.gov.
//     Section 3.3.2.3 for the G1 and G2 generators and their all-ones
//     initialisation; Table 3-Ia for PRNs 1 to 37 and Table 3-Ib for 38 to
//     63, both read as G2 delays in chips; Table 6-I, section 6.3.6.1, for
//     PRNs 64 to 210, which covers SBAS (120 to 158) and QZSS (193 to 202).
//     Transcribed with citation, as a US Government work.
//   - QZSS L1 C/A, "IS-QZSS-PNT-006", Cabinet Office of Japan, Table
//     3.2.2-1, which repeats the Table 6-I delays for 193 to 202.
//   - GLONASS L1OF standard-accuracy code, "GLONASS Interface Control
//     Document, Edition 5.1" (2008), section 3.3.2.1: G(X) = 1 + X^5 + X^9,
//     all ones, output from stage 7, first chips 111111100.
//   - BeiDou B1I, "BDS-SIS-ICD-B1I-3.0" (2019-02), section 4.3 and Table
//     4-1: two 11-stage generators, both initialised 01010101010, the G2
//     phase made by XORing the two stages Table 4-1 names, truncated to 2046
//     chips. Section 5.2.1 for the 20-bit Neumann-Hoffman code.
//
// WHAT THE FRONT END DOES
//
// Acquisition is the textbook FFT parallel code-phase search (circular
// correlation by the convolution theorem, one transform of the local code per
// PRN reused across Doppler bins), done on a block of whole code periods
// resampled by linear interpolation to a power-of-two number of samples per
// period so characterise::fft_in_place serves every rate the tap can deliver.
// Coherent over one code period, non-coherent over `noncoherent_periods`.
// The statistic is the peak over the mean of every other cell, and a found
// satellite's Doppler is refined from the phase advance of its own prompt
// across the block, squared so that a data or overlay sign flip does not
// alias it.
//
// Tracking is one channel per satellite at the input rate: a carrier NCO,
// early, prompt and late correlators half a chip apart, dumped once a code
// period. The carrier loop starts as a first-order FLL (cross-product
// discriminator, sign-corrected for data), hands over to a second-order Costas
// PLL with the FLL still assisting, and the code loop is a carrier-aided
// first-order DLL on the normalised early-minus-late envelope. C/N0 comes from
// the second and fourth moments of the prompt over 200 code periods. These
// are the standard textbook loops: E. D. Kaplan and C. J. Hegarty,
// "Understanding GPS/GNSS", 3rd edition, chapter 8, and nothing more specific.
//
// WHAT IT HANDS ON
//
// For every code period of every tracked satellite, a Prompt: the complex
// prompt correlator after carrier wipe-off, its absolute sample index, the
// running Doppler and C/N0 estimate. Bit synchronisation and everything after
// it belong to the mode, because the bit and symbol structure is the part
// that differs: 20 periods a bit for GPS, an NH overlay for BeiDou D1, a
// 10 ms meander for GLONASS.
//
// CHUNK INDEPENDENCE
//
// Acquisition runs on blocks aligned to absolute sample indices, and a
// satellite found in a block is tracked from that block's first sample, so
// the prompt stream of every satellite is the same whatever sizes the input
// arrives in. tests/decode/test_gnss_acq.cpp holds it to that.
//
// CLEAN ROOM
//
// No GNSS receiver implementation was read: not GNSS-SDR, RTKLIB,
// GNSS-SDRLIB or any other. The code tables are transcribed from the
// interface documents above, with the document's own first-chip columns
// checked by test.

#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"

namespace revenant::decode::gnss {

using dsp::Complex32;
using dsp::SampleIndex;

// ---------------------------------------------------------------------------
// Ranging codes. Chips as +1/-1, logic 0 mapped to +1.

// GPS and GPS-compatible C/A Gold codes, PRN 1 to 210. Empty for an
// unassigned number.
[[nodiscard]] std::vector<std::int8_t> gps_ca_code(unsigned prn);

// The G2 delay in chips IS-GPS-200N gives the PRN, or 0 when none.
[[nodiscard]] unsigned gps_ca_g2_delay(unsigned prn);

// GLONASS L1OF standard-accuracy ranging code, 511 chips.
[[nodiscard]] std::vector<std::int8_t> glonass_st_code();

// BeiDou B1I ranging code, PRN 1 to 63, 2046 chips. Empty out of range.
[[nodiscard]] std::vector<std::int8_t> beidou_b1i_code(unsigned prn);

// BeiDou D1 Neumann-Hoffman secondary code, 20 chips, BDS-SIS-ICD-B1I-3.0
// section 5.2.1: 00000100110101001110.
[[nodiscard]] std::vector<std::int8_t> beidou_nh20();

// The first `count` chips of a code as 0/1 bits packed MSB-first, for
// comparing against a document's octal first-chips column.
[[nodiscard]] unsigned first_chips(std::span<const std::int8_t> code, unsigned count);

// ---------------------------------------------------------------------------
// One signal to search for and track.
struct SignalSpec {
    unsigned id = 0;  // the caller's satellite number, reported back
    std::vector<std::int8_t> code;
    double chip_rate_hz = 1.023e6;
    double carrier_hz = 1575.42e6;  // for scaling code Doppler from carrier Doppler
    // Where the signal's nominal carrier sits relative to the stream's DC.
    // Zero for CDMA signals on a tap centred on them; the FDMA channel offset
    // for GLONASS.
    double offset_hz = 0.0;
    // A second code on the same carrier and timing, correlated at the prompt
    // only and reported as Prompt::aux: GPS L1C tracks its pilot and reads its
    // data channel this way. Same length as `code`, or empty.
    std::vector<std::int8_t> aux_code;
};

struct AcquisitionConfig {
    double doppler_max_hz = 5000.0;
    // A quarter of 1/T for a 1 ms code, so the squared-phase refinement,
    // unambiguous over +-1/(4T), always starts inside its range.
    double doppler_step_hz = 250.0;
    unsigned noncoherent_periods = 40;
    // Peak over the mean of every other cell. Set from the noise-only runs in
    // tests/decode/test_gnss_acq.cpp, whose worst case is printed there.
    double threshold = 2.1;
    // How long, in code periods, before a satellite that was not found is
    // searched for again. Acquisition is the expensive part.
    unsigned retry_periods = 30000;
};

struct TrackingConfig {
    double fll_bandwidth_hz = 10.0;
    double pll_bandwidth_hz = 15.0;
    double dll_bandwidth_hz = 1.0;
    double early_late_spacing_chips = 0.5;  // each side of prompt
    unsigned fll_only_periods = 300;
    unsigned fll_assist_periods = 1000;
    // A channel whose C/N0 estimate stays under this for `loss_periods` is
    // dropped and its satellite searched for again.
    double loss_cn0_dbhz = 24.0;
    unsigned loss_periods = 1500;
};

struct FrontEndConfig {
    dsp::SampleRate sample_rate = 2'048'000;
    std::size_t max_channels = 12;
    AcquisitionConfig acquisition;
    TrackingConfig tracking;
};

// One code period of one tracked signal.
struct Prompt {
    unsigned id = 0;
    SampleIndex sample = 0;    // absolute index of the period's first sample
    std::uint64_t period = 0;  // periods since the channel started
    std::complex<double> prompt;
    std::complex<double> aux;  // SignalSpec::aux_code at the prompt, else zero
    // The code phase, in chips, the loop held at `sample`: the fraction of a
    // chip by which the period really began before that sample.
    double code_phase_chips = 0.0;
    double doppler_hz = 0.0;   // carrier Doppler, relative to offset_hz
    double cn0_dbhz = 0.0;     // 0 until the first estimate
    bool phase_locked = false;
};

struct AcquisitionEvent {
    unsigned id = 0;
    SampleIndex block_start = 0;
    bool found = false;
    double code_phase_chips = 0.0;
    double doppler_hz = 0.0;
    double peak_ratio = 0.0;
};

struct LossEvent {
    unsigned id = 0;
    SampleIndex sample = 0;
};

struct FrontEndOutput {
    std::vector<Prompt> prompts;
    std::vector<AcquisitionEvent> acquisitions;
    std::vector<LossEvent> losses;
};

// Single-block acquisition, exposed for the threshold measurements. The
// block is whole code periods of `spec` at `sample_rate`.
[[nodiscard]] AcquisitionEvent acquire(std::span<const Complex32> block,
                                       dsp::SampleRate sample_rate,
                                       const SignalSpec& spec,
                                       const AcquisitionConfig& config);

class TrackingChannel {
public:
    TrackingChannel(const SignalSpec& spec, const FrontEndConfig& config,
                    SampleIndex start_sample, double code_phase_chips, double doppler_hz);

    // Consume samples starting at absolute index `first`, which must be the
    // channel's next expected sample.
    void process(std::span<const Complex32> samples, SampleIndex first,
                 std::vector<Prompt>& out);

    [[nodiscard]] unsigned id() const { return spec_.id; }
    [[nodiscard]] SampleIndex next_sample() const { return next_; }
    [[nodiscard]] bool lost() const { return lost_; }
    [[nodiscard]] double cn0_dbhz() const { return cn0_; }
    [[nodiscard]] double doppler_hz() const { return carrier_hz_; }

private:
    void dump(std::vector<Prompt>& out);

    SignalSpec spec_;
    TrackingConfig cfg_;
    double fs_;
    double code_len_;
    double period_s_;
    SampleIndex next_;
    SampleIndex period_start_;
    std::uint64_t periods_ = 0;

    double code_phase_;      // chips into the current period
    double code_rate_;       // chips per second, current
    double carrier_hz_;      // Doppler estimate, loop state
    double nco_hz_;          // what the NCO runs at, relative to DC minus offset
    double carrier_phase_ = 0.0;  // cycles
    // The NCO's phasor, carried across calls so the output does not depend
    // on how the input was chunked.
    std::complex<double> lo_{1.0, 0.0};
    std::complex<double> lo_step_{1.0, 0.0};
    bool nco_ready_ = false;
    std::complex<double> early_{}, prompt_{}, late_{}, aux_{};
    double start_phase_ = 0.0;  // code phase at period_start_
    std::complex<double> last_prompt_{};
    bool have_last_ = false;

    double m2_ = 0.0, m4_ = 0.0;
    unsigned moments_ = 0;
    double cn0_ = 0.0;
    double lock_metric_ = 0.0;
    unsigned weak_ = 0;
    bool lost_ = false;
};

// Acquisition scheduling and a bank of tracking channels over one stream.
class FrontEnd {
public:
    // Every signal in `signals` must have the same code period, which is
    // the block granularity. The caller groups signals by system.
    [[nodiscard]] static Expected<FrontEnd> create(const FrontEndConfig& config,
                                                   std::vector<SignalSpec> signals);

    // Any chunk size, including one sample. Appends to `out`.
    void feed(std::span<const Complex32> samples, FrontEndOutput& out);

    [[nodiscard]] std::size_t tracked() const { return channels_.size(); }
    [[nodiscard]] SampleIndex samples_seen() const { return next_; }

private:
    FrontEnd() = default;
    void run_block(FrontEndOutput& out);

    FrontEndConfig config_{};
    std::vector<SignalSpec> signals_;
    std::size_t block_samples_ = 0;
    std::uint64_t block_periods_ = 0;
    std::vector<Complex32> block_;
    SampleIndex block_start_ = 0;
    SampleIndex next_ = 0;
    std::uint64_t blocks_done_ = 0;
    std::map<unsigned, std::uint64_t> retry_at_block_;
    std::vector<TrackingChannel> channels_;
};

}  // namespace revenant::decode::gnss
