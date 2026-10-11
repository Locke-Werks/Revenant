# Lists attached RTL-SDRs by serial and rewrites a dongle's serial in its
# EEPROM, so two identical dongles can be told apart by something that does
# not move when one is replugged. Ships beside revenant-engine.exe.

# Again here and not only in core/: an imported target from find_package is
# visible only in the directory that found it.
find_package(rtlsdr CONFIG REQUIRED)

add_executable(revenant-rtl-serial main.cpp)
target_link_libraries(revenant-rtl-serial PRIVATE
    revenant_device_lock
    $<IF:$<TARGET_EXISTS:rtlsdr::rtlsdr>,rtlsdr::rtlsdr,rtlsdr::rtlsdr_static>
)
target_include_directories(revenant-rtl-serial PRIVATE "${CMAKE_SOURCE_DIR}")
revenant_apply_warnings(revenant-rtl-serial)
revenant_embed_version(revenant-rtl-serial DESCRIPTION "Revenant RTL-SDR serial tool")
