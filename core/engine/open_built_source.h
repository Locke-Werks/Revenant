// Opening an engine on a source object rather than a URI.
//
// WHY THIS EXISTS. Engine::set_source_center moves every receiver, cancels
// every probe and can remove receivers, and none of that could be run without a
// dongle: a file refuses a retune because its centre is a property of the bytes
// on disk, and the synthetic scene refuses because its centre is a label on a
// baseband generated around DC. Both refusals are right for those backends and
// neither should be weakened to make a test possible. A test instead builds its
// own Source, typically a synthetic scene wrapped so that tune() moves the
// label, and hands the object here.
//
// Everything after the source exists is Engine::open_source's own code: the
// grid, the ring, the graph and the probe pool are sized exactly as they are
// for a URI.

#pragma once

#include <memory>

#include "core/engine/engine.h"
#include "core/error.h"
#include "core/source/source.h"

namespace revenant::engine {

// `engine` must have come from Engine::create and have no source open. The
// source's capabilities().uri stands in for the URI in any message that would
// quote one.
[[nodiscard]] Status open_built_source(Engine& engine, std::unique_ptr<source::Source> source);

}  // namespace revenant::engine
