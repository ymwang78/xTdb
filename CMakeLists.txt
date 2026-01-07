cmake_minimum_required(VERSION 3.14)
project(xTdb VERSION 1.6.0 LANGUAGES C CXX)

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
# Dependencies
# ============================================================================
find_package(SQLite3 REQUIRED)

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
    src/state_mutator.cpp
    src/wal_writer.cpp
    src/mem_buffer.cpp
    src/block_writer.cpp
    src/directory_builder.cpp
    src/chunk_sealer.cpp
    src/raw_scanner.cpp
    src/block_reader.cpp
    src/metadata_sync.cpp
    src/storage_engine.cpp
)

target_include_directories(xtdb_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(xtdb_core PUBLIC
    SQLite::SQLite3
)

# ============================================================================
# Library: xTdb C API (Phase 11)
# ============================================================================
add_library(xtdb_api
    src/xtdb_api.cpp
)

target_include_directories(xtdb_api PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(xtdb_api PUBLIC
    xtdb_core
)

# ============================================================================
# Examples
# ============================================================================
option(BUILD_EXAMPLES "Build examples" ON)

if(BUILD_EXAMPLES)
    # Example: C API Usage
    add_executable(api_example
        examples/api_example.c
    )
    set_target_properties(api_example PROPERTIES
        LINKER_LANGUAGE C
    )
    target_link_libraries(api_example
        xtdb_api
        pthread
    )
    message(STATUS "Examples enabled: api_example")
endif()

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

    # Test: Struct Size (T3)
    add_executable(test_struct_size
        tests/test_struct_size.cpp
    )
    target_link_libraries(test_struct_size
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME StructSizeTest COMMAND test_struct_size)

    # Test: State Machine (T4)
    add_executable(test_state_machine
        tests/test_state_machine.cpp
    )
    target_link_libraries(test_state_machine
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME StateMachineTest COMMAND test_state_machine)

    # Test: Write Path (T5)
    add_executable(test_write_path
        tests/test_write_path.cpp
    )
    target_link_libraries(test_write_path
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME WritePathTest COMMAND test_write_path)

    # Test: Seal & Directory (T6)
    add_executable(test_seal_directory
        tests/test_seal_directory.cpp
    )
    target_link_libraries(test_seal_directory
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME SealDirectoryTest COMMAND test_seal_directory)

    # Test: Read & Recovery (T7, T8)
    add_executable(test_read_recovery
        tests/test_read_recovery.cpp
    )
    target_link_libraries(test_read_recovery
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME ReadRecoveryTest COMMAND test_read_recovery)

    # Test: End-to-End (T9)
    add_executable(test_end_to_end
        tests/test_end_to_end.cpp
    )
    target_link_libraries(test_end_to_end
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME EndToEndTest COMMAND test_end_to_end)

    # Test: Restart Consistency (T10)
    add_executable(test_restart_consistency
        tests/test_restart_consistency.cpp
    )
    target_link_libraries(test_restart_consistency
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME RestartConsistencyTest COMMAND test_restart_consistency)

    # Test: Write Coordinator (T11)
    add_executable(test_write_coordinator
        tests/test_write_coordinator.cpp
    )
    target_link_libraries(test_write_coordinator
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME WriteCoordinatorTest COMMAND test_write_coordinator)

    # Test: Read Coordinator (T12)
    add_executable(test_read_coordinator
        tests/test_read_coordinator.cpp
    )
    target_link_libraries(test_read_coordinator
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME ReadCoordinatorTest COMMAND test_read_coordinator)

    # Test: Hybrid Debug
    add_executable(test_hybrid_debug
        tests/test_hybrid_debug.cpp
    )
    target_link_libraries(test_hybrid_debug
        xtdb_core
        pthread
    )

    # Test: Maintenance Services (T13 - Phase 10)
    add_executable(test_maintenance
        tests/test_maintenance.cpp
    )
    target_link_libraries(test_maintenance
        xtdb_core
        GTest::GTest
        GTest::Main
        pthread
    )
    add_test(NAME MaintenanceServiceTest COMMAND test_maintenance)

    # Summary
    message(STATUS "Tests enabled: test_alignment, test_layout, test_struct_size, test_state_machine, test_write_path, test_seal_directory, test_read_recovery, test_end_to_end, test_restart_consistency, test_write_coordinator, test_read_coordinator, test_maintenance")
endif()

# ============================================================================
# Installation (optional)
# ============================================================================
install(TARGETS xtdb_core xtdb_api
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

install(DIRECTORY include/xTdb
    DESTINATION include
)

install(FILES include/xTdb/xtdb_api.h
    DESTINATION include/xTdb
)
