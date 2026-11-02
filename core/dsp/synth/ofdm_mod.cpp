#include "core/dsp/synth/ofdm_mod.h"

#include <algorithm>
#include <cmath>
#include <complex>

#include "core/characterise/transform.h"

namespace revenant::siggen {

Expected<OfdmTransmission> ofdm_modulate(const decode::OfdmConfig& config,
                                         const std::vector<std::vector<Complex32>>& grid,
                                         std::size_t first_pilot_phase)
{
    if (auto ok = decode::validate(config); !ok) {
        return std::unexpected(ok.error());
    }
    const std::size_t n = config.fft_size;
    const std::size_t cp = config.cp_length;
    const std::size_t nc = config.carriers.size();
    const std::size_t period = config.pilot_table.size();

    OfdmTransmission tx;
    tx.grid = grid;
    tx.samples.reserve(grid.size() * (n + cp));
    std::vector<std::complex<double>> work(n);
    for (std::size_t s = 0; s < grid.size(); ++s) {
        auto& row = tx.grid[s];
        if (row.size() != nc) {
            return fail("ofdm_modulate: grid row width differs from the carrier count");
        }
        for (const auto& pilot : config.pilot_table[(first_pilot_phase + s) % period]) {
            for (std::size_t c = 0; c < nc; ++c) {
                if (config.carriers[c] == pilot.carrier) {
                    row[c] = pilot.value;
                }
            }
        }
        std::fill(work.begin(), work.end(), std::complex<double>{});
        for (std::size_t c = 0; c < nc; ++c) {
            const long long b = (static_cast<long long>(config.carriers[c]) +
                                 static_cast<long long>(n)) % static_cast<long long>(n);
            // Inverse transform as conj(fft(conj(X))), so the conjugate goes in.
            work[static_cast<std::size_t>(b)] = std::conj(std::complex<double>(row[c].real(), row[c].imag()));
        }
        characterise::fft_in_place(work);
        const double scale = 1.0 / std::sqrt(static_cast<double>(n));
        std::vector<Complex32> symbol(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = std::conj(work[i]) * scale;
            symbol[i] = Complex32(static_cast<float>(v.real()), static_cast<float>(v.imag()));
        }
        tx.samples.insert(tx.samples.end(), symbol.end() - static_cast<std::ptrdiff_t>(cp), symbol.end());
        tx.samples.insert(tx.samples.end(), symbol.begin(), symbol.end());
    }
    return tx;
}

}  // namespace revenant::siggen
