# The engine. No Qt anywhere in this tree, enforced by the headless preset and
# by the source scan in the root CMakeLists.

add_library(revenant_core STATIC
    dsp/complex_ops.cpp
    dsp/denormal_mode.cpp
    gpu/buffer.cpp
    gpu/context.cpp
    gpu/kernel.cpp
    gpu/shaders.cpp
    gpu/vma_impl.cpp
)

target_include_directories(revenant_core PUBLIC "${CMAKE_SOURCE_DIR}")

target_link_libraries(revenant_core PUBLIC
    Vulkan::Vulkan
    GPUOpen::VulkanMemoryAllocator
)

revenant_add_shaders(revenant_core
    TARGET_ENV vulkan1.3
    SHADERS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/identity.comp"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/cmul.comp"
)

revenant_apply_warnings(revenant_core)

# Applied to the whole engine, not only to the reference twins that strictly
# need it. An SDR engine has no business being built with fast math: a
# reassociated filter is a different filter. Making it explicit here means the
# setting is a decision on the record rather than a compiler default nobody
# checked.
revenant_apply_reference_fp(revenant_core)
