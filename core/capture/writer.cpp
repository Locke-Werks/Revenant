#include "core/capture/writer.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <utility>

#ifndef REVENANT_CAPTURE_RECORDER_VERSION
#define REVENANT_CAPTURE_RECORDER_VERSION "unknown"
#endif

namespace revenant::capture {

namespace {

namespace fs = std::filesystem;
using namespace std::chrono;

// The SigMF version the meta declares. 1.x is what the file source accepts.
constexpr std::string_view kSigmfVersion = "1.2.0";

struct Civil {
    int year = 0;
    unsigned month = 0;
    unsigned day = 0;
    long long hour = 0;
    long long minute = 0;
    long long second = 0;
    long long nanos = 0;
};

[[nodiscard]] Civil to_civil(std::int64_t ns)
{
    const sys_time<nanoseconds> tp{nanoseconds{ns}};
    const auto day_point = floor<days>(tp);
    const year_month_day ymd{day_point};
    const hh_mm_ss<nanoseconds> hms{tp - day_point};
    return Civil{static_cast<int>(ymd.year()),
                 static_cast<unsigned>(ymd.month()),
                 static_cast<unsigned>(ymd.day()),
                 hms.hours().count(),
                 hms.minutes().count(),
                 hms.seconds().count(),
                 hms.subseconds().count()};
}

[[nodiscard]] std::string iso_datetime(std::int64_t ns)
{
    const Civil c = to_civil(ns);
    return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:09}Z", c.year, c.month, c.day,
                       c.hour, c.minute, c.second, c.nanos);
}

[[nodiscard]] std::string path_text(const fs::path& path)
{
    const std::u8string utf8 = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// The datatype strings the file source reads back. Cs24 has no reader there
// yet, so a cs24 segment is written faithfully and will not replay until the
// SigMF reader learns ci24_le.
[[nodiscard]] std::string_view sigmf_datatype(source::SampleFormat format)
{
    switch (format) {
        case source::SampleFormat::Cu8: return "cu8";
        case source::SampleFormat::Cs8: return "ci8";
        case source::SampleFormat::Cs16: return "ci16_le";
        case source::SampleFormat::Cf32: return "cf32_le";
        case source::SampleFormat::Cs24: return "ci24_le";
    }
    return "unknown";
}

[[nodiscard]] std::optional<source::SampleFormat> format_from_name(std::string_view name)
{
    for (const auto format :
         {source::SampleFormat::Cu8, source::SampleFormat::Cs8, source::SampleFormat::Cs16,
          source::SampleFormat::Cf32, source::SampleFormat::Cs24}) {
        if (name == source::format_name(format)) {
            return format;
        }
    }
    return std::nullopt;
}

template <class T>
[[nodiscard]] bool parse_integer(std::string_view text, T& out, int base = 10)
{
    if (text.empty()) {
        return false;
    }
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out, base);
    return ec == std::errc{} && end == text.data() + text.size();
}

// A minimal JSON emitter. The meta is small and its shape is fixed, so a
// builder over a string is all it needs and the escaping is the one part that
// has to be right: core:hw and a family name are free text.
class Json {
public:
    void open_object() { separate(); out_ += '{'; first_ = true; }
    void close_object() { out_ += '}'; first_ = false; }
    void open_array() { separate(); out_ += '['; first_ = true; }
    void close_array() { out_ += ']'; first_ = false; }

    void key(std::string_view name)
    {
        separate();
        string_literal(name);
        out_ += ':';
        after_key_ = true;
    }

    void value(std::string_view text) { separate(); string_literal(text); }
    void value(const char* text) { value(std::string_view(text)); }
    void value(std::uint64_t number) { separate(); out_ += std::to_string(number); }
    void value(std::int64_t number) { separate(); out_ += std::to_string(number); }
    void value(bool flag) { separate(); out_ += flag ? "true" : "false"; }
    void value(double number)
    {
        separate();
        // JSON has no spelling for either, and a reader that meets "nan" fails
        // on the whole document rather than on the one field.
        if (!std::isfinite(number)) {
            out_ += "null";
            return;
        }
        out_ += std::format("{}", number);
    }

    [[nodiscard]] std::string take() { return std::move(out_); }

private:
    void separate()
    {
        if (after_key_) {
            after_key_ = false;
            return;
        }
        if (!first_) {
            out_ += ',';
        }
        first_ = false;
    }

    void string_literal(std::string_view text)
    {
        out_ += '"';
        for (const char ch : text) {
            const auto c = static_cast<unsigned char>(ch);
            switch (ch) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        out_ += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    } else {
                        out_ += ch;
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    bool first_ = true;
    bool after_key_ = false;
};

[[nodiscard]] std::string strip_newlines(std::string_view text)
{
    std::string out(text);
    std::ranges::replace(out, '\n', ' ');
    std::ranges::replace(out, '\r', ' ');
    return out;
}

struct SideFile {
    dsp::SampleRate rate = 0;
    source::SampleFormat format = source::SampleFormat::Cu8;
    std::int64_t anchor_ns = 0;
    std::uint64_t dropped_before = 0;
    std::optional<double> gain_db;
    std::string hw;
};

[[nodiscard]] std::string render_side(const SideFile& side)
{
    std::string out = std::format("rate={}\nformat={}\nanchor_ns={}\ndropped_before={}\n",
                                  side.rate, source::format_name(side.format), side.anchor_ns,
                                  side.dropped_before);
    if (side.gain_db) {
        out += std::format("gain_db={}\n", *side.gain_db);
    }
    out += std::format("hw={}\n", strip_newlines(side.hw));
    return out;
}

[[nodiscard]] Expected<SideFile> read_side(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return fail(std::format("side file '{}' is missing, so the segment's rate and format "
                                "are unknown",
                                path_text(path)));
    }
    SideFile side;
    bool have_rate = false;
    bool have_format = false;
    bool have_anchor = false;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string_view name(line.data(), eq);
        const std::string_view text(line.data() + eq + 1, line.size() - eq - 1);
        if (name == "rate") {
            have_rate = parse_integer(text, side.rate) && side.rate > 0;
        } else if (name == "format") {
            const auto format = format_from_name(text);
            have_format = format.has_value();
            if (format) {
                side.format = *format;
            }
        } else if (name == "anchor_ns") {
            have_anchor = parse_integer(text, side.anchor_ns);
        } else if (name == "dropped_before") {
            static_cast<void>(parse_integer(text, side.dropped_before));
        } else if (name == "gain_db") {
            double gain = 0.0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), gain);
            if (ec == std::errc{}) {
                side.gain_db = gain;
            }
        } else if (name == "hw") {
            side.hw = std::string(text);
        }
    }
    if (!have_rate || !have_format || !have_anchor) {
        return fail(std::format("side file '{}' lacks a valid rate, format or anchor_ns",
                                path_text(path)));
    }
    return side;
}

[[nodiscard]] Status write_text(const fs::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return fail(std::format("cannot create '{}'", path_text(path)));
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) {
        return fail(std::format("writing '{}' failed, most likely a full disk", path_text(path)));
    }
    return {};
}

[[nodiscard]] fs::path open_data_path(const fs::path& dir, std::string_view stem)
{
    return dir / std::format("{}{}{}", stem, kOpenSuffix, kDataExtension);
}
[[nodiscard]] fs::path side_path(const fs::path& dir, std::string_view stem)
{
    return dir / std::format("{}{}{}", stem, kOpenSuffix, kSideExtension);
}
[[nodiscard]] fs::path data_path(const fs::path& dir, std::string_view stem)
{
    return dir / std::format("{}{}", stem, kDataExtension);
}
[[nodiscard]] fs::path meta_path(const fs::path& dir, std::string_view stem)
{
    return dir / std::format("{}{}", stem, kMetaExtension);
}

[[nodiscard]] std::uint64_t draw_epoch()
{
    // An identifier rather than anything in the sample path, so the seed rule
    // for randomness does not apply. Kept under 2^63 so a reader whose JSON
    // integers are signed 64-bit reads it exactly.
    std::random_device device;
    const std::uint64_t value =
        (static_cast<std::uint64_t>(device()) << 32) ^ static_cast<std::uint64_t>(device());
    const std::uint64_t masked = value & 0x7FFF'FFFF'FFFF'FFFFULL;
    return masked == 0 ? 1 : masked;
}

}  // namespace

std::string make_stem(const SegmentInfo& info)
{
    const Civil c = to_civil(info.start_ns());
    return std::format("rev-{:04}{:02}{:02}T{:02}{:02}{:02}.{:03}Z-{}-{:016x}-{}", c.year, c.month,
                       c.day, c.hour, c.minute, c.second, c.nanos / 1'000'000, info.center,
                       info.stream_epoch, info.first_index);
}

std::string day_directory(std::int64_t start_ns)
{
    const Civil c = to_civil(start_ns);
    return std::format("{:04}-{:02}-{:02}", c.year, c.month, c.day);
}

Expected<StemFields> parse_stem(std::string_view stem)
{
    const auto bad = [&](std::string_view why) {
        return fail(std::format("'{}' is not a capture stem: {}", stem, why));
    };
    if (!stem.starts_with("rev-")) {
        return bad("no rev- prefix");
    }
    std::string_view rest = stem.substr(4);
    const auto first_dash = rest.find('-');
    const auto last_dash = rest.rfind('-');
    if (first_dash == std::string_view::npos || last_dash == first_dash) {
        return bad("too few fields");
    }
    const auto epoch_dash = rest.rfind('-', last_dash - 1);
    if (epoch_dash == std::string_view::npos || epoch_dash <= first_dash) {
        return bad("too few fields");
    }
    const std::string_view time = rest.substr(0, first_dash);
    const std::string_view center = rest.substr(first_dash + 1, epoch_dash - first_dash - 1);
    const std::string_view epoch = rest.substr(epoch_dash + 1, last_dash - epoch_dash - 1);
    const std::string_view first = rest.substr(last_dash + 1);

    StemFields out;
    if (!parse_integer(center, out.center)) {
        return bad("centre is not an integer");
    }
    if (epoch.size() != 16 || !parse_integer(epoch, out.stream_epoch, 16)) {
        return bad("epoch is not 16 hex digits");
    }
    if (!parse_integer(first, out.first_index)) {
        return bad("first index is not an integer");
    }

    // yyyymmddThhmmss.fffZ
    if (time.size() != 20 || time[8] != 'T' || time[15] != '.' || time[19] != 'Z') {
        return bad("time is not yyyymmddThhmmss.fffZ");
    }
    int y = 0;
    unsigned mo = 0;
    unsigned d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    int ms = 0;
    if (!parse_integer(time.substr(0, 4), y) || !parse_integer(time.substr(4, 2), mo) ||
        !parse_integer(time.substr(6, 2), d) || !parse_integer(time.substr(9, 2), h) ||
        !parse_integer(time.substr(11, 2), mi) || !parse_integer(time.substr(13, 2), s) ||
        !parse_integer(time.substr(16, 3), ms)) {
        return bad("time has a non-digit");
    }
    const year_month_day ymd{year{y}, month{mo}, day{d}};
    if (!ymd.ok()) {
        return bad("date does not exist");
    }
    const auto tp = sys_days{ymd} + hours{h} + minutes{mi} + seconds{s} + milliseconds{ms};
    out.start_ns = duration_cast<nanoseconds>(tp.time_since_epoch()).count();
    return out;
}

std::string render_meta(const MetaFields& fields)
{
    const SegmentInfo& info = fields.info;
    Json json;
    json.open_object();

    json.key("global");
    json.open_object();
    json.key("core:datatype");
    json.value(sigmf_datatype(info.format));
    json.key("core:sample_rate");
    json.value(static_cast<std::int64_t>(info.rate));
    json.key("core:version");
    json.value(kSigmfVersion);
    json.key("core:num_channels");
    json.value(std::uint64_t{1});
    json.key("core:recorder");
    json.value(std::format("Revenant {}", REVENANT_CAPTURE_RECORDER_VERSION));
    json.key("core:hw");
    json.value(fields.hw);
    json.key("core:extensions");
    json.open_array();
    json.open_object();
    json.key("name");
    json.value(kExtensionName);
    json.key("version");
    json.value(kExtensionVersion);
    json.key("optional");
    json.value(true);
    json.close_object();
    json.close_array();
    json.key("revenant:stream_epoch");
    json.value(info.stream_epoch);
    json.key("revenant:first_index");
    json.value(static_cast<std::uint64_t>(info.first_index));
    json.key("revenant:epoch_anchor_ns");
    json.value(info.epoch_anchor_ns);
    json.key("revenant:dropped_before");
    json.value(info.dropped_before);
    json.key("revenant:close_reason");
    json.value(fields.close_reason);
    if (fields.gain_db) {
        json.key("revenant:gain_db");
        json.value(*fields.gain_db);
    }
    json.close_object();

    json.key("captures");
    json.open_array();
    json.open_object();
    json.key("core:sample_start");
    json.value(std::uint64_t{0});
    json.key("core:frequency");
    json.value(static_cast<std::int64_t>(info.center));
    json.key("core:datetime");
    json.value(iso_datetime(info.start_ns()));
    json.close_object();
    json.close_array();

    json.key("annotations");
    json.open_array();
    for (const MetaFields::Entry& entry : fields.annotations) {
        const Annotation& a = entry.annotation;
        json.open_object();
        json.key("core:sample_start");
        json.value(a.sample_start);
        json.key("core:sample_count");
        json.value(a.sample_count);
        json.key("core:freq_lower_edge");
        json.value(static_cast<std::int64_t>(a.freq_lower));
        json.key("core:freq_upper_edge");
        json.value(static_cast<std::int64_t>(a.freq_upper));
        json.key("core:label");
        json.value(a.family.empty() ? std::string_view("signal") : std::string_view(a.family));
        json.key("core:generator");
        json.value(entry.generator);
        json.key("revenant:confidence");
        json.value(a.confidence);
        json.key("revenant:peak_db");
        json.value(a.peak_db);
        json.key("revenant:snr_db");
        json.value(a.snr_db);
        if (!a.family.empty()) {
            json.key("revenant:family");
            json.value(a.family);
        }
        if (entry.track_id) {
            json.key("revenant:track_id");
            json.value(*entry.track_id);
        }
        json.close_object();
    }
    json.close_array();

    json.close_object();
    std::string out = json.take();
    out += '\n';
    return out;
}

Expected<RecoveryReport> recover_open_segments(const fs::path& root)
{
    RecoveryReport report;
    std::error_code ec;
    if (!fs::exists(root, ec)) {
        return report;
    }
    const std::string open_tail = std::format("{}{}", kOpenSuffix, kDataExtension);

    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        const std::string name = path_text(it->path().filename());
        if (name.ends_with(open_tail)) {
            found.push_back(it->path());
        }
    }
    if (ec) {
        return fail(std::format("listing capture root '{}': {}", path_text(root), ec.message()));
    }

    for (const fs::path& open_data : found) {
        const fs::path dir = open_data.parent_path();
        const std::string name = path_text(open_data.filename());
        const std::string stem = name.substr(0, name.size() - open_tail.size());
        const auto failure = [&](std::string_view why) {
            report.failures.push_back(std::format("{}: {}", path_text(open_data), why));
        };

        auto fields = parse_stem(stem);
        if (!fields) {
            failure(fields.error().message);
            continue;
        }
        auto side = read_side(side_path(dir, stem));
        if (!side) {
            failure(side.error().message);
            continue;
        }

        const std::uint64_t size = fs::file_size(open_data, ec);
        if (ec) {
            failure(ec.message());
            continue;
        }
        const std::uint64_t bps = source::bytes_per_sample(side->format);
        const std::uint64_t whole = size / bps * bps;
        if (whole != size) {
            // A partial sample is the tail of a write the process died in.
            // Truncating it is what makes the declared extent equal the bytes
            // on disk, which the file source checks.
            fs::resize_file(open_data, whole, ec);
            if (ec) {
                failure(std::format("truncating a partial sample: {}", ec.message()));
                continue;
            }
        }
        if (whole == 0) {
            fs::remove(open_data, ec);
            fs::remove(side_path(dir, stem), ec);
            ++report.removed_empty;
            continue;
        }

        const fs::path meta = meta_path(dir, stem);
        // A meta already present means the writer died between writing it
        // and the rename, and that meta is the complete one.
        if (!fs::exists(meta, ec)) {
            MetaFields meta_fields;
            meta_fields.info.stream_epoch = fields->stream_epoch;
            meta_fields.info.first_index = fields->first_index;
            meta_fields.info.sample_count = whole / bps;
            meta_fields.info.epoch_anchor_ns = side->anchor_ns;
            meta_fields.info.rate = side->rate;
            meta_fields.info.center = fields->center;
            meta_fields.info.format = side->format;
            meta_fields.info.dropped_before = side->dropped_before;
            meta_fields.close_reason = "recovered";
            meta_fields.hw = side->hw;
            meta_fields.gain_db = side->gain_db;
            if (auto wrote = write_text(meta, render_meta(meta_fields)); !wrote) {
                failure(wrote.error().message);
                continue;
            }
        }
        fs::rename(open_data, data_path(dir, stem), ec);
        if (ec) {
            failure(std::format("rename: {}", ec.message()));
            continue;
        }
        fs::remove(side_path(dir, stem), ec);
        ++report.recovered;
    }
    return report;
}

struct SegmentWriter::OpenSegment {
    SegmentInfo info;
    fs::path dir;
    std::string stem;
    std::optional<double> gain_db;
    std::ofstream out;
    std::uint64_t bytes = 0;
};

SegmentWriter::SegmentWriter(WriterConfig config) : config_(std::move(config))
{
    if (!config_.now_ns) {
        config_.now_ns = [] {
            return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
        };
    }
    if (config_.queue_blocks == 0) {
        config_.queue_blocks = 1;
    }
}

SegmentWriter::~SegmentWriter()
{
    stop();
}

std::int64_t SegmentWriter::now() const
{
    return config_.now_ns();
}

Status SegmentWriter::start()
{
    {
        std::lock_guard lock(mutex_);
        if (running_) {
            return fail("SegmentWriter::start called on a writer that is already running");
        }
    }
    std::error_code ec;
    fs::create_directories(config_.root, ec);
    if (ec) {
        return fail(std::format("cannot create capture root '{}': {}", path_text(config_.root),
                                ec.message()));
    }
    if (config_.recover_on_start) {
        auto recovered = recover_open_segments(config_.root);
        if (!recovered) {
            return std::unexpected(with_context(recovered.error(), "recovering open segments"));
        }
    }
    if (config_.stream_epoch == 0) {
        config_.stream_epoch = draw_epoch();
    }
    index_existing();
    // A restart re-indexes from disk, which already sees any pair a refused
    // delete left behind.
    pending_delete_.clear();
    have_last_ = false;

    std::lock_guard lock(mutex_);
    stats_.retained_bytes = 0;
    stats_.pending_deletes = 0;
    for (const RetainedSegment& segment : retained_) {
        stats_.retained_bytes += segment.bytes;
    }
    errored_ = false;
    stats_.last_error.clear();
    stopping_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return {};
}

void SegmentWriter::stop()
{
    {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return;
        }
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    running_ = false;
    stopping_ = false;
}

void SegmentWriter::consume(const source::SourceBlock& block, dsp::Hertz center)
{
    std::vector<std::byte> buffer;
    {
        std::lock_guard lock(mutex_);
        if (!running_ || stopping_) {
            return;
        }
        if (errored_) {
            ++stats_.blocks_unwritten;
            return;
        }
        if (queue_.size() + buffers_out_ >= config_.queue_blocks) {
            ++stats_.blocks_dropped;
            stats_.samples_dropped += block.sample_count;
            pending_loss_ = true;
            pending_loss_samples_ += block.dropped_before + block.sample_count;
            return;
        }
        if (!pool_.empty()) {
            buffer = std::move(pool_.back());
            pool_.pop_back();
        }
        ++buffers_out_;
    }

    // The copy is outside the lock so the writer thread is never held up
    // behind a block-sized memcpy, and the buffer is reused so the steady
    // state allocates nothing.
    buffer.resize(block.bytes.size());
    if (!block.bytes.empty()) {
        std::memcpy(buffer.data(), block.bytes.data(), block.bytes.size());
    }

    Item item;
    item.stamp = block.stamp;
    item.format = block.format;
    item.sample_count = block.sample_count;
    item.center = center;
    item.bytes = std::move(buffer);

    {
        std::lock_guard lock(mutex_);
        --buffers_out_;
        item.loss = pending_loss_ || block.dropped_before > 0;
        item.lost_before = pending_loss_samples_ + block.dropped_before;
        pending_loss_ = false;
        pending_loss_samples_ = 0;
        queue_.push_back(std::move(item));
    }
    wake_.notify_one();
}

source::BlockSink SegmentWriter::sink(std::function<dsp::Hertz()> center)
{
    return [this, center = std::move(center)](const source::SourceBlock& block) -> Status {
        consume(block, center ? center() : 0);
        return {};
    };
}

void SegmentWriter::set_gain_db(std::optional<double> gain_db)
{
    std::lock_guard lock(mutex_);
    gain_db_ = gain_db;
}

void SegmentWriter::add_annotation(dsp::SampleIndex absolute_start, const Annotation& annotation,
                                   std::optional<std::uint64_t> track_id)
{
    std::lock_guard lock(mutex_);
    annotations_.push_back(PendingAnnotation{absolute_start, annotation, track_id});
}

WriterStats SegmentWriter::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

void SegmentWriter::run()
{
    for (;;) {
        Item item;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }
        process(item);
        std::lock_guard lock(mutex_);
        pool_.push_back(std::move(item.bytes));
    }
    if (open_) {
        close_segment("stop");
    }
}

void SegmentWriter::fail_writing(std::string message)
{
    // The open data file and its side file are left where they are, so the
    // next start's recovery pass finalises whatever reached the disk.
    if (open_) {
        open_->out.close();
        open_.reset();
    }
    std::lock_guard lock(mutex_);
    errored_ = true;
    stats_.last_error = std::move(message);
}

void SegmentWriter::process(Item& item)
{
    {
        std::lock_guard lock(mutex_);
        if (errored_) {
            ++stats_.blocks_unwritten;
            return;
        }
    }

    const dsp::SampleIndex start = item.stamp.start;
    bool restarted = false;
    if (open_) {
        const SegmentInfo& info = open_->info;
        std::string_view reason;
        if (item.loss) {
            reason = "loss";
        } else if (start > info.end_index()) {
            reason = "loss";
        } else if (start < info.end_index()) {
            // The index went backwards, which is a new run of the source and
            // not a gap. Indices are not comparable across runs, so it gets a
            // new epoch.
            reason = "stop";
            restarted = true;
        } else if (item.format != info.format) {
            reason = "format";
        } else if (item.stamp.rate != info.rate) {
            reason = "rate";
        } else if (item.center != info.center) {
            reason = "retune";
        } else if (config_.max_segment_bytes != 0 &&
                   open_->bytes + item.bytes.size() > config_.max_segment_bytes) {
            reason = "size";
        } else if (config_.max_segment_seconds > 0.0 && info.rate > 0 &&
                   static_cast<double>(info.sample_count) >=
                       config_.max_segment_seconds * static_cast<double>(info.rate)) {
            reason = "size";
        }
        if (!reason.empty()) {
            close_segment(reason);
        }
    } else if (have_last_ && start < last_end_) {
        restarted = true;
    }
    if (restarted) {
        config_.stream_epoch = draw_epoch();
        have_last_ = false;
    }

    if (!open_) {
        std::uint64_t dropped = 0;
        if (have_last_ && start > last_end_) {
            dropped = start - last_end_;
        } else if (item.loss) {
            dropped = item.lost_before;
        }
        open_segment(item, dropped);
        if (!open_) {
            return;
        }
    }

    open_->out.write(reinterpret_cast<const char*>(item.bytes.data()),
                     static_cast<std::streamsize>(item.bytes.size()));
    if (!open_->out) {
        fail_writing(std::format("writing '{}' failed, most likely a full disk; capture stopped",
                                 path_text(open_data_path(open_->dir, open_->stem))));
        return;
    }
    open_->bytes += item.bytes.size();
    open_->info.sample_count += item.sample_count;
    last_end_ = open_->info.end_index();
    have_last_ = true;

    std::lock_guard lock(mutex_);
    stats_.bytes_written += item.bytes.size();
}

void SegmentWriter::open_segment(const Item& item, std::uint64_t dropped_before)
{
    auto segment = std::make_unique<OpenSegment>();
    segment->info.stream_epoch = config_.stream_epoch;
    segment->info.first_index = item.stamp.start;
    segment->info.epoch_anchor_ns = item.stamp.epoch_anchor_ns;
    segment->info.rate = item.stamp.rate;
    segment->info.center = item.center;
    segment->info.format = item.format;
    segment->info.dropped_before = dropped_before;
    {
        std::lock_guard lock(mutex_);
        segment->gain_db = gain_db_;
    }
    segment->stem = make_stem(segment->info);
    segment->dir = config_.root / day_directory(segment->info.start_ns());

    std::error_code ec;
    fs::create_directories(segment->dir, ec);
    if (ec) {
        fail_writing(std::format("cannot create '{}': {}", path_text(segment->dir), ec.message()));
        return;
    }

    // The side file goes down before the first sample, so there is never a
    // data file recovery cannot interpret.
    SideFile side;
    side.rate = segment->info.rate;
    side.format = segment->info.format;
    side.anchor_ns = segment->info.epoch_anchor_ns;
    side.dropped_before = dropped_before;
    side.gain_db = segment->gain_db;
    side.hw = config_.hw;
    if (auto wrote = write_text(side_path(segment->dir, segment->stem), render_side(side));
        !wrote) {
        fail_writing(wrote.error().message);
        return;
    }

    segment->out.open(open_data_path(segment->dir, segment->stem),
                      std::ios::binary | std::ios::trunc);
    if (!segment->out) {
        fail_writing(std::format("cannot create '{}'",
                                 path_text(open_data_path(segment->dir, segment->stem))));
        return;
    }
    open_ = std::move(segment);
}

void SegmentWriter::close_segment(std::string_view reason)
{
    std::unique_ptr<OpenSegment> segment = std::move(open_);
    segment->out.flush();
    const bool good = static_cast<bool>(segment->out);
    segment->out.close();
    const fs::path open_data = open_data_path(segment->dir, segment->stem);
    if (!good) {
        open_ = std::move(segment);
        fail_writing(std::format("closing '{}' failed, most likely a full disk; capture stopped",
                                 path_text(open_data)));
        return;
    }

    std::error_code ec;
    if (segment->info.sample_count == 0) {
        fs::remove(open_data, ec);
        fs::remove(side_path(segment->dir, segment->stem), ec);
        return;
    }

    MetaFields fields;
    fields.info = segment->info;
    fields.close_reason = std::string(reason);
    fields.hw = config_.hw;
    fields.gain_db = segment->gain_db;
    {
        const dsp::SampleIndex first = segment->info.first_index;
        const dsp::SampleIndex end = segment->info.end_index();
        std::lock_guard lock(mutex_);
        std::vector<PendingAnnotation> keep;
        for (PendingAnnotation& pending : annotations_) {
            if (pending.absolute_start >= end) {
                keep.push_back(std::move(pending));
            } else if (pending.absolute_start < first) {
                ++stats_.annotations_discarded;
            } else {
                MetaFields::Entry entry;
                entry.annotation = std::move(pending.annotation);
                entry.annotation.sample_start = pending.absolute_start - first;
                entry.annotation.sample_count =
                    std::min<std::uint64_t>(entry.annotation.sample_count,
                                            end - pending.absolute_start);
                entry.track_id = pending.track_id;
                fields.annotations.push_back(std::move(entry));
            }
        }
        annotations_ = std::move(keep);
    }
    std::ranges::sort(fields.annotations, {}, [](const MetaFields::Entry& e) {
        return e.annotation.sample_start;
    });

    const fs::path meta = meta_path(segment->dir, segment->stem);
    if (auto wrote = write_text(meta, render_meta(fields)); !wrote) {
        fs::remove(meta, ec);
        open_ = std::move(segment);
        fail_writing(wrote.error().message);
        return;
    }
    const fs::path data = data_path(segment->dir, segment->stem);
    fs::rename(open_data, data, ec);
    if (ec) {
        fail_writing(std::format("renaming '{}': {}", path_text(open_data), ec.message()));
        return;
    }
    fs::remove(side_path(segment->dir, segment->stem), ec);

    const std::uint64_t meta_bytes = fs::file_size(meta, ec);
    RetainedSegment retained;
    retained.data = data;
    retained.meta = meta;
    retained.bytes = segment->bytes + (ec ? 0 : meta_bytes);
    retained.start_ns = segment->info.start_ns();
    retained.order_key = path_text(segment->dir.filename()) + "/" + segment->stem;
    retained_.push_back(std::move(retained));
    {
        std::lock_guard lock(mutex_);
        ++stats_.segments_written;
        stats_.retained_bytes += retained_.back().bytes;
    }
    apply_retention();
}

void SegmentWriter::index_existing()
{
    retained_.clear();
    std::error_code ec;
    for (fs::directory_iterator days(config_.root, ec), end; !ec && days != end;
         days.increment(ec)) {
        if (!days->is_directory(ec)) {
            continue;
        }
        std::error_code inner;
        for (fs::directory_iterator it(days->path(), inner), done; !inner && it != done;
             it.increment(inner)) {
            const std::string name = path_text(it->path().filename());
            if (!name.ends_with(kMetaExtension)) {
                continue;
            }
            const std::string stem = name.substr(0, name.size() - kMetaExtension.size());
            auto fields = parse_stem(stem);
            if (!fields) {
                continue;
            }
            const fs::path data = data_path(days->path(), stem);
            std::error_code size_ec;
            const std::uint64_t data_bytes = fs::file_size(data, size_ec);
            if (size_ec) {
                continue;
            }
            const std::uint64_t meta_bytes = fs::file_size(it->path(), size_ec);
            RetainedSegment retained;
            retained.data = data;
            retained.meta = it->path();
            retained.bytes = data_bytes + (size_ec ? 0 : meta_bytes);
            retained.start_ns = fields->start_ns;
            retained.order_key = path_text(days->path().filename()) + "/" + stem;
            retained_.push_back(std::move(retained));
        }
    }
    std::ranges::sort(retained_, {}, &RetainedSegment::order_key);
}

bool SegmentWriter::remove_pair(const RetainedSegment& segment)
{
    // Meta first: if the data then refuses to go, what is left is a data file
    // with no meta, which the catalog reports as orphaned and never offers
    // for replay. The other order would leave a meta naming missing data,
    // which the catalog has to call a problem.
    std::error_code ec;
    fs::remove(segment.meta, ec);
    if (ec && fs::exists(segment.meta)) {
        return false;
    }
    ec.clear();
    fs::remove(segment.data, ec);
    if (ec && fs::exists(segment.data)) {
        return false;
    }
    const fs::path dir = segment.data.parent_path();
    ec.clear();
    if (fs::is_empty(dir, ec) && !ec) {
        fs::remove(dir, ec);
    }
    return true;
}

void SegmentWriter::retry_pending_deletes()
{
    std::uint64_t freed = 0;
    std::uint64_t done = 0;
    std::erase_if(pending_delete_, [&](const RetainedSegment& segment) {
        if (!remove_pair(segment)) {
            return false;
        }
        freed += segment.bytes;
        ++done;
        return true;
    });
    std::lock_guard lock(mutex_);
    stats_.segments_deleted += done;
    stats_.retained_bytes -= std::min(stats_.retained_bytes, freed);
    stats_.pending_deletes = pending_delete_.size();
}

void SegmentWriter::apply_retention()
{
    if (!pending_delete_.empty()) {
        retry_pending_deletes();
    }
    if (config_.retention_bytes == 0 && config_.retention_max_age_ns <= 0) {
        return;
    }
    std::ranges::sort(retained_, {}, &RetainedSegment::order_key);

    // A pair still waiting on a refused delete occupies disk, so it counts
    // against the budget; the ring takes the next oldest to make up for it.
    std::uint64_t total = 0;
    for (const RetainedSegment& segment : retained_) {
        total += segment.bytes;
    }
    for (const RetainedSegment& segment : pending_delete_) {
        total += segment.bytes;
    }
    if (open_) {
        total += open_->bytes;
    }
    const std::int64_t now_ns = now();

    std::size_t taken = 0;
    std::uint64_t deleted = 0;
    std::uint64_t deleted_bytes = 0;
    std::uint64_t refused = 0;
    while (taken < retained_.size()) {
        RetainedSegment& oldest = retained_[taken];
        const bool over_budget = config_.retention_bytes != 0 && total > config_.retention_bytes;
        const bool too_old = config_.retention_max_age_ns > 0 &&
                             now_ns - oldest.start_ns > config_.retention_max_age_ns;
        if (!over_budget && !too_old) {
            break;
        }
        if (remove_pair(oldest)) {
            total -= std::min(total, oldest.bytes);
            deleted_bytes += oldest.bytes;
            ++deleted;
        } else {
            ++refused;
            pending_delete_.push_back(std::move(oldest));
        }
        ++taken;
    }
    if (taken == 0) {
        return;
    }
    retained_.erase(retained_.begin(), retained_.begin() + static_cast<std::ptrdiff_t>(taken));
    std::lock_guard lock(mutex_);
    stats_.segments_deleted += deleted;
    stats_.delete_failures += refused;
    stats_.pending_deletes = pending_delete_.size();
    stats_.retained_bytes -= std::min(stats_.retained_bytes, deleted_bytes);
}

}  // namespace revenant::capture
