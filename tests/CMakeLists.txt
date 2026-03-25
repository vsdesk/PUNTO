find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(punto-tests
    test_mapping.cpp
    test_swapper.cpp
    test_heuristic.cpp
)

target_link_libraries(punto-tests
    PRIVATE
        PuntoSwitcher::Core
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(punto-tests PRIVATE cxx_std_17)

gtest_discover_tests(punto-tests)
