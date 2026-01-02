cmake_minimum_required(VERSION 3.14)
project(xTdb VERSION 1.6.0 LANGUAGES CXX)

# ============================================================================
# C++ Standard
# ============================================================================
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ============================================================================
# Compiler Flags
# ============================================================================
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -O2
    )
endif()

# ============================================================================
# Include Directories
# ============================================================================
include_directories(${CMAKE_SOURCE_DIR}/include)

# ============================================================================
# Library: xTdb Core
# ============================================================================
add_library(xtdb_core
    src/layout_calculator.cpp
    src/aligned_io.cpp
)

target_include_directories(xtdb_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

# ============================================================================
# Testing
# ============================================================================
option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    # Enable testing
    enable_testing()

    # Find Google Test
    find_package(GTest REQUIRED)
    include_directories(${GTEST_INCLUDE_DIRS})

    # Test: Alignment
    add_executable(test_alignment
        tests/test_alignment.cpp
    )
    target_link_libraries(test_alignment
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME AlignmentTest COMMAND test_alignment)

    # Test: Layout
    add_executable(test_layout
        tests/test_layout.cpp
    )
    target_link_libraries(test_layout
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME LayoutTest COMMAND test_layout)

    # Summary
    message(STATUS "Tests enabled: test_alignment, test_layout")
endif()

# ============================================================================
# Installation (optional)
# ============================================================================
install(TARGETS xtdb_core
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/xTdb
    DESTINATION include
)
