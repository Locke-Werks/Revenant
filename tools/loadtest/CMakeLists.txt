# revenant-loadtest: an engine, a server and a client in one process, loaded
# the way a live session loads them, reporting where the time went. See
# main.cpp for what it measures and docs/rpc.md, under Threading, for what it
# found.
#
# The server half of core/rpc/CMakeLists.txt's split, because it builds the
# engine and reads its load counters directly; the client half comes with it.

add_executable(revenant-loadtest main.cpp)

target_link_libraries(revenant-loadtest PRIVATE revenant_rpc_server revenant_rpc_client)

revenant_apply_warnings(revenant-loadtest)
revenant_embed_version(revenant-loadtest DESCRIPTION "Revenant contention measurement")
