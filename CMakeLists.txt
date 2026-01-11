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

# Windows-specific flags
if(WIN32)
    add_compile_definitions(
        NOMINMAX  # Prevent Windows.h from defining max/min macros
        WIN32_LEAN_AND_MEAN  # Exclude rarely-used stuff from Windows headers
    )
endif()

# ============================================================================
# Platform Detection
# ============================================================================
if(WIN32)
    set(IS_WINDOWS ON)
else()
    set(IS_WINDOWS OFF)
endif()

# ============================================================================
# Dependencies
# ============================================================================
find_package(SQLite3 REQUIRED)
find_package(zstd REQUIRED)

# ============================================================================
# Threading Library (cross-platform)
# ============================================================================
# On Windows with MSVC, pthread is not available
# On Windows with MinGW, pthread may be available via pthread-win32
# On Unix-like systems, pthread is standard
if(IS_WINDOWS AND MSVC)
    # MSVC uses Windows native threading (no need to link pthread)
    set(THREAD_LIB "")
else()
    # Try to find pthread library
    find_package(Threads QUIET)
    if(Threads_FOUND)
        set(THREAD_LIB Threads::Threads)
    else()
        # Fallback to explicit pthread link for Unix systems
        set(THREAD_LIB pthread)
    endif()
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
    src/state_mutator.cpp
    src/wal_writer.cpp
    src/wal_reader.cpp
    src/rotating_wal.cpp
    src/mem_buffer.cpp
    src/block_writer.cpp
    src/directory_builder.cpp
    src/chunk_sealer.cpp
    src/raw_scanner.cpp
    src/block_reader.cpp
    src/metadata_sync.cpp
    src/storage_engine.cpp
    src/swinging_door_encoder.cpp
    src/swinging_door_decoder.cpp
    src/quantized_16_encoder.cpp
    src/quantized_16_decoder.cpp
    src/resampling_engine.cpp
    src/archive_manager.cpp
    src/thread_pool.cpp
    src/container.cpp
    src/file_container.cpp
    src/block_device_container.cpp
    src/container_manager.cpp
    src/compressor.cpp
    src/compressor_zstd.cpp
)

target_include_directories(xtdb_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(xtdb_core PUBLIC
    SQLite::SQLite3
    zstd::libzstd_shared
    ${THREAD_LIB}
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
        ${THREAD_LIB}
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
    find_package(GTest CONFIG REQUIRED)

    # Test: Alignment
    add_executable(test_alignment
        tests/test_alignment.cpp
    )
    target_link_libraries(test_alignment
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME AlignmentTest COMMAND test_alignment)

    # Test: Layout
    add_executable(test_layout
        tests/test_layout.cpp
    )
    target_link_libraries(test_layout
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME LayoutTest COMMAND test_layout)

    # Test: Struct Size (T3)
    add_executable(test_struct_size
        tests/test_struct_size.cpp
    )
    target_link_libraries(test_struct_size
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME StructSizeTest COMMAND test_struct_size)

    # Test: State Machine (T4)
    add_executable(test_state_machine
        tests/test_state_machine.cpp
    )
    target_link_libraries(test_state_machine
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME StateMachineTest COMMAND test_state_machine)

    # Test: Write Path (T5)
    add_executable(test_write_path
        tests/test_write_path.cpp
    )
    target_link_libraries(test_write_path
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME WritePathTest COMMAND test_write_path)

    # Test: Seal & Directory (T6)
    add_executable(test_seal_directory
        tests/test_seal_directory.cpp
    )
    target_link_libraries(test_seal_directory
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME SealDirectoryTest COMMAND test_seal_directory)

    # Test: Read & Recovery (T7, T8)
    add_executable(test_read_recovery
        tests/test_read_recovery.cpp
    )
    target_link_libraries(test_read_recovery
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME ReadRecoveryTest COMMAND test_read_recovery)

    # Test: End-to-End (T9)
    add_executable(test_end_to_end
        tests/test_end_to_end.cpp
    )
    target_link_libraries(test_end_to_end
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME EndToEndTest COMMAND test_end_to_end)

    # Test: Restart Consistency (T10)
    add_executable(test_restart_consistency
        tests/test_restart_consistency.cpp
    )
    target_link_libraries(test_restart_consistency
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME RestartConsistencyTest COMMAND test_restart_consistency)

    # Test: Write Coordinator (T11)
    add_executable(test_write_coordinator
        tests/test_write_coordinator.cpp
    )
    target_link_libraries(test_write_coordinator
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME WriteCoordinatorTest COMMAND test_write_coordinator)

    # Test: Read Coordinator (T12)
    add_executable(test_read_coordinator
        tests/test_read_coordinator.cpp
    )
    target_link_libraries(test_read_coordinator
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME ReadCoordinatorTest COMMAND test_read_coordinator)

    # Test: Hybrid Debug
    add_executable(test_hybrid_debug
        tests/test_hybrid_debug.cpp
    )
    target_link_libraries(test_hybrid_debug
        xtdb_core
        ${THREAD_LIB}
    )

    # Test: Maintenance Services (T13 - Phase 10)
    add_executable(test_maintenance
        tests/test_maintenance.cpp
    )
    target_link_libraries(test_maintenance
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME MaintenanceServiceTest COMMAND test_maintenance)

    # Test: Swinging Door Compression (T14 - Phase 12)
    add_executable(test_swinging_door
        tests/test_swinging_door.cpp
    )
    target_link_libraries(test_swinging_door
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME SwingingDoorTest COMMAND test_swinging_door)

    # Test: Compression End-to-End (T15 - Phase 12)
    add_executable(test_compression_e2e
        tests/test_compression_e2e.cpp
    )
    target_link_libraries(test_compression_e2e
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME CompressionE2ETest COMMAND test_compression_e2e)

    # Test: 16-bit Quantization (T16 - Phase 13)
    add_executable(test_quantized_16
        tests/test_quantized_16.cpp
    )
    target_link_libraries(test_quantized_16
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME Quantized16Test COMMAND test_quantized_16)

    # Test: Resampling Engine (T17 - Phase 14)
    add_executable(test_resampling
        tests/test_resampling.cpp
    )
    target_link_libraries(test_resampling
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME ResamplingTest COMMAND test_resampling)

    # Test: Archive Manager (T18 - Phase 14)
    add_executable(test_small_chunk
        tests/test_small_chunk.cpp
    )
    target_link_libraries(test_small_chunk
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME ArchiveManagerTest COMMAND test_small_chunk)

    # Test: Compression Integration (T19 - Phase 15)
    add_executable(test_compression_integration
        tests/test_compression_integration.cpp
    )
    target_link_libraries(test_compression_integration
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME CompressionIntegrationTest COMMAND test_compression_integration)

    # Test: Multi-Resolution Query (T20 - Phase 15)
    add_executable(test_multi_resolution_query
        tests/test_multi_resolution_query.cpp
    )
    target_link_libraries(test_multi_resolution_query
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME MultiResolutionQueryTest COMMAND test_multi_resolution_query)

    # Test: Performance Benchmark (T21 - Phase 15)
    add_executable(test_performance_benchmark
        tests/test_performance_benchmark.cpp
    )
    target_link_libraries(test_performance_benchmark
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME PerformanceBenchmarkTest COMMAND test_performance_benchmark)

    # Test: Crash Recovery (T22 - Phase 15)
    add_executable(test_crash_recovery
        tests/test_crash_recovery.cpp
    )
    target_link_libraries(test_crash_recovery
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME CrashRecoveryTest COMMAND test_crash_recovery)

    # Test: Large Scale Simulation (T23 - Phase 15)
    add_executable(test_large_scale_simulation
        tests/test_large_scale_simulation.cpp
    )
    target_link_libraries(test_large_scale_simulation
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME LargeScaleSimulationTest COMMAND test_large_scale_simulation)

    # Test: Rotating WAL (T24)
    add_executable(test_rotating_wal
        tests/test_rotating_wal.cpp
    )
    target_link_libraries(test_rotating_wal
        xtdb_core
        ${THREAD_LIB}
    )
    add_test(NAME RotatingWALTest COMMAND test_rotating_wal)

    # Test: Container Abstraction (T25)
    add_executable(test_container_abstraction
        tests/test_container_abstraction.cpp
    )
    target_link_libraries(test_container_abstraction
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME ContainerAbstractionTest COMMAND test_container_abstraction)

    # Test: Block Device Integration (T26)
    add_executable(test_block_device_integration
        tests/test_block_device_integration.cpp
    )
    target_link_libraries(test_block_device_integration
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME BlockDeviceIntegrationTest COMMAND test_block_device_integration)

    # Test: Block Device Advanced (T27)
    add_executable(test_block_device_advanced
        tests/test_block_device_advanced.cpp
    )
    target_link_libraries(test_block_device_advanced
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME BlockDeviceAdvancedTest COMMAND test_block_device_advanced)

    # Test: Compressor (T28 - Phase 16)
    add_executable(test_compressor
        tests/test_compressor.cpp
    )
    target_link_libraries(test_compressor
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME CompressorTest COMMAND test_compressor)

    # Test: Compact Struct (T29 - Phase 16)
    add_executable(test_compact_struct
        tests/test_compact_struct.cpp
    )
    target_link_libraries(test_compact_struct
        xtdb_core
        GTest::gtest
        GTest::gtest_main
        ${THREAD_LIB}
    )
    add_test(NAME CompactStructTest COMMAND test_compact_struct)

    # Summary
    message(STATUS "Tests enabled: test_alignment, test_layout, test_struct_size, test_state_machine, test_write_path, test_seal_directory, test_read_recovery, test_end_to_end, test_restart_consistency, test_write_coordinator, test_read_coordinator, test_maintenance, test_swinging_door, test_compression_e2e, test_quantized_16, test_resampling, test_small_chunk, test_compression_integration, test_multi_resolution_query, test_performance_benchmark, test_crash_recovery, test_large_scale_simulation, test_rotating_wal, test_container_abstraction, test_block_device_integration, test_block_device_advanced, test_compressor, test_compact_struct")
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
