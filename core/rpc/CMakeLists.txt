# The Cap'n Proto service, as two libraries rather than one.
#
# WHY TWO
#
# revenant_rpc_client links capnp and nothing of ours except the generated
# schema and core/rpc/types.h. revenant_rpc_server links the engine as well.
# The split is not tidiness: the Qt UI compiles the client's sources itself,
# in a separate CMake project, against the x64-windows triplet, because Qt is
# built for the dynamic CRT and revenant_core is /MT. A client that reached
# into the engine could not be compiled there at all, and the split is what
# keeps that a build error here instead of a heap corruption there.
#
# The consequence to respect: nothing in client.cpp, types.h or the schema
# may depend on core/engine or core/dsp. core/rpc/convert.* is the server's
# alone for exactly that reason.

find_package(CapnProto CONFIG REQUIRED)

# Generated into the build tree beside the schema, and included as
# "core/rpc/revenant.capnp.h" so it reads like every other header here.
set(CAPNPC_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}")
set(CAPNPC_SRC_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}")
capnp_generate_cpp(revenant_capnp_sources revenant_capnp_headers revenant.capnp)

# The generated translation unit is not ours and does not pass /W4. Scoped to
# that file so the hand-written sources stay held to the project's level.
if(MSVC)
    set_source_files_properties(${revenant_capnp_sources}
        PROPERTIES COMPILE_OPTIONS "/W0")
endif()

add_library(revenant_rpc_client STATIC
    ${revenant_capnp_sources}
    client.cpp
    token.cpp
)

target_link_libraries(revenant_rpc_client
    PUBLIC
        CapnProto::capnp-rpc
        CapnProto::capnp
        CapnProto::kj-async
        CapnProto::kj
)

# token.cpp only, and all four are Windows SDK import libraries rather than
# dependencies: bcrypt for BCryptGenRandom, shell32 and ole32 for
# SHGetKnownFolderPath and the CoTaskMemFree that pairs with it, advapi32 for
# the token file's ACL. Nothing is added to vcpkg.json, the x64-windows-static
# triplet is unaffected, and the one-file signed binary gains four OS imports.
# core/CMakeLists.txt links ole32 and avrt on exactly these terms.
#
# ui/CMakeLists.txt compiles these same sources in a separate project under the
# dynamic triplet and needs the same four. That is a second edit in a file
# sharing no code with this one, and forgetting it fails at link naming a token
# function rather than naming the omission.
if(WIN32)
    target_link_libraries(revenant_rpc_client PRIVATE bcrypt shell32 ole32 advapi32)
endif()

target_include_directories(revenant_rpc_client
    PUBLIC
        "${CMAKE_SOURCE_DIR}"
        "${CMAKE_BINARY_DIR}"
)

revenant_apply_warnings(revenant_rpc_client)

add_library(revenant_rpc_server STATIC
    convert.cpp
    server.cpp
)

target_link_libraries(revenant_rpc_server
    PUBLIC
        revenant_rpc_client
        revenant_core
)

revenant_apply_warnings(revenant_rpc_server)

# The same reasoning as revenant_core's: an SDR has no business being built
# with fast math, and a conversion layer that reassociated a rational would
# be the worst place to find that out.
revenant_apply_reference_fp(revenant_rpc_server)
