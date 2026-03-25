find_package(PkgConfig QUIET)
if(NOT PKG_CONFIG_FOUND)
    message(STATUS "pkg-config missing — skipping punto-switcher-daemon")
    return()
endif()

pkg_check_modules(LIBEVDEV libevdev)
pkg_check_modules(XKBCOMMON xkbcommon)
if(NOT LIBEVDEV_FOUND OR NOT XKBCOMMON_FOUND)
    message(STATUS "libevdev or xkbcommon not found — skipping punto-switcher-daemon (install libevdev-dev libxkbcommon-dev)")
    return()
endif()

add_executable(punto-switcher-daemon
    main.cpp
    SessionEnv.cpp
    DaemonConfig.cpp
    EvdevUinput.cpp
    PuntoDaemon.cpp
    TextInjector.cpp
    LayoutController.cpp
    ../engine/InputTracker.cpp
)

target_include_directories(punto-switcher-daemon
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_SOURCE_DIR}/src/engine
        ${LIBEVDEV_INCLUDE_DIRS}
        ${XKBCOMMON_INCLUDE_DIRS}
)

target_link_libraries(punto-switcher-daemon
    PRIVATE
        PuntoSwitcher::Core
        ${LIBEVDEV_LIBRARIES}
        ${XKBCOMMON_LIBRARIES}
)

target_compile_options(punto-switcher-daemon PRIVATE
    ${LIBEVDEV_CFLAGS_OTHER}
    ${XKBCOMMON_CFLAGS_OTHER}
)

target_compile_features(punto-switcher-daemon PRIVATE cxx_std_17)

install(TARGETS punto-switcher-daemon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS punto-switcher-stop
    DESTINATION ${CMAKE_INSTALL_BINDIR})

# Autostart + udev are installed via debian/punto-switcher.install (correct DESTDIR).
