cmake_minimum_required(VERSION 3.20)

project(PuntoSwitcher
    VERSION 1.0.0
    DESCRIPTION "Punto-style ru<->en keyboard layout switcher for Wayland/Fcitx5"
    HOMEPAGE_URL "https://github.com/your-org/punto-switcher"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json for IDEs/clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Hardening flags for Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2 -D_FORTIFY_SOURCE=2 -fstack-protector-strong)
    add_link_options(-Wl,-z,relro,-z,now)
endif()

# ---------------------------------------------------------------------------
# Installation prefix defaults (for packaging)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

# Fcitx5 cmake module search path — set by Fcitx5Core package
# Can be overridden: -DFCITX_INSTALL_CMAKECONFIG_DIR=...
find_package(ECM QUIET)  # KDE Extra CMake Modules (optional, for FindFcitx5)

# ---------------------------------------------------------------------------
# Submodules
# ---------------------------------------------------------------------------
add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/gui)

# ---------------------------------------------------------------------------
# Setup script
# ---------------------------------------------------------------------------
install(PROGRAMS scripts/punto-switcher-setup
        DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Tests (optional, enabled with -DBUILD_TESTING=ON)
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# CPack configuration (for reference — packaging is done via debhelper/rpmbuild)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME              "punto-switcher")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SHORT "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_CONTACT           "maintainer@example.com")
set(CPACK_PACKAGE_VENDOR            "PuntoSwitcher Project")

include(CPack)
