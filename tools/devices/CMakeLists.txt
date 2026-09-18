# Prints the Vulkan devices and the index each answers to, which is what makes
# REVENANT_GPU_INDEX usable by someone who does not already know the answer.

add_executable(revenant-devices main.cpp)
target_link_libraries(revenant-devices PRIVATE revenant_core)
target_include_directories(revenant-devices PRIVATE "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(revenant-devices)
revenant_embed_version(revenant-devices DESCRIPTION "Revenant device list")
