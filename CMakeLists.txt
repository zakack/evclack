cmake_minimum_required(VERSION 3.10)

project(evclack VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

include(GNUInstallDirs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(LIBEVDEV REQUIRED libevdev)
pkg_check_modules(LIBYAML  REQUIRED yaml-0.1)
pkg_check_modules(SAMPLERATE REQUIRED samplerate)
pkg_check_modules(SNDFILE    REQUIRED sndfile)

# Installation directories for the systemd user unit and udev rule. Default
# to what the host's systemd/udev report; override with -D for packaging.
if(NOT SYSTEMD_USER_UNIT_DIR)
    pkg_get_variable(SYSTEMD_USER_UNIT_DIR systemd systemduserunitdir)
    if(NOT SYSTEMD_USER_UNIT_DIR)
        set(SYSTEMD_USER_UNIT_DIR "${CMAKE_INSTALL_LIBDIR}/systemd/user")
    endif()
endif()

add_executable(evclack evclack.c)

target_compile_definitions(evclack PRIVATE
    DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/evclack/click.wav"
    DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/evclack/config.yaml"
)

target_compile_options(evclack PRIVATE
    -Wall -Wextra
    ${PIPEWIRE_CFLAGS_OTHER}
    ${LIBEVDEV_CFLAGS_OTHER}
    ${LIBYAML_CFLAGS_OTHER}
    ${SAMPLERATE_CFLAGS_OTHER}
    ${SNDFILE_CFLAGS_OTHER}
)

target_include_directories(evclack PRIVATE
    ${PIPEWIRE_INCLUDE_DIRS}
    ${LIBEVDEV_INCLUDE_DIRS}
    ${LIBYAML_INCLUDE_DIRS}
    ${SAMPLERATE_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
)

target_link_directories(evclack PRIVATE
    ${PIPEWIRE_LIBRARY_DIRS}
    ${LIBEVDEV_LIBRARY_DIRS}
    ${LIBYAML_LIBRARY_DIRS}
    ${SAMPLERATE_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
)

target_link_libraries(evclack PRIVATE
    ${PIPEWIRE_LIBRARIES}
    ${LIBEVDEV_LIBRARIES}
    ${LIBYAML_LIBRARIES}
    ${SAMPLERATE_LIBRARIES}
    ${SNDFILE_LIBRARIES}
)

# Developer tools. Not installed, but deliberately part of the default build:
# each #includes evclack.c so it exercises the daemon's own code rather than a
# copy that could drift, and that guarantee is worthless if the binary can go
# stale. EXCLUDE_FROM_ALL let `cmake --build build` skip them silently, which
# is how you end up shipping a test that no longer compiles against the thing
# it tests. A few seconds of build time is the cheaper mistake.
#   ./build/mixtest            assertions over the audio voice pool
#   ./build/maptest            assertions over the key -> sample layer
foreach(tool mixtest maptest)
    add_executable(${tool} tools/${tool}.c)

    target_compile_definitions(${tool} PRIVATE
        DEF_WAV="${CMAKE_INSTALL_FULL_DATADIR}/evclack/click.wav"
        DEF_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/evclack/config.yaml"
    )
    target_compile_options(${tool} PRIVATE
        -Wall -Wextra
        ${PIPEWIRE_CFLAGS_OTHER} ${LIBEVDEV_CFLAGS_OTHER} ${LIBYAML_CFLAGS_OTHER}
        ${SAMPLERATE_CFLAGS_OTHER} ${SNDFILE_CFLAGS_OTHER}
    )
    target_include_directories(${tool} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PIPEWIRE_INCLUDE_DIRS} ${LIBEVDEV_INCLUDE_DIRS} ${LIBYAML_INCLUDE_DIRS}
        ${SAMPLERATE_INCLUDE_DIRS} ${SNDFILE_INCLUDE_DIRS}
    )
    target_link_directories(${tool} PRIVATE
        ${PIPEWIRE_LIBRARY_DIRS} ${LIBEVDEV_LIBRARY_DIRS} ${LIBYAML_LIBRARY_DIRS}
        ${SAMPLERATE_LIBRARY_DIRS} ${SNDFILE_LIBRARY_DIRS}
    )
    target_link_libraries(${tool} PRIVATE
        ${PIPEWIRE_LIBRARIES} ${LIBEVDEV_LIBRARIES} ${LIBYAML_LIBRARIES}
        ${SAMPLERATE_LIBRARIES} ${SNDFILE_LIBRARIES}
    )
endforeach()

configure_file(evclack.service.in evclack.service @ONLY)

install(TARGETS evclack RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES   config.yaml             DESTINATION ${CMAKE_INSTALL_DATADIR}/evclack)
install(FILES   click.wav               DESTINATION ${CMAKE_INSTALL_DATADIR}/evclack)
install(FILES   ${CMAKE_CURRENT_BINARY_DIR}/evclack.service
                                        DESTINATION ${SYSTEMD_USER_UNIT_DIR})
