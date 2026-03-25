find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(punto-switcher-config
    main.cpp
    MainWindow.cpp
    HotkeyEditor.cpp
)

target_link_libraries(punto-switcher-config
    PRIVATE
        Qt6::Widgets
)

if(BUILD_FCITX)
    target_compile_definitions(punto-switcher-config PRIVATE PUNTO_WITH_FCITX=1)
else()
    target_compile_definitions(punto-switcher-config PRIVATE PUNTO_WITH_FCITX=0)
endif()

target_compile_features(punto-switcher-config PRIVATE cxx_std_17)

# ---- Install ----
include(GNUInstallDirs)

install(TARGETS punto-switcher-config
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

# Desktop file
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/punto-switcher-config.desktop.in"
    "${CMAKE_CURRENT_BINARY_DIR}/punto-switcher-config.desktop"
    @ONLY
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/punto-switcher-config.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
)
