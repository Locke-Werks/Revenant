// The wire surfaces this client uses that the engine may not have
// yet, reached through one file so the merge is one file.
//
// WHY THIS EXISTS, AND WHY IT IS NOT A HACK
//
// Session.setSourceCenter, Session.sourceCanRetune and the two EngineInfo
// pacing fields were specified and landed by a separate branch in the same
// window as the client work that uses them. core/rpc is read-only from
// here: this client compiles core/rpc/client.cpp from the main tree by
// path, and a call to a method that tree does not have yet is a link error
// and not a feature flag.
//
// So every use goes through a template whose body is discarded by
// `if constexpr` when the method is absent. A discarded statement inside a
// template is not instantiated, which is exactly the property needed: the
// client builds today against an engine with no retune call, and the
// surfaces light up on the merge with no edit to their call sites.
//
// WHAT A READER HAS TO CHECK ON THAT MERGE, AND IT IS ONLY THIS FILE
//
// The names and the shapes below are what the seam specified. If the
// branch that landed them chose a different spelling, the `requires`
// clauses go on reporting the surface as absent, the window greys the
// control out and says the engine does not carry it, and nothing breaks
// loudly. That failure mode is quiet, which this round is otherwise about
// refusing, so it is stated here and reported by the client: EngineLink
// publishes seamRetuneCompiled, and the window says "this build of the
// client was compiled against a wire with no retune call" rather than
// blaming the engine.
//
// Fix it by correcting the expressions in this file. Nothing else in
// ui/ names the methods.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "core/error.h"
#include "core/rpc/client.h"
#include "core/rpc/types.h"

namespace revenant::ui {

// Whether the wire this client was compiled against carries the retune
// pair. Published, because "greyed out" has two causes and they need
// different actions: a source that cannot retune, which is every file and
// synthetic source, and a client built before the call existed.
// THROUGH A VARIABLE TEMPLATE AND NOT WRITTEN DIRECTLY, WHICH IS NOT A
// STYLE CHOICE. MSVC 19.44 hard-errors on a requires-expression that names
// a missing member of a COMPLETE, NON-DEPENDENT type: it reports C2039
// rather than evaluating the expression to false, which is the whole
// behaviour being relied on here. Substituting the type through a template
// parameter makes the expression dependent at parse time and the
// substitution failure becomes the false it is supposed to be. Measured on
// this toolchain, not assumed; writing it the obvious way fails to compile
// and the error names the missing method rather than the technique.
template <typename C>
inline constexpr bool client_has_retune_v =
    requires(C& client) { client.set_source_center(std::int64_t{0}); };

template <typename C>
inline constexpr bool client_has_can_retune_v =
    requires(C& client) { client.source_can_retune(); };

template <typename I>
inline constexpr bool info_has_pacing_v = requires(const I& info) {
    info.realtime_factor;
    info.source_paced_by;
};

inline constexpr bool kSeamHasRetune = client_has_retune_v<rpc::Client>;
inline constexpr bool kSeamHasCanRetune = client_has_can_retune_v<rpc::Client>;

// Whether EngineInfo carries the pacing pair.
inline constexpr bool kSeamHasPacing = info_has_pacing_v<rpc::EngineInfo>;

// What sourceCanRetune answers, in this client's own terms.
//
// A STRUCT AND NOT THREE OUT PARAMETERS, because the three are one answer
// and a caller that read can_retune without the range would offer a
// control with no stops on it.
struct SourceTuning {
    bool can_retune = false;
    std::int64_t low_hz = 0;
    std::int64_t high_hz = 0;

    // Why not, when the engine said. Empty when it can retune and when the
    // client was built without the call.
    std::string refusal;
};

// Retune the front end. Answers the centre the source actually took, which
// a device with a tuning step will round.
//
// A template so the body is discarded rather than compiled when the method
// is absent; C is deduced and is always rpc::Client.
template <typename C = rpc::Client>
[[nodiscard]] Expected<std::int64_t> seam_set_source_center(C& client, std::int64_t hz)
{
    if constexpr (requires { client.set_source_center(hz); }) {
        return client.set_source_center(hz);
    } else {
        static_cast<void>(client);
        static_cast<void>(hz);
        return fail("this client was built against an engine wire with no "
                    "setSourceCenter call, so it cannot retune the front end");
    }
}

// The same retune, answering with the receivers it removed as well as the
// centre it took. Session.setSourceCenter grew the removed list after the
// call above had shipped, so a wire without retune_source falls back to it
// and reports nothing removed; the client then finds a removal the old way,
// by the inventory, and says less about why.
template <typename C = rpc::Client>
[[nodiscard]] Expected<rpc::SourceRetune> seam_retune_source(C& client, std::int64_t hz)
{
    if constexpr (requires { client.retune_source(hz); }) {
        return client.retune_source(hz);
    } else {
        auto granted = seam_set_source_center(client, hz);
        if (!granted) {
            return std::unexpected(granted.error());
        }
        rpc::SourceRetune out;
        out.granted_hz = *granted;
        return out;
    }
}

// Whether the surface above will work, and the tuning range.
//
// A refusal to ANSWER is treated as "cannot retune" rather than as an
// error worth a fault line, because the question is asked once per
// connection and the only thing a caller does with it is decide whether to
// grey a control out. The engine's sentence is carried through so the
// greyed control can say why.
template <typename C = rpc::Client>
[[nodiscard]] SourceTuning seam_source_can_retune(C& client)
{
    SourceTuning out;
    if constexpr (requires { client.source_can_retune(); }) {
        auto answer = client.source_can_retune();
        if (!answer.has_value()) {
            out.refusal = answer.error().message;
            return out;
        }
        out.can_retune = answer->can_retune;
        out.low_hz = answer->low_hz;
        out.high_hz = answer->high_hz;
        if (!out.can_retune && out.refusal.empty()) {
            out.refusal = "this source cannot retune.";
        }
        return out;
    } else {
        static_cast<void>(client);
        out.refusal = "this client was built against an engine wire with no "
                      "sourceCanRetune call.";
        return out;
    }
}

// Measured samples of capture per wall second over the source rate. 1.0 is
// realtime. Nothing when the wire does not carry it, which is what stops a
// missing field reading as a source stopped dead.
template <typename I = rpc::EngineInfo>
[[nodiscard]] std::optional<double> seam_realtime_factor(const I& info)
{
    if constexpr (requires { info.realtime_factor; }) {
        return static_cast<double>(info.realtime_factor);
    } else {
        static_cast<void>(info);
        return std::nullopt;
    }
}

// The --pace setting, 0 for unthrottled.
template <typename I = rpc::EngineInfo>
[[nodiscard]] double seam_source_paced_by(const I& info)
{
    if constexpr (requires { info.source_paced_by; }) {
        return static_cast<double>(info.source_paced_by);
    } else {
        static_cast<void>(info);
        return 0.0;
    }
}

}  // namespace revenant::ui
