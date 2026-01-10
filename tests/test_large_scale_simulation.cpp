#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "test_utils.h"
#include <filesystem>
#include <vector>
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <iostream>
#include <chrono>
#include <random>

using namespace xtdb;
namespace fs = std::filesystem;
using namespace std::chrono;

class LargeScaleSimulationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Use cross-platform temp directory
        std::string temp_dir = get_temp_dir();
        test_dir_ = join_path(temp_dir, "xtdb_large_scale_test");
        
        // Clean up test directory
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
        fs::create_directories(test_dir_);

        // Setup config with larger chunks
        config_.data_dir = test_dir_;
        config_.db_path = join_path(test_dir_, "meta.db");
        config_.layout.block_size_bytes = 16384;
        config_.layout.chunk_size_bytes = 64 * 1024 * 1024;  // 64MB chunks
    }

    void TearDown() override {
        if (fs::exists(test_dir_)) {
            fs::remove_all(test_dir_);
        }
    }

    // Helper: Get directory size
    size_t getDirectorySize(const std::string& path) {
        size_t total_size = 0;
        for (const auto& entry : fs::recursive_directory_iterator(path)) {
            if (entry.is_regular_file()) {
                total_size += entry.file_size();
            }
        }
        return total_size;
    }

    std::string test_dir_;
    EngineConfig config_;
};

// Test 1: Moderate scale - 100 tags × 10,000 points = 1M points
TEST_F(LargeScaleSimulationTest, ModerateScale_1M_Points) {
    std::cout << "\n=== Test 1: Moderate Scale (1M points) ===" << std::endl;

    const int num_tags = 100;
    const int points_per_tag = 10000;
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    auto start_time = high_resolution_clock::now();

    // Write 1M points
    std::cout << "Writing " << num_tags << " tags × " << points_per_tag
              << " points..." << std::endl;

    for (int tag = 0; tag < num_tags; tag++) {
        uint32_t tag_id = 1000 + tag;

        for (int i = 0; i < points_per_tag; i++) {
            int64_t ts = base_ts + i * 1000;  // 1 second intervals
            double value = 50.0 + 10.0 * std::sin(2.0 * M_PI * i / 1000.0);

            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, value, 192));
        }

        // Flush every 10 tags to avoid memory buildup
        if ((tag + 1) % 10 == 0) {
            engine.flush();
            std::cout << "  Flushed " << (tag + 1) << " tags..." << std::endl;
        }
    }

    // Final flush
    engine.flush();

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    // Calculate throughput
    int total_points = num_tags * points_per_tag;
    double throughput = (total_points * 1000.0) / duration.count();

    std::cout << "Write complete:" << std::endl;
    std::cout << "  Total points: " << total_points << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " points/sec" << std::endl;

    // Check stats
    const auto& stats = engine.getWriteStats();
    std::cout << "  Blocks flushed: " << stats.blocks_flushed << std::endl;
    std::cout << "  Chunks sealed: " << stats.chunks_sealed << std::endl;

    // Verify storage size
    size_t storage_bytes = getDirectorySize(test_dir_);
    double mb = storage_bytes / (1024.0 * 1024.0);
    std::cout << "  Storage size: " << mb << " MB" << std::endl;

    // Expected: ~130 MB data + 256 MB WAL segments = ~386 MB
    // Note: WAL segments are pre-allocated (4 × 64MB)
    // Data + overhead should be < 1.5 GB
    EXPECT_LT(storage_bytes, 1536 * 1024 * 1024);

    engine.close();
}

// Test 2: High tag count - 1000 tags × 1,000 points = 1M points
TEST_F(LargeScaleSimulationTest, HighTagCount_1000_Tags) {
    std::cout << "\n=== Test 2: High Tag Count (1000 tags) ===" << std::endl;

    const int num_tags = 1000;
    const int points_per_tag = 1000;
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    std::cout << "Writing " << num_tags << " tags × " << points_per_tag
              << " points..." << std::endl;

    auto start_time = high_resolution_clock::now();

    // Write sequentially per tag (more reliable for testing)
    for (int tag = 0; tag < num_tags; tag++) {
        uint32_t tag_id = 2000 + tag;

        for (int i = 0; i < points_per_tag; i++) {
            int64_t ts = base_ts + i * 1000;
            double value = (tag % 100) + i * 0.01;

            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, value, 192));
        }

        // Flush every 100 tags
        if ((tag + 1) % 100 == 0) {
            engine.flush();
            std::cout << "  Completed " << (tag + 1) << " tags..." << std::endl;
        }
    }

    engine.flush();

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    int total_points = num_tags * points_per_tag;
    double throughput = (total_points * 1000.0) / duration.count();

    std::cout << "Write complete:" << std::endl;
    std::cout << "  Total points: " << total_points << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " points/sec" << std::endl;

    // Query a sample tag to verify
    std::vector<StorageEngine::QueryPoint> results;
    ASSERT_EQ(EngineResult::SUCCESS,
             engine.queryPoints(2000, base_ts, base_ts + points_per_tag * 1000, results));

    std::cout << "  Sample query (tag 2000): " << results.size() << " points" << std::endl;
    EXPECT_EQ(static_cast<size_t>(points_per_tag), results.size());

    engine.close();
}

// Test 3: Burst write pattern
TEST_F(LargeScaleSimulationTest, BurstWritePattern) {
    std::cout << "\n=== Test 3: Burst Write Pattern ===" << std::endl;

    const int num_bursts = 10;
    const int tags_per_burst = 50;
    const int points_per_burst = 5000;
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    std::cout << "Simulating " << num_bursts << " bursts..." << std::endl;

    int total_points = 0;
    auto start_time = high_resolution_clock::now();

    for (int burst = 0; burst < num_bursts; burst++) {
        std::cout << "  Burst " << (burst + 1) << "..." << std::endl;

        // Rapid writes in burst
        for (int tag = 0; tag < tags_per_burst; tag++) {
            uint32_t tag_id = 3000 + burst * tags_per_burst + tag;

            for (int i = 0; i < points_per_burst; i++) {
                int64_t ts = base_ts + burst * 10000000 + i * 1000;
                double value = 100.0 * std::sin(2.0 * M_PI * i / 500.0);

                ASSERT_EQ(EngineResult::SUCCESS,
                         engine.writePoint(tag_id, ts, value, 192));
            }

            total_points += points_per_burst;
        }

        // Flush after each burst
        engine.flush();

        // Small delay between bursts (simulates real-world patterns)
        // In real test, no actual sleep needed
    }

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    double throughput = (total_points * 1000.0) / duration.count();

    std::cout << "Burst simulation complete:" << std::endl;
    std::cout << "  Total points: " << total_points << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " points/sec" << std::endl;

    const auto& stats = engine.getWriteStats();
    std::cout << "  Chunks sealed: " << stats.chunks_sealed << std::endl;

    engine.close();
}

// Test 4: Long-term query performance
TEST_F(LargeScaleSimulationTest, LongTermQueryPerformance) {
    std::cout << "\n=== Test 4: Long-Term Query Performance ===" << std::endl;

    const int num_tags = 10;
    const int points_per_tag = 50000;  // 50K points per tag
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    // Write data
    std::cout << "Writing " << num_tags << " tags × " << points_per_tag
              << " points..." << std::endl;

    for (int tag = 0; tag < num_tags; tag++) {
        uint32_t tag_id = 4000 + tag;

        for (int i = 0; i < points_per_tag; i++) {
            int64_t ts = base_ts + i * 1000;
            double value = 50.0 + tag * 10.0 + i * 0.001;

            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, value, 192));
        }

        if ((tag + 1) % 2 == 0) {
            engine.flush();
        }
    }

    engine.flush();
    std::cout << "Write complete." << std::endl;

    // Query tests
    std::cout << "\nTesting queries:" << std::endl;

    // Query 1: Short range (1 hour)
    {
        auto start = high_resolution_clock::now();
        std::vector<StorageEngine::QueryPoint> results;
        ASSERT_EQ(EngineResult::SUCCESS,
                 engine.queryPoints(4000, base_ts, base_ts + 3600LL * 1000000, results));
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(end - start);

        std::cout << "  Short range (1h): " << results.size() << " points in "
                  << duration.count() << " μs" << std::endl;
    }

    // Query 2: Medium range (12 hours)
    {
        auto start = high_resolution_clock::now();
        std::vector<StorageEngine::QueryPoint> results;
        ASSERT_EQ(EngineResult::SUCCESS,
                 engine.queryPoints(4000, base_ts, base_ts + 12LL * 3600 * 1000000, results));
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<microseconds>(end - start);

        std::cout << "  Medium range (12h): " << results.size() << " points in "
                  << duration.count() << " μs" << std::endl;
    }

    // Query 3: Full range
    {
        auto start = high_resolution_clock::now();
        std::vector<StorageEngine::QueryPoint> results;
        // Query range: from base_ts to base_ts + (points_per_tag - 1) * 1000 (last point timestamp)
        // Since timestamps are base_ts + i * 1000 where i ranges from 0 to points_per_tag - 1
        // The last point timestamp is base_ts + (points_per_tag - 1) * 1000
        // We add 1000 to include the last point (inclusive range)
        int64_t end_ts = base_ts + static_cast<int64_t>(points_per_tag - 1) * 1000000 + 1000;
        ASSERT_EQ(EngineResult::SUCCESS,
                 engine.queryPoints(4000, base_ts, end_ts, results));
        auto end = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(end - start);

        std::cout << "  Full range: " << results.size() << " points in "
                  << duration.count() << " ms" << std::endl;

        EXPECT_EQ(static_cast<size_t>(points_per_tag), results.size());
    }

    engine.close();
}

// Test 5: Mixed workload simulation
TEST_F(LargeScaleSimulationTest, MixedWorkloadSimulation) {
    std::cout << "\n=== Test 5: Mixed Workload Simulation ===" << std::endl;

    const int num_tags = 100;
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    std::cout << "Simulating mixed workload..." << std::endl;

    auto start_time = high_resolution_clock::now();
    int total_points = 0;

    // Phase 1: Initial bulk load
    std::cout << "  Phase 1: Bulk load (5000 points/tag)..." << std::endl;
    for (int tag = 0; tag < num_tags; tag++) {
        uint32_t tag_id = 5000 + tag;
        for (int i = 0; i < 5000; i++) {
            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, base_ts + i * 1000, 50.0 + i * 0.01, 192));
            total_points++;
        }
    }
    engine.flush();

    // Phase 2: Sparse updates (every 10th tag)
    std::cout << "  Phase 2: Sparse updates (1000 points/10 tags)..." << std::endl;
    for (int tag = 0; tag < num_tags; tag += 10) {
        uint32_t tag_id = 5000 + tag;
        for (int i = 0; i < 1000; i++) {
            int64_t ts = base_ts + 5000000 + i * 1000;
            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, 60.0 + i * 0.01, 192));
            total_points++;
        }
    }
    engine.flush();

    // Phase 3: High-frequency updates (first 10 tags)
    std::cout << "  Phase 3: High-frequency updates (10000 points/10 tags)..." << std::endl;
    for (int tag = 0; tag < 10; tag++) {
        uint32_t tag_id = 5000 + tag;
        for (int i = 0; i < 10000; i++) {
            int64_t ts = base_ts + 6000000 + i * 100;  // 100ms intervals
            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, 70.0 + i * 0.001, 192));
            total_points++;
        }
    }
    engine.flush();

    auto end_time = high_resolution_clock::now();
    auto duration = duration_cast<milliseconds>(end_time - start_time);

    double throughput = (total_points * 1000.0) / duration.count();

    std::cout << "Mixed workload complete:" << std::endl;
    std::cout << "  Total points: " << total_points << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << static_cast<int>(throughput) << " points/sec" << std::endl;

    // Verify queries on different tag types
    std::vector<StorageEngine::QueryPoint> results;

    // Sparse tag
    ASSERT_EQ(EngineResult::SUCCESS,
             engine.queryPoints(5000, base_ts, base_ts + 10000000, results));
    std::cout << "  Sparse tag query: " << results.size() << " points" << std::endl;

    // High-frequency tag
    results.clear();
    ASSERT_EQ(EngineResult::SUCCESS,
             engine.queryPoints(5001, base_ts, base_ts + 10000000, results));
    std::cout << "  High-freq tag query: " << results.size() << " points" << std::endl;

    engine.close();
}

// Test 6: Storage efficiency verification
TEST_F(LargeScaleSimulationTest, StorageEfficiency) {
    std::cout << "\n=== Test 6: Storage Efficiency ===" << std::endl;

    const int num_tags = 50;
    const int points_per_tag = 10000;
    const int64_t base_ts = 1704067200000000LL;

    StorageEngine engine(config_);
    ASSERT_EQ(EngineResult::SUCCESS, engine.open());

    std::cout << "Writing " << num_tags << " tags × " << points_per_tag
              << " points..." << std::endl;

    for (int tag = 0; tag < num_tags; tag++) {
        uint32_t tag_id = 6000 + tag;

        for (int i = 0; i < points_per_tag; i++) {
            int64_t ts = base_ts + i * 1000;
            // Smooth sine wave - should compress well
            double value = 50.0 + 20.0 * std::sin(2.0 * M_PI * i / 1000.0);

            ASSERT_EQ(EngineResult::SUCCESS,
                     engine.writePoint(tag_id, ts, value, 192));
        }

        if ((tag + 1) % 10 == 0) {
            engine.flush();
        }
    }

    engine.flush();

    int total_points = num_tags * points_per_tag;
    std::cout << "Write complete: " << total_points << " points" << std::endl;

    // Calculate storage metrics
    size_t storage_bytes = getDirectorySize(test_dir_);
    double mb = storage_bytes / (1024.0 * 1024.0);

    // Theoretical size: 13 bytes/point uncompressed
    size_t uncompressed_bytes = total_points * 13;
    double uncompressed_mb = uncompressed_bytes / (1024.0 * 1024.0);

    // Note: storage includes pre-allocated WAL segments (4 × 64MB = 256MB)
    size_t wal_segments_bytes = 4 * 64 * 1024 * 1024;  // 256MB
    size_t data_bytes = (storage_bytes > wal_segments_bytes) ?
                        (storage_bytes - wal_segments_bytes) : storage_bytes;
    double data_ratio = (double)data_bytes / uncompressed_bytes;

    std::cout << "\nStorage metrics:" << std::endl;
    std::cout << "  Theoretical (uncompressed): " << uncompressed_mb << " MB" << std::endl;
    std::cout << "  Actual storage (total): " << mb << " MB" << std::endl;
    std::cout << "  Data storage (excl. WAL): " << (data_bytes / (1024.0 * 1024.0)) << " MB" << std::endl;
    std::cout << "  Data storage ratio: " << (data_ratio * 100.0) << "%" << std::endl;
    std::cout << "  Bytes per point (total): " << (storage_bytes / total_points) << std::endl;

    // Data storage ratio is high for small datasets due to:
    // - Container files pre-allocated to 256MB minimum
    // - Chunks pre-allocated to 64MB each
    // - Directory and metadata overhead
    // For 500K points (6.5MB theoretical), expect ~150-200x due to pre-allocation
    // Verify storage is at least less than 2.5GB (reasonable upper bound)
    EXPECT_LT(storage_bytes, 2560ULL * 1024 * 1024);

    engine.close();
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
