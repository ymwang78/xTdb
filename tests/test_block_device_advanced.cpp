#include "xTdb/storage_engine.h"
#include "xTdb/block_device_container.h"
#include "xTdb/container_manager.h"
#include "xTdb/aligned_io.h"
#include "test_utils.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace xtdb;
namespace fs = std::filesystem;

class BlockDeviceAdvancedTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use cross-platform temp directory
        std::string temp_dir = get_temp_dir();
        test_dir_ = join_path(temp_dir, "test_block_device_advanced");
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Check if test device is provided via environment variable
        const char* env_device = std::getenv("XTDB_TEST_DEVICE");
        if (env_device && strlen(env_device) > 0) {
            test_device_ = env_device;
            has_real_block_device_ = BlockDeviceContainer::isBlockDevice(test_device_);
            use_test_mode_ = false;
            std::cout << "[Setup] Using test device: " << test_device_ << std::endl;
            std::cout << "[Setup] Is block device: " << (has_real_block_device_ ? "YES" : "NO") << std::endl;
        } else {
            // No real block device - create simulated file for testing
            has_real_block_device_ = false;
            use_test_mode_ = true;
            test_device_ = join_path(test_dir_, "simulated_block_device.img");

            // Create a 512MB file for testing
            std::cout << "[Setup] No XTDB_TEST_DEVICE provided. Using simulated file." << std::endl;
            std::cout << "[Setup] Creating 512MB test file: " << test_device_ << std::endl;

            std::ofstream file(test_device_, std::ios::binary);
            if (!file) {
                std::cerr << "[Setup] ERROR: Failed to create test file!" << std::endl;
                return;
            }

            // Write 512MB of zeros
            const size_t chunk_size = 16 * 1024;  // 16KB chunks
            const size_t total_size = 512 * 1024 * 1024;  // 512MB
            std::vector<char> zero_buffer(chunk_size, 0);

            for (size_t written = 0; written < total_size; written += chunk_size) {
                file.write(zero_buffer.data(), chunk_size);
            }
            file.close();

            std::cout << "[Setup] Test file created successfully (512 MB)" << std::endl;
            std::cout << "[Setup] Running in TEST MODE (no real block device required)" << std::endl;
        }
    }

    void TearDown() override {
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    std::string test_dir_;
    std::string test_device_;
    bool has_real_block_device_;
    bool use_test_mode_;
};

// Test 1: BlockDeviceContainer basic operations
TEST_F(BlockDeviceAdvancedTest, BlockDeviceContainerBasicOps) {
    std::cout << "\n=== Test 1: BlockDeviceContainer Basic Operations ===" << std::endl;

    ChunkLayout layout;
    layout.block_size_bytes = 16384;
    layout.chunk_size_bytes = 16 * 1024 * 1024;  // 16MB

    std::cout << "  Creating BlockDeviceContainer..." << std::endl;
    auto container = std::make_unique<BlockDeviceContainer>(
        test_device_,       // device_path
        layout,             // layout
        false,              // read_only
        use_test_mode_      // test_mode
    );

    std::cout << "  Opening block device..." << std::endl;
    ContainerResult result = container->open(true);  // create_if_not_exists = true
    ASSERT_EQ(result, ContainerResult::SUCCESS) << "Open failed: " << container->getLastError();
    std::cout << "    Opened successfully" << std::endl;

    // Verify properties
    EXPECT_EQ(container->getType(), ContainerType::BLOCK_DEVICE);
    EXPECT_EQ(container->getIdentifier(), test_device_);
    EXPECT_GT(container->getCapacity(), 0);
    std::cout << "    Capacity: " << container->getCapacity() / (1024*1024) << " MB" << std::endl;

    // Test write operation
    std::cout << "  Testing write operation..." << std::endl;
    AlignedBuffer write_data(16384);
    std::memset(write_data.data(), 0xAB, 16384);
    // Write to offset 16384 (after header)
    result = container->write(write_data.data(), 16384, 16384);
    EXPECT_EQ(result, ContainerResult::SUCCESS);
    std::cout << "    Write successful" << std::endl;

    // Test read operation
    std::cout << "  Testing read operation..." << std::endl;
    AlignedBuffer read_data(16384);
    result = container->read(read_data.data(), 16384, 16384);
    EXPECT_EQ(result, ContainerResult::SUCCESS);
    std::cout << "    Read successful" << std::endl;

    // Verify data
    std::cout << "  Verifying data..." << std::endl;
    bool data_match = (std::memcmp(write_data.data(), read_data.data(), 16384) == 0);
    EXPECT_TRUE(data_match);
    std::cout << "    Data verification passed" << std::endl;

    // Test AlignedIO interface
    std::cout << "  Testing AlignedIO interface..." << std::endl;
    AlignedIO* io = container->getIO();
    ASSERT_NE(io, nullptr);
    std::cout << "    AlignedIO interface available" << std::endl;

    container->close();
    std::cout << "  Test completed successfully" << std::endl;
}

// Test 2: StorageEngine with block device
TEST_F(BlockDeviceAdvancedTest, StorageEngineWithBlockDevice) {
    std::cout << "\n=== Test 2: StorageEngine with Block Device ===" << std::endl;

    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");
    config.container_type = ContainerType::BLOCK_DEVICE;
    config.block_device_path = test_device_;
    config.block_device_test_mode = use_test_mode_;  // Use test mode for simulated files
    config.rollover_strategy = RolloverStrategy::NONE;
    config.direct_io = false;  // Disable O_DIRECT for testing

    std::cout << "  Creating StorageEngine with block device configuration..." << std::endl;
    StorageEngine engine(config);

    std::cout << "  Opening engine..." << std::endl;
    EngineResult result = engine.open();
    ASSERT_EQ(result, EngineResult::SUCCESS) << "Failed to open: " << engine.getLastError();
    std::cout << "    Engine opened successfully" << std::endl;

    // Verify container info
    const auto& containers = engine.getContainers();
    ASSERT_EQ(containers.size(), 1);
    EXPECT_EQ(containers[0].file_path, test_device_);
    std::cout << "    Container: " << containers[0].file_path << std::endl;
    std::cout << "    Capacity: " << containers[0].capacity_bytes / (1024*1024) << " MB" << std::endl;

    // Write test data
    std::cout << "  Writing data points..." << std::endl;
    StorageEngine::TagConfig tag_config;
    tag_config.tag_id = 1;
    tag_config.tag_name = "BlockDevice_Test_Tag";
    tag_config.value_type = ValueType::VT_F64;
    tag_config.time_unit = TimeUnit::TU_MS;
    tag_config.encoding_type = EncodingType::ENC_RAW;

    int64_t base_time = 1000000;
    for (int i = 0; i < 50; i++) {
        result = engine.writePoint(&tag_config, base_time + i * 1000, 100.0 + i, 192);
        EXPECT_EQ(result, EngineResult::SUCCESS);
    }
    std::cout << "    Wrote 50 points" << std::endl;

    // Flush to device
    std::cout << "  Flushing to block device..." << std::endl;
    result = engine.flush();
    EXPECT_EQ(result, EngineResult::SUCCESS);
    std::cout << "    Flush successful" << std::endl;

    // Verify write statistics
    const auto& write_stats = engine.getWriteStats();
    std::cout << "  Write statistics:" << std::endl;
    std::cout << "    Points written: " << write_stats.points_written << std::endl;
    std::cout << "    Blocks flushed: " << write_stats.blocks_flushed << std::endl;
    EXPECT_EQ(write_stats.points_written, 50);
    EXPECT_GT(write_stats.blocks_flushed, 0);

    // Query data back
    std::cout << "  Querying data..." << std::endl;
    std::vector<StorageEngine::QueryPoint> query_results;
    result = engine.queryPoints(1, base_time, base_time + 50000, query_results);
    EXPECT_EQ(result, EngineResult::SUCCESS);
    std::cout << "    Query returned " << query_results.size() << " points" << std::endl;
    EXPECT_EQ(query_results.size(), 50);

    // Verify data correctness
    std::cout << "  Verifying data correctness..." << std::endl;
    for (size_t i = 0; i < query_results.size() && i < 5; i++) {
        EXPECT_EQ(query_results[i].value, 100.0 + i);
        std::cout << "    Point[" << i << "]: value=" << query_results[i].value << std::endl;
    }

    engine.close();
    std::cout << "  Test completed successfully" << std::endl;
}

// Test 3: Write persistence - verify data survives engine restart
TEST_F(BlockDeviceAdvancedTest, DataPersistenceOnBlockDevice) {
    std::cout << "\n=== Test 3: Data Persistence on Block Device ===" << std::endl;

    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");
    config.container_type = ContainerType::BLOCK_DEVICE;
    config.block_device_path = test_device_;
    config.block_device_test_mode = use_test_mode_;  // Use test mode for simulated files
    config.rollover_strategy = RolloverStrategy::NONE;
    config.direct_io = false;

    int64_t base_time = 2000000;
    size_t expected_points = 100;

    // Phase 1: Write data
    {
        std::cout << "  Phase 1: Writing data..." << std::endl;
        StorageEngine engine(config);
        EngineResult result = engine.open();
        ASSERT_EQ(result, EngineResult::SUCCESS);

        StorageEngine::TagConfig tag_config;
        tag_config.tag_id = 999;
        tag_config.tag_name = "Persistence_Test";
        tag_config.value_type = ValueType::VT_F64;
        tag_config.time_unit = TimeUnit::TU_MS;
        tag_config.encoding_type = EncodingType::ENC_RAW;

        for (size_t i = 0; i < expected_points; i++) {
            result = engine.writePoint(&tag_config, base_time + i * 1000,
                                      200.0 + i * 0.5, 192);
            EXPECT_EQ(result, EngineResult::SUCCESS);
        }

        result = engine.flush();
        EXPECT_EQ(result, EngineResult::SUCCESS);
        std::cout << "    Wrote and flushed " << expected_points << " points" << std::endl;

        engine.close();
        std::cout << "    Engine closed" << std::endl;
    }

    // Phase 2: Reopen and verify data
    {
        std::cout << "  Phase 2: Reopening and verifying data..." << std::endl;
        StorageEngine engine(config);
        EngineResult result = engine.open();
        ASSERT_EQ(result, EngineResult::SUCCESS);
        std::cout << "    Engine reopened successfully" << std::endl;

        std::vector<StorageEngine::QueryPoint> results;
        result = engine.queryPoints(999, base_time, base_time + expected_points * 1000, results);
        EXPECT_EQ(result, EngineResult::SUCCESS);
        std::cout << "    Query returned " << results.size() << " points" << std::endl;
        EXPECT_EQ(results.size(), expected_points);

        // Verify first and last points
        if (!results.empty()) {
            EXPECT_DOUBLE_EQ(results[0].value, 200.0);
            EXPECT_DOUBLE_EQ(results[results.size() - 1].value,
                           200.0 + (expected_points - 1) * 0.5);
            std::cout << "    First point: " << results[0].value << std::endl;
            std::cout << "    Last point: " << results[results.size() - 1].value << std::endl;
        }

        engine.close();
        std::cout << "  Data persistence verified!" << std::endl;
    }
}

// Test 4: Performance comparison - block device vs file
TEST_F(BlockDeviceAdvancedTest, PerformanceComparison) {
    std::cout << "\n=== Test 4: Performance Comparison ===" << std::endl;

    const size_t num_points = 1000;
    int64_t base_time = 3000000;

    auto measureWriteTime = [&](ContainerType type, const std::string& path, bool test_mode) -> double {
        EngineConfig config;
        config.data_dir = test_dir_;
        config.db_path = join_path(test_dir_, std::string("meta_") +
                        (type == ContainerType::BLOCK_DEVICE ? "block" : "file") + ".db");
        config.container_type = type;

        if (type == ContainerType::BLOCK_DEVICE) {
            config.block_device_path = path;
            config.block_device_test_mode = test_mode;  // Use test mode for simulated files
        } else {
            config.container_name_pattern = "perf_test_{index}.raw";
        }

        config.rollover_strategy = RolloverStrategy::NONE;
        config.direct_io = false;

        StorageEngine engine(config);
        EngineResult result = engine.open();
        if (result != EngineResult::SUCCESS) {
            std::cerr << "Failed to open: " << engine.getLastError() << std::endl;
            return -1.0;
        }

        StorageEngine::TagConfig tag_config;
        tag_config.tag_id = 5000;
        tag_config.value_type = ValueType::VT_F64;
        tag_config.time_unit = TimeUnit::TU_MS;
        tag_config.encoding_type = EncodingType::ENC_RAW;

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < num_points; i++) {
            engine.writePoint(&tag_config, base_time + i * 1000, 50.0 + i, 192);
        }
        engine.flush();

        auto end = std::chrono::high_resolution_clock::now();
        engine.close();

        std::chrono::duration<double, std::milli> elapsed = end - start;
        return elapsed.count();
    };

    std::cout << "  Testing block device/file performance..." << std::endl;
    double block_time = measureWriteTime(ContainerType::BLOCK_DEVICE, test_device_, use_test_mode_);
    std::cout << "    " << (use_test_mode_ ? "Simulated file" : "Block device") << ": "
              << block_time << " ms for " << num_points << " points" << std::endl;
    std::cout << "    Throughput: " << (num_points / (block_time / 1000.0)) << " points/sec" << std::endl;

    std::cout << "  Testing file-based performance..." << std::endl;
    double file_time = measureWriteTime(ContainerType::FILE_BASED, "", false);
    std::cout << "    File-based: " << file_time << " ms for " << num_points << " points" << std::endl;
    std::cout << "    Throughput: " << (num_points / (file_time / 1000.0)) << " points/sec" << std::endl;

    if (block_time > 0 && file_time > 0) {
        double ratio = file_time / block_time;
        std::cout << "  Performance ratio (file/" << (use_test_mode_ ? "simulated" : "block") << "): "
                  << ratio << "x" << std::endl;
    }
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
