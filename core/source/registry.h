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

#include "core/error.h"
#include "core/source/capabilities.h"
#include "core/source/source.h"

namespace revenant::source {

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
[[nodiscard]] Expected<std::unique_ptr<Source>> open_source(std::string_view uri);

}  // namespace revenant::source
