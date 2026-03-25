find_package(Fcitx5Core REQUIRED)
find_package(Fcitx5Utils REQUIRED)

# Fcitx5 provides a helper macro for addons/modules
include("${FCITX_INSTALL_CMAKECONFIG_DIR}/Fcitx5Utils/Fcitx5Macros.cmake" OPTIONAL)

add_library(punto-switcher-engine MODULE
    PuntoModule.cpp
    InputTracker.cpp
)

set_target_properties(punto-switcher-engine PROPERTIES
    PREFIX ""           # no "lib" prefix required by Fcitx5
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/fcitx5"
)

target_link_libraries(punto-switcher-engine
    PRIVATE
        PuntoSwitcher::Core
        Fcitx5::Core
        Fcitx5::Utils
        Fcitx5::Config
)

# Fcitx5 5.1.x headers use std::span which requires C++20
target_compile_features(punto-switcher-engine PRIVATE cxx_std_20)

# ---- Install targets ----
# Shared library goes to Fcitx5 addon directory
include(GNUInstallDirs)

install(TARGETS punto-switcher-engine
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/fcitx5"
)

# Addon manifest
install(FILES conf/punto-switcher.conf
    DESTINATION "${CMAKE_INSTALL_DATADIR}/fcitx5/addon"
)
