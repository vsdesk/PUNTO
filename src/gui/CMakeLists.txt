find_package(Qt6 REQUIRED COMPONENTS Widgets DBus)

qt_standard_project_setup()

qt_add_executable(rocket-switcher-config
    main.cpp
    MainWindow.cpp
    HotkeyEditor.cpp
    DaemonClient.cpp
)

target_link_libraries(rocket-switcher-config
    PRIVATE
        Qt6::Widgets
        Qt6::DBus
        PuntoSwitcher::Core
)

if(BUILD_FCITX)
    target_compile_definitions(rocket-switcher-config PRIVATE PUNTO_WITH_FCITX=1)
else()
    target_compile_definitions(rocket-switcher-config PRIVATE PUNTO_WITH_FCITX=0)
endif()

target_compile_features(rocket-switcher-config PRIVATE cxx_std_17)

# ---- Install ----
include(GNUInstallDirs)

install(TARGETS rocket-switcher-config
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# Desktop file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/rocket-switcher-config.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/rocket-switcher-config.desktop"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rocket-switcher-config.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
