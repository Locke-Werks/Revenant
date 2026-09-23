// A decoded-message subscriber for the wire cases, and the field readers they
// assert with.
//
// Shared by tests/rpc/test_rpc_decode.cpp, which feeds the complex decoders,
// and tests/rpc/test_rpc_decode_audio.cpp, which feeds the audio ones. The two
// read the same DecodedMessage off the same subscription and a second copy of
// the log would be a second place for the locking to be got wrong.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/rpc/client.h"
#include "core/rpc/types.h"

namespace revenant::test {

// Long enough for the last message to cross the socket after the engine has
// finished the file on a loaded machine.
inline constexpr int kDecodedWaitMs = 10'000;

// What arrived, safe to read from the test thread while the client's loop is
// still delivering.
class MessageLog {
public:
    void record(const rpc::DecodedMessage& message) {
        const std::lock_guard<std::mutex> held(lock_);
        messages_.push_back(message);
    }
    void end(const std::string& reason) {
        const std::lock_guard<std::mutex> held(lock_);
        ended_ = true;
        reason_ = reason;
    }
    [[nodiscard]] std::vector<rpc::DecodedMessage> messages() const {
        const std::lock_guard<std::mutex> held(lock_);
        return messages_;
    }
    [[nodiscard]] bool ended() const {
        const std::lock_guard<std::mutex> held(lock_);
        return ended_;
    }
    [[nodiscard]] std::string reason() const {
        const std::lock_guard<std::mutex> held(lock_);
        return reason_;
    }

private:
    mutable std::mutex lock_;
    std::vector<rpc::DecodedMessage> messages_;
    bool ended_ = false;
    std::string reason_;
};

[[nodiscard]] inline rpc::Client::DecodedCallback into(std::shared_ptr<MessageLog> log) {
    return [log](const rpc::DecodedMessage& message) { log->record(message); };
}
[[nodiscard]] inline rpc::Client::DecodedEndedCallback ending(std::shared_ptr<MessageLog> log) {
    return [log](const std::string& reason) { log->end(reason); };
}

// Polls the log until `want` is satisfied or the wait runs out, and hands back
// the last list either way.
template <typename Predicate>
[[nodiscard]] std::vector<rpc::DecodedMessage> wait_for(const MessageLog& log, Predicate want,
                                                        int wait_ms = kDecodedWaitMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    for (;;) {
        auto seen = log.messages();
        if (want(seen) || std::chrono::steady_clock::now() >= deadline) {
            return seen;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

[[nodiscard]] inline std::int64_t integer_of(const rpc::DecodedMessage& message,
                                             std::string_view key) {
    const rpc::DecodedField* field = message.field(key);
    INFO("field " << key << " of a " << message.decoder << " " << message.kind);
    REQUIRE(field != nullptr);
    const std::int64_t* value = field->integer();
    REQUIRE(value != nullptr);
    return *value;
}

[[nodiscard]] inline double real_of(const rpc::DecodedMessage& message, std::string_view key) {
    const rpc::DecodedField* field = message.field(key);
    INFO("field " << key << " of a " << message.decoder << " " << message.kind);
    REQUIRE(field != nullptr);
    const double* value = field->real();
    REQUIRE(value != nullptr);
    return *value;
}

[[nodiscard]] inline bool flag_of(const rpc::DecodedMessage& message, std::string_view key) {
    const rpc::DecodedField* field = message.field(key);
    INFO("field " << key << " of a " << message.decoder << " " << message.kind);
    REQUIRE(field != nullptr);
    const bool* value = field->flag();
    REQUIRE(value != nullptr);
    return *value;
}

[[nodiscard]] inline std::string text_of(const rpc::DecodedMessage& message,
                                         std::string_view key) {
    const rpc::DecodedField* field = message.field(key);
    INFO("field " << key << " of a " << message.decoder << " " << message.kind);
    REQUIRE(field != nullptr);
    const std::string* value = field->text();
    REQUIRE(value != nullptr);
    return *value;
}

}  // namespace revenant::test
