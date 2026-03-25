add_library(punto-core STATIC
    CharMapping.cpp
    WordSwapper.cpp
    AutoSwitchHeuristic.cpp
)

target_include_directories(punto-core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(punto-core PUBLIC cxx_std_17)

# Required when punto-core is linked into a shared library (Fcitx5 module)
set_target_properties(punto-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Export alias for use in other subdirectories
add_library(PuntoSwitcher::Core ALIAS punto-core)
