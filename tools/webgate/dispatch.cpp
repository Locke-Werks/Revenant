#include "tools/webgate/dispatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.h"
#include "core/rpc/types.h"

namespace revenant::webgate {

namespace {

using Result = Expected<std::string>;

// ---- Modes -----------------------------------------------------------------

struct ModeName {
    std::string_view name;
    rpc::Demod demod;
};

constexpr ModeName kModes[] = {
    {"raw", rpc::Demod::Raw},     {"am", rpc::Demod::Am},       {"nfm", rpc::Demod::Nfm},
    {"wfm", rpc::Demod::Wfm},     {"usb", rpc::Demod::Usb},     {"lsb", rpc::Demod::Lsb},
    {"dsb", rpc::Demod::Dsb},     {"cw", rpc::Demod::Cw},       {"p25p1", rpc::Demod::P25p1},
    {"dstar", rpc::Demod::Dstar}, {"tetra", rpc::Demod::Tetra}, {"dmr", rpc::Demod::Dmr},
    {"sam", rpc::Demod::Sam},
};

[[nodiscard]] std::string_view mode_name(rpc::Demod demod)
{
    for (const auto& mode : kModes) {
        if (mode.demod == demod) {
            return mode.name;
        }
    }
    return "raw";
}

[[nodiscard]] Expected<rpc::Demod> parse_mode(const Command& command)
{
    auto text = command.get_string("mode");
    if (!text) {
        return std::unexpected(text.error());
    }
    for (const auto& mode : kModes) {
        if (mode.name == *text) {
            return mode.demod;
        }
    }
    return fail("mode: \"" + std::string(text->substr(0, 32)) +
                "\" is not a mode this gateway knows (raw am nfm wfm usb lsb dsb cw p25p1 dstar "
                "tetra dmr sam)");
}

[[nodiscard]] std::string_view choice_name(rpc::TranscribeChoice choice)
{
    switch (choice) {
    case rpc::TranscribeChoice::On:
        return "on";
    case rpc::TranscribeChoice::Off:
        return "off";
    case rpc::TranscribeChoice::Auto:
        break;
    }
    return "auto";
}

[[nodiscard]] std::string_view model_state_name(rpc::TranscriptionModelState state)
{
    switch (state) {
    case rpc::TranscriptionModelState::Downloading:
        return "downloading";
    case rpc::TranscriptionModelState::Verifying:
        return "verifying";
    case rpc::TranscriptionModelState::Loading:
        return "loading";
    case rpc::TranscriptionModelState::Ready:
        return "ready";
    case rpc::TranscriptionModelState::Failed:
        return "failed";
    case rpc::TranscriptionModelState::Absent:
        break;
    }
    return "absent";
}

// ---- JSON builders ---------------------------------------------------------

[[nodiscard]] std::string join(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        out += items[i];
    }
    out += ']';
    return out;
}

// Frequencies on the wire are absolute hertz; the engine's receivers sit at an
// offset from baseband DC, and EngineInfo::source_center is what turns one
// into the other. It moves on a retune, so it is read per command.
[[nodiscard]] Expected<std::int64_t> source_center(PhoneSession& session)
{
    auto info = session.client().info();
    if (!info) {
        return std::unexpected(info.error());
    }
    return info->source_center;
}

[[nodiscard]] std::string vrx_json(const rpc::VrxStatus& status, std::int64_t center)
{
    JsonObject out;
    out.add_u64("vrx", status.id)
        .add_i64("hz", center + status.params.center)
        .add_string("mode", mode_name(status.params.demod))
        .add_i64("bandwidthHz", status.params.bandwidth)
        .add_double("squelchDbfs", status.params.squelch_dbfs)
        .add_i64("grantedLowHz", status.placement.granted_low)
        .add_i64("grantedHighHz", status.placement.granted_high)
        .add_bool("clamped", status.placement.bandwidth_clamped)
        .add_string("transcribe", choice_name(status.transcribe))
        .add_bool("transcribing", status.transcribing);
    if (!status.placement.clamp_reason.empty()) {
        out.add_string("clampReason", status.placement.clamp_reason);
    }
    return out.str();
}

[[nodiscard]] std::string source_json(const rpc::SourceDescriptor& source)
{
    std::vector<std::string> notes;
    for (const auto& note : source.notes) {
        notes.push_back(json_string(note));
    }

    std::vector<std::string> ranges;
    for (const auto& range : source.tune_ranges) {
        JsonObject r;
        r.add_i64("lowHz", range.low_hz).add_i64("highHz", range.high_hz).add_i64("stepHz", range.step_hz);
        ranges.push_back(r.str());
    }

    std::vector<std::string> rates;
    for (const auto rate : source.sample_rates) {
        rates.push_back(std::to_string(rate));
    }

    std::vector<std::string> stages;
    for (const auto& stage : source.gain_stages) {
        std::vector<std::string> steps;
        for (const double step : stage.steps_db) {
            JsonObject one;
            one.add_double("v", step);
            // A bare number list: reuse the double formatter through a
            // one-field object and cut the value back out.
            const std::string text = one.str();
            steps.push_back(text.substr(5, text.size() - 6));
        }
        JsonObject s;
        s.add_string("name", stage.name)
            .add_double("minDb", stage.min_db)
            .add_double("maxDb", stage.max_db)
            .add_raw("stepsDb", join(steps))
            .add_bool("hasAuto", stage.has_auto);
        if (stage.in_force_db) {
            s.add_double("inForceDb", *stage.in_force_db);
        } else {
            s.add_null("inForceDb");
        }
        s.add_bool("inForceAuto", stage.in_force_auto);
        stages.push_back(s.str());
    }

    JsonObject out;
    out.add_string("uri", source.uri)
        .add_string("backend", source.backend)
        .add_string("name", source.display_name)
        .add_bool("available", source.available());
    if (!source.available()) {
        out.add_string("unavailable", source.unavailable);
    }
    out.add_raw("notes", join(notes))
        .add_raw("tuneRanges", join(ranges))
        .add_raw("sampleRates", join(rates))
        .add_u64("minRate", source.min_rate)
        .add_u64("maxRate", source.max_rate)
        .add_raw("gainStages", join(stages))
        .add_bool("seekable", source.seekable);
    return out.str();
}

[[nodiscard]] std::string transcription_json(const rpc::TranscriptionStatus& status)
{
    JsonObject out;
    out.add_bool("enabled", status.enabled)
        .add_string("modelState", model_state_name(status.model_state))
        .add_string("modelName", status.model_name)
        .add_u64("bytesDone", status.bytes_done)
        .add_u64("bytesTotal", status.bytes_total)
        .add_string("detail", status.detail)
        .add_string("backend", status.backend)
        .add_u64("receivers", status.receivers)
        .add_u64("queued", status.queued)
        .add_u64("transcribed", status.transcribed)
        .add_u64("dropped", status.dropped);
    return out.str();
}

// ---- Receiver access -------------------------------------------------------

// A phone may only touch receivers it created. The engine would let it: every
// session's receivers are visible and settable, and that is the Qt client's
// business, not a second phone's.
[[nodiscard]] Expected<std::uint64_t> owned_vrx(PhoneSession& session, const Command& command,
                                                std::string_view key = "vrx")
{
    const auto vrx = command.get_u64(key);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    if (!session.owns(*vrx)) {
        return fail(std::string(key) + ": receiver " + std::to_string(*vrx) +
                    " is not one of yours");
    }
    return *vrx;
}

// Reads the receiver, lets `change` edit one thing in its params, sends them
// back and answers with the receiver as the engine now holds it.
template <class Change>
[[nodiscard]] Result change_vrx(PhoneSession& session, const Command& command, Change&& change)
{
    const auto vrx = owned_vrx(session, command);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    const auto center = source_center(session);
    if (!center) {
        return std::unexpected(center.error());
    }
    auto before = session.client().vrx_status(*vrx);
    if (!before) {
        return std::unexpected(before.error());
    }
    rpc::VrxParams params = before->params;
    if (auto edited = change(params, *center); !edited) {
        return std::unexpected(edited.error());
    }
    if (auto set = session.client().set_vrx_params(*vrx, params); !set) {
        return std::unexpected(set.error());
    }
    session.remember(*vrx, params);
    auto after = session.client().vrx_status(*vrx);
    if (!after) {
        return std::unexpected(after.error());
    }
    return vrx_json(*after, *center);
}

// ---- Operations ------------------------------------------------------------

[[nodiscard]] Result op_add_vrx(PhoneSession& session, const Command& command)
{
    const auto hz = command.get_i64("hz");
    if (!hz) {
        return std::unexpected(hz.error());
    }
    rpc::VrxParams params;
    if (command.has("mode")) {
        const auto mode = parse_mode(command);
        if (!mode) {
            return std::unexpected(mode.error());
        }
        params.demod = *mode;
    }
    if (command.has("bandwidthHz")) {
        const auto width = command.get_i64("bandwidthHz");
        if (!width) {
            return std::unexpected(width.error());
        }
        params.bandwidth = *width;
    }
    if (command.has("squelchDbfs")) {
        const auto squelch = command.get_double("squelchDbfs");
        if (!squelch) {
            return std::unexpected(squelch.error());
        }
        params.squelch_dbfs = *squelch;
    }
    const auto center = source_center(session);
    if (!center) {
        return std::unexpected(center.error());
    }
    params.center = *hz - *center;

    const auto id = session.client().add_vrx(params);
    if (!id) {
        return std::unexpected(id.error());
    }
    session.adopt(*id, params);
    const auto status = session.client().vrx_status(*id);
    if (!status) {
        return std::unexpected(status.error());
    }
    return vrx_json(*status, *center);
}

[[nodiscard]] Result op_remove_vrx(PhoneSession& session, const Command& command)
{
    const auto vrx = owned_vrx(session, command);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    if (auto removed = session.client().remove_vrx(*vrx); !removed) {
        return std::unexpected(removed.error());
    }
    session.forget(*vrx);
    return JsonObject().add_u64("vrx", *vrx).str();
}

[[nodiscard]] Result op_tune(PhoneSession& session, const Command& command)
{
    const auto hz = command.get_i64("hz");
    if (!hz) {
        return std::unexpected(hz.error());
    }
    return change_vrx(session, command, [&](rpc::VrxParams& params, std::int64_t center) -> Status {
        params.center = *hz - center;
        return {};
    });
}

// The engine will not change a receiver's demodulator or filter width in
// place (the demodulator is the stage, and a new width is a new tap table), so
// either is an add and a remove. The new receiver goes in first: if the engine
// refuses it the old one is still there and the phone keeps listening. The id
// changes, which the reply says with "replaced", and an audio selection
// follows it.
template <class Edit>
[[nodiscard]] Result recreate_vrx(PhoneSession& session, const Command& command, Edit&& edit)
{
    const auto old_id = owned_vrx(session, command);
    if (!old_id) {
        return std::unexpected(old_id.error());
    }
    const auto center = source_center(session);
    if (!center) {
        return std::unexpected(center.error());
    }
    auto before = session.client().vrx_status(*old_id);
    if (!before) {
        return std::unexpected(before.error());
    }
    rpc::VrxParams params = before->params;
    edit(params);
    if (params.demod == before->params.demod && params.bandwidth == before->params.bandwidth &&
        params.passband_low == before->params.passband_low &&
        params.passband_high == before->params.passband_high) {
        return vrx_json(*before, *center);
    }

    const auto new_id = session.client().add_vrx(params);
    if (!new_id) {
        return std::unexpected(new_id.error());
    }
    session.adopt(*new_id, params);
    (void)session.client().remove_vrx(*old_id);
    const bool had_audio = session.audio_vrx() == *old_id;
    session.forget(*old_id);
    if (had_audio) {
        session.set_audio_vrx(*new_id);
    }
    auto after = session.client().vrx_status(*new_id);
    if (!after) {
        return std::unexpected(after.error());
    }
    // vrx_json's object, plus the id this receiver replaced.
    std::string body = vrx_json(*after, *center);
    body.pop_back();
    body += ",\"replaced\":" + std::to_string(*old_id) + "}";
    return body;
}

[[nodiscard]] Result op_set_mode(PhoneSession& session, const Command& command)
{
    const auto mode = parse_mode(command);
    if (!mode) {
        return std::unexpected(mode.error());
    }
    return recreate_vrx(session, command, [&](rpc::VrxParams& params) {
        params.demod = *mode;
        // The old passband belonged to the old mode and would be a wrong guess
        // for the new one; the bandwidth shorthand expands through the new rule.
        params.passband_low = 0;
        params.passband_high = 0;
    });
}

[[nodiscard]] Result op_set_bandwidth(PhoneSession& session, const Command& command)
{
    const auto hz = command.get_i64("hz");
    if (!hz) {
        return std::unexpected(hz.error());
    }
    return recreate_vrx(session, command, [&](rpc::VrxParams& params) {
        // The shorthand only applies while the explicit passband is unstated,
        // so clear it or a USB receiver would ignore the new width.
        params.bandwidth = *hz;
        params.passband_low = 0;
        params.passband_high = 0;
    });
}

[[nodiscard]] Result op_set_squelch(PhoneSession& session, const Command& command)
{
    const auto dbfs = command.get_double("dbfs");
    if (!dbfs) {
        return std::unexpected(dbfs.error());
    }
    return change_vrx(session, command, [&](rpc::VrxParams& params, std::int64_t) -> Status {
        params.squelch_dbfs = *dbfs;
        return {};
    });
}

[[nodiscard]] Result op_set_center(PhoneSession& session, const Command& command)
{
    const auto hz = command.get_i64("hz");
    if (!hz) {
        return std::unexpected(hz.error());
    }
    const auto granted = session.client().set_source_center(*hz);
    if (!granted) {
        return std::unexpected(granted.error());
    }
    // A retune removes receivers whose centre falls outside the new span, so
    // the ones this phone thought it had may be gone.
    std::vector<std::string> removed;
    if (const auto ids = session.client().vrx_ids()) {
        for (const auto owned : session.owned_ids()) {
            if (std::ranges::find(*ids, owned) == ids->end()) {
                session.forget(owned);
                removed.push_back(std::to_string(owned));
            }
        }
    }
    return JsonObject().add_i64("grantedHz", *granted).add_raw("removed", join(removed)).str();
}

[[nodiscard]] Result op_set_gain(PhoneSession& session, const Command& command)
{
    const auto stage = command.get_string("stage");
    if (!stage) {
        return std::unexpected(stage.error());
    }
    const auto db = command.get_double("db");
    if (!db) {
        return std::unexpected(db.error());
    }
    const auto granted = session.client().set_source_gain(*stage, *db);
    if (!granted) {
        return std::unexpected(granted.error());
    }
    return JsonObject().add_string("stage", *stage).add_double("grantedDb", *granted).str();
}

[[nodiscard]] Result op_set_gain_auto(PhoneSession& session, const Command& command)
{
    const auto stage = command.get_string("stage");
    if (!stage) {
        return std::unexpected(stage.error());
    }
    const auto on = command.get_bool("on");
    if (!on) {
        return std::unexpected(on.error());
    }
    if (auto set = session.client().set_source_gain_auto(*stage, *on); !set) {
        return std::unexpected(set.error());
    }
    return JsonObject().add_string("stage", *stage).add_bool("on", *on).str();
}

[[nodiscard]] Result op_list_sources(PhoneSession& session, const Command&)
{
    const auto sources = session.client().list_sources();
    if (!sources) {
        return std::unexpected(sources.error());
    }
    std::vector<std::string> items;
    for (const auto& source : *sources) {
        items.push_back(source_json(source));
    }
    return JsonObject().add_raw("sources", join(items)).str();
}

[[nodiscard]] Result op_open_source(PhoneSession& session, const Command& command)
{
    const auto uri = command.get_string("uri");
    if (!uri) {
        return std::unexpected(uri.error());
    }
    if (auto opened = session.client().open_source(*uri); !opened) {
        return std::unexpected(opened.error());
    }
    const auto descriptor = session.client().source_descriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    JsonObject out;
    if (descriptor->has_value()) {
        out.add_raw("source", source_json(**descriptor));
    } else {
        out.add_null("source");
    }
    return out.str();
}

// setView, audio and keepListening only record what the phone asked for. The
// spectrum and audio streams that act on it are wired in Task 10; recording
// here keeps their argument checking in one place with the other ops.
[[nodiscard]] Result op_set_view(PhoneSession& session, const Command& command)
{
    const auto centre = command.get_double("centerHz");
    if (!centre) {
        return std::unexpected(centre.error());
    }
    const auto span = command.get_double("spanHz");
    if (!span) {
        return std::unexpected(span.error());
    }
    const auto width = command.get_u64("widthPx");
    if (!width) {
        return std::unexpected(width.error());
    }
    if (!(*span > 0.0)) {
        return fail("spanHz: must be greater than 0");
    }
    if (*width == 0) {
        return fail("widthPx: must be at least 1");
    }
    // The reducer caps the row at kMaxWidthPx; record the capped value so the
    // reply says what the phone will actually receive.
    const auto capped = static_cast<std::uint32_t>(std::min<std::uint64_t>(*width, kMaxWidthPx));
    session.set_view(View{*centre, *span, capped});
    return JsonObject()
        .add_double("centerHz", *centre)
        .add_double("spanHz", *span)
        .add_u64("widthPx", capped)
        .str();
}

[[nodiscard]] Result op_audio(PhoneSession& session, const Command& command)
{
    bool on = true;
    if (command.has("on")) {
        const auto value = command.get_bool("on");
        if (!value) {
            return std::unexpected(value.error());
        }
        on = *value;
    }
    const auto vrx = owned_vrx(session, command);
    if (!vrx) {
        // Turning audio off for a receiver that is already gone is not worth
        // refusing.
        if (!on) {
            if (const auto id = command.get_u64("vrx"); id && session.audio_vrx() == *id) {
                session.set_audio_vrx(std::nullopt);
                return JsonObject().add_u64("vrx", *id).add_bool("on", false).str();
            }
        }
        return std::unexpected(vrx.error());
    }
    if (on) {
        session.set_audio_vrx(*vrx);
    } else if (session.audio_vrx() == *vrx) {
        session.set_audio_vrx(std::nullopt);
    }
    return JsonObject().add_u64("vrx", *vrx).add_bool("on", on).str();
}

[[nodiscard]] Result op_keep_listening(PhoneSession& session, const Command& command)
{
    const auto on = command.get_bool("on");
    if (!on) {
        return std::unexpected(on.error());
    }
    session.set_keep_listening(*on);
    return JsonObject().add_bool("on", *on).str();
}

[[nodiscard]] Result op_set_transcription(PhoneSession& session, const Command& command)
{
    const auto on = command.get_bool("on");
    if (!on) {
        return std::unexpected(on.error());
    }
    const auto status = session.client().set_transcription(*on);
    if (!status) {
        return std::unexpected(status.error());
    }
    return transcription_json(*status);
}

[[nodiscard]] Result op_set_vrx_transcribe(PhoneSession& session, const Command& command)
{
    const auto vrx = owned_vrx(session, command);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    const auto text = command.get_string("choice");
    if (!text) {
        return std::unexpected(text.error());
    }
    rpc::TranscribeChoice choice{};
    if (*text == "auto") {
        choice = rpc::TranscribeChoice::Auto;
    } else if (*text == "on") {
        choice = rpc::TranscribeChoice::On;
    } else if (*text == "off") {
        choice = rpc::TranscribeChoice::Off;
    } else {
        return fail("choice: must be \"auto\", \"on\" or \"off\"");
    }
    if (auto set = session.client().set_vrx_transcribe(*vrx, choice); !set) {
        return std::unexpected(set.error());
    }
    const auto center = source_center(session);
    if (!center) {
        return std::unexpected(center.error());
    }
    const auto status = session.client().vrx_status(*vrx);
    if (!status) {
        return std::unexpected(status.error());
    }
    return vrx_json(*status, *center);
}

[[nodiscard]] Result run(PhoneSession& session, const Command& command)
{
    const std::string_view op = command.op;
    if (op == "addVrx") return op_add_vrx(session, command);
    if (op == "removeVrx") return op_remove_vrx(session, command);
    if (op == "tune") return op_tune(session, command);
    if (op == "setMode") return op_set_mode(session, command);
    if (op == "setBandwidth") return op_set_bandwidth(session, command);
    if (op == "setSquelch") return op_set_squelch(session, command);
    if (op == "setCenter") return op_set_center(session, command);
    if (op == "setGain") return op_set_gain(session, command);
    if (op == "setGainAuto") return op_set_gain_auto(session, command);
    if (op == "listSources") return op_list_sources(session, command);
    if (op == "openSource") return op_open_source(session, command);
    if (op == "setView") return op_set_view(session, command);
    if (op == "audio") return op_audio(session, command);
    if (op == "keepListening") return op_keep_listening(session, command);
    if (op == "setTranscription") return op_set_transcription(session, command);
    if (op == "setVrxTranscribe") return op_set_vrx_transcribe(session, command);
    return fail("unknown op: " + std::string(op.substr(0, 64)));
}

}  // namespace

std::string dispatch(PhoneSession& session, const Command& command)
{
    const auto result = run(session, command);
    if (!result) {
        return error_reply(command.id, result.error().message);
    }
    return ok_reply(command.id, *result);
}

std::string handle_text(PhoneSession& session, std::string_view text)
{
    auto command = parse_command(text);
    if (!command) {
        const auto id = peek_id(text);
        if (!id) {
            return {};
        }
        return error_reply(*id, command.error().message);
    }
    return dispatch(session, *command);
}

}  // namespace revenant::webgate
