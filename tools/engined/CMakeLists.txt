# The engine as a service. revenant-cli is the same core driven by a command
# line for the length of one run; this is the same core left running with a
# Cap'n Proto session on the front of it.
#
# It links revenant_rpc_server and not revenant_core directly. That target
# already carries the engine behind it, and naming it here is what says which
# side of core/rpc/CMakeLists.txt's split this program is on: the server half,
# which may see the engine, rather than the client half, which may not.

add_executable(revenant-engine main.cpp)

target_link_libraries(revenant-engine PRIVATE revenant_rpc_server)

revenant_apply_warnings(revenant-engine)
revenant_embed_version(revenant-engine DESCRIPTION "Revenant engine service")
