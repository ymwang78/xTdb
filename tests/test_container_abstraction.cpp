#include <gtest/gtest.h>
#include "xTdb/container.h"
#include "xTdb/file_container.h"
#include "xTdb/block_device_container.h"
#include "xTdb/container_manager.h"
#include "test_utils.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <chrono>

using namespace xtdb;
namespace fs = std::filesystem;

class ContainerAbstractionTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use cross-platform temp directory
        std::string temp_dir = get_temp_dir();
        test_dir_ = join_path(temp_dir, "xtdb_container_test");
        
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Setup default layout
        layout_.block_size_bytes = 16384;
        layout_.chunk_size_bytes = 256 * 1024 * 1024;
        layout_.meta_blocks = 0;
        layout_.data_blocks = 0;
    }

    void TearDown() override {
        // On Windows, file handles may be held briefly after close
        // Wait a bit before attempting to remove directory
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Try multiple times to remove directory (Windows may hold files briefly)
        for (int i = 0; i < 3; i++) {
            if (fs::exists(test_dir_)) {
                try {
                    fs::remove_all(test_dir_);
                    break;
                } catch (const fs::filesystem_error& e) {
                    if (i < 2) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                }
            }
        }
    }

    std::string test_dir_;
    ChunkLayout layout_;
};

// Test 1: FileContainer Basic Operations
TEST_F(ContainerAbstractionTest, FileContainerBasicOperations) {
    std::cout << "\n=== Test 1: FileContainer Basic Operations ===" << std::endl;

    std::string container_path = join_path(test_dir_, "file_container.raw");

    // Create file container
    FileContainer container(container_path, layout_, false, false);

    // Open container
    ContainerResult result = container.open(true);
    ASSERT_EQ(ContainerResult::SUCCESS, result);
    ASSERT_TRUE(container.isOpen());
    ASSERT_EQ(ContainerType::FILE_BASED, container.getType());
    ASSERT_EQ(container_path, container.getIdentifier());

    std::cout << "  Container opened successfully" << std::endl;
    std::cout << "  Capacity: " << (container.getCapacity() / (1024*1024)) << " MB" << std::endl;

    // Write data
    AlignedBuffer write_buf(kExtentSizeBytes);
    std::memset(write_buf.data(), 0xAB, kExtentSizeBytes);
    result = container.write(write_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    std::cout << "  Written " << kExtentSizeBytes << " bytes" << std::endl;

    // Read data back
    AlignedBuffer read_buf(kExtentSizeBytes);
    result = container.read(read_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    // Verify data
    ASSERT_EQ(0, std::memcmp(write_buf.data(), read_buf.data(), kExtentSizeBytes));
    std::cout << "  Data verified successfully" << std::endl;

    // Sync
    result = container.sync();
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    // Check statistics
    const auto& stats = container.getStats();
    ASSERT_EQ(kExtentSizeBytes, stats.bytes_written);
    ASSERT_EQ(kExtentSizeBytes, stats.bytes_read);
    ASSERT_EQ(1u, stats.write_operations);
    ASSERT_EQ(1u, stats.read_operations);
    ASSERT_EQ(1u, stats.sync_operations);

    std::cout << "  Statistics: " << stats.write_operations << " writes, "
              << stats.read_operations << " reads" << std::endl;

    // Close
    container.close();
    ASSERT_FALSE(container.isOpen());

    std::cout << "  Container closed successfully" << std::endl;
}

// Test 2: ContainerFactory
TEST_F(ContainerAbstractionTest, ContainerFactory) {
    std::cout << "\n=== Test 2: ContainerFactory ===" << std::endl;

    std::string container_path = join_path(test_dir_, "factory_container.raw");

    // Create container config
    ContainerConfig config;
    config.type = ContainerType::FILE_BASED;
    config.path = container_path;
    config.layout = layout_;
    config.create_if_not_exists = true;
    config.direct_io = false;
    config.read_only = false;

    // Validate config
    ASSERT_TRUE(ContainerFactory::validateConfig(config));

    // Create container using factory
    auto container = ContainerFactory::create(config);
    ASSERT_NE(nullptr, container);
    ASSERT_TRUE(container->isOpen());

    std::cout << "  Container created via factory" << std::endl;
    std::cout << "  Type: " << (container->getType() == ContainerType::FILE_BASED ? "FILE_BASED" : "BLOCK_DEVICE") << std::endl;

    // Perform I/O
    AlignedBuffer write_buf(kExtentSizeBytes);
    std::memset(write_buf.data(), 0xCD, kExtentSizeBytes);
    ContainerResult result = container->write(write_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    AlignedBuffer read_buf(kExtentSizeBytes);
    result = container->read(read_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    ASSERT_EQ(0, std::memcmp(write_buf.data(), read_buf.data(), kExtentSizeBytes));

    std::cout << "  I/O operations successful" << std::endl;

    container->close();
}

// Test 3: ContainerManager Single Container
TEST_F(ContainerAbstractionTest, ContainerManagerSingleContainer) {
    std::cout << "\n=== Test 3: ContainerManager Single Container ===" << std::endl;

    std::string container_path = join_path(test_dir_, "manager_container.raw");

    // Create manager config
    ManagerConfig manager_config;
    ContainerConfig container_config;
    container_config.type = ContainerType::FILE_BASED;
    container_config.path = container_path;
    container_config.layout = layout_;
    container_config.create_if_not_exists = true;
    manager_config.containers.push_back(container_config);
    manager_config.rollover_strategy = RolloverStrategy::NONE;

    // Create manager
    ContainerManager manager(manager_config);
    ManagerResult result = manager.initialize();
    ASSERT_EQ(ManagerResult::SUCCESS, result);
    ASSERT_TRUE(manager.isInitialized());
    ASSERT_EQ(1u, manager.getContainerCount());

    std::cout << "  Manager initialized with " << manager.getContainerCount() << " container" << std::endl;

    // Get writable container
    IContainer* writable = manager.getWritableContainer();
    ASSERT_NE(nullptr, writable);
    ASSERT_FALSE(writable->isReadOnly());

    std::cout << "  Writable container obtained" << std::endl;

    // Perform I/O
    AlignedBuffer write_buf(kExtentSizeBytes);
    std::memset(write_buf.data(), 0xEF, kExtentSizeBytes);
    ContainerResult io_result = writable->write(write_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
    ASSERT_EQ(ContainerResult::SUCCESS, io_result);

    std::cout << "  Write operation successful" << std::endl;

    // Get all containers
    auto all_containers = manager.getAllContainers();
    ASSERT_EQ(1u, all_containers.size());

    std::cout << "  Retrieved " << all_containers.size() << " container(s)" << std::endl;

    // Check statistics
    ContainerStats total_stats = manager.getTotalStats();
    ASSERT_EQ(kExtentSizeBytes, total_stats.bytes_written);

    std::cout << "  Total bytes written: " << total_stats.bytes_written << std::endl;

    manager.close();
    ASSERT_FALSE(manager.isInitialized());
}

// Test 4: ContainerManager Multiple Containers
TEST_F(ContainerAbstractionTest, ContainerManagerMultipleContainers) {
    std::cout << "\n=== Test 4: ContainerManager Multiple Containers ===" << std::endl;

    // Create manager config with 3 containers
    ManagerConfig manager_config;
    for (int i = 0; i < 3; i++) {
        ContainerConfig container_config;
        container_config.type = ContainerType::FILE_BASED;
        container_config.path = join_path(test_dir_, "multi_container_" + std::to_string(i) + ".raw");
        container_config.layout = layout_;
        container_config.create_if_not_exists = true;
        container_config.read_only = (i < 2);  // First 2 are read-only
        manager_config.containers.push_back(container_config);
    }
    manager_config.rollover_strategy = RolloverStrategy::NONE;

    // Create manager
    ContainerManager manager(manager_config);
    ManagerResult result = manager.initialize();
    ASSERT_EQ(ManagerResult::SUCCESS, result);
    ASSERT_EQ(3u, manager.getContainerCount());

    std::cout << "  Manager initialized with " << manager.getContainerCount() << " containers" << std::endl;

    // Get writable container (should be the last one)
    IContainer* writable = manager.getWritableContainer();
    ASSERT_NE(nullptr, writable);
    ASSERT_FALSE(writable->isReadOnly());
    ASSERT_EQ(2u, manager.getActiveContainerIndex());

    std::cout << "  Active container index: " << manager.getActiveContainerIndex() << std::endl;

    // Get all containers
    auto all_containers = manager.getAllContainers();
    ASSERT_EQ(3u, all_containers.size());

    // Verify read-only status
    ASSERT_TRUE(all_containers[0]->isReadOnly());
    ASSERT_TRUE(all_containers[1]->isReadOnly());
    ASSERT_FALSE(all_containers[2]->isReadOnly());

    std::cout << "  Container 0: " << (all_containers[0]->isReadOnly() ? "READ-ONLY" : "WRITABLE") << std::endl;
    std::cout << "  Container 1: " << (all_containers[1]->isReadOnly() ? "READ-ONLY" : "WRITABLE") << std::endl;
    std::cout << "  Container 2: " << (all_containers[2]->isReadOnly() ? "READ-ONLY" : "WRITABLE") << std::endl;

    manager.close();
}

// Test 5: Container Metadata
TEST_F(ContainerAbstractionTest, ContainerMetadata) {
    std::cout << "\n=== Test 5: Container Metadata ===" << std::endl;

    std::string container_path = join_path(test_dir_, "metadata_container.raw");

    FileContainer container(container_path, layout_, false, false);
    ContainerResult result = container.open(true);
    ASSERT_EQ(ContainerResult::SUCCESS, result);

    // Get metadata
    const auto& metadata = container.getMetadata();
    ASSERT_EQ(ContainerLayout::LAYOUT_RAW_FIXED, metadata.layout);
    ASSERT_EQ(CapacityType::CAP_DYNAMIC, metadata.capacity_type);
    ASSERT_EQ(ArchiveLevel::ARCHIVE_RAW, metadata.archive_level);
    ASSERT_GT(metadata.capacity_extents, 0u);
    ASSERT_GT(metadata.capacity_bytes, 0u);
    ASSERT_EQ(layout_.chunk_size_bytes / kExtentSizeBytes, metadata.chunk_size_extents);
    ASSERT_EQ(layout_.block_size_bytes / kExtentSizeBytes, metadata.block_size_extents);

    std::cout << "  Metadata:" << std::endl;
    std::cout << "    Layout: RAW_FIXED" << std::endl;
    std::cout << "    Capacity Type: CAP_DYNAMIC" << std::endl;
    std::cout << "    Archive Level: ARCHIVE_RAW" << std::endl;
    std::cout << "    Capacity: " << (metadata.capacity_bytes / (1024*1024)) << " MB" << std::endl;
    std::cout << "    Chunk size: " << metadata.chunk_size_extents << " extents" << std::endl;
    std::cout << "    Block size: " << metadata.block_size_extents << " extents" << std::endl;

    container.close();
}

// Test 6: Container Persistence
TEST_F(ContainerAbstractionTest, ContainerPersistence) {
    std::cout << "\n=== Test 6: Container Persistence ===" << std::endl;

    std::string container_path = join_path(test_dir_, "persistence_container.raw");

    // Create and write data
    {
        FileContainer container(container_path, layout_, false, false);
        ContainerResult result = container.open(true);
        ASSERT_EQ(ContainerResult::SUCCESS, result);

        AlignedBuffer write_buf(kExtentSizeBytes);
        std::memset(write_buf.data(), 0x42, kExtentSizeBytes);
        result = container.write(write_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
        ASSERT_EQ(ContainerResult::SUCCESS, result);

        // Verify data was written before closing
        AlignedBuffer verify_buf(kExtentSizeBytes);
        result = container.read(verify_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
        ASSERT_EQ(ContainerResult::SUCCESS, result);
        ASSERT_EQ(0, std::memcmp(write_buf.data(), verify_buf.data(), kExtentSizeBytes))
            << "Data should match immediately after write";
        std::cout << "  Data verified before close" << std::endl;

        // Sync multiple times to ensure data is flushed on Windows
        result = container.sync();
        ASSERT_EQ(ContainerResult::SUCCESS, result);
        result = container.sync();  // Second sync to ensure data is persisted
        ASSERT_EQ(ContainerResult::SUCCESS, result);
        
        container.close();

        std::cout << "  Data written and container closed" << std::endl;
        
        // On Windows, file handles may be released asynchronously
        // Wait a brief moment to ensure the file is fully closed
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Verify file exists before reopening
        ASSERT_TRUE(fs::exists(container_path)) << "Container file should exist: " << container_path;
        std::cout << "  Container file exists: " << container_path << std::endl;
    }

    // Reopen and verify data
    {
        // Double-check file exists
        if (!fs::exists(container_path)) {
            FAIL() << "Container file does not exist before reopen: " << container_path;
        }
        
        // On Windows, may need a small delay before reopening
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
        FileContainer container(container_path, layout_, false, false);
        ContainerResult result = container.open(false);  // Don't create, must exist
        if (result != ContainerResult::SUCCESS) {
            FAIL() << "Failed to reopen container: " << container.getLastError() 
                   << " (file exists: " << (fs::exists(container_path) ? "yes" : "no") << ")";
        }
        ASSERT_EQ(ContainerResult::SUCCESS, result);

        AlignedBuffer read_buf(kExtentSizeBytes);
        result = container.read(read_buf.data(), kExtentSizeBytes, kExtentSizeBytes);
        if (result != ContainerResult::SUCCESS) {
            FAIL() << "Failed to read container data: " << container.getLastError();
        }
        ASSERT_EQ(ContainerResult::SUCCESS, result);

        // Verify data - check first few bytes
        uint8_t* data = static_cast<uint8_t*>(read_buf.data());
        std::cout << "  First 16 bytes read: ";
        for (size_t i = 0; i < 16 && i < kExtentSizeBytes; i++) {
            std::cout << std::hex << static_cast<int>(data[i]) << " ";
        }
        std::cout << std::dec << std::endl;
        
        for (size_t i = 0; i < kExtentSizeBytes; i++) {
            ASSERT_EQ(0x42, data[i]) << "Mismatch at offset " << i;
        }

        std::cout << "  Data verified after reopen" << std::endl;

        container.close();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
