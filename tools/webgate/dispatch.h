// Maps a phone's JSON commands onto its engine session.
//
// RESULT SHAPES, which web/ consumes exactly (frequencies are ABSOLUTE hertz;
// the engine's offset-from-baseband is converted here so the phone never sees
// it):
//
//   addVrx, tune, setMode, setBandwidth, setSquelch, setVrxTranscribe
//        -> {"vrx", "hz", "mode", "bandwidthHz", "squelchDbfs", "grantedLowHz",
//            "grantedHighHz", "clamped", "clampReason"?, "transcribe",
//            "transcribing", "decoders":[names]}
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
//   setDetection       -> {"minConfidence"}
//   setTranscription   -> {"enabled", "modelState", "modelName", "bytesDone",
//                          "bytesTotal", "detail", "backend", "receivers",
//                          "queued", "transcribed", "dropped"}
//   listDecoders       -> {"decoders":[{name, description, input, modes[]}],
//                          "voiceModes":[modes whose voice plays] | null}
//   setDecoders        -> {"vrx", "decoders":[names]}, the receiver's whole
//                         set, which replaces the last; [] decodes nothing
//
// Modes are lowercase: raw am nfm wfm usb lsb dsb cw p25p1 dstar tetra dmr sam.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/rpc/types.h"
#include "tools/webgate/protocol.h"
#include "tools/webgate/session.h"

namespace revenant::webgate {

// The "transcriptionStatus" event for the engine's speech state. Its fields
// are those of the setTranscription result.
[[nodiscard]] std::string transcription_status_event(const rpc::TranscriptionStatus& status);

// Whether now differs from sent enough to send another event: the switch, the
// model state, its name, the detail sentence, or the download by a whole
// percent. Receiver counts and the transcribed tally move constantly and do
// not count.
[[nodiscard]] bool transcription_report_due(const rpc::TranscriptionStatus& sent,
                                            const rpc::TranscriptionStatus& now);

// One reply line (ok_reply or error_reply) for a parsed command. Engine error
// text goes back verbatim. Bad arguments are refused with a sentence naming
// the argument, without calling the engine.
// The digital voice modes whose voice will play, as listDecoders reports
// them: P25 always (its vocoder is built in), and every mode a loaded
// plugin's offer serves. Null when that cannot be told: an engine older than
// VocoderOfferInfo::modes reports loaded offers with no modes, and inferring
// "no vocoder" from that would mark a working DMR receiver as silent.
[[nodiscard]] std::optional<std::vector<std::string>> voice_modes(const rpc::VocoderPlugins& plugins);

[[nodiscard]] std::string dispatch(PhoneSession& session, const Command& command);

// For the socket layer: parses a text frame and dispatches it. A frame
// parse_command refuses is answered with an error_reply when it carried a
// usable id; otherwise there is nothing the phone could match and the result
// is empty.
[[nodiscard]] std::string handle_text(PhoneSession& session, std::string_view text);

}  // namespace revenant::webgate
