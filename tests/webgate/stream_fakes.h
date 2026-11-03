// A strand the test thread pumps by hand, and a socket that records what it
// is handed and can be held busy. Shared by the stream and hub tests.

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

#include "tools/webgate/phone_streams.h"

namespace revenant::webgate::test {

class ManualStrand final : public Strand {
public:
    void post(std::function<void()> task) override
    {
        const std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }

    void run_pending()
    {
        for (;;) {
            std::deque<std::function<void()>> batch;
            {
                const std::lock_guard lock(mutex_);
                batch.swap(tasks_);
            }
            if (batch.empty()) {
                return;
            }
            for (auto& task : batch) {
                task();
            }
        }
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
};

// Records every message. While held, the message handed over stays "in
// flight": its done is kept until release().
class FakeSocket final : public PhoneSocket {
public:
protected:
    void do_send(OutgoingMessage message, std::function<void()> done) override
    {
        // A second send while one is outstanding is what kj refuses with
        // "another message send is already in progress".
        if (waiting) {
            concurrent_sends += 1;
        }
        sent.push_back(std::move(message));
        if (held) {
            waiting = std::move(done);
        } else {
            done();
        }
    }

public:
    void close() override { closed = true; }

    void hold() { held = true; }

    void release()
    {
        held = false;
        if (waiting) {
            auto done = std::move(waiting);
            waiting = nullptr;
            done();
        }
    }

    [[nodiscard]] std::size_t count(OutgoingMessage::Kind kind, std::size_t from = 0) const
    {
        std::size_t n = 0;
        for (std::size_t i = from; i < sent.size(); ++i) {
            n += sent[i].kind == kind ? 1U : 0U;
        }
        return n;
    }

    std::vector<OutgoingMessage> sent;
    bool held = false;
    bool closed = false;
    int concurrent_sends = 0;
    std::function<void()> waiting;
};

}  // namespace revenant::webgate::test
