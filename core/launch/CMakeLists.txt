# The rules for starting an engine beside a client, and the Job object that
# ties it to the client's life. Shared by the Qt client (ui/, a separate CMake
# project that adds this directory itself) and webgate. No Qt in here.
#
# The include root is found from this file and not from CMAKE_SOURCE_DIR,
# because ui/ is its own project and its source dir is not the tree's.

add_library(revenant_launch STATIC
    engine_launcher.cpp
)

target_include_directories(revenant_launch PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../..")

if(WIN32)
    # engine_launcher.cpp's loopback probe is a plain Winsock connect.
    target_link_libraries(revenant_launch PRIVATE ws2_32)
endif()

find_package(Threads REQUIRED)
target_link_libraries(revenant_launch PUBLIC Threads::Threads)

if(COMMAND revenant_apply_warnings)
    revenant_apply_warnings(revenant_launch)
endif()
