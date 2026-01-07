#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include <filesystem>
#include <cstring>
#include <thread>
#include <chrono>

using namespace xtdb;
namespace fs = std::filesystem;

class MaintenanceServiceTest : public ::testing::Test {
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
        config_.layout.chunk_size_bytes = 4 * 1024 * 1024;  // 4MB for faster testing
        config_.retention_days = 7;  // 7 days retention
    }

    void TearDown() override {
        // Clean up
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    std::string test_dir_ = "/tmp/xtdb_test_maintenance";
    EngineConfig config_;
};

// Test: Basic retention service configuration
TEST_F(MaintenanceServiceTest, RetentionConfiguration) {
    StorageEngine engine(config_);
    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Verify configuration
    EXPECT_EQ(7, config_.retention_days);

    // Run retention service (should succeed even with no data)
    result = engine.runRetentionService();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    engine.close();
}

// Test: No retention when retention_days = 0
TEST_F(MaintenanceServiceTest, NoRetentionWhenDisabled) {
    config_.retention_days = 0;  // Disable retention
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Write some old data
    result = engine.writePoint(100, 1000000, 42.5, 192);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    result = engine.flush();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Run retention service
    result = engine.runRetentionService();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // No chunks should be deprecated (retention disabled)
    const auto& stats = engine.getMaintenanceStats();
    EXPECT_EQ(0u, stats.chunks_deprecated);

    engine.close();
}

// Test: Retention service with old data
TEST_F(MaintenanceServiceTest, T13_RetentionService) {
    config_.retention_days = 1;  // 1 day retention for testing
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Get current time
    auto now = std::chrono::system_clock::now();
    int64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();

    // Write old data (3 days ago, should be expired)
    int64_t old_time_us = current_time_us - (3LL * 24 * 3600 * 1000000);  // 3 days ago

    // Write 1001 points to trigger flush
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(100, old_time_us + i * 1000, 100.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Explicitly seal the chunk (automatic sealing only happens when chunk is full)
    result = engine.sealCurrentChunk();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Verify chunk was sealed
    auto write_stats = engine.getWriteStats();
    EXPECT_GT(write_stats.blocks_flushed, 0u);
    EXPECT_GT(write_stats.chunks_sealed, 0u);

    std::cout << "Chunks sealed: " << write_stats.chunks_sealed << std::endl;

    // Run retention service with current time
    result = engine.runRetentionService(current_time_us);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Check statistics
    const auto& maint_stats = engine.getMaintenanceStats();
    std::cout << "Chunks deprecated: " << maint_stats.chunks_deprecated << std::endl;
    std::cout << "Last retention run: " << maint_stats.last_retention_run_ts << std::endl;

    // At least one chunk should be deprecated
    EXPECT_GT(maint_stats.chunks_deprecated, 0u);
    EXPECT_EQ(current_time_us, maint_stats.last_retention_run_ts);

    engine.close();
}

// Test: Recent data is not deleted
TEST_F(MaintenanceServiceTest, RecentDataNotDeleted) {
    config_.retention_days = 7;  // 7 days retention
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Get current time
    auto now = std::chrono::system_clock::now();
    int64_t current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()
    ).count();

    // Write recent data (today)
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(100, current_time_us + i * 1000, 100.0 + i, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);
    }

    // Explicitly seal the chunk
    result = engine.sealCurrentChunk();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Verify chunk was sealed
    auto write_stats = engine.getWriteStats();
    EXPECT_GT(write_stats.chunks_sealed, 0u);

    // Run retention service
    result = engine.runRetentionService(current_time_us);
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // No chunks should be deprecated (data is recent)
    const auto& maint_stats = engine.getMaintenanceStats();
    EXPECT_EQ(0u, maint_stats.chunks_deprecated);

    engine.close();
}

// Test: Reclaim deprecated chunks
TEST_F(MaintenanceServiceTest, ReclaimDeprecatedChunks) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Run reclaim service (should succeed even with no deprecated chunks)
    result = engine.reclaimDeprecatedChunks();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    const auto& stats = engine.getMaintenanceStats();
    std::cout << "Chunks freed: " << stats.chunks_freed << std::endl;

    engine.close();
}

// Test: Maintenance statistics tracking
TEST_F(MaintenanceServiceTest, MaintenanceStatistics) {
    StorageEngine engine(config_);

    EngineResult result = engine.open();
    ASSERT_EQ(EngineResult::SUCCESS, result);

    // Get initial statistics
    const auto& stats1 = engine.getMaintenanceStats();
    EXPECT_EQ(0u, stats1.chunks_deprecated);
    EXPECT_EQ(0u, stats1.chunks_freed);
    EXPECT_EQ(0u, stats1.last_retention_run_ts);

    // Run retention service
    result = engine.runRetentionService();
    EXPECT_EQ(EngineResult::SUCCESS, result);

    // Statistics should be updated
    const auto& stats2 = engine.getMaintenanceStats();
    EXPECT_GT(stats2.last_retention_run_ts, 0u);

    engine.close();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
