#
# 86Box headless frontend: no window, no audio; driven over a unix socket.
#

add_library(plat OBJECT
    hl_main.c
    hl_plat.c
    hl_plat_unix.c
    hl_ports.c
    hl_keys.c
    hl_video.c
    hl_ctl.c
    hl_prof.c
    hl_joystick.c
    ../unix/unix_netsocket.c
    ../unix/unix_dynld.c
)

add_library(ui OBJECT
    hl_ui.c
    ../unix/sdl_cdrom.c
)

if(NOT CPPTHREADS)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)
    target_link_libraries(86Box Threads::Threads)
    target_sources(plat PRIVATE ../unix/unix_thread.c)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(plat PRIVATE ../unix/linux_cdrom_ioctl.c ../unix/dummy_floppy_ioctl.c)
elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    target_sources(plat PRIVATE ../unix/dummy_cdrom_ioctl.c ../unix/unix_floppy_ioctl.c)
else()
    target_sources(plat PRIVATE ../unix/dummy_cdrom_ioctl.c ../unix/dummy_floppy_ioctl.c)
endif()

target_compile_definitions(plat PUBLIC _FILE_OFFSET_BITS=64)
target_link_libraries(plat ${CMAKE_DL_LIBS})

# The binary is a plain console tool, not an app bundle.
set_target_properties(86Box PROPERTIES OUTPUT_NAME 86Box-headless MACOSX_BUNDLE OFF)
