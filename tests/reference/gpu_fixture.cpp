#include "tests/reference/gpu_fixture.h"

#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>

namespace revenant::test {
namespace {

struct Shared {
    std::optional<gpu::Context> context;
    std::string failure;
    bool attempted = false;
};

Shared& shared() {
    static Shared state;
    return state;
}

// Created once, on first use, and never retried. A device that failed to open
// will fail the same way for every subsequent case, and retrying turns one
// clear message into one per test.
void attempt_once() {
    Shared& state = shared();
    if (state.attempted) {
        return;
    }
    state.attempted = true;

    auto created = gpu::Context::create();
    if (!created) {
        state.failure = created.error().message;
        return;
    }
    state.context.emplace(std::move(*created));
}

bool env_flag_set(const char* name) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') {
        return false;
    }
    // Anything but an explicit off. A variable that is set at all is set for a
    // reason, and treating "0" as on would surprise someone trying to disable
    // it for one run.
    return !(raw[0] == '0' && raw[1] == '\0');
}

}  // namespace

bool gpu_available() {
    attempt_once();
    return shared().context.has_value();
}

const std::string& gpu_unavailable_reason() {
    attempt_once();
    return shared().failure;
}

gpu::Context& shared_context() {
    attempt_once();
    if (!shared().context.has_value()) {
        // Throwing rather than aborting. Catch2 turns this into a reported
        // failure for the one case; abort() would take the process down and
        // lose every other result, and on Windows would do it behind a modal
        // dialog. Cases should call REVENANT_NEEDS_GPU() first and never reach
        // this.
        throw std::runtime_error("shared_context() called with no Vulkan device: " +
                                 shared().failure);
    }
    return *shared().context;
}

std::string shared_context_description() {
    if (!gpu_available()) {
        return "no Vulkan device (" + shared().failure + ")";
    }
    return shared().context->info().describe();
}

bool gpu_is_required() { return env_flag_set("REVENANT_REQUIRE_GPU"); }

}  // namespace revenant::test
