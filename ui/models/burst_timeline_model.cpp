#include "models/burst_timeline_model.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#include <QDateTime>
#include <QFile>
#include <QStringList>
#include <QVariantMap>

#include "models/capture_browser.h"
#include "models/decoded_log.h"
#include "models/engine_link.h"

namespace revenant::ui {

namespace {

constexpr std::int64_t kMsNs = 1'000'000;

// Adopted receivers share the observation key space with detector tracks, so
// they are kept in the top half where no detector id will reach.
constexpr std::uint64_t kReceiverSource = 1ULL << 63;

// The decoded fields a burst is labelled with: who and on what, never the
// payload. Every key here is one a decoder in core/rpc/decoders.h publishes.
constexpr std::array<std::string_view, 15> kIdKeys{
    "talkgroup", "source",   "source_address", "destination", "destination_address",
    "callsign",  "my",       "ur",             "rpt1",        "rpt2",
    "mmsi",      "address",  "nac",            "pi",          "unit",
};

[[nodiscard]] std::int64_t wall_now_ns()
{
    return QDateTime::currentMSecsSinceEpoch() * kMsNs;
}

[[nodiscard]] std::vector<std::string> ids_of(const std::vector<rpc::DecodedField>& fields)
{
    std::vector<std::string> out;
    for (const rpc::DecodedField& field : fields) {
        if (std::find(kIdKeys.begin(), kIdKeys.end(), field.key) == kIdKeys.end()) {
            continue;
        }
        out.push_back(field.key + "=" + field_value_text(field.key, field.value));
    }
    return out;
}

[[nodiscard]] QString mhz_text(std::int64_t hz)
{
    return QString::number(static_cast<double>(hz) / 1e6, 'f', 4) + QStringLiteral(" MHz");
}

}  // namespace

BurstTimelineModel::BurstTimelineModel(QObject* parent) : QObject(parent)
{
    // Twice a second: often enough for the live edge to move smoothly at the
    // default zoom, and bursts close within one tick of their hang time.
    tick_.setInterval(500);
    connect(&tick_, &QTimer::timeout, this, &BurstTimelineModel::on_tick);
    tick_.start();
}

QObject* BurstTimelineModel::link() const
{
    return link_.data();
}

void BurstTimelineModel::setLink(QObject* link)
{
    auto* engine = qobject_cast<EngineLink*>(link);
    if (engine == link_.data()) {
        return;
    }
    if (link_ != nullptr) {
        disconnect(link_, nullptr, this, nullptr);
    }
    link_ = engine;
    track_seen_.clear();
    if (link_ != nullptr) {
        connect(link_, &EngineLink::detectionsChanged, this, &BurstTimelineModel::on_detections);
        connect(link_, &EngineLink::transcriptsArrived, this, &BurstTimelineModel::on_transcripts);
        connect(link_, &EngineLink::burstDecodedArrived, this, &BurstTimelineModel::on_decoded);
        connect(link_, &EngineLink::captureHitsChanged, this,
                &BurstTimelineModel::on_capture_hits);
        // A new engine starts its sample count again, so the per-track marks
        // from the last one would hold every track back.
        connect(link_, &EngineLink::connectionChanged, this, [this] { track_seen_.clear(); });
        // What arrived before the timeline was opened is skipped rather than
        // replayed: its detections are already gone, so it would attach to
        // nothing.
        if (!link_->captionFeed().empty()) {
            caption_serial_ = link_->captionFeed().back().serial;
        }
        if (!link_->burstDecodedFeed().empty()) {
            decoded_serial_ = link_->burstDecodedFeed().back().serial;
        }
    }
    emit linkChanged();
}

double BurstTimelineModel::nowMs() const
{
    return static_cast<double>(QDateTime::currentMSecsSinceEpoch());
}

QVariantList BurstTimelineModel::lanes() const
{
    QVariantList out;
    for (const std::int64_t hz : timeline_.lanes()) {
        out.append(QVariantMap{{QStringLiteral("hz"), static_cast<double>(hz)},
                               {QStringLiteral("label"), mhz_text(hz)}});
    }
    return out;
}

double BurstTimelineModel::oldestMs() const
{
    if (timeline_.bursts().empty()) {
        return nowMs();
    }
    return static_cast<double>(timeline_.bursts().front().start_ns / kMsNs);
}

bool BurstTimelineModel::captureAvailable() const
{
    return link_ != nullptr && link_->connected() && !link_->captureText().isEmpty();
}

QVariantList BurstTimelineModel::bursts(double fromMs, double toMs) const
{
    QVariantList out;
    const auto from = static_cast<std::int64_t>(fromMs) * kMsNs;
    const auto to = static_cast<std::int64_t>(toMs) * kMsNs;
    for (const Burst* b : timeline_.visible(from, to)) {
        QStringList ids;
        for (const std::string& id : b->ids()) {
            ids.append(QString::fromStdString(id));
        }
        out.append(QVariantMap{
            {QStringLiteral("id"), QVariant::fromValue<qulonglong>(b->id)},
            {QStringLiteral("laneHz"), static_cast<double>(b->lane_hz)},
            {QStringLiteral("hz"), static_cast<double>(b->center_hz)},
            {QStringLiteral("frequency"), mhz_text(b->center_hz)},
            {QStringLiteral("bandwidthHz"), static_cast<double>(b->bandwidth_hz)},
            {QStringLiteral("startMs"), static_cast<double>(b->start_ns / kMsNs)},
            {QStringLiteral("endMs"), static_cast<double>(b->end_ns / kMsNs)},
            {QStringLiteral("open"), b->open},
            {QStringLiteral("mode"), QString::fromStdString(b->mode)},
            {QStringLiteral("snr"), b->peak_snr_db},
            {QStringLiteral("ids"), ids.join(QStringLiteral("  "))},
            {QStringLiteral("decoded"), QString::fromStdString(b->decoded())},
            {QStringLiteral("transcript"), QString::fromStdString(b->transcript())},
        });
    }
    return out;
}

bool BurstTimelineModel::exportCsv(double fromMs, double toMs, const QUrl& file)
{
    const auto from = static_cast<std::int64_t>(fromMs) * kMsNs;
    const auto to = static_cast<std::int64_t>(toMs) * kMsNs;
    const std::vector<const Burst*> shown = timeline_.visible(from, to);
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        set_note(QStringLiteral("could not write %1: %2").arg(path, out.errorString()));
        return false;
    }
    const std::string csv = burst_csv(shown);
    out.write(csv.data(), static_cast<qint64>(csv.size()));
    out.close();
    set_note(QStringLiteral("%1 bursts written to %2").arg(shown.size()).arg(path));
    return true;
}

void BurstTimelineModel::tune(qulonglong id)
{
    const Burst* b = timeline_.find(id);
    if (b == nullptr || link_ == nullptr) {
        return;
    }
    // An empty mode lets the measurement choose, as a click on the span does;
    // a burst's mode is a label and not always a demodulator's name.
    link_->tuneReceiverToDetection(static_cast<double>(b->center_hz), QString(),
                                   static_cast<double>(b->bandwidth_hz));
    set_note(QStringLiteral("tuned to %1").arg(mhz_text(b->center_hz)));
}

void BurstTimelineModel::openCapture(qulonglong id)
{
    const Burst* b = timeline_.find(id);
    if (b == nullptr || link_ == nullptr) {
        return;
    }
    if (!captureAvailable()) {
        set_note(QStringLiteral("the engine is not keeping a capture"));
        return;
    }
    // The shortest window of the capture panel's that reaches back to it.
    const std::int64_t age = wall_now_ns() - b->start_ns;
    QString window = QStringLiteral("24 h");
    for (const CaptureChoice& choice : kCaptureWindows) {
        if (choice.ns > age) {
            window = QString::fromUtf8(choice.label.data(), static_cast<qsizetype>(choice.label.size()));
            break;
        }
    }
    const std::int64_t half = std::max<std::int64_t>(b->bandwidth_hz / 2, 5'000);
    const QString band = QStringLiteral("%1:%2")
                             .arg(static_cast<double>(b->center_hz - half) / 1e6, 0, 'f', 6)
                             .arg(static_cast<double>(b->center_hz + half) / 1e6, 0, 'f', 6);
    capture_pending_ = id;
    capture_search_seen_busy_ = false;
    link_->searchCapture(window, band);
    set_note(QStringLiteral("looking for %1 in the capture").arg(mhz_text(b->center_hz)));
}

void BurstTimelineModel::clear()
{
    timeline_.clear();
    emit changed();
}

void BurstTimelineModel::on_capture_hits()
{
    if (capture_pending_ == 0 || link_ == nullptr) {
        return;
    }
    if (link_->captureSearchBusy()) {
        capture_search_seen_busy_ = true;
        return;
    }
    if (!capture_search_seen_busy_) {
        return;
    }
    const std::uint64_t id = std::exchange(capture_pending_, 0);
    const Burst* b = timeline_.find(id);
    if (b == nullptr) {
        return;
    }
    if (!link_->captureSearchFault().isEmpty()) {
        set_note(link_->captureSearchFault());
        return;
    }
    const QVariantList hits = link_->captureHits();
    if (hits.isEmpty()) {
        set_note(QStringLiteral("the capture holds nothing on %1 then").arg(mhz_text(b->center_hz)));
        return;
    }
    const std::string any = hits.front().toMap().value(QStringLiteral("uri")).toString().toStdString();
    const auto uri = burst_replay_uri(any, b->start_ns - kBurstSecondNs, b->end_ns + kBurstSecondNs);
    if (!uri) {
        set_note(QStringLiteral("the capture answered with no replay"));
        return;
    }
    link_->openSource(QString::fromStdString(*uri));
    set_note(QStringLiteral("replaying %1 from the capture").arg(mhz_text(b->center_hz)));
}

std::int64_t BurstTimelineModel::sample_to_ns(std::uint64_t sample) const
{
    if (rate_ <= 0.0) {
        return anchor_ns_;
    }
    const double back = (static_cast<double>(anchor_sample_) - static_cast<double>(sample)) / rate_;
    return anchor_ns_ - static_cast<std::int64_t>(back * 1e9);
}

void BurstTimelineModel::on_detections()
{
    if (link_ == nullptr) {
        return;
    }
    rate_ = static_cast<double>(link_->sourceRate());
    anchor_sample_ = static_cast<std::uint64_t>(link_->lastDecisionSample());
    anchor_ns_ = wall_now_ns();
    if (rate_ <= 0.0) {
        return;
    }
    for (const rpc::Detection& d : link_->detections()) {
        if (d.last_detected == 0) {
            continue;
        }
        auto [it, fresh] = track_seen_.try_emplace(d.id, d.first_seen);
        if (!fresh && it->second >= d.last_detected) {
            continue;
        }
        BurstObservation obs;
        obs.source = d.id;
        obs.center_hz = d.center_hz;
        obs.bandwidth_hz = d.bandwidth_hz;
        obs.start_ns = sample_to_ns(std::max(d.first_seen, it->second));
        obs.end_ns = sample_to_ns(d.last_detected);
        obs.snr_db = d.snr_2500_db;
        if (d.label.kind != rpc::LabelKind::Unknown) {
            obs.mode = d.label.name;
        }
        it->second = d.last_detected;
        timeline_.observe(obs);
    }
    // Ids are never reused, so a long session would otherwise keep every one.
    if (track_seen_.size() > 8192) {
        track_seen_.clear();
    }
}

void BurstTimelineModel::on_transcripts()
{
    if (link_ == nullptr) {
        return;
    }
    const std::int64_t now = wall_now_ns();
    for (const EngineLink::CaptionFeedEntry& entry : link_->captionFeed()) {
        if (entry.serial <= caption_serial_) {
            continue;
        }
        caption_serial_ = entry.serial;
        const rpc::Transcript& t = entry.transcript;
        vrx_hz_[t.vrx] = t.center_hz;
        BurstAttachment att;
        att.kind = BurstAttachmentKind::Transcript;
        att.hz = t.center_hz;
        if (rate_ > 0.0 && t.source_end > 0) {
            att.start_ns = sample_to_ns(t.source_start);
            att.end_ns = sample_to_ns(t.source_end);
        } else {
            att.start_ns = now - static_cast<std::int64_t>(t.latency_ms * 1e6);
            att.end_ns = att.start_ns;
        }
        att.origin = t.mode;
        att.text = t.text;
        att.ids = ids_of(t.fields);
        timeline_.attach(std::move(att), now);
    }
    emit changed();
}

void BurstTimelineModel::on_decoded()
{
    if (link_ == nullptr) {
        return;
    }
    const std::int64_t now = wall_now_ns();
    for (const EngineLink::BurstDecodedEntry& entry : link_->burstDecodedFeed()) {
        if (entry.serial <= decoded_serial_) {
            continue;
        }
        decoded_serial_ = entry.serial;
        const rpc::DecodedMessage& m = entry.message;
        auto hz = static_cast<std::int64_t>(link_->burstReceiverHz(m.vrx));
        if (hz == 0) {
            const auto known = vrx_hz_.find(m.vrx);
            if (known == vrx_hz_.end()) {
                continue;
            }
            hz = known->second;
        }
        BurstAttachment att;
        att.kind = BurstAttachmentKind::Decoded;
        att.hz = hz;
        // Stamped when it completed; its own span says how long it took.
        att.end_ns = entry.arrived_ms * kMsNs;
        att.start_ns = att.end_ns;
        if (m.sample_rate > 0 && m.end_sample > m.start_sample) {
            att.start_ns -= static_cast<std::int64_t>(static_cast<double>(m.end_sample - m.start_sample) /
                                                      static_cast<double>(m.sample_rate) * 1e9);
        }
        att.origin = m.decoder;
        att.text = m.text.empty() ? m.decoder + " " + m.kind : m.decoder + ": " + m.text;
        att.ids = ids_of(m.fields);
        timeline_.attach(std::move(att), now);
    }
    emit changed();
}

void BurstTimelineModel::on_tick()
{
    const std::int64_t now = wall_now_ns();
    if (link_ != nullptr && link_->connected()) {
        // A plugin's receiver is a transmission for as long as it is open.
        for (const QVariant& v : link_->rackEntries()) {
            const QVariantMap e = v.toMap();
            if (!e.value(QStringLiteral("adopted")).toBool()) {
                continue;
            }
            const double hz = e.value(QStringLiteral("frequencyHz")).toDouble();
            if (hz <= 0.0) {
                continue;
            }
            BurstObservation obs;
            obs.source = kReceiverSource | e.value(QStringLiteral("key")).toULongLong();
            obs.center_hz = static_cast<std::int64_t>(hz);
            obs.bandwidth_hz = static_cast<std::int64_t>(e.value(QStringLiteral("highHz")).toDouble() -
                                                         e.value(QStringLiteral("lowHz")).toDouble());
            obs.start_ns = now - tick_.interval() * kMsNs;
            obs.end_ns = now;
            obs.mode = e.value(QStringLiteral("mode")).toString().toStdString();
            obs.snr_db = 0.0;
            timeline_.observe(obs);
        }
    }
    timeline_.tick(now);
    emit changed();
}

void BurstTimelineModel::set_note(const QString& note)
{
    if (note == note_) {
        return;
    }
    note_ = note;
    emit noteChanged();
}

}  // namespace revenant::ui
