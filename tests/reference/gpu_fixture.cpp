#include "tests/reference/gpu_fixture.h"

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace revenant::test {
namespace {

std::optional<gpu::Context>& storage() {
    static std::optional<gpu::Context> context;
    return context;
}

void ensure_created() {
    if (storage().has_value()) {
        return;
    }

    auto created = gpu::Context::create();
    if (!created) {
        std::fprintf(stderr,
                     "\nFATAL: could not create a Vulkan compute context.\n"
                     "  %s\n\n"
                     "Revenant is GPU-resident and this suite cannot be meaningfully run\n"
                     "without a Vulkan device. Check that a driver is installed and that\n"
                     "REVENANT_GPU_INDEX, if set, names a device that exists.\n",
                     created.error().message.c_str());
        std::abort();
    }

    storage().emplace(std::move(*created));
}

}  // namespace

gpu::Context& shared_context() {
    ensure_created();
    return *storage();
}

std::string shared_context_description() {
    ensure_created();
    return storage()->info().describe();
}

}  // namespace revenant::test
