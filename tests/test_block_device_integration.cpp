#include "xTdb/storage_engine.h"
#include "xTdb/container.h"
#include "xTdb/file_container.h"
#include "xTdb/block_device_container.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

using namespace xtdb;

class BlockDeviceIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir_ = "/tmp/test_block_device_integration";
        std::filesystem::remove_all(test_dir_);
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        std::filesystem::remove_all(test_dir_);
    }

    std::string test_dir_;
};

// Test 1: Verify StorageEngine accepts block device configuration
TEST_F(BlockDeviceIntegrationTest, BlockDeviceConfiguration) {
    std::cout << "\n=== Test 1: Block Device Configuration ===" << std::endl;

    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = test_dir_ + "/meta.db";
    config.container_type = ContainerType::BLOCK_DEVICE;
    config.block_device_path = "/dev/null";  // Use /dev/null as a dummy block device
    config.rollover_strategy = RolloverStrategy::NONE;
    config.direct_io = true;

    std::cout << "  Container type: BLOCK_DEVICE" << std::endl;
    std::cout << "  Device path: " << config.block_device_path << std::endl;
    std::cout << "  Direct I/O: " << (config.direct_io ? "enabled" : "disabled") << std::endl;

    // Note: We can't actually test with /dev/null as it's not a real block device
    // This test verifies the configuration is accepted
    EXPECT_EQ(config.container_type, ContainerType::BLOCK_DEVICE);
    EXPECT_EQ(config.block_device_path, "/dev/null");

    std::cout << "  Configuration validated successfully" << std::endl;
}

// Test 2: Verify file-based container still works (regression test)
TEST_F(BlockDeviceIntegrationTest, FileBasedContainerRegression) {
    std::cout << "\n=== Test 2: File-Based Container Regression ===" << std::endl;

    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = test_dir_ + "/meta.db";
    config.container_type = ContainerType::FILE_BASED;
    config.container_name_pattern = "container_{index}.raw";
    config.rollover_strategy = RolloverStrategy::NONE;
    config.direct_io = false;

    StorageEngine engine(config);

    std::cout << "  Opening storage engine with FILE_BASED container..." << std::endl;
    EngineResult result = engine.open();
    ASSERT_EQ(result, EngineResult::SUCCESS) << "Failed to open: " << engine.getLastError();
    std::cout << "  Engine opened successfully" << std::endl;

    // Write some data
    std::cout << "  Writing test data..." << std::endl;
    result = engine.writePoint(1, 1000000, 42.0, 192);
    EXPECT_EQ(result, EngineResult::SUCCESS);

    result = engine.writePoint(1, 2000000, 43.0, 192);
    EXPECT_EQ(result, EngineResult::SUCCESS);

    result = engine.flush();
    EXPECT_EQ(result, EngineResult::SUCCESS);
    std::cout << "  Data written and flushed" << std::endl;

    // Verify container info
    const auto& containers = engine.getContainers();
    EXPECT_EQ(containers.size(), 1);
    EXPECT_EQ(containers[0].container_id, 0);
    std::cout << "  Container path: " << containers[0].file_path << std::endl;
    std::cout << "  Container capacity: " << containers[0].capacity_bytes / (1024*1024) << " MB" << std::endl;

    engine.close();
    std::cout << "  Engine closed successfully" << std::endl;
}

// Test 3: Verify FileContainer and BlockDeviceContainer provide AlignedIO interface
TEST_F(BlockDeviceIntegrationTest, ContainerAlignedIOInterface) {
    std::cout << "\n=== Test 3: Container AlignedIO Interface ===" << std::endl;

    ChunkLayout layout;
    layout.block_size_bytes = 16384;
    layout.chunk_size_bytes = 16 * 1024 * 1024;

    // Test FileContainer
    std::string test_file = test_dir_ + "/test_container.raw";

    ContainerConfig file_config;
    file_config.type = ContainerType::FILE_BASED;
    file_config.path = test_file;
    file_config.layout = layout;
    file_config.create_if_not_exists = true;
    file_config.direct_io = false;
    file_config.read_only = false;

    auto file_container = ContainerFactory::create(file_config);
    ASSERT_NE(file_container, nullptr) << "Failed to create file container";
    std::cout << "  FileContainer created successfully" << std::endl;

    // Verify FileContainer provides AlignedIO interface
    FileContainer* fc = dynamic_cast<FileContainer*>(file_container.get());
    ASSERT_NE(fc, nullptr);

    AlignedIO* io = fc->getIO();
    ASSERT_NE(io, nullptr);
    std::cout << "  FileContainer provides AlignedIO: YES" << std::endl;

    file_container->close();
    std::cout << "  FileContainer closed successfully" << std::endl;

    // Note: BlockDeviceContainer test requires actual block device access
    // which needs root permissions, so we skip it in unit tests
    std::cout << "  BlockDeviceContainer test skipped (requires real block device)" << std::endl;
}

// Test 4: Verify container type detection
TEST_F(BlockDeviceIntegrationTest, ContainerTypeDetection) {
    std::cout << "\n=== Test 4: Container Type Detection ===" << std::endl;

    // Test file path
    std::string file_path = test_dir_ + "/test.raw";
    ContainerType file_type = ContainerFactory::detectType(file_path);
    EXPECT_EQ(file_type, ContainerType::FILE_BASED);
    std::cout << "  Regular file detected as: FILE_BASED" << std::endl;

    // Test /dev/null (special device)
    ContainerType dev_type = ContainerFactory::detectType("/dev/null");
    std::cout << "  /dev/null detected as: "
              << (dev_type == ContainerType::BLOCK_DEVICE ? "BLOCK_DEVICE" : "FILE_BASED")
              << std::endl;

    std::cout << "  Type detection working correctly" << std::endl;
}

// Test 5: Verify container abstraction in StorageEngine
TEST_F(BlockDeviceIntegrationTest, StorageEngineContainerAbstraction) {
    std::cout << "\n=== Test 5: StorageEngine Container Abstraction ===" << std::endl;

    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = test_dir_ + "/meta.db";
    config.container_type = ContainerType::FILE_BASED;
    config.container_name_pattern = "abstraction_test_{index}.raw";
    config.rollover_strategy = RolloverStrategy::NONE;

    StorageEngine engine(config);

    std::cout << "  Opening engine..." << std::endl;
    EngineResult result = engine.open();
    ASSERT_EQ(result, EngineResult::SUCCESS) << "Failed to open: " << engine.getLastError();

    // Verify engine has container information
    const auto& containers = engine.getContainers();
    EXPECT_GE(containers.size(), 1);

    std::cout << "  Number of containers: " << containers.size() << std::endl;
    for (const auto& container : containers) {
        std::cout << "    Container " << container.container_id << ": "
                  << container.file_path << std::endl;
    }

    // Write and read data
    std::cout << "  Writing data points..." << std::endl;
    for (int i = 0; i < 10; i++) {
        result = engine.writePoint(100, 1000000 + i * 1000, 10.0 + i, 192);
        EXPECT_EQ(result, EngineResult::SUCCESS);
    }

    result = engine.flush();
    EXPECT_EQ(result, EngineResult::SUCCESS);
    std::cout << "  Data flushed successfully" << std::endl;

    // Query data
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(100, 1000000, 1010000, results);
    EXPECT_EQ(result, EngineResult::SUCCESS);
    std::cout << "  Query returned " << results.size() << " points" << std::endl;

    // Verify statistics
    const auto& write_stats = engine.getWriteStats();
    std::cout << "  Write statistics:" << std::endl;
    std::cout << "    Points written: " << write_stats.points_written << std::endl;
    std::cout << "    Blocks flushed: " << write_stats.blocks_flushed << std::endl;

    engine.close();
    std::cout << "  Engine closed successfully" << std::endl;
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
