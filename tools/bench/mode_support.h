// Scoring and payload helpers shared by the mode subjects in
// tools/bench/mode_subjects.h. Internal to the bench library.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "tools/bench/mode_subjects.h"
#include "tools/bench/sweep.h"

namespace revenant::bench::detail {

// The harness moves complex baseband. An audio mode rides in the real part and
// its subject reads only that, as the RDS subject does.
[[nodiscard]] std::vector<dsp::Complex32> audio_as_baseband(std::span<const float> audio);
[[nodiscard]] std::vector<float> real_part(dsp::ConstComplexSpan samples);

[[nodiscard]] double real_mean_power(std::span<const float> audio);

// One character of `pool` per payload byte, by byte value modulo the pool.
[[nodiscard]] std::string text_from_payload(std::span<const std::uint8_t> payload, std::string_view pool);
[[nodiscard]] std::u32string widen(std::string_view text);

// Levenshtein distance, so a dropped or invented character costs one rather
// than shifting every comparison after it. The same measure every text-mode
// case in tests/decode uses.
[[nodiscard]] std::size_t edit_distance(std::string_view a, std::string_view b);

// A text trial: one unit per character sent, the edit distance capped at the
// characters sent, decoded when the text came back exactly.
[[nodiscard]] TrialResult score_text(std::string_view sent, std::string_view got);

// A framed trial: one unit per frame, page or message sent.
[[nodiscard]] TrialResult score_units(std::size_t sent, std::size_t received);

// A bit trial the decoder produced nothing alignable for: every bit half
// wrong, as the RDS subject scores a trial that never locked.
[[nodiscard]] TrialResult failed_bits(std::size_t bits);

// Bits of the payload, most significant first, one per byte.
[[nodiscard]] std::vector<std::uint8_t> payload_bits(std::span<const std::uint8_t> payload);

[[nodiscard]] std::string bits_as_text(std::span<const std::uint8_t> bits);
[[nodiscard]] std::string hex_lines(std::span<const std::uint8_t> payload, std::size_t bytes_per_line);

// The per-family builders mode_subjects.cpp dispatches to, one file each, by
// the decoders they share: the two-level FSK modes, the tone modes read
// through core/decode/tone_frontend.h, and the complex baseband modes. Each
// fails on a name it does not own.
[[nodiscard]] Expected<ModeSubject> make_fsk_subject(std::string_view mode);
[[nodiscard]] Expected<ModeSubject> make_tone_subject(std::string_view mode);
[[nodiscard]] Expected<ModeSubject> make_dv_subject(std::string_view mode);

// AIS and DSC, tools/bench/maritime_subjects.cpp.
[[nodiscard]] Expected<ModeSubject> make_maritime_subject(std::string_view mode);

}  // namespace revenant::bench::detail
