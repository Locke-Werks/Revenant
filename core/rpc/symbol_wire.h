// The constellation and eye panel's reader (#17): a schema SymbolScope into
// the client's rpc::SymbolScope.
//
// Header only, and depending on nothing but the schema and core/rpc/types.h,
// because the UI compiles the client's sources in its own project and a new
// translation unit would be a second list to keep in step. The writer is the
// server's, in core/rpc/convert.cpp, since it reads the engine's struct.

#pragma once

#include "core/rpc/revenant.capnp.h"
#include "core/rpc/types.h"

namespace revenant::rpc {

[[nodiscard]] inline SymbolKind read_symbol_kind(schema::SymbolKind in) {
    switch (in) {
        case schema::SymbolKind::LEVELS: return SymbolKind::Levels;
        case schema::SymbolKind::CONSTELLATION: return SymbolKind::Constellation;
        case schema::SymbolKind::NONE: break;
    }
    return SymbolKind::None;
}

// Into `out` rather than returned, so a recycled frame keeps its capacity.
inline void read_symbol_scope(SymbolScope& out, schema::SymbolScope::Reader in) {
    out.kind = read_symbol_kind(in.getKind());
    out.symbol_rate_hz = in.getSymbolRateHz();
    const auto copy = [](std::vector<float>& to, capnp::List<float>::Reader from) {
        to.resize(from.size());
        for (unsigned i = 0; i < from.size(); ++i) {
            to[i] = from[i];
        }
    };
    copy(out.points, in.getPoints());
    copy(out.ideal, in.getIdeal());
    copy(out.eye, in.getEye());
    out.eye_trace_length = in.getEyeTraceLength();
    out.error_rms = in.getErrorRms();
    out.timing_offset = in.getTimingOffset();
    out.rate_error_ppm = in.getRateErrorPpm();
}

}  // namespace revenant::rpc
