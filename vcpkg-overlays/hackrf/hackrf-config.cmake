# A static libhackrf carries pthreads as a link requirement, so a consumer has
# to be able to find it too. libusb is linked by path and needs no lookup.
include(CMakeFindDependencyMacro)
if(WIN32 AND NOT MINGW)
    find_dependency(PThreads4W)
else()
    find_dependency(Threads)
endif()
if(NOT TARGET HackRF::hackrf)
    include("${CMAKE_CURRENT_LIST_DIR}/HackRFTargets.cmake")
endif()
