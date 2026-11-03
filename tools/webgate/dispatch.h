// Maps a phone's JSON commands onto its engine session.
//
// RESULT SHAPES, which web/ consumes exactly (frequencies are ABSOLUTE hertz;
// the engine's offset-from-baseband is converted here so the phone never sees
// it):
//
//   addVrx, tune, setMode, setBandwidth, setSquelch, setVrxTranscribe
//        -> {"vrx", "hz", "mode", "bandwidthHz", "squelchDbfs", "grantedLowHz",
//            "grantedHighHz", "clamped", "clampReason"?, "transcribe",
//            "transcribing"}
//        setMode and setBandwidth add "replaced": <old id>, because the engine
//        will not change a demodulator or filter width in place, so the
//        receiver is recreated under a NEW id. tune and setSquelch keep the id.
//   removeVrx          -> {"vrx"}
//   setCenter          -> {"grantedHz", "removed":[vrx ids the retune dropped]}
//   setGain            -> {"stage", "grantedDb"}
//   setGainAuto        -> {"stage", "on"}
//   listSources        -> {"sources":[{uri, backend, name, available,
//                          unavailable?, notes[], tuneRanges[], sampleRates[],
//                          minRate, maxRate, gainStages[], seekable}]}
//   openSource         -> {"source": <one entry as above> | null}
//   setView            -> {"centerHz", "spanHz", "widthPx"}
//   audio              -> {"vrx", "on"}
//   keepListening      -> {"on"}
//   setTranscription   -> {"enabled", "modelState", "modelName", "bytesDone",
//                          "bytesTotal", "detail", "backend", "receivers",
//                          "queued", "transcribed", "dropped"}
//
// Modes are lowercase: raw am nfm wfm usb lsb dsb cw p25p1 dstar tetra dmr sam.

#pragma once

#include <string>
#include <string_view>

#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"

namespace revenant::webgate {

// One reply line (ok_reply or error_reply) for a parsed command. Engine error
// text goes back verbatim. Bad arguments are refused with a sentence naming
// the argument, without calling the engine.
[[nodiscard]] std::string dispatch(PhoneSession& session, const Command& command);

// For the socket layer: parses a text frame and dispatches it. A frame
// parse_command refuses is answered with an error_reply when it carried a
// usable id; otherwise there is nothing the phone could match and the result
// is empty.
[[nodiscard]] std::string handle_text(PhoneSession& session, std::string_view text);

}  // namespace revenant::webgate
