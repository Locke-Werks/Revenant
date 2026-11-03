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
    audio_opus.cpp
    protocol.cpp
    session.cpp
    dispatch.cpp
    phone_streams.cpp
    phone_hub.cpp
    gateway.cpp
)

target_include_directories(revenant_webgate_lib PUBLIC "${CMAKE_SOURCE_DIR}")

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
# Two commands keyed on different inputs: npm ci reruns only when the lockfile
# changes, the bundle only when a source does, so an ordinary build touches
# neither.
find_program(REVENANT_NPM npm)
find_program(REVENANT_NODE node)
if(REVENANT_NPM AND REVENANT_NODE)
    set(WEB_DIR "${CMAKE_SOURCE_DIR}/web")
    file(GLOB_RECURSE WEB_SOURCES CONFIGURE_DEPENDS
        "${WEB_DIR}/src/*" "${WEB_DIR}/static/*")
    set(WEB_DEPS_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-npm-ci.stamp")
    set(WEB_BUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/web-build.stamp")

    add_custom_command(
        OUTPUT "${WEB_DEPS_STAMP}"
        COMMAND "${REVENANT_NPM}" ci
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_DEPS_STAMP}"
        DEPENDS "${WEB_DIR}/package.json" "${WEB_DIR}/package-lock.json"
        WORKING_DIRECTORY "${WEB_DIR}"
        COMMENT "web: npm ci"
        VERBATIM)

    add_custom_command(
        OUTPUT "${WEB_BUILD_STAMP}"
        COMMAND "${REVENANT_NODE}" build.mjs
        COMMAND "${CMAKE_COMMAND}" -E remove_directory "$<TARGET_FILE_DIR:revenant-webgate>/web"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${WEB_DIR}/dist"
                "$<TARGET_FILE_DIR:revenant-webgate>/web"
        COMMAND "${CMAKE_COMMAND}" -E touch "${WEB_BUILD_STAMP}"
        DEPENDS "${WEB_DEPS_STAMP}" ${WEB_SOURCES}
                "${WEB_DIR}/build.mjs" "${WEB_DIR}/tsconfig.json"
        WORKING_DIRECTORY "${WEB_DIR}"
        COMMENT "web: bundle and copy beside revenant-webgate"
        VERBATIM)

    add_custom_target(revenant_web ALL DEPENDS "${WEB_BUILD_STAMP}")
    add_dependencies(revenant_web revenant-webgate)
else()
    message(STATUS "npm or node not found: skipping the revenant_web target (phone web app not built)")
endif()
