# Revenant's overlay of vcpkg's ggml port, which vcpkg-configuration.json puts
# ahead of the registry. ggml is the tensor library under whisper.cpp, which
# transcribes speech on the engine's receivers; core/transcribe/ is the only
# code that reaches it.
#
# The five patches are the registry's port at the baseline d3a1e8c, unchanged.
# Three things differ: the source, and two that are about what ships rather
# than what compiles.
#
# 0. THE SOURCE IS THE ggml WHISPER.CPP 1.8.3 WAS BUILT AGAINST, NOT THE
#    BASELINE'S. The baseline pairs whisper-cpp 1.8.3 with ggml 2025-11-17,
#    and the two disagree about the attention mask. ggml of 2025-11-17 still
#    requires the KQ mask padded to GGML_KQ_MASK_PAD, 64; whisper.cpp 1.8.3 was
#    synced to a ggml that had dropped the padding and no longer pads it. On
#    2026-10-03 the first transcription through that pair died in
#    ggml_flash_attn_ext on "GGML_ASSERT(mask->ne[1] >= GGML_PAD(q->ne[1],
#    GGML_KQ_MASK_PAD) ...)". whisper.cpp records the ggml it was synced to in
#    scripts/sync-ggml.last, and at v1.8.3 that is ggml-org/ggml b6d1f0f of
#    2026-01-13, version 0.9.5. That commit's ggml.h, ggml-backend.h, ggml.c
#    and ggml-vulkan.cpp are byte-identical to the copies vendored in
#    whisper.cpp v1.8.3's ggml/ directory, compared the same day, and all five
#    patches apply to it unchanged. Retire this point when the baseline moves
#    to a whisper-cpp and ggml that agree: the registry's master carried
#    whisper-cpp 1.8.6 and ggml 0.11.1 on 2026-10-03.
#
# 1. THE CPU BASELINE IS PINNED. ggml's CMake defaults GGML_NATIVE to ON, which
#    tunes the CPU backend to the machine running the compiler. The release
#    build runs on a Ryzen 9 7950X, so a native build emits AVX-512, and the
#    installer then dies with an illegal instruction on every CPU without it.
#    The pin is the one OracleX carries (cmake/FetchDeps.cmake there): AVX,
#    AVX2, FMA and F16C on, AVX-512 off. Every CPU on Microsoft's supported
#    list for Windows 11 has AVX2. Under MSVC, GGML_AVX2 is /arch:AVX2 and
#    ggml-cpu's CMake defines __FMA__ and __F16C__ itself, so the FMA and F16C
#    switches state the intent rather than adding a flag. BMI2 is ggml's own
#    default and is written down anyway: it arrived with AVX2 in the same
#    generation on both vendors, Haswell and Excavator, so it narrows nothing
#    the AVX2 pin has not already narrowed. The configure log shows what took
#    effect, in the line "Adding CPU backend variant ggml-cpu: /arch:AVX2
#    GGML_AVX2;GGML_FMA;GGML_F16C;__BMI2__;GGML_BMI2" on 2026-10-03, and a
#    reader checking the pin should look there rather than here. The CPU
#    backend carries Whisper's sampling and whatever the Vulkan backend hands
#    back, so it is in every binary even when the GPU does the matrix work.
#
#    GGML_CPU_ALL_VARIANTS would be the other answer, one CPU backend per
#    instruction set picked at load time, and it needs GGML_BACKEND_DL: each
#    variant is a DLL. The engine ships no DLL of its own, so that answer is
#    not available here.
#
# 2. THE VULKAN FEATURE DEPENDS ON THE LUNARG SDK, NOT ON VCPKG'S LOADER. The
#    registry's vulkan feature pulls the "vulkan" stub, which pulls
#    vulkan-loader, and that port forces dynamic linkage whatever the triplet
#    says. It would put a vcpkg-built vulkan-1.dll in this static triplet's
#    bin directory, where the toolchain's app-local deployment copies it next
#    to every executable that imports vulkan-1.dll, the engine included, and
#    its vulkan-headers would sit ahead of the SDK's in find_package(Vulkan)
#    for the whole tree. The engine already builds against the SDK named in
#    docs/building.md and loads the system's vulkan-1.dll, so ggml does the
#    same: FindVulkan finds the SDK through VULKAN_SDK, which vcpkg passes
#    into port builds, and glslc comes from the same SDK as the engine's own
#    glslangValidator. shaderc goes with the loader, because the SDK's glslc
#    is what it was there to provide.
#
#    The patches are untouched by this. vulkan-shaders-gen.diff still builds
#    the shader generator inside this port, which is what a native build does.
#
# CUDA, Metal, OpenCL, BLAS and OpenMP stay features nobody asks for, so
# vcpkg_check_features passes OFF for each. OpenMP matters on its own: on by
# default in ggml's CMake, and under MSVC it is vcomp140.dll, a DLL the static
# CRT does not carry.
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ggml-org/ggml
    REF b6d1f0f247adcfa25c0ca1ffe97e651fe1afd5e2
    SHA512 2e78c89750c93b295fee28f3f2e7833cf18ac378e393f8970ed76afd7578b07281f216e6387f6ae559997c2acaddbd1e242648032f3120c0f3330462c57c38e3
    HEAD_REF master
    PATCHES
        cmake-config.diff
        pkgconfig.diff
        relax-link-options.diff
        vulkan-shaders-gen.diff
        fix-dequant_funcs.diff
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        blas     GGML_BLAS
        cuda     GGML_CUDA
        metal    GGML_METAL
        opencl   GGML_OPENCL
        openmp   GGML_OPENMP
        vulkan   GGML_VULKAN
)

if("blas" IN_LIST FEATURES)
    vcpkg_find_acquire_program(PKGCONFIG)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_REQUIRE_FIND_PACKAGE_BLAS=ON" # workaround message(ERROR ...)
        "-DPKG_CONFIG_EXECUTABLE=${PKGCONFIG}"
    )
endif()

if("cuda" IN_LIST FEATURES)
    vcpkg_find_cuda(OUT_CUDA_TOOLKIT_ROOT cuda_toolkit_root)
    list(APPEND FEATURE_OPTIONS
        "-DCMAKE_CUDA_COMPILER=${NVCC}"
        "-DCUDAToolkit_ROOT=${cuda_toolkit_root}"
    )
endif()

if("opencl" IN_LIST FEATURES)
    vcpkg_find_acquire_program(PYTHON3)
    list(APPEND FEATURE_OPTIONS
        "-DPython3_EXECUTABLE=${PYTHON3}"
    )
endif()

if(VCPKG_TARGET_IS_WINDOWS AND NOT VCPKG_TARGET_IS_MINGW AND VCPKG_TARGET_ARCHITECTURE STREQUAL "arm64")
    message(STATUS "The CPU backend is not supported for arm64 with MSVC.")
    list(APPEND FEATURE_OPTIONS
        "-DGGML_CPU=OFF"
    )
    if(FEATURES STREQUAL "core")
        message(WARNING "No backend enabled!")
    endif()
endif()

# The x86 baseline, point 1 above. Only on x64: the switches mean nothing to
# another architecture and ggml ignores them there.
if(VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
    list(APPEND FEATURE_OPTIONS
        -DGGML_NATIVE=OFF
        -DGGML_AVX=ON
        -DGGML_AVX2=ON
        -DGGML_FMA=ON
        -DGGML_F16C=ON
        -DGGML_BMI2=ON
        -DGGML_AVX512=OFF
        -DGGML_AVX512_VBMI=OFF
        -DGGML_AVX512_VNNI=OFF
        -DGGML_AVX512_BF16=OFF
        -DGGML_AVX_VNNI=OFF
        -DGGML_AMX_TILE=OFF
        -DGGML_AMX_INT8=OFF
        -DGGML_AMX_BF16=OFF
        -DGGML_CPU_ALL_VARIANTS=OFF
        -DGGML_BACKEND_DL=OFF
    )
endif()

if("vulkan" IN_LIST FEATURES)
    # Point 2 above. Said here rather than left to fail inside ggml's CMake,
    # where a missing SDK reads as "Could NOT find Vulkan" with no hint that
    # the environment variable is the thing to set.
    if(NOT DEFINED ENV{VULKAN_SDK} OR NOT EXISTS "$ENV{VULKAN_SDK}")
        message(FATAL_ERROR
            "ggml[vulkan] in Revenant's overlay builds against the LunarG Vulkan "
            "SDK, and VULKAN_SDK is not set or does not exist. Install the SDK "
            "docs/building.md names and open a new shell.")
    endif()
    file(TO_CMAKE_PATH "$ENV{VULKAN_SDK}" vulkan_sdk)
    list(APPEND FEATURE_OPTIONS
        "-DVulkan_INCLUDE_DIR=${vulkan_sdk}/Include"
        "-DVulkan_LIBRARY=${vulkan_sdk}/Lib/vulkan-1.lib"
        "-DVulkan_GLSLC_EXECUTABLE=${vulkan_sdk}/Bin/glslc.exe"
    )
    if(VCPKG_CROSSCOMPILING)
        list(APPEND FEATURE_OPTIONS
            "-DVULKAN_SHADERS_GEN_EXECUTABLE=${CURRENT_HOST_INSTALLED_DIR}/tools/${PORT}/vulkan-shaders-gen${VCPKG_HOST_EXECUTABLE_SUFFIX}"
        )
    endif()
endif()

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static"  GGML_STATIC)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DGGML_STATIC=${GGML_STATIC}
        -DGGML_CCACHE=OFF
        -DGGML_BUILD_NUMBER=1
        -DGGML_BUILD_TESTS=OFF
        -DGGML_BUILD_EXAMPLES=OFF
        -DGGML_HIP=OFF
        -DGGML_SYCL=OFF
        ${FEATURE_OPTIONS}
    MAYBE_UNUSED_VARIABLES
        PKG_CONFIG_EXECUTABLE
        GGML_AMX_TILE
        GGML_AMX_INT8
        GGML_AMX_BF16
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME ggml CONFIG_PATH "lib/cmake/ggml")

# Point 2 again, in the pkg-config file. pkgconfig.diff writes the Vulkan
# loader as "Requires.private: vulkan", naming the vulkan.pc that vcpkg's
# vulkan-loader installs, and vcpkg_fixup_pkgconfig refuses a file whose
# requirement it cannot find. The SDK installs no .pc, so the loader goes in as
# the library it is. Nothing in Revenant reads this file; CMake's config is
# what links. It is corrected so the port's own check passes on the truth
# rather than being switched off.
foreach(pc_file IN ITEMS
        "${CURRENT_PACKAGES_DIR}/lib/pkgconfig/ggml.pc"
        "${CURRENT_PACKAGES_DIR}/debug/lib/pkgconfig/ggml.pc")
    if(EXISTS "${pc_file}")
        file(READ "${pc_file}" pc_text)
        string(REGEX REPLACE "(Requires\\.private:[^\n]*) vulkan" "\\1" pc_text "${pc_text}")
        string(REGEX REPLACE "(Libs\\.private:[^\n]*)" "\\1 -lvulkan-1" pc_text "${pc_text}")
        file(WRITE "${pc_file}" "${pc_text}")
    endif()
endforeach()

vcpkg_fixup_pkgconfig()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ggml.h" "#ifdef GGML_SHARED" "#if 1")
    vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/ggml-backend.h" "#ifdef GGML_BACKEND_SHARED" "#if 1")
endif()

if("vulkan" IN_LIST FEATURES AND NOT VCPKG_CROSSCOMPILING)
    vcpkg_copy_tools(TOOL_NAMES vulkan-shaders-gen AUTO_CLEAN)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
