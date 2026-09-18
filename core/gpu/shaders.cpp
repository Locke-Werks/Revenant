#include "core/gpu/shaders.h"

// The generated headers declare a plain `const uint32_t name[]`, so cstdint has
// to be in scope before them. They are included here and nowhere else.
#include <cstdint>

#include "shaders/identity_comp.h"
#include "shaders/cmul_comp.h"
#include "shaders/pfb_branch_comp.h"
#include "shaders/pfb_fft_comp.h"

namespace revenant::gpu::shaders {

std::span<const std::uint32_t> identity() {
    return std::span<const std::uint32_t>(identity_comp_spv, std::size(identity_comp_spv));
}

std::span<const std::uint32_t> cmul() {
    return std::span<const std::uint32_t>(cmul_comp_spv, std::size(cmul_comp_spv));
}

std::span<const std::uint32_t> pfb_branch() {
    return std::span<const std::uint32_t>(pfb_branch_comp_spv, std::size(pfb_branch_comp_spv));
}

std::span<const std::uint32_t> pfb_fft() {
    return std::span<const std::uint32_t>(pfb_fft_comp_spv, std::size(pfb_fft_comp_spv));
}

}  // namespace revenant::gpu::shaders
