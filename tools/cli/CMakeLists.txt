# The application. Everything else under tools/ is an instrument; this is the
# program somebody runs to listen to a radio.

add_executable(revenant-cli main.cpp)
target_link_libraries(revenant-cli PRIVATE revenant_core)
revenant_apply_warnings(revenant-cli)
# main.cpp instantiates every decoder adapter in core/rpc/decoders.h, which
# exceeds the default COFF section limit (C1128) in the headless build.
if(MSVC)
    target_compile_options(revenant-cli PRIVATE /bigobj)
endif()
revenant_embed_version(revenant-cli DESCRIPTION "Revenant receiver")
