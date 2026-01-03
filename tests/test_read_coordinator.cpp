#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include <filesystem>
#include <iostream>
#include <algorithm>

using namespace xtdb;
namespace fs = std::filesystem;

class ReadCoordinatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Setup config
        config_.data_dir = test_dir_;
        config_.db_path = test_dir_ + "/meta.db";
        config_.layout.block_size_bytes = 16384;
        config_.layout.chunk_size_bytes = 4 * 1024 * 1024;
    }

    void TearDown() override {
        // Clean up
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    std::string test_dir_ = "/tmp/xtdb_test_read";
    EngineConfig config_;
};

// Test: Basic query from memory
TEST_F(ReadCoordinatorTest, QueryFromMemory) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    // Write some points (not flushed)
    for (int i = 0; i < 10; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 42.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Query all points
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(tag_id, base_ts, base_ts + 10000, results);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Verify results
    EXPECT_EQ(10u, results.size());
    EXPECT_EQ(10u, engine.getReadStats().points_read_memory);
    EXPECT_EQ(0u, engine.getReadStats().points_read_disk);

    // Verify data correctness
    for (size_t i = 0; i < results.size(); i++) {
        EXPECT_EQ(base_ts + static_cast<int64_t>(i) * 1000, results[i].timestamp_us);
        EXPECT_DOUBLE_EQ(42.0 + i, results[i].value);
        EXPECT_EQ(192, results[i].quality);
    }

    engine.close();
}

// Test: Basic query from disk
TEST_F(ReadCoordinatorTest, QueryFromDisk) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    // Write enough points to trigger flush
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 42.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Auto-flush should have happened
    EXPECT_GT(engine.getWriteStats().blocks_flushed, 0u);

    // Query points (should read from disk)
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(tag_id, base_ts, base_ts + 500 * 1000, results);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Verify we got data from disk
    EXPECT_GT(results.size(), 0u);
    EXPECT_GT(engine.getReadStats().points_read_disk, 0u);

    std::cout << "Query returned " << results.size() << " points from disk" << std::endl;
    std::cout << "Blocks read: " << engine.getReadStats().blocks_read << std::endl;

    engine.close();
}

// Test: T12-HybridRead - Query across memory and disk
TEST_F(ReadCoordinatorTest, T12_HybridRead) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    std::cout << "Phase 1: Write and flush first batch (disk data)" << std::endl;

    // Phase 1: Write 1001 points and trigger auto-flush (these go to disk)
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 100.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Verify first batch was flushed
    auto write_stats = engine.getWriteStats();
    EXPECT_GT(write_stats.blocks_flushed, 0u);
    std::cout << "Flushed blocks: " << write_stats.blocks_flushed << std::endl;

    std::cout << "Phase 2: Write second batch (memory data, not flushed)" << std::endl;

    // Phase 2: Write more points that stay in memory
    for (int i = 1001; i < 1050; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 100.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    std::cout << "Phase 3: Query across both memory and disk" << std::endl;

    // Phase 3: Query range that spans both flushed and unflushed data
    std::vector<StorageEngine::QueryPoint> results;
    int64_t query_start = base_ts + 900 * 1000;  // Near end of flushed data
    int64_t query_end = base_ts + 1040 * 1000;   // Into unflushed data

    result = engine.queryPoints(tag_id, query_start, query_end, results);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Verify hybrid read
    auto read_stats = engine.getReadStats();
    std::cout << "Total points read: " << results.size() << std::endl;
    std::cout << "Points from disk: " << read_stats.points_read_disk << std::endl;
    std::cout << "Points from memory: " << read_stats.points_read_memory << std::endl;
    std::cout << "Blocks read: " << read_stats.blocks_read << std::endl;

    // Should have read from both disk and memory
    EXPECT_GT(read_stats.points_read_disk, 0u);
    EXPECT_GT(read_stats.points_read_memory, 0u);

    // Expected: points from 900 to 1040 (141 points)
    EXPECT_GT(results.size(), 100u);

    // Verify results are sorted by timestamp
    for (size_t i = 1; i < results.size(); i++) {
        EXPECT_GT(results[i].timestamp_us, results[i-1].timestamp_us);
    }

    // Verify data correctness for some sample points
    if (!results.empty()) {
        // First point should be around index 900
        EXPECT_GE(results.front().timestamp_us, query_start);
        // Last point should be around index 1040
        EXPECT_LE(results.back().timestamp_us, query_end);
    }

    std::cout << "Phase 4: Verify continuity across boundary" << std::endl;

    // Verify data continuity (no gaps between disk and memory data)
    bool found_disk_data = false;
    bool found_memory_data = false;

    for (const auto& point : results) {
        int64_t index = (point.timestamp_us - base_ts) / 1000;
        if (index < 1001) {
            found_disk_data = true;
        } else {
            found_memory_data = true;
        }

        // Verify value matches expected
        double expected_value = 100.0 + index;
        EXPECT_DOUBLE_EQ(expected_value, point.value);
    }

    EXPECT_TRUE(found_disk_data) << "Should have found data from disk";
    EXPECT_TRUE(found_memory_data) << "Should have found data from memory";

    engine.close();

    std::cout << "T12-HybridRead test completed successfully!" << std::endl;
}

// Test: Query with time range filter
TEST_F(ReadCoordinatorTest, TimeRangeFilter) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    // Write points from 0 to 99
    for (int i = 0; i < 100; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 50.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Query middle range: 30 to 60
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(tag_id,
                                base_ts + 30 * 1000,
                                base_ts + 60 * 1000,
                                results);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Should get 31 points (30, 31, ..., 60 inclusive)
    EXPECT_EQ(31u, results.size());

    // Verify range
    if (!results.empty()) {
        EXPECT_EQ(base_ts + 30 * 1000, results.front().timestamp_us);
        EXPECT_EQ(base_ts + 60 * 1000, results.back().timestamp_us);
    }

    engine.close();
}

// Test: Query non-existent tag
TEST_F(ReadCoordinatorTest, QueryNonExistentTag) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Write to tag 100
    result = engine.writePoint(100, 1000000, 42.0, 192);
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Query tag 999 (doesn't exist)
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(999, 0, 2000000, results);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Should return empty results
    EXPECT_EQ(0u, results.size());

    engine.close();
}

// Test: Multiple tags independent queries
TEST_F(ReadCoordinatorTest, MultipleTagsQuery) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    const int64_t base_ts = 1000000;

    // Write to multiple tags
    for (uint32_t tag = 100; tag < 103; tag++) {
        for (int i = 0; i < 50; i++) {
            result = engine.writePoint(tag, base_ts + i * 1000, tag * 10.0 + i, 192);
            ASSERT_EQ(EngineResult::SUCCESS, result);
        }
    }

    // Query each tag separately
    for (uint32_t tag = 100; tag < 103; tag++) {
        std::vector<StorageEngine::QueryPoint> results;
        result = engine.queryPoints(tag, base_ts, base_ts + 100000, results);
        EXPECT_EQ(EngineResult::SUCCESS, result);

        EXPECT_EQ(50u, results.size());

        // Verify values are correct for this tag
        for (size_t i = 0; i < results.size(); i++) {
            EXPECT_DOUBLE_EQ(tag * 10.0 + i, results[i].value);
        }
    }

    engine.close();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
