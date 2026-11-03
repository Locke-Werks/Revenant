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
#include "tools/webgate/dv_feed.h"

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

[[nodiscard]] std::string string_array(const std::vector<std::string>& items)
{
    std::vector<std::string> quoted;
    quoted.reserve(items.size());
    for (const auto& item : items) {
        quoted.push_back(json_string(item));
    }
    return join(quoted);
}

[[nodiscard]] std::string vrx_json(const rpc::VrxStatus& status, std::int64_t center,
                                   const std::vector<std::string>& decoders)
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
        .add_bool("transcribing", status.transcribing)
        .add_raw("decoders", string_array(decoders));
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
            steps.push_back(json_number(step));
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
        .add_string("serial", source.serial)
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

[[nodiscard]] JsonObject transcription_fields(const rpc::TranscriptionStatus& status)
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
    return out;
}

[[nodiscard]] std::string transcription_json(const rpc::TranscriptionStatus& status)
{
    return transcription_fields(status).str();
}

// The receiver as the engine reports it, or, when that read fails after the
// engine has already changed, what was sent, marked so the phone knows the
// granted figures are missing.
[[nodiscard]] std::string status_or_sent(PhoneSession& session, std::uint64_t id,
                                         const rpc::VrxParams& sent,
                                         rpc::TranscribeChoice transcribe, std::int64_t center)
{
    if (const auto status = session.receiver_status(id)) {
        return vrx_json(*status, center, session.decoders(id));
    }
    rpc::VrxStatus guess;
    guess.id = id;
    guess.params = sent;
    guess.transcribe = transcribe;
    std::string body = vrx_json(guess, center, session.decoders(id));
    body.pop_back();
    body += ",\"statusUnavailable\":true}";
    return body;
}

// ---- Receiver access -------------------------------------------------------

// Any receiver the engine has: one user holds the engine, so whoever opened a
// receiver it is that user's. An id the engine does not know is the engine's
// to refuse, in its own words. The session learns of a receiver from the
// engine's events, which may trail a command naming one just opened
// elsewhere, so the id is learned here from the engine's own status.
[[nodiscard]] Expected<std::uint64_t> receiver_arg(PhoneSession& session, const Command& command,
                                                   std::string_view key = "vrx")
{
    const auto vrx = command.get_u64(key);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    if (!session.has_receiver(*vrx)) {
        const auto status = session.client().vrx_status(*vrx);
        if (!status) {
            return std::unexpected(status.error());
        }
        session.learn(*vrx, status->params, status->owned_by_caller);
    }
    return *vrx;
}

// Reads the receiver, lets `change` edit one thing in its params, sends them
// back and answers with the receiver as the engine now holds it.
template <class Change>
[[nodiscard]] Result change_vrx(PhoneSession& session, const Command& command, Change&& change)
{
    const auto vrx = receiver_arg(session, command);
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
    return vrx_json(*after, *center, session.decoders(*vrx));
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
    session.learn(*id, params);
    return status_or_sent(session, *id, params, rpc::TranscribeChoice::Auto, *center);
}

[[nodiscard]] Result op_remove_vrx(PhoneSession& session, const Command& command)
{
    const auto vrx = receiver_arg(session, command);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    // While Auto DV is on, a receiver the phone takes away means "not that
    // frequency", as the desktop's dismissReceiver does, or the next pass
    // would open it again.
    std::optional<double> dismissed;
    if (session.auto_dv()) {
        if (const auto* params = session.last_params(*vrx)) {
            if (const auto center = source_center(session)) {
                dismissed = static_cast<double>(*center + params->center);
            }
        }
    }
    if (auto removed = session.client().remove_vrx(*vrx); !removed) {
        return std::unexpected(removed.error());
    }
    if (dismissed) {
        session.auto_dv_dismissed().push_back(*dismissed);
    }
    session.auto_dv_spawned().erase(*vrx);
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
    const auto old_id = receiver_arg(session, command);
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
        return vrx_json(*before, *center, session.decoders(*old_id));
    }

    const auto new_id = session.client().add_vrx(params);
    if (!new_id) {
        return std::unexpected(new_id.error());
    }
    session.learn(*new_id, params);

    // From here the engine holds two receivers. Anything that fails before the
    // old one is gone undoes the new one, so the phone still has exactly what
    // it had and the error is the engine's.
    const auto roll_back = [&](const Error& why) -> Result {
        // If even that fails the new id stays listed: it is a real receiver
        // the phone can still see and remove.
        if (session.client().remove_vrx(*new_id)) {
            session.forget(*new_id);
        }
        return std::unexpected(why);
    };

    if (before->transcribe != rpc::TranscribeChoice::Auto) {
        if (auto set = session.client().set_vrx_transcribe(*new_id, before->transcribe); !set) {
            return roll_back(set.error());
        }
    }
    if (auto removed = session.remove_receiver(*old_id); !removed) {
        return roll_back(removed.error());
    }
    const bool had_audio = session.audio_vrx() == *old_id;
    // A decoder reads one mode's output, so a choice only survives a new
    // width; a new mode starts from that mode's default.
    std::optional<std::vector<std::string>> kept_decoders;
    if (params.demod == before->params.demod) {
        if (const auto* chosen = session.decoder_choice(*old_id)) {
            kept_decoders = *chosen;
        }
    }
    session.forget(*old_id);
    if (kept_decoders) {
        session.set_decoders(*new_id, std::move(*kept_decoders));
    }
    if (had_audio) {
        session.set_audio_vrx(*new_id);
    }
    session.set_transcribe_choice(*new_id, before->transcribe);
    // The change is done, so a failed read must not turn it into an error that
    // hides the new id: answer from what was sent.
    std::string body = status_or_sent(session, *new_id, params, before->transcribe, *center);
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
    // the ones this phone knew of may be gone.
    std::vector<std::string> removed;
    if (const auto ids = session.client().vrx_ids()) {
        for (const auto known : session.receiver_ids()) {
            if (std::ranges::find(*ids, known) == ids->end()) {
                session.forget(known);
                removed.push_back(std::to_string(known));
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
    // The engine refuses an open over an open source so that a failed open
    // cannot destroy a working one. A phone switching radios has asked for
    // exactly that trade, and has no other way to close, so close first. A
    // close with nothing open succeeds.
    if (auto closed = session.client().close_source(); !closed) {
        return std::unexpected(closed.error());
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
    const auto vrx = receiver_arg(session, command);
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

[[nodiscard]] Result op_set_detection(PhoneSession& session, const Command& command)
{
    const auto bar = command.get_double("minConfidence");
    if (!bar) {
        return std::unexpected(bar.error());
    }
    // The negated form also refuses NaN. The engine rejects 1 and above, and
    // a refusal here says why instead of surfacing from the next poll.
    if (!(*bar >= 0.0 && *bar < 1.0)) {
        return fail("minConfidence: must be at least 0 and less than 1");
    }
    session.set_min_confidence(*bar);
    return JsonObject().add_double("minConfidence", *bar).str();
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
    const auto vrx = receiver_arg(session, command);
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
    session.set_transcribe_choice(*vrx, choice);
    const auto center = source_center(session);
    if (!center) {
        return std::unexpected(center.error());
    }
    const auto status = session.client().vrx_status(*vrx);
    if (!status) {
        return std::unexpected(status.error());
    }
    return vrx_json(*status, *center, session.decoders(*vrx));
}

[[nodiscard]] Result op_list_decoders(PhoneSession& session, const Command&)
{
    const auto decoders = session.client().decoders();
    if (!decoders) {
        return std::unexpected(decoders.error());
    }
    std::vector<std::string> items;
    items.reserve(decoders->size());
    for (const auto& d : *decoders) {
        items.push_back(JsonObject()
                            .add_string("name", d.name)
                            .add_string("description", d.description)
                            .add_string("input", d.input == rpc::DecoderInput::ComplexBaseband
                                                     ? "complexBaseband"
                                                     : "realAudio")
                            .add_raw("modes", string_array(d.modes))
                            .str());
    }
    JsonObject out;
    out.add_raw("decoders", join(items));
    // Which digital voice modes will actually play. D-STAR and DMR need a
    // plugin, and with none the engine plays silence without saying why, so
    // the phone has to be told here. Null is unknown, which is not "none".
    const auto plugins = session.client().vocoder_plugins();
    const auto voice = plugins ? voice_modes(*plugins) : std::nullopt;
    if (voice) {
        out.add_raw("voiceModes", string_array(*voice));
    } else {
        out.add_null("voiceModes");
    }
    return out.str();
}

[[nodiscard]] Result op_set_decoders(PhoneSession& session, const Command& command)
{
    const auto vrx = receiver_arg(session, command);
    if (!vrx) {
        return std::unexpected(vrx.error());
    }
    auto names = command.get_string_list("names");
    if (!names) {
        return std::unexpected(names.error());
    }
    std::vector<std::string> unique;
    for (auto& name : *names) {
        if (name.empty()) {
            return fail("names: a decoder name cannot be empty");
        }
        if (std::ranges::find(unique, name) == unique.end()) {
            unique.push_back(std::move(name));
        }
    }
    // receiver_arg learned the id, so this is null only if that went wrong.
    const auto* params = session.last_params(*vrx);
    if (params == nullptr) {
        return fail("vrx: receiver " + std::to_string(*vrx) + " is not known to the gateway");
    }
    const std::string mode(mode_name(params->demod));
    if (!unique.empty()) {
        const auto known = session.client().decoders();
        if (!known) {
            return std::unexpected(known.error());
        }
        // Checked here so the phone hears the refusal as the reply; the engine
        // would refuse the same at subscribe time, where only an event can say it.
        for (const auto& name : unique) {
            const auto it = std::ranges::find(*known, name, &rpc::DecoderInfo::name);
            if (it == known->end()) {
                return fail("names: the engine has no decoder called \"" + name + "\"");
            }
            if (!it->modes.empty() && std::ranges::find(it->modes, mode) == it->modes.end()) {
                std::string reads;
                for (const auto& m : it->modes) {
                    reads += (reads.empty() ? "" : ", ") + m;
                }
                return fail("names: " + name + " does not read a " + mode + " receiver (it reads " +
                            reads + ")");
            }
        }
    }
    session.set_decoders(*vrx, unique);
    return JsonObject().add_u64("vrx", *vrx).add_raw("decoders", string_array(unique)).str();
}

// ---- The DV tab --------------------------------------------------------------

// Auto DV, the desktop's way: on, the engine closes the plugin's receivers and
// opens no more, so Auto DV and the trunk tracker never share a channel
// (EngineLink::apply_auto_dv). Off lifts that hold. A refusal is an engine
// older than the call, and Auto DV still runs beside the tracker.
[[nodiscard]] Result op_auto_dv(PhoneSession& session, const Command& command)
{
    const auto on = command.get_bool("on");
    if (!on) {
        return std::unexpected(on.error());
    }
    if (session.auto_dv() != *on) {
        session.set_auto_dv(*on);
    }
    auto& client = session.client();
    if (*on && !session.plugin_hold()) {
        session.set_plugin_hold(static_cast<bool>(client.set_plugin_receivers(false)));
    } else if (!*on && session.plugin_hold()) {
        // Only a hold this phone set: the desktop's Auto DV keeps its own.
        static_cast<void>(client.set_plugin_receivers(true));
        session.set_plugin_hold(false);
        // Following the phone still wants picks up again, as the desktop's
        // tracker does once its Auto DV is off.
        if (const auto& follow = session.trunk_follow()) {
            static_cast<void>(client.set_trunk_follow(follow->vrx, true, follow->max_followers));
        }
    }
    return JsonObject().add_bool("on", *on).str();
}

[[nodiscard]] Result op_trunk_follow(PhoneSession& session, const Command& command)
{
    const auto on = command.get_bool("on");
    if (!on) {
        return std::unexpected(on.error());
    }
    std::uint32_t max = session.desktop_max_followers();
    if (command.has("maxFollowers")) {
        const auto n = command.get_i64("maxFollowers");
        if (!n) {
            return std::unexpected(n.error());
        }
        if (*n < kMinFollowers || *n > kMaxFollowers) {
            return fail("maxFollowers: must be 1 to 16");
        }
        max = clamp_followers(*n);
    }
    std::uint64_t vrx = 0;
    if (*on) {
        const auto id = receiver_arg(session, command);
        if (!id) {
            return std::unexpected(id.error());
        }
        vrx = *id;
        // Rules first, so the first grant is judged by them, as the desktop does.
        static_cast<void>(session.client().set_trunk_rules(session.wanted_trunk_rules()));
    } else if (session.trunk_follow()) {
        vrx = session.trunk_follow()->vrx;
    } else if (command.has("vrx")) {
        const auto id = command.get_u64("vrx");
        if (!id) {
            return std::unexpected(id.error());
        }
        vrx = *id;
    }
    if (auto set = session.client().set_trunk_follow(vrx, *on, max); !set) {
        return std::unexpected(set.error());
    }
    session.set_trunk_follow(*on ? std::optional(PhoneSession::TrunkFollow{vrx, max}) : std::nullopt);
    return JsonObject().add_bool("on", *on).add_u64("vrx", vrx).add_u64("maxFollowers", max).str();
}

[[nodiscard]] Result op_trunk_status(PhoneSession& session, const Command&)
{
    const auto status = session.client().trunk_status();
    if (!status) {
        return std::unexpected(status.error());
    }
    return trunk_status_json(*status, {});
}

// The rules in the desktop panel's own text form, so a list pasted on either
// reads the same. Absent fields clear back to the desktop's saved rules.
[[nodiscard]] Result op_set_trunk_rules(PhoneSession& session, const Command& command)
{
    if (!command.has("allow") && !command.has("deny") && !command.has("priority")) {
        session.set_trunk_rules(std::nullopt);
    } else {
        std::string texts[3];
        const char* keys[3] = {"allow", "deny", "priority"};
        for (int i = 0; i < 3; ++i) {
            if (command.has(keys[i])) {
                auto text = command.get_string(keys[i]);
                if (!text) {
                    return std::unexpected(text.error());
                }
                texts[i] = std::move(*text);
            }
        }
        auto parsed = trunk_rules_from_text(texts[0], texts[1], texts[2]);
        session.set_trunk_rules(std::move(parsed.rules));
        if (auto set = session.client().set_trunk_rules(session.wanted_trunk_rules()); !set) {
            return std::unexpected(set.error());
        }
        std::string body = trunk_rules_json(session.wanted_trunk_rules());
        body.pop_back();
        return body + ",\"skipped\":" + std::to_string(parsed.skipped) + "}";
    }
    if (auto set = session.client().set_trunk_rules(session.wanted_trunk_rules()); !set) {
        return std::unexpected(set.error());
    }
    std::string body = trunk_rules_json(session.wanted_trunk_rules());
    body.pop_back();
    return body + ",\"skipped\":0}";
}

[[nodiscard]] Result op_dv_tab(PhoneSession& session, const Command& command)
{
    const auto visible = command.get_bool("visible");
    if (!visible) {
        return std::unexpected(visible.error());
    }
    session.set_dv_watch(*visible);
    return JsonObject().add_bool("visible", *visible).str();
}

[[nodiscard]] Result run(PhoneSession& session, const Command& command)
{
    const std::string_view op = command.op;
    if (op == "autoDv") return op_auto_dv(session, command);
    if (op == "trunkFollow") return op_trunk_follow(session, command);
    if (op == "trunkStatus") return op_trunk_status(session, command);
    if (op == "setTrunkRules") return op_set_trunk_rules(session, command);
    if (op == "dvTab") return op_dv_tab(session, command);
    if (op == "listDecoders") return op_list_decoders(session, command);
    if (op == "setDecoders") return op_set_decoders(session, command);
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
    if (op == "setDetection") return op_set_detection(session, command);
    if (op == "setTranscription") return op_set_transcription(session, command);
    if (op == "setVrxTranscribe") return op_set_vrx_transcribe(session, command);
    return fail("unknown op: " + std::string(op.substr(0, 64)));
}

}  // namespace

std::optional<std::vector<std::string>> voice_modes(const rpc::VocoderPlugins& plugins)
{
    std::vector<std::string> voice{"p25p1"};
    for (const auto& file : plugins.files) {
        if (!file.loaded) {
            continue;
        }
        for (const auto& offer : file.offers) {
            if (offer.modes.empty()) {
                return std::nullopt;
            }
            for (const auto& mode : offer.modes) {
                if (std::ranges::find(voice, mode) == voice.end()) {
                    voice.push_back(mode);
                }
            }
        }
    }
    return voice;
}

std::string_view demod_name(rpc::Demod demod)
{
    return mode_name(demod);
}

std::string receiver_json(const rpc::VrxStatus& status, std::int64_t center,
                          const std::vector<std::string>& decoders, std::string_view opened_by)
{
    std::string body = vrx_json(status, center, decoders);
    body.pop_back();
    body += ",\"openedBy\":" + json_string(opened_by) + "}";
    return body;
}

std::string receiver_event(const rpc::VrxStatus& status, std::int64_t center,
                           const std::vector<std::string>& decoders, std::string_view opened_by)
{
    return R"({"event":"vrx",)" + receiver_json(status, center, decoders, opened_by).substr(1);
}

std::string transcription_status_event(const rpc::TranscriptionStatus& status)
{
    return event("transcriptionStatus", transcription_fields(status));
}

bool transcription_report_due(const rpc::TranscriptionStatus& sent, const rpc::TranscriptionStatus& now)
{
    const auto percent = [](const rpc::TranscriptionStatus& s) -> std::uint64_t {
        return s.bytes_total == 0 ? 0 : std::min<std::uint64_t>(100, s.bytes_done * 100 / s.bytes_total);
    };
    return sent.enabled != now.enabled || sent.model_state != now.model_state ||
           sent.model_name != now.model_name || sent.detail != now.detail ||
           percent(sent) != percent(now);
}

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

std::string dv_state_event(const PhoneSession& session)
{
    const auto& follow = session.trunk_follow();
    const std::string follow_json = follow ? JsonObject()
                                                 .add_u64("vrx", follow->vrx)
                                                 .add_u64("maxFollowers", follow->max_followers)
                                                 .str()
                                           : std::string("null");
    return event("dv", JsonObject()
                           .add_bool("autoDv", session.auto_dv())
                           .add_raw("follow", follow_json)
                           .add_u64("maxFollowers", session.desktop_max_followers()));
}

}  // namespace revenant::webgate
