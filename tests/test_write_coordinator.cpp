#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "test_utils.h"
#include <filesystem>
#include <cstring>
#include <thread>
#include <chrono>

using namespace xtdb;
namespace fs = std::filesystem;

class WriteCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use cross-platform temp directory
        std::string temp_dir = get_temp_dir();
        test_dir_ = join_path(temp_dir, "xtdb_test_write");
        
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Setup config with small chunk for testing
        config_.data_dir = test_dir_;
        config_.db_path = join_path(test_dir_, "meta.db");
        config_.layout.block_size_bytes = 16384;
        // Use smaller chunk size for faster testing (4MB instead of 256MB)
        config_.layout.chunk_size_bytes = 4 * 1024 * 1024;
    }

    void TearDown() override {
        // Clean up - wait a bit on Windows to ensure files are closed
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
    EngineConfig config_;
};

// Test: Basic write and flush
TEST_F(WriteCoordinatorTest, BasicWriteFlush) {
    StorageEngine engine(config_);

    // Open engine
    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Write single point
    result = engine.writePoint(100, 1000000, 42.5, 192);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Check stats
    const auto& stats = engine.getWriteStats();
    EXPECT_EQ(1u, stats.points_written);

    // Explicit flush
    result = engine.flush();
    EXPECT_EQ(EngineResult::SUCCESS, result);
    EXPECT_GT(stats.blocks_flushed, 0u);

    engine.close();
}

// Test: Buffer threshold auto-flush
TEST_F(WriteCoordinatorTest, BufferThresholdFlush) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Write enough points to trigger auto-flush
    // Threshold is 1000 records per buffer
    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(tag_id, base_ts + i, 42.5 + i, 192);
        EXPECT_EQ(EngineResult::SUCCESS, result);
    }

    // Check that auto-flush happened
    const auto& stats = engine.getWriteStats();
    EXPECT_EQ(1001u, stats.points_written);
    EXPECT_GT(stats.blocks_flushed, 0u);

    engine.close();
}

// Test: T11-AutoRolling - Chunk switching when full
TEST_F(WriteCoordinatorTest, T11_AutoRolling) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Get initial chunk info
    const auto& initial_chunk = engine.getActiveChunk();
    uint32_t initial_chunk_id = initial_chunk.chunk_id;
    uint32_t blocks_total = initial_chunk.blocks_total;

    std::cout << "Initial chunk ID: " << initial_chunk_id << std::endl;
    std::cout << "Blocks total: " << blocks_total << std::endl;

    // Write enough data to fill the chunk
    // Each block is 16KB, we need to fill all data blocks
    const uint32_t tag_id = 100;
    int64_t timestamp = 1000000;

    // Write blocks_total + 1 blocks worth of data to trigger roll
    for (uint32_t block_num = 0; block_num <= blocks_total; block_num++) {
        // Write 700 points per block (~16KB)
        for (int i = 0; i < 700; i++) {
            result = engine.writePoint(tag_id, timestamp++, 42.5 + i, 192);
            if (result != EngineResult::SUCCESS) {
                std::cerr << "Write failed at block " << block_num
                         << ", point " << i << std::endl;
            }
            EXPECT_EQ(EngineResult::SUCCESS, result);
        }

        // Explicit flush after each block
        result = engine.flush();
        EXPECT_EQ(EngineResult::SUCCESS, result);

        std::cout << "Wrote block " << block_num << std::endl;
    }

    // Check statistics
    const auto& stats = engine.getWriteStats();
    std::cout << "Points written: " << stats.points_written << std::endl;
    std::cout << "Blocks flushed: " << stats.blocks_flushed << std::endl;
    std::cout << "Chunks sealed: " << stats.chunks_sealed << std::endl;
    std::cout << "Chunks allocated: " << stats.chunks_allocated << std::endl;

    // Verify that at least one chunk was sealed and a new one allocated
    EXPECT_GT(stats.chunks_sealed, 0u);
    EXPECT_GT(stats.chunks_allocated, 0u);

    // Verify new chunk ID
    const auto& final_chunk = engine.getActiveChunk();
    EXPECT_GT(final_chunk.chunk_id, initial_chunk_id);
    std::cout << "Final chunk ID: " << final_chunk.chunk_id << std::endl;

    engine.close();
}

// Test: Multiple tags with independent buffers
TEST_F(WriteCoordinatorTest, MultipleTagBuffers) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Write to multiple tags
    const int num_tags = 5;
    const int points_per_tag = 100;
    int64_t base_ts = 1000000;

    for (int tag = 0; tag < num_tags; tag++) {
        for (int i = 0; i < points_per_tag; i++) {
            result = engine.writePoint(100 + tag, base_ts + i, 42.5 + i, 192);
            EXPECT_EQ(EngineResult::SUCCESS, result);
        }
    }

    // Check stats
    const auto& stats = engine.getWriteStats();
    EXPECT_EQ(static_cast<uint64_t>(num_tags * points_per_tag), stats.points_written);

    // Flush all buffers
    result = engine.flush();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    engine.close();
}

// Test: Write after engine restart
TEST_F(WriteCoordinatorTest, WriteAfterRestart) {
    // First session: write and close
    {
        StorageEngine engine(config_);
        EngineResult result = engine.open();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        result = engine.writePoint(100, 1000000, 42.5, 192);
        EXPECT_EQ(EngineResult::SUCCESS, result);

        result = engine.flush();
        EXPECT_EQ(EngineResult::SUCCESS, result);

        engine.close();
    }

    // Second session: reopen and write more
    {
        StorageEngine engine(config_);
        EngineResult result = engine.open();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        // Should be able to write more data
        result = engine.writePoint(100, 2000000, 43.5, 192);
        EXPECT_EQ(EngineResult::SUCCESS, result);

        result = engine.flush();
        EXPECT_EQ(EngineResult::SUCCESS, result);

        const auto& stats = engine.getWriteStats();
        EXPECT_GT(stats.points_written, 0u);

        engine.close();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
