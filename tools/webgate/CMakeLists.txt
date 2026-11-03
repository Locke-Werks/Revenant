# The web gateway: an HTTPS and WebSocket front for the engine's Cap'n Proto
# session, so a phone on the LAN can drive the radio.
#
# The units live in a library so tests/webgate can link them without the
# executable. Later stages add their sources to this list.
#
# OpenSSL arrives through capnproto[openssl] in vcpkg.json (kj-tls links it),
# not as its own dependency line, so the version used for certificates is the
# one the TLS listener will use.

find_package(OpenSSL REQUIRED)
find_package(CapnProto CONFIG REQUIRED)
find_package(Opus CONFIG REQUIRED)

# listen.cpp is compiled in directly rather than linking revenant_rpc_server,
# which would pull the whole engine (core, transcribe) into a web gateway for
# the sake of one socket function.
add_library(revenant_webgate_lib STATIC
    "${CMAKE_SOURCE_DIR}/core/rpc/listen.cpp"
    certs.cpp
    static_files.cpp
    audio_hls.cpp
    hls_playlist.cpp
    http.cpp
    engine_link.cpp
    spectrum_reducer.cpp
    display_sync.cpp
    voice_gain.cpp
    audio_opus.cpp
    protocol.cpp
    session.cpp
    dispatch.cpp
    phone_streams.cpp
    phone_hub.cpp
    gateway.cpp
)

target_include_directories(revenant_webgate_lib PUBLIC "${CMAKE_SOURCE_DIR}")
# The desktop's scale rules, ui/render's Qt-free headers, which include each
# other as "render/...". No Qt and no ui source file is compiled in.
target_include_directories(revenant_webgate_lib PUBLIC "${CMAKE_SOURCE_DIR}/ui")

# revenant_rpc_client carries token.cpp, whose write_private_file is the one
# implementation of "readable by this account and SYSTEM only".
target_link_libraries(revenant_webgate_lib
    PUBLIC
        revenant_rpc_client
        revenant_launch
        OpenSSL::SSL
        OpenSSL::Crypto
        Opus::opus
        CapnProto::capnp-json
        CapnProto::kj-http
        CapnProto::kj-tls
)

# Windows SDK import libraries: GetAdaptersAddresses, inet_pton, and the
# CryptoAPI calls the static OpenSSL build pulls in.
if(WIN32)
    # mfplat/mfuuid/wmcodecdspuuid: the AAC encoder MFT used by the HLS path.
    target_link_libraries(revenant_webgate_lib PUBLIC iphlpapi ws2_32 crypt32
                          mfplat mfuuid wmcodecdspuuid ole32)
endif()

revenant_apply_warnings(revenant_webgate_lib)

add_executable(revenant-webgate main.cpp)
target_link_libraries(revenant-webgate PRIVATE revenant_webgate_lib)

revenant_apply_warnings(revenant-webgate)
revenant_embed_version(revenant-webgate DESCRIPTION "Revenant web gateway")

# The phone web app. Needs Node at configure time; without it the target is
# skipped so a machine that only builds the C++ (CI without Node) still
# configures. The gateway then runs without a web directory and serves 404s.
#
# Three steps keyed on different inputs, so an ordinary build repeats none of
# the expensive ones: npm ci reruns when the lockfile changes, the bundle when a
# source changes, and the copy beside the exe on every build of the target.
#
# The bundle goes to one directory per build tree, shared by the configurations
# of a multi-config generator, and its output file is the custom command's
# OUTPUT, so deleting it also reruns the bundle. The copy is deliberately not
# stamped: a stamp is shared across configurations, and a Release build after a
# Debug one would find it current and never populate the Release exe's web/.
find_program(REVENANT_NPM npm)
find_program(REVENANT_NODE node)
if(REVENANT_NPM AND REVENANT_NODE)
    set(WEB_DIR "${CMAKE_SOURCE_DIR}/web")
    set(WEB_OUT "${CMAKE_CURRENT_BINARY_DIR}/web-dist")
    file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS
        "${WEB_DIR}/src/*" "${WEB_DIR}/static/*")
    if(WIN32)
        set(WEB_NPM_RUN cmd /c "${REVENANT_NPM}")
    else()
        set(WEB_NPM_RUN "${REVENANT_NPM}")
    endif()
    set(WEB_DEPS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-npm-ci.stamp")

    add_custom_command(
        OUTPUT "${WEB_DEPS_STAMP}"
        # npm is npm.cmd on Windows. In the batch file the Visual Studio generator
        # writes, calling a .cmd without `call` ends the script, so the touch
        # below would never run; cmd /c is an executable and returns.
        COMMAND ${WEB_NPM_RUN} ci
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_DEPS_STAMP}"
        DEPENDS "${WEB_DIR}/package.json" "${WEB_DIR}/package-lock.json"
        WORKING_DIRECTORY "${WEB_DIR}"
        COMMENT "web: npm ci"
        VERBATIM)

    # sw.js is the last file build.mjs writes, so it stands for the whole
    # directory.
    add_custom_command(
        OUTPUT "${WEB_OUT}/sw.js"
        COMMAND "${REVENANT_NODE}" build.mjs "${WEB_OUT}"
        DEPENDS "${WEB_DEPS_STAMP}" ${WEB_SOURCES}
                "${WEB_DIR}/build.mjs" "${WEB_DIR}/tsconfig.json"
        WORKING_DIRECTORY "${WEB_DIR}"
        COMMENT "web: bundle"
        VERBATIM)

    add_custom_target(revenant_web ALL
        DEPENDS "${WEB_OUT}/sw.js"
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "$<TARGET_FILE_DIR:revenant-webgate>/web"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WEB_OUT}"
                "$<TARGET_FILE_DIR:revenant-webgate>/web"
        COMMENT "web: copy beside revenant-webgate"
        VERBATIM)
else()
    message(STATUS "npm or node not found: skipping the revenant_web target (phone web app not built)")
endif()
