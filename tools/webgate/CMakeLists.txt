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
