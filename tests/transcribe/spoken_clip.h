// Speech for the recogniser's tests, made at test time by Windows' own
// text-to-speech.
//
// WHY SYNTHESISED AND NOT A RECORDING IN THE TREE. The repository holds no
// audio or IQ, tests/corpus/README.md says why, and a few seconds of WAV would
// be the first binary blob a test reads. SAPI ships with every Windows desktop
// install, so the clip costs no new dependency: sapi.lib is in the Windows SDK
// the build already uses. What it cannot do is sound like a radio; this is a
// check that words come back, not a measure of accuracy on real traffic.
//
// Deterministic for a given machine and voice, not across them: SAPI's voices
// differ between Windows versions. The tests assert on words a recogniser
// cannot miss on clean synthetic speech, never on the exact transcript.

#pragma once

#include <string>
#include <vector>

#include "core/error.h"

namespace revenant::test {

// The text spoken at 16000 S/s mono, as floats in [-1, 1]. An error, which the
// tests turn into a skip, when SAPI or a voice is missing.
[[nodiscard]] Expected<std::vector<float>> speak_16k(const std::wstring& text);

}  // namespace revenant::test
