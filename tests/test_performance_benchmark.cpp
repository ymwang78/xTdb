#include <gtest/gtest.h>
#include "xTdb/swinging_door_encoder.h"
#include "xTdb/swinging_door_decoder.h"
#include "xTdb/quantized_16_encoder.h"
#include "xTdb/quantized_16_decoder.h"
#include "xTdb/resampling_engine.h"
#include "xTdb/archive_manager.h"
#include <vector>
#include <cmath>
#include <chrono>
#include <iostream>
#include <iomanip>

using namespace xtdb;
using namespace std::chrono;

class PerformanceBenchmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }

    // Helper: Generate realistic industrial data (temperature sensor)
    std::vector<MemRecord> generateIndustrialData(size_t count, double base_temp = 25.0) {
        std::vector<MemRecord> records;
        double temp = base_temp;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * 1000;  // 1 Hz sampling

            // Simulate realistic temperature variations
            // Slow drift + small noise
            temp += (std::rand() % 100 - 50) * 0.001;  // ±0.05°C drift
            temp += (std::rand() % 100 - 50) * 0.0002; // ±0.01°C noise

            // Keep in reasonable range
            if (temp < 20.0) temp = 20.0;
            if (temp > 30.0) temp = 30.0;

            rec.value.f64_value = temp;
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Generate high-frequency oscillating data (pressure sensor)
    std::vector<MemRecord> generateOscillatingData(size_t count, double mean = 100.0, double amplitude = 10.0) {
        std::vector<MemRecord> records;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * 100;  // 10 Hz sampling

            // Oscillating pattern with noise
            double value = mean + amplitude * std::sin(2.0 * M_PI * i / 100.0);
            value += (std::rand() % 100 - 50) * 0.01;  // Small noise

            rec.value.f64_value = value;
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Calculate throughput (points per second)
    double calculateThroughput(size_t point_count, double duration_ms) {
        return (point_count * 1000.0) / duration_ms;
    }

    // Helper: Format bytes
    std::string formatBytes(size_t bytes) {
        if (bytes < 1024) return std::to_string(bytes) + " B";
        if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + " KB";
        return std::to_string(bytes / (1024 * 1024)) + " MB";
    }
};

// Benchmark 1: Swinging Door compression performance
TEST_F(PerformanceBenchmarkTest, SwingingDoorCompressionBenchmark) {
    std::cout << "\n=== Benchmark 1: Swinging Door Compression ===" << std::endl;

    // Test with different data sizes
    std::vector<size_t> sizes = {1000, 10000, 100000};
    int64_t base_ts = 1000000000;

    for (size_t size : sizes) {
        auto records = generateIndustrialData(size, 25.0);

        SwingingDoorEncoder encoder(0.1, 1.0);  // 0.1°C tolerance
        std::vector<SwingingDoorEncoder::CompressedPoint> compressed;

        auto start = high_resolution_clock::now();
        auto result = encoder.encode(base_ts, records, compressed);
        auto end = high_resolution_clock::now();

        ASSERT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);

        auto duration_ms = duration_cast<milliseconds>(end - start).count();
        double throughput = calculateThroughput(size, duration_ms);
        double compression_ratio = encoder.getCompressionRatio();

        std::cout << "\nData size: " << size << " points" << std::endl;
        std::cout << "  Compressed to: " << compressed.size() << " points" << std::endl;
        std::cout << "  Compression ratio: " << std::fixed << std::setprecision(2)
                  << compression_ratio << ":1" << std::endl;
        std::cout << "  Encoding time: " << duration_ms << " ms" << std::endl;
        std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " points/sec" << std::endl;

        // Performance targets
        EXPECT_GT(compression_ratio, 10.0);  // > 10:1 for industrial data
        EXPECT_GT(throughput, 10000.0);      // > 10K points/sec
    }
}

// Benchmark 2: 16-bit quantization performance
TEST_F(PerformanceBenchmarkTest, Quantization16Benchmark) {
    std::cout << "\n=== Benchmark 2: 16-bit Quantization ===" << std::endl;

    std::vector<size_t> sizes = {1000, 10000, 100000};
    int64_t base_ts = 1000000000;

    for (size_t size : sizes) {
        auto records = generateIndustrialData(size, 25.0);

        Quantized16Encoder encoder(0.0, 100.0);
        std::vector<Quantized16Encoder::QuantizedPoint> quantized;

        auto start = high_resolution_clock::now();
        auto result = encoder.encode(base_ts, records, quantized);
        auto end = high_resolution_clock::now();

        ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, result);

        auto duration_ms = duration_cast<milliseconds>(end - start).count();
        if (duration_ms == 0) duration_ms = 1;  // Avoid division by zero
        double throughput = calculateThroughput(size, duration_ms);

        size_t original_bytes = size * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
        size_t quantized_bytes = size * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));
        double saving_percent = 100.0 * (1.0 - (double)quantized_bytes / original_bytes);

        std::cout << "\nData size: " << size << " points" << std::endl;
        std::cout << "  Original: " << formatBytes(original_bytes) << std::endl;
        std::cout << "  Quantized: " << formatBytes(quantized_bytes) << std::endl;
        std::cout << "  Saving: " << std::fixed << std::setprecision(2)
                  << saving_percent << "%" << std::endl;
        std::cout << "  Encoding time: " << duration_ms << " ms" << std::endl;
        std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
                  << throughput << " points/sec" << std::endl;

        // Performance targets
        EXPECT_GT(saving_percent, 45.0);    // > 45% saving
        EXPECT_GT(throughput, 50000.0);     // > 50K points/sec
    }
}

// Benchmark 3: Combined compression (SD + Q16)
TEST_F(PerformanceBenchmarkTest, CombinedCompressionBenchmark) {
    std::cout << "\n=== Benchmark 3: Combined Compression (SD + Q16) ===" << std::endl;

    size_t size = 100000;  // 100K points
    int64_t base_ts = 1000000000;

    auto records = generateIndustrialData(size, 25.0);

    // Step 1: Swinging Door
    SwingingDoorEncoder sd_encoder(0.1, 1.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> sd_compressed;

    auto sd_start = high_resolution_clock::now();
    sd_encoder.encode(base_ts, records, sd_compressed);
    auto sd_end = high_resolution_clock::now();

    auto sd_duration = duration_cast<milliseconds>(sd_end - sd_start).count();

    // Calculate storage
    size_t original_bytes = size * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
    size_t sd_bytes = sd_compressed.size() * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
    size_t combined_bytes = sd_compressed.size() * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));

    double sd_ratio = (double)size / sd_compressed.size();
    double combined_saving = 100.0 * (1.0 - (double)combined_bytes / original_bytes);

    std::cout << "\n100K points industrial data:" << std::endl;
    std::cout << "  Original storage: " << formatBytes(original_bytes) << std::endl;
    std::cout << "  After SD: " << formatBytes(sd_bytes)
              << " (" << sd_compressed.size() << " points, ratio: "
              << std::fixed << std::setprecision(1) << sd_ratio << ":1)" << std::endl;
    std::cout << "  After SD+Q16: " << formatBytes(combined_bytes)
              << " (saving: " << std::fixed << std::setprecision(2)
              << combined_saving << "%)" << std::endl;
    std::cout << "  SD encoding time: " << sd_duration << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << calculateThroughput(size, sd_duration) << " points/sec" << std::endl;

    // Performance targets
    EXPECT_GT(sd_ratio, 10.0);
    EXPECT_GT(combined_saving, 90.0);
}

// Benchmark 4: Resampling performance
TEST_F(PerformanceBenchmarkTest, ResamplingBenchmark) {
    std::cout << "\n=== Benchmark 4: Resampling Performance ===" << std::endl;

    // Test 1-minute resampling on 1-hour data (3600 points @ 1Hz)
    size_t size = 3600;
    int64_t base_ts = 60 * 1000000LL;  // Aligned to 1-minute

    auto records = generateOscillatingData(size * 10, 100.0, 10.0);  // 10 Hz, 1 hour

    ResamplingEngine engine(60 * 1000000ULL, AggregationMethod::AVG);
    std::vector<ResampledPoint> resampled;

    auto start = high_resolution_clock::now();
    auto result = engine.resample(base_ts, records, resampled);
    auto end = high_resolution_clock::now();

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    auto duration_ms = duration_cast<milliseconds>(end - start).count();
    if (duration_ms == 0) duration_ms = 1;
    double throughput = calculateThroughput(records.size(), duration_ms);
    double compression_ratio = engine.getCompressionRatio();

    std::cout << "\n1-hour data (10 Hz):" << std::endl;
    std::cout << "  Input: " << records.size() << " points" << std::endl;
    std::cout << "  Output: " << resampled.size() << " points (1-minute intervals)" << std::endl;
    std::cout << "  Compression ratio: " << std::fixed << std::setprecision(2)
              << compression_ratio << ":1" << std::endl;
    std::cout << "  Resampling time: " << duration_ms << " ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << throughput << " points/sec" << std::endl;

    // Performance targets
    EXPECT_GT(compression_ratio, 50.0);  // Should be ~60:1
    EXPECT_GT(throughput, 10000.0);
}

// Benchmark 5: Decoding/Interpolation performance
TEST_F(PerformanceBenchmarkTest, DecodingBenchmark) {
    std::cout << "\n=== Benchmark 5: Decoding/Interpolation Performance ===" << std::endl;

    // Prepare compressed data
    size_t original_size = 10000;
    int64_t base_ts = 1000000000;
    auto records = generateIndustrialData(original_size, 25.0);

    SwingingDoorEncoder encoder(0.1, 1.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    encoder.encode(base_ts, records, compressed);

    std::cout << "\nCompressed: " << compressed.size() << " points (from "
              << original_size << ")" << std::endl;

    // Test interpolation performance
    SwingingDoorDecoder decoder;
    size_t interpolation_count = 1000;  // Interpolate 1000 random points

    auto start = high_resolution_clock::now();
    for (size_t i = 0; i < interpolation_count; i++) {
        size_t idx = (i * 10) % original_size;  // Sample points
        int64_t query_ts = base_ts + records[idx].time_offset * 1000;
        SwingingDoorDecoder::DecodedPoint decoded;
        decoder.interpolate(base_ts, compressed, query_ts, decoded);
    }
    auto end = high_resolution_clock::now();

    auto duration_us = duration_cast<microseconds>(end - start).count();
    double avg_interpolation_us = (double)duration_us / interpolation_count;

    std::cout << "  Interpolations: " << interpolation_count << std::endl;
    std::cout << "  Total time: " << duration_us << " μs" << std::endl;
    std::cout << "  Average per interpolation: " << std::fixed << std::setprecision(2)
              << avg_interpolation_us << " μs" << std::endl;
    std::cout << "  Interpolations/sec: " << std::fixed << std::setprecision(0)
              << (1000000.0 / avg_interpolation_us) << std::endl;

    // Performance target: < 100 μs per interpolation
    EXPECT_LT(avg_interpolation_us, 100.0);
}

// Benchmark 6: Archive selection performance
TEST_F(PerformanceBenchmarkTest, ArchiveSelectionBenchmark) {
    std::cout << "\n=== Benchmark 6: Archive Selection Performance ===" << std::endl;

    ArchiveManager manager;
    int64_t base_ts = 1704067200000000LL;

    // Register multiple archives
    for (int i = 0; i < 10; i++) {
        ArchiveMetadata raw_meta;
        raw_meta.level = ArchiveLevel::ARCHIVE_RAW;
        raw_meta.start_ts_us = base_ts + i * 24 * 3600 * 1000000LL;
        raw_meta.end_ts_us = base_ts + (i + 1) * 24 * 3600 * 1000000LL;
        raw_meta.record_count = 86400;
        manager.registerArchive(raw_meta, i, "/data/raw_" + std::to_string(i) + ".dat");

        ArchiveMetadata m1_meta;
        m1_meta.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
        m1_meta.start_ts_us = base_ts + i * 24 * 3600 * 1000000LL;
        m1_meta.end_ts_us = base_ts + (i + 1) * 24 * 3600 * 1000000LL;
        m1_meta.resampling_interval_us = 60 * 1000000ULL;
        m1_meta.record_count = 1440;
        manager.registerArchive(m1_meta, i + 100, "/data/1m_" + std::to_string(i) + ".dat");
    }

    std::cout << "\nRegistered 20 archives (10 RAW + 10 1M)" << std::endl;

    // Test selection performance
    size_t query_count = 1000;
    auto start = high_resolution_clock::now();

    for (size_t i = 0; i < query_count; i++) {
        ArchiveQuery query;
        query.tag_id = 1;
        query.start_ts_us = base_ts + (i % 10) * 24 * 3600 * 1000000LL;
        query.end_ts_us = query.start_ts_us + 3600 * 1000000LL;  // 1 hour
        query.prefer_raw = false;

        std::vector<ArchiveSelection> selections;
        manager.selectArchives(query, selections);
    }

    auto end = high_resolution_clock::now();
    auto duration_us = duration_cast<microseconds>(end - start).count();
    double avg_selection_us = (double)duration_us / query_count;

    std::cout << "  Queries: " << query_count << std::endl;
    std::cout << "  Total time: " << duration_us << " μs" << std::endl;
    std::cout << "  Average per query: " << std::fixed << std::setprecision(2)
              << avg_selection_us << " μs" << std::endl;
    std::cout << "  Queries/sec: " << std::fixed << std::setprecision(0)
              << (1000000.0 / avg_selection_us) << std::endl;

    // Performance target: < 50 μs per query
    EXPECT_LT(avg_selection_us, 50.0);
}

// Benchmark 7: Memory usage estimation
TEST_F(PerformanceBenchmarkTest, MemoryUsageEstimation) {
    std::cout << "\n=== Benchmark 7: Memory Usage Estimation ===" << std::endl;

    size_t point_count = 100000;

    // Original data
    size_t original_mem = point_count * sizeof(MemRecord);

    // Swinging Door compressed (assume 100:1 ratio)
    size_t sd_point_count = point_count / 100;
    size_t sd_mem = sd_point_count * sizeof(SwingingDoorEncoder::CompressedPoint);

    // Quantized16
    size_t q16_mem = point_count * sizeof(Quantized16Encoder::QuantizedPoint);

    // Resampled (1-minute from 1 Hz = 60:1)
    size_t resampled_count = point_count / 60;
    size_t resampled_mem = resampled_count * sizeof(ResampledPoint);

    std::cout << "\n100K points memory estimation:" << std::endl;
    std::cout << "  Original (MemRecord): " << formatBytes(original_mem) << std::endl;
    std::cout << "  Swinging Door (100:1): " << formatBytes(sd_mem)
              << " (-" << std::fixed << std::setprecision(1)
              << (100.0 * (1.0 - (double)sd_mem / original_mem)) << "%)" << std::endl;
    std::cout << "  Quantized16: " << formatBytes(q16_mem)
              << " (-" << std::fixed << std::setprecision(1)
              << (100.0 * (1.0 - (double)q16_mem / original_mem)) << "%)" << std::endl;
    std::cout << "  Resampled (60:1): " << formatBytes(resampled_mem)
              << " (-" << std::fixed << std::setprecision(1)
              << (100.0 * (1.0 - (double)resampled_mem / original_mem)) << "%)" << std::endl;

    // Combined SD + Q16
    size_t combined_mem = sd_point_count * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));
    std::cout << "  Combined (SD+Q16): " << formatBytes(combined_mem)
              << " (-" << std::fixed << std::setprecision(1)
              << (100.0 * (1.0 - (double)combined_mem / original_mem)) << "%)" << std::endl;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
