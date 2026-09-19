// The demodulator package's registration hook.
//
// core/engine/graph.cpp ships one stage of its own, the raw complex tap, and
// asks an installed factory for everything else. This is that factory: the
// per-receiver fine stage of core/shaders/vrx_fine.comp followed by the
// detector of core/shaders/vrx_demod.comp, both already proved bit-exact
// against their twins in tests/reference/test_vrx.cpp.
//
// A free function rather than a static initialiser. A static initialiser
// would install the factory in every binary that links revenant_core whether
// or not it wanted one, and the order of static initialisers across
// translation units is not specified, so a graph constructed from another
// initialiser could see the registry either full or empty depending on link
// order. Engine::create calls this, which is the one place that knows a graph
// is about to exist.

#pragma once

namespace revenant::engine {

// Installs the fine stage and the seven demodulators as the graph's stage
// factory. Idempotent: calling it again replaces the factory with an
// equivalent one and disturbs no running engine, because a graph copies the
// factory at construction.
//
// Demod::Raw is declined rather than handled. The raw tap needs no kernel at
// all, the graph's own copy is a buffer copy, and building a mixer and a
// filter to hand back the samples unchanged would be slower and less exact.
void install_default_vrx_stages();

}  // namespace revenant::engine
