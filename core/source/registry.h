// Finding and opening sources.
//
// Mirrors gpu::enumerate_devices deliberately: enumeration opens nothing, so
// it still answers on a machine where opening would fail, which is exactly
// when somebody is trying to find out why.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/dsp/types.h"
#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

// Parses 7100000, 7.1M, 162.550M, 14074k, 500 into exact integer hertz.
//
// The fraction is folded in with integer arithmetic rather than by parsing a
// double and multiplying. 162.550 * 1e6 in binary floating point is
// 162549999.99999997, and while a round would recover the right answer here,
// the conventions put frequency in integer hertz precisely so that no stage
// has to be trusted to round the same way twice.
//
// It lives here because this file owns the text layer of a source and the
// same spelling has to work in a URI and on a command line. It began in
// tools/cli and moved when the RTL-SDR backend needed freq= to accept what
// --vrx already accepted: one grammar for a frequency, in one place, or the
// two drift and a number means different things depending on where it was
// typed.
//
// `what` names the thing being parsed and is quoted back in the error, so a
// caller says "--vrx frequency" or "the rtlsdr freq= parameter" and the
// message reads as a sentence.
[[nodiscard]] Expected<dsp::Hertz> parse_frequency(std::string_view text, std::string_view what);

struct SourceDescriptor {
    // The URI open_source takes. See below for the grammar.
    std::string uri;
    std::string display_name;
    std::string backend;
};

// Everything attached or constructible, without opening any of it.
[[nodiscard]] Expected<std::vector<SourceDescriptor>> enumerate_sources();

// The same list with each source's full capability description. Costs more,
// because some backends must briefly open a device to answer.
[[nodiscard]] Expected<std::vector<SourceCapabilities>> describe_sources();

// Opens a source from a URI.
//
// A URI rather than a struct because it is one text field over RPC, one
// argument on a command line, and one line in a session file, and because a
// session that cannot be reproduced from what was written down is not
// reproducible. The grammar:
//
//   file:///C:/captures/hf.cf32?rate=2400000&format=cf32&center=7100000
//   synthetic:wideband?rate=20000000&emitters=64&seed=20260918
//   rtlsdr://00000001
//
// Query parameters are backend-specific and an unknown one is an error rather
// than being ignored, because a typo in a rate silently producing the default
// is a capture whose metadata is wrong.
//
// The file backend reads containers as well as raw bytes, so rate=, format=
// and center= are no longer required when the recording carries them:
//
//   file:///C:/captures/20m.sigmf-data
//   file:///C:/captures/20m.sigmf-data?segment=2
//   file:///D:/hf/HDSDR_20260921_140307Z_7100kHz_RF.wav
//
// container= forces raw, sigmf or wav instead of sniffing. meta= names a
// SigMF sidecar that is not beside its dataset. segment= picks one capture
// segment out of a recording that retunes, which is refused without it
// rather than played at one frequency. A value stated in both the URI and
// the container has to agree, and the backend says which two numbers
// disagreed rather than preferring one.
//
// pace= sets how fast a file plays, as a multiple of realtime: 1 to listen,
// 4 for four times, 0 or max for as fast as the engine retires it. A file
// with it runs at that whatever the host asked for; one without takes the
// host's pace, which for revenant-engine is --pace. See Source::own_pace.
//
//   file:///D:/hf/HDSDR_20260921_140307Z_7100kHz_RF.wav?pace=1
[[nodiscard]] Expected<std::unique_ptr<Source>> open_source(std::string_view uri);

// `uri` with pace=`pace` added when it opens a file and states no pace, and
// unchanged otherwise, including when it cannot be parsed: the open reports
// that.
//
// Session.openSource puts every URI through this with "1", so a recording a
// client opens plays at realtime unless it says otherwise. A person listening
// is the reason to open one from a window, and the engine a window talks to
// was most likely started for a dongle at --pace 0. The engine's own
// command-line source is not put through it: --pace is how that one is paced.
[[nodiscard]] std::string with_default_file_pace(std::string_view uri, std::string_view pace);

}  // namespace revenant::source
