// EngineLink's side of P25 trunk following: the panel's text fields, the
// switch, and the engine's call log polled twice a second. The follower
// itself is the engine's, core/engine/trunk_follower.h; this is plumbing.
//
// THE RULES ARE RECONCILED, NOT SENT ONCE. Each poll compares the lists the
// engine reports with the ones the panel holds and sends them again when they
// differ, so a reconnect or a restarted engine picks them up with no
// bookkeeping of which connection was told what.

#include "models/engine_link.h"

#include <QDateTime>
#include <QSettings>
#include <QVariantMap>

#include <algorithm>

#include "models/settings.h"
#include "models/trunk_aliases.h"

namespace revenant::ui {

namespace {

constexpr auto kTrunkPoll = std::chrono::milliseconds(500);
// An engine that refuses trunkStatus predates it; asking twice a second
// would only fill the log.
constexpr auto kTrunkRefusedPoll = std::chrono::seconds(10);

rpc::TrunkRuleSet sorted(rpc::TrunkRuleSet rules)
{
    std::ranges::sort(rules.allow);
    std::ranges::sort(rules.deny);
    std::ranges::sort(rules.priorities, {}, &rpc::TrunkPriorityRule::talkgroup);
    return rules;
}

bool same_rules(const rpc::TrunkRuleSet& a, const rpc::TrunkRuleSet& b)
{
    const auto x = sorted(a);
    const auto y = sorted(b);
    if (x.allow != y.allow || x.deny != y.deny || x.priorities.size() != y.priorities.size()) {
        return false;
    }
    for (std::size_t i = 0; i < x.priorities.size(); ++i) {
        if (x.priorities[i].talkgroup != y.priorities[i].talkgroup ||
            x.priorities[i].priority != y.priorities[i].priority) {
            return false;
        }
    }
    return true;
}

QString clock_of(std::int64_t ns)
{
    return QDateTime::fromMSecsSinceEpoch(ns / 1'000'000).toString(QStringLiteral("HH:mm:ss"));
}

}  // namespace

void EngineLink::load_trunk_text() const
{
    if (trunk_loaded_) {
        return;
    }
    trunk_loaded_ = true;
    const QSettings store;
    trunk_aliases_text_ = store.value(settings::kTrunkAliases).toString();
    trunk_allow_text_ = store.value(settings::kTrunkAllow).toString();
    trunk_deny_text_ = store.value(settings::kTrunkDeny).toString();
    trunk_priority_text_ = store.value(settings::kTrunkPriority).toString();
    trunk_max_followers_ = std::clamp(store.value(settings::kTrunkMaxFollowers, 2).toInt(), 1, 16);
    trunk_aliases_ = parse_trunk_aliases(trunk_aliases_text_.toStdString()).value;
}

void EngineLink::store_trunk_rules()
{
    load_trunk_text();
    rpc::TrunkRuleSet rules;
    rules.allow = parse_trunk_groups(trunk_allow_text_.toStdString()).value;
    rules.deny = parse_trunk_groups(trunk_deny_text_.toStdString()).value;
    for (const auto& [tg, priority] : parse_trunk_priorities(trunk_priority_text_.toStdString()).value) {
        rules.priorities.push_back({tg, priority});
    }
    const std::lock_guard<std::mutex> lock(trunk_mutex_);
    trunk_rules_wanted_ = std::move(rules);
    trunk_max_wanted_ = static_cast<std::uint32_t>(trunk_max_followers_);
}

void EngineLink::setTrunkFollow(bool on)
{
    store_trunk_rules();
    {
        const std::lock_guard<std::mutex> lock(trunk_mutex_);
        trunk_follow_pending_ = true;
        trunk_follow_on_ = on;
        trunk_follow_vrx_ = receiver_id_;
    }
    {
        const std::lock_guard<std::mutex> lock(supervisor_mutex_);
        source_work_pending_ = true;
    }
    supervisor_wake_.notify_all();
}

QString EngineLink::trunkAliases() const
{
    load_trunk_text();
    return trunk_aliases_text_;
}

void EngineLink::setTrunkAliases(const QString& text)
{
    load_trunk_text();
    if (text == trunk_aliases_text_) {
        return;
    }
    trunk_aliases_text_ = text;
    trunk_aliases_ = parse_trunk_aliases(text.toStdString()).value;
    QSettings().setValue(settings::kTrunkAliases, text);
    rebuild_trunk_rows();
    emit trunkTextChanged();
    emit trunkChanged();
}

QString EngineLink::trunkAllow() const
{
    load_trunk_text();
    return trunk_allow_text_;
}

void EngineLink::setTrunkAllow(const QString& text)
{
    load_trunk_text();
    if (text == trunk_allow_text_) {
        return;
    }
    trunk_allow_text_ = text;
    QSettings().setValue(settings::kTrunkAllow, text);
    store_trunk_rules();
    emit trunkTextChanged();
}

QString EngineLink::trunkDeny() const
{
    load_trunk_text();
    return trunk_deny_text_;
}

void EngineLink::setTrunkDeny(const QString& text)
{
    load_trunk_text();
    if (text == trunk_deny_text_) {
        return;
    }
    trunk_deny_text_ = text;
    QSettings().setValue(settings::kTrunkDeny, text);
    store_trunk_rules();
    emit trunkTextChanged();
}

QString EngineLink::trunkPriority() const
{
    load_trunk_text();
    return trunk_priority_text_;
}

void EngineLink::setTrunkPriority(const QString& text)
{
    load_trunk_text();
    if (text == trunk_priority_text_) {
        return;
    }
    trunk_priority_text_ = text;
    QSettings().setValue(settings::kTrunkPriority, text);
    store_trunk_rules();
    emit trunkTextChanged();
}

int EngineLink::trunkMaxFollowers() const
{
    load_trunk_text();
    return trunk_max_followers_;
}

void EngineLink::setTrunkMaxFollowers(int count)
{
    load_trunk_text();
    count = std::clamp(count, 1, 16);
    if (count == trunk_max_followers_) {
        return;
    }
    trunk_max_followers_ = count;
    QSettings().setValue(settings::kTrunkMaxFollowers, count);
    store_trunk_rules();
    emit trunkTextChanged();
}

QString EngineLink::trunkSummary() const
{
    if (!trunk_fault_.isEmpty()) {
        return QStringLiteral("this engine does not follow trunks: %1").arg(trunk_fault_);
    }
    const rpc::TrunkStatusInfo& s = trunk_status_;
    if (!s.enabled) {
        return s.note.empty() ? QStringLiteral("not following")
                              : QStringLiteral("not following: %1").arg(QString::fromStdString(s.note));
    }
    const auto active = std::ranges::count_if(s.calls, &rpc::TrunkCallInfo::active);
    return QStringLiteral("control channel on receiver %1, %2 blocks, %3 grants, %4 on the air")
        .arg(s.control_vrx)
        .arg(s.tsbks)
        .arg(s.grants)
        .arg(active);
}

void EngineLink::apply_trunk()
{
    if (client_ == nullptr) {
        return;
    }
    bool follow = false;
    bool on = false;
    std::uint64_t vrx = 0;
    rpc::TrunkRuleSet rules;
    std::uint32_t max = 0;
    {
        const std::lock_guard<std::mutex> lock(trunk_mutex_);
        follow = std::exchange(trunk_follow_pending_, false);
        on = trunk_follow_on_;
        vrx = trunk_follow_vrx_;
        rules = trunk_rules_wanted_;
        max = trunk_max_wanted_;
    }
    const auto now = std::chrono::steady_clock::now();
    QString fault;
    if (follow) {
        // Rules first, so the first grant is judged by them.
        (void)client_->set_trunk_rules(rules);
        if (auto set = client_->set_trunk_follow(vrx, on, max); !set) {
            fault = QString::fromStdString(set.error().message);
        }
        trunk_next_poll_ = now;
    }
    if (now < trunk_next_poll_) {
        return;
    }
    auto status = client_->trunk_status();
    if (!status) {
        trunk_next_poll_ = now + kTrunkRefusedPoll;
        fault = QString::fromStdString(status.error().message);
    } else {
        trunk_next_poll_ = now + kTrunkPoll;
        if (!same_rules(status->rules, rules)) {
            (void)client_->set_trunk_rules(rules);
        }
        if (status->enabled && status->max_followers != max) {
            (void)client_->set_trunk_follow(status->control_vrx, true, max);
        }
        if (fault.isEmpty() && status->revision == trunk_revision_posted_) {
            return;
        }
        trunk_revision_posted_ = status->revision;
    }
    {
        const std::lock_guard<std::mutex> lock(trunk_mutex_);
        trunk_handover_ = status ? *std::move(status) : rpc::TrunkStatusInfo{};
        trunk_handover_fault_ = fault;
        trunk_has_handover_ = true;
    }
    QMetaObject::invokeMethod(this, [this] { adopt_trunk(); }, Qt::QueuedConnection);
}

void EngineLink::adopt_trunk()
{
    {
        const std::lock_guard<std::mutex> lock(trunk_mutex_);
        if (!trunk_has_handover_) {
            return;
        }
        trunk_has_handover_ = false;
        trunk_status_ = std::move(trunk_handover_);
        trunk_handover_ = {};
        trunk_fault_ = trunk_handover_fault_;
    }
    rebuild_trunk_rows();
    emit trunkChanged();
}

void EngineLink::rebuild_trunk_rows()
{
    load_trunk_text();
    trunk_rows_.clear();
    for (const rpc::TrunkCallInfo& call : trunk_status_.calls) {
        QVariantMap row;
        row[QStringLiteral("talkgroup")] = static_cast<qulonglong>(call.talkgroup);
        row[QStringLiteral("label")] =
            QString::fromStdString(trunk_group_label(trunk_aliases_, call.talkgroup));
        row[QStringLiteral("source")] =
            call.source ? QString::number(*call.source) : QString();
        row[QStringLiteral("frequency")] =
            QString::number(static_cast<double>(call.frequency_hz) / 1e6, 'f', 5);
        row[QStringLiteral("start")] = clock_of(call.start_ns);
        row[QStringLiteral("seconds")] =
            static_cast<int>(std::max<std::int64_t>(0, call.end_ns - call.start_ns) / 1'000'000'000);
        row[QStringLiteral("active")] = call.active;
        row[QStringLiteral("encrypted")] = call.encrypted;
        row[QStringLiteral("emergency")] = call.emergency;
        row[QStringLiteral("followed")] = call.followed;
        row[QStringLiteral("hearing")] = call.vrx != 0;
        row[QStringLiteral("note")] = QString::fromStdString(call.active ? call.skip : call.end_reason);
        trunk_rows_.push_back(row);
    }
}

}  // namespace revenant::ui
