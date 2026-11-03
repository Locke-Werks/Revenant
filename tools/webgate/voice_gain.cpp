#include "tools/webgate/voice_gain.h"

#include <charconv>
#include <utility>

namespace revenant::webgate {

double parse_voice_gain_db(std::optional<std::string_view> text)
{
    if (!text) {
        return 0.0;
    }
    double value = 0.0;
    const char* end = text->data() + text->size();
    const auto [at, error] = std::from_chars(text->data(), end, value);
    if (error != std::errc{} || at != end) {
        return 0.0;
    }
    return ui::clamp_voice_gain_db(value);
}

double read_voice_gain_db(const DisplayValueReader& read)
{
    if (!read) {
        return 0.0;
    }
    const auto text = read("voiceGainDb");
    return parse_voice_gain_db(text ? std::optional<std::string_view>(*text) : std::nullopt);
}

DisplayValueReader registry_audio_reader()
{
    return registry_key_reader("audio");
}

VoiceGainSource::VoiceGainSource(DisplayValueReader reader,
                                 std::function<Clock::time_point()> clock,
                                 std::chrono::milliseconds interval)
    : reader_(std::move(reader)), clock_(std::move(clock)), interval_(interval)
{
}

double VoiceGainSource::current()
{
    const auto now = clock_ ? clock_() : Clock::now();
    const std::lock_guard lock(mutex_);
    if (!read_at_ || now - *read_at_ >= interval_) {
        db_ = read_voice_gain_db(reader_);
        read_at_ = now;
    }
    return db_;
}

}  // namespace revenant::webgate
