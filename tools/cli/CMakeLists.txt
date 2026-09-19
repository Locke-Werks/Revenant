# The application. Everything else under tools/ is an instrument; this is the
# program somebody runs to listen to a radio.

add_executable(revenant-cli main.cpp)
target_link_libraries(revenant-cli PRIVATE revenant_core)
revenant_apply_warnings(revenant-cli)
revenant_embed_version(revenant-cli DESCRIPTION "Revenant receiver")
