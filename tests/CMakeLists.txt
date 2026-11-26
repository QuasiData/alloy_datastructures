include(FetchContent)

if(NOT TARGET GTest::gtest_main)
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest")
    set(gtest_force_shared_crt ON CACHE BOOL "Use shared MSVC runtime")
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.16.0
    )
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)
function(add_datastructures_tests target prefix)
    add_executable(${target}
        test.cpp
        hash_map_tests.cpp
    )

    target_link_libraries(${target} PRIVATE
        alloy::datastructures
        alloy_datastructures_compile_options
        GTest::gtest_main
    )

    set_target_properties(${target} PROPERTIES
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES OFF
    )

    gtest_discover_tests(${target}
        TEST_PREFIX "alloy_datastructures.${prefix}."
    )
endfunction()

add_datastructures_tests(alloy_datastructures_tests native)
add_datastructures_tests(alloy_datastructures_tests_scalar scalar)
target_compile_definitions(alloy_datastructures_tests_scalar PRIVATE ALLOY_HASH_MAP_USE_SSE2=0)
