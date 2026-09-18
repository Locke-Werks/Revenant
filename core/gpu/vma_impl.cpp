// The single translation unit that instantiates the Vulkan Memory Allocator.
//
// VMA is a large single-header library and does not compile clean at /W4. Its
// warnings are isolated here rather than suppressed project-wide, so the rest
// of the tree keeps the full warning set and CI keeps /WX.

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)  // unreferenced formal parameter
#pragma warning(disable : 4127)  // conditional expression is constant
#pragma warning(disable : 4189)  // local variable initialised but not referenced
#pragma warning(disable : 4324)  // structure padded due to alignment specifier
#pragma warning(disable : 4505)  // unreferenced local function removed
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
