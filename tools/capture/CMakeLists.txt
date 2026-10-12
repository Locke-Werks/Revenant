# The rolling capture from a terminal: list, search, occupancy, index, extract
# and recover over a capture root.
#
# The commands are a library so tests/tools drives the code the executable
# runs and reads back the text it prints.

add_library(revenant_capture_tool STATIC capture_tool.cpp)
target_link_libraries(revenant_capture_tool PUBLIC revenant_core)
target_include_directories(revenant_capture_tool PUBLIC "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(revenant_capture_tool)

add_executable(revenant-capture main.cpp)
target_link_libraries(revenant-capture PRIVATE revenant_capture_tool)
revenant_apply_warnings(revenant-capture)
revenant_embed_version(revenant-capture DESCRIPTION "Revenant capture tool")
