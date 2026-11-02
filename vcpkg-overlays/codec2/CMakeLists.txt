# libcodec2, the vocoder alone, for the codec2 overlay port.
#
# Upstream's src/CMakeLists.txt builds the vocoder and every FreeDV modem into
# one library. The modems need C99 complex arithmetic, which MSVC does not
# compile, and Revenant links Codec 2 for the vocoder only: docs/modes.md,
# "Digital voice, collected". This file replaces upstream's build, so the
# library holds the speech codec, its FFT and its generated codebooks and
# nothing else. It is a build file, not part of Revenant's source tree.

cmake_minimum_required(VERSION 3.20)
project(codec2 LANGUAGES C VERSION 1.2.0)

include(GNUInstallDirs)

set(CODEC2_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CODEC2_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CODEC2_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CODEC2_VERSION ${PROJECT_VERSION})
configure_file(cmake/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/codec2/version.h")

set(D ${CMAKE_CURRENT_SOURCE_DIR}/src/codebook)

add_executable(generate_codebook src/generate_codebook.c)
if(MSVC)
    target_compile_definitions(generate_codebook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Each generated codebook: output name, table name, input files. The same
# table names and inputs upstream's src/CMakeLists.txt uses.
set(GENERATED)
function(codebook out name)
    set(files)
    foreach(f ${ARGN})
        list(APPEND files ${D}/${f})
    endforeach()
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${out}.c
        COMMAND generate_codebook ${name} ${files} > ${CMAKE_CURRENT_BINARY_DIR}/${out}.c
        DEPENDS generate_codebook ${files})
    set(GENERATED ${GENERATED} ${CMAKE_CURRENT_BINARY_DIR}/${out}.c PARENT_SCOPE)
endfunction()

codebook(codebook lsp_cb lsp1.txt lsp2.txt lsp3.txt lsp4.txt lsp5.txt lsp6.txt lsp7.txt lsp8.txt lsp9.txt lsp10.txt)
codebook(codebookd lsp_cbd dlsp1.txt dlsp2.txt dlsp3.txt dlsp4.txt dlsp5.txt dlsp6.txt dlsp7.txt dlsp8.txt dlsp9.txt dlsp10.txt)
codebook(codebookjmv lsp_cbjmv lspjmv1.txt lspjmv2.txt lspjmv3.txt)
codebook(codebookge ge_cb gecb.txt)
codebook(codebooknewamp1 newamp1vq_cb train_120_1.txt train_120_2.txt)
codebook(codebooknewamp1_energy newamp1_energy_cb newamp1_energy_q.txt)
codebook(codebooknewamp2 newamp2vq_cb codes_450.txt)
codebook(codebooknewamp2_energy newamp2_energy_cb newamp2_energy_q.txt)

add_library(codec2
    src/dump.c src/lpc.c src/nlp.c src/postfilter.c src/sine.c src/codec2.c
    src/codec2_fft.c src/kiss_fft.c src/kiss_fftr.c src/interp.c src/lsp.c
    src/mbest.c src/newamp1.c src/phase.c src/quantise.c src/pack.c
    ${GENERATED})
target_include_directories(codec2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(MSVC)
    target_compile_definitions(codec2 PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
endif()
if(UNIX)
    target_link_libraries(codec2 PRIVATE m)
endif()

install(TARGETS codec2 EXPORT codec2Targets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES src/codec2.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/codec2)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/codec2/version.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/codec2)
install(EXPORT codec2Targets NAMESPACE codec2:: FILE codec2-config.cmake DESTINATION share/codec2)
