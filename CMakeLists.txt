cmake_minimum_required(VERSION 3.16)
project(bgp_simulator VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find OpenMP for parallelization
find_package(OpenMP)
if(OpenMP_CXX_FOUND)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${OpenMP_EXE_LINKER_FLAGS}")
endif()

# Aggressive optimization flags for release builds
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE MATCHES Release)
    if(MSVC)
        add_compile_options(/O2 /Ob2 /GL)
    else()
        add_compile_options(-O3 -march=native -flto -ffast-math -funroll-loops)
        add_link_options(-flto)
    endif()
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(bgp_core
    src/announcement.cpp
    src/prefix_map.cpp
    src/as_graph.cpp
    src/as_node.cpp
    src/bgp.cpp
    src/rov.cpp
)

target_include_directories(bgp_core
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(bgp_simulator
    src/main.cpp
)

target_link_libraries(bgp_simulator
    PRIVATE bgp_core
)

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    include(CTest)

    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    FetchContent_MakeAvailable(googletest)

    set(BGP_TEST_SUITES
        test_announcement
        test_as_graph
        test_conflicts
        test_flattening
        test_graph_builder
        test_output
        test_policy
        test_propagation
        test_rov
        test_seeding
    )

    set(BGP_TESTS_WITH_CUSTOM_MAIN
        test_conflicts
        test_propagation
    )

    foreach(test_name IN LISTS BGP_TEST_SUITES)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_link_libraries(${test_name}
            PRIVATE bgp_core GTest::gtest
        )

        target_compile_definitions(${test_name}
            PRIVATE PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        )

        list(FIND BGP_TESTS_WITH_CUSTOM_MAIN ${test_name} _has_custom_main)
        if(_has_custom_main EQUAL -1)
            target_link_libraries(${test_name} PRIVATE GTest::gtest_main)
        endif()

        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()
endif()
