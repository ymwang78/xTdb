#include "../include/xTdb/storage_engine.h"
#include "test_utils.h"
#include <gtest/gtest.h>
#include <chrono>
#include <vector>
#include <algorithm>
#include <cmath>
#include <iomanip>

using namespace xtdb;

class PerformanceBenchmark : public ::testing::Test {
protected:
    void SetUp() override {
        std::string temp_dir = get_temp_dir();
        test_dir_ = join_path(temp_dir, "perf_bench_data");
        remove_directory(test_dir_);
        create_directory(test_dir_);
    }

    void TearDown() override {
        remove_directory(test_dir_);
    }
    
    std::string test_dir_;

    // Helper: Calculate percentile from sorted vector
    double percentile(const std::vector<double>& sorted_values, double p) {
        if (sorted_values.empty()) return 0.0;
        size_t index = static_cast<size_t>(sorted_values.size() * p / 100.0);
        if (index >= sorted_values.size()) index = sorted_values.size() - 1;
        return sorted_values[index];
    }

    // Helper: Print performance report
    void printReport(const std::string& title,
                     size_t total_ops,
                     double duration_sec,
                     const std::vector<double>& latencies_us) {
        std::cout << "\n========================================\n";
        std::cout << title << "\n";
        std::cout << "========================================\n";
        std::cout << "Total operations: " << total_ops << "\n";
        std::cout << "Duration: " << std::fixed << std::setprecision(2)
                  << duration_sec << " seconds\n";
        std::cout << "Throughput: " << std::fixed << std::setprecision(0)
                  << (total_ops / duration_sec) << " ops/sec\n";

        if (!latencies_us.empty()) {
            std::vector<double> sorted_latencies = latencies_us;
            std::sort(sorted_latencies.begin(), sorted_latencies.end());

            double sum = 0.0;
            for (double lat : sorted_latencies) {
                sum += lat;
            }
            double avg = sum / sorted_latencies.size();

            std::cout << "\nLatency (microseconds):\n";
            std::cout << "  Average: " << std::fixed << std::setprecision(2) << avg << " μs\n";
            std::cout << "  P50:     " << std::fixed << std::setprecision(2)
                      << percentile(sorted_latencies, 50) << " μs\n";
            std::cout << "  P90:     " << std::fixed << std::setprecision(2)
                      << percentile(sorted_latencies, 90) << " μs\n";
            std::cout << "  P99:     " << std::fixed << std::setprecision(2)
                      << percentile(sorted_latencies, 99) << " μs\n";
            std::cout << "  P999:    " << std::fixed << std::setprecision(2)
                      << percentile(sorted_latencies, 99.9) << " μs\n";
            std::cout << "  Max:     " << std::fixed << std::setprecision(2)
                      << sorted_latencies.back() << " μs\n";
        }
        std::cout << "========================================\n\n";
    }
};

// ============================================================================
// Benchmark 1: Single-Tag Write Throughput
// ============================================================================
TEST_F(PerformanceBenchmark, SingleTagWriteThroughput) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    const uint32_t tag_id = 1;
    const size_t num_writes = 100000;
    int64_t timestamp_us = 1000000;

    std::vector<double> latencies_us;
    latencies_us.reserve(num_writes);

    auto start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_writes; ++i) {
        auto write_start = std::chrono::high_resolution_clock::now();

        EngineResult result = engine.writePoint(tag_id, timestamp_us,
                                               static_cast<double>(i), 192);
        ASSERT_EQ(result, EngineResult::SUCCESS);

        auto write_end = std::chrono::high_resolution_clock::now();
        double latency = std::chrono::duration<double, std::micro>(
            write_end - write_start).count();
        latencies_us.push_back(latency);

        timestamp_us += 1000;
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();

    engine.flush();
    engine.close();

    printReport("Single-Tag Write Throughput", num_writes, duration, latencies_us);
}

// ============================================================================
// Benchmark 2: Multi-Tag Write Throughput
// ============================================================================
TEST_F(PerformanceBenchmark, MultiTagWriteThroughput) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    const size_t num_tags = 10;
    const size_t writes_per_tag = 10000;
    const size_t total_writes = num_tags * writes_per_tag;

    std::vector<double> latencies_us;
    latencies_us.reserve(total_writes);

    auto start = std::chrono::high_resolution_clock::now();

    // Round-robin writes across tags
    for (size_t i = 0; i < writes_per_tag; ++i) {
        for (uint32_t tag_id = 1; tag_id <= num_tags; ++tag_id) {
            int64_t timestamp_us = 1000000 + (i * 1000);

            auto write_start = std::chrono::high_resolution_clock::now();

            EngineResult result = engine.writePoint(tag_id, timestamp_us,
                                                   static_cast<double>(i), 192);
            ASSERT_EQ(result, EngineResult::SUCCESS);

            auto write_end = std::chrono::high_resolution_clock::now();
            double latency = std::chrono::duration<double, std::micro>(
                write_end - write_start).count();
            latencies_us.push_back(latency);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();

    engine.flush();
    engine.close();

    printReport("Multi-Tag Write Throughput (10 tags)", total_writes, duration, latencies_us);
}

// ============================================================================
// Benchmark 3: High-Volume Write Stress Test
// ============================================================================
TEST_F(PerformanceBenchmark, HighVolumeWriteStress) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    const size_t num_tags = 100;
    const size_t writes_per_tag = 1000;
    const size_t total_writes = num_tags * writes_per_tag;

    std::cout << "\n[High-Volume Stress Test]\n";
    std::cout << "Writing " << total_writes << " points across " << num_tags << " tags...\n";

    auto start = std::chrono::high_resolution_clock::now();

    // Interleaved writes
    for (size_t i = 0; i < writes_per_tag; ++i) {
        for (uint32_t tag_id = 1; tag_id <= num_tags; ++tag_id) {
            int64_t timestamp_us = 1000000 + (i * 1000);
            EngineResult result = engine.writePoint(tag_id, timestamp_us,
                                                   static_cast<double>(i), 192);
            ASSERT_EQ(result, EngineResult::SUCCESS);
        }

        // Progress update every 10%
        if ((i + 1) % (writes_per_tag / 10) == 0) {
            std::cout << "  Progress: " << ((i + 1) * 100 / writes_per_tag) << "%\n";
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();

    // Flush and measure flush time
    auto flush_start = std::chrono::high_resolution_clock::now();
    engine.flush();
    auto flush_end = std::chrono::high_resolution_clock::now();
    double flush_duration = std::chrono::duration<double>(flush_end - flush_start).count();

    engine.close();

    // Empty latencies vector for stress test (don't track individual latencies)
    std::vector<double> empty_latencies;
    printReport("High-Volume Write Stress (100 tags)", total_writes, duration, empty_latencies);

    std::cout << "Flush time: " << std::fixed << std::setprecision(3)
              << flush_duration << " seconds\n";
    std::cout << "Write stats:\n";
    std::cout << "  Points written: " << engine.getWriteStats().points_written << "\n";
    std::cout << "  Blocks flushed: " << engine.getWriteStats().blocks_flushed << "\n";
    std::cout << "  Chunks sealed: " << engine.getWriteStats().chunks_sealed << "\n";
}

// ============================================================================
// Benchmark 4: Query Performance (Small Dataset)
// ============================================================================
TEST_F(PerformanceBenchmark, QueryPerformanceSmall) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    // Write test data: 10 blocks worth
    const uint32_t tag_id = 1;
    const size_t points_per_block = 700;
    const size_t num_blocks = 10;
    const size_t total_points = points_per_block * num_blocks;

    std::cout << "\n[Query Performance - Small Dataset]\n";
    std::cout << "Writing " << total_points << " points...\n";

    int64_t timestamp_us = 1000000;
    for (size_t i = 0; i < total_points; ++i) {
        engine.writePoint(tag_id, timestamp_us, static_cast<double>(i), 192);
        timestamp_us += 1000;
    }

    engine.flush();

    // Query all data
    std::vector<double> query_latencies_us;
    const size_t num_queries = 100;
    query_latencies_us.reserve(num_queries);

    int64_t start_ts = 1000000;
    int64_t end_ts = timestamp_us;

    std::cout << "Running " << num_queries << " queries...\n";

    auto bench_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_queries; ++i) {
        std::vector<StorageEngine::QueryPoint> results;

        auto query_start = std::chrono::high_resolution_clock::now();
        EngineResult result = engine.queryPoints(tag_id, start_ts, end_ts, results);
        auto query_end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(result, EngineResult::SUCCESS);
        ASSERT_EQ(results.size(), total_points);

        double latency = std::chrono::duration<double, std::micro>(
            query_end - query_start).count();
        query_latencies_us.push_back(latency);
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(bench_end - bench_start).count();

    engine.close();

    printReport("Query Performance (10 blocks, 7K points)", num_queries, duration, query_latencies_us);
}

// ============================================================================
// Benchmark 5: Query Performance (Large Dataset)
// ============================================================================
TEST_F(PerformanceBenchmark, QueryPerformanceLarge) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    // Write test data: 100 blocks worth
    const uint32_t tag_id = 1;
    const size_t points_per_block = 700;
    const size_t num_blocks = 100;
    const size_t total_points = points_per_block * num_blocks;

    std::cout << "\n[Query Performance - Large Dataset]\n";
    std::cout << "Writing " << total_points << " points...\n";

    int64_t timestamp_us = 1000000;
    for (size_t i = 0; i < total_points; ++i) {
        engine.writePoint(tag_id, timestamp_us, static_cast<double>(i), 192);
        timestamp_us += 1000;

        if ((i + 1) % (total_points / 10) == 0) {
            std::cout << "  Progress: " << ((i + 1) * 100 / total_points) << "%\n";
        }
    }

    engine.flush();

    // Query all data
    std::vector<double> query_latencies_us;
    const size_t num_queries = 50;
    query_latencies_us.reserve(num_queries);

    int64_t start_ts = 1000000;
    int64_t end_ts = timestamp_us;

    std::cout << "Running " << num_queries << " queries...\n";

    auto bench_start = std::chrono::high_resolution_clock::now();

    for (size_t i = 0; i < num_queries; ++i) {
        std::vector<StorageEngine::QueryPoint> results;

        auto query_start = std::chrono::high_resolution_clock::now();
        EngineResult result = engine.queryPoints(tag_id, start_ts, end_ts, results);
        auto query_end = std::chrono::high_resolution_clock::now();

        ASSERT_EQ(result, EngineResult::SUCCESS);
        ASSERT_EQ(results.size(), total_points);

        double latency = std::chrono::duration<double, std::micro>(
            query_end - query_start).count();
        query_latencies_us.push_back(latency);
    }

    auto bench_end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(bench_end - bench_start).count();

    engine.close();

    printReport("Query Performance (100 blocks, 70K points)", num_queries, duration, query_latencies_us);
}

// ============================================================================
// Benchmark 6: Mixed Read/Write Workload
// ============================================================================
TEST_F(PerformanceBenchmark, MixedReadWriteWorkload) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    const uint32_t tag_id = 1;
    const size_t initial_points = 10000;
    const size_t additional_writes = 10000;

    std::cout << "\n[Mixed Read/Write Workload]\n";
    std::cout << "Initial write: " << initial_points << " points...\n";

    // Initial data
    int64_t timestamp_us = 1000000;
    for (size_t i = 0; i < initial_points; ++i) {
        engine.writePoint(tag_id, timestamp_us, static_cast<double>(i), 192);
        timestamp_us += 1000;
    }
    engine.flush();

    std::cout << "Running mixed workload...\n";

    std::vector<double> write_latencies_us;
    std::vector<double> query_latencies_us;

    auto start = std::chrono::high_resolution_clock::now();

    // Mixed workload: 10 writes, then 1 query
    for (size_t i = 0; i < additional_writes / 10; ++i) {
        // 10 writes
        for (size_t j = 0; j < 10; ++j) {
            auto write_start = std::chrono::high_resolution_clock::now();
            engine.writePoint(tag_id, timestamp_us, static_cast<double>(timestamp_us), 192);
            auto write_end = std::chrono::high_resolution_clock::now();

            double latency = std::chrono::duration<double, std::micro>(
                write_end - write_start).count();
            write_latencies_us.push_back(latency);

            timestamp_us += 1000;
        }

        // 1 query
        if (i % 10 == 0) {  // Query every 100 writes
            std::vector<StorageEngine::QueryPoint> results;

            auto query_start = std::chrono::high_resolution_clock::now();
            engine.queryPoints(tag_id, 1000000, timestamp_us, results);
            auto query_end = std::chrono::high_resolution_clock::now();

            double latency = std::chrono::duration<double, std::micro>(
                query_end - query_start).count();
            query_latencies_us.push_back(latency);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();

    engine.close();

    std::cout << "\n========================================\n";
    std::cout << "Mixed Read/Write Workload Results\n";
    std::cout << "========================================\n";
    std::cout << "Total duration: " << std::fixed << std::setprecision(2)
              << duration << " seconds\n";
    std::cout << "Total writes: " << write_latencies_us.size() << "\n";
    std::cout << "Total queries: " << query_latencies_us.size() << "\n";
    std::cout << "Combined throughput: " << std::fixed << std::setprecision(0)
              << ((write_latencies_us.size() + query_latencies_us.size()) / duration)
              << " ops/sec\n";

    // Write latencies
    std::vector<double> sorted_writes = write_latencies_us;
    std::sort(sorted_writes.begin(), sorted_writes.end());
    std::cout << "\nWrite Latency:\n";
    std::cout << "  P50: " << percentile(sorted_writes, 50) << " μs\n";
    std::cout << "  P99: " << percentile(sorted_writes, 99) << " μs\n";

    // Query latencies
    std::vector<double> sorted_queries = query_latencies_us;
    std::sort(sorted_queries.begin(), sorted_queries.end());
    std::cout << "\nQuery Latency:\n";
    std::cout << "  P50: " << percentile(sorted_queries, 50) << " μs\n";
    std::cout << "  P99: " << percentile(sorted_queries, 99) << " μs\n";
    std::cout << "========================================\n\n";
}

// ============================================================================
// Benchmark 7: Burst Write Performance
// ============================================================================
TEST_F(PerformanceBenchmark, BurstWritePerformance) {
    EngineConfig config;
    config.data_dir = test_dir_;
    config.db_path = join_path(test_dir_, "meta.db");

    StorageEngine engine(config);
    ASSERT_EQ(engine.open(), EngineResult::SUCCESS);

    const uint32_t tag_id = 1;
    const size_t burst_size = 10000;
    const size_t num_bursts = 5;

    std::cout << "\n[Burst Write Performance]\n";
    std::cout << "Bursts: " << num_bursts << ", Size: " << burst_size << " points each\n\n";

    std::vector<double> burst_throughputs;

    for (size_t burst = 0; burst < num_bursts; ++burst) {
        int64_t timestamp_us = 1000000 + (burst * burst_size * 1000);

        auto start = std::chrono::high_resolution_clock::now();

        for (size_t i = 0; i < burst_size; ++i) {
            engine.writePoint(tag_id, timestamp_us, static_cast<double>(i), 192);
            timestamp_us += 1000;
        }

        auto end = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration<double>(end - start).count();
        double throughput = burst_size / duration;

        burst_throughputs.push_back(throughput);

        std::cout << "Burst " << (burst + 1) << ": "
                  << std::fixed << std::setprecision(0) << throughput << " writes/sec\n";

        // Small delay between bursts
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    engine.close();

    // Calculate statistics
    double sum = 0.0;
    for (double tp : burst_throughputs) {
        sum += tp;
    }
    double avg_throughput = sum / burst_throughputs.size();

    std::cout << "\nBurst Statistics:\n";
    std::cout << "  Average throughput: " << std::fixed << std::setprecision(0)
              << avg_throughput << " writes/sec\n";
    std::cout << "  Min throughput: " << std::fixed << std::setprecision(0)
              << *std::min_element(burst_throughputs.begin(), burst_throughputs.end())
              << " writes/sec\n";
    std::cout << "  Max throughput: " << std::fixed << std::setprecision(0)
              << *std::max_element(burst_throughputs.begin(), burst_throughputs.end())
              << " writes/sec\n";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
