#include <gtest/gtest.h>
#include "xTdb/resampling_engine.h"
#include <cmath>

using namespace xtdb;

class ResamplingTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }
};

// Test basic 1-minute resampling
TEST_F(ResamplingTest, BasicOneMinuteResampling) {
    // Resample to 1-minute intervals
    uint64_t interval_us = 60 * 1000000ULL;  // 1 minute
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create 1 second data for 5 minutes (300 points)
    // Use aligned base timestamp (multiple of 1 minute)
    std::vector<MemRecord> records;
    int64_t base_ts_us = 60 * 1000000LL;  // Aligned to 1-minute boundary

    for (int i = 0; i < 300; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;  // milliseconds
        rec.value.f64_value = 100.0 + i;  // Increasing values
        rec.quality = 192;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // Should have 5 resampled points (one per minute)
    EXPECT_EQ(5u, resampled.size());

    // Check compression ratio
    double ratio = engine.getCompressionRatio();
    std::cout << "Compression ratio: " << ratio << ":1" << std::endl;
    EXPECT_NEAR(60.0, ratio, 1.0);  // Approximately 60:1

    // Verify first resampled point
    EXPECT_EQ(60u, resampled[0].count);  // 60 seconds of data
    EXPECT_EQ(192, resampled[0].quality);

    // Verify aggregations
    // First window: values from 100 to 159 (60 values)
    // Average should be around 129.5
    EXPECT_NEAR(129.5, resampled[0].avg_value, 0.5);
    EXPECT_DOUBLE_EQ(100.0, resampled[0].min_value);
    EXPECT_DOUBLE_EQ(159.0, resampled[0].max_value);
    EXPECT_DOUBLE_EQ(100.0, resampled[0].first_value);
    EXPECT_DOUBLE_EQ(159.0, resampled[0].last_value);
}

// Test 1-hour resampling
TEST_F(ResamplingTest, OneHourResampling) {
    // Resample to 1-hour intervals
    uint64_t interval_us = 3600 * 1000000ULL;  // 1 hour
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create 1-minute data for 3 hours (180 points)
    // Use aligned base timestamp (multiple of 1 hour)
    std::vector<MemRecord> records;
    int64_t base_ts_us = 3600 * 1000000LL;  // Aligned to 1-hour boundary

    for (int i = 0; i < 180; i++) {
        MemRecord rec;
        rec.time_offset = i * 60 * 1000;  // 1 minute intervals
        rec.value.f64_value = 50.0 + (i % 60);  // Cyclical pattern
        rec.quality = 192;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // Should have 3 resampled points (one per hour)
    EXPECT_EQ(3u, resampled.size());

    // Check compression ratio
    double ratio = engine.getCompressionRatio();
    std::cout << "1-hour resampling compression: " << ratio << ":1" << std::endl;
    EXPECT_NEAR(60.0, ratio, 1.0);

    // Each hour should have 60 points
    for (const auto& rp : resampled) {
        EXPECT_EQ(60u, rp.count);
    }
}

// Test sparse data resampling
TEST_F(ResamplingTest, SparseDataResampling) {
    uint64_t interval_us = 60 * 1000000ULL;  // 1 minute
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create sparse data: only 10 points across 10 minutes
    std::vector<MemRecord> records;
    int64_t base_ts_us = 1000000000;

    for (int i = 0; i < 10; i++) {
        MemRecord rec;
        rec.time_offset = i * 60 * 1000;  // 1 point per minute
        rec.value.f64_value = 100.0 * i;
        rec.quality = 192;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // Should have 10 resampled points (one per minute with data)
    EXPECT_EQ(10u, resampled.size());

    // Each window should have 1 point
    for (const auto& rp : resampled) {
        EXPECT_EQ(1u, rp.count);
        EXPECT_EQ(rp.avg_value, rp.min_value);
        EXPECT_EQ(rp.avg_value, rp.max_value);
        EXPECT_EQ(rp.avg_value, rp.first_value);
        EXPECT_EQ(rp.avg_value, rp.last_value);
    }
}

// Test dense cluster resampling
TEST_F(ResamplingTest, DenseClusterResampling) {
    uint64_t interval_us = 60 * 1000000ULL;  // 1 minute
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create dense data in first minute, then sparse
    // Use aligned base timestamp
    std::vector<MemRecord> records;
    int64_t base_ts_us = 60 * 1000000LL;  // Aligned to 1-minute boundary

    // First 60 seconds: 60 points at 1 Hz
    for (int i = 0; i < 60; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;  // 1 second intervals
        rec.value.f64_value = 100.0;
        rec.quality = 192;
        records.push_back(rec);
    }

    // Next 3 minutes: 3 points at 1-minute intervals
    for (int i = 1; i <= 3; i++) {
        MemRecord rec;
        rec.time_offset = i * 60 * 1000;
        rec.value.f64_value = 200.0;
        rec.quality = 192;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // First window should have 60 points
    EXPECT_EQ(60u, resampled[0].count);
    EXPECT_NEAR(100.0, resampled[0].avg_value, 0.1);

    // Remaining windows should have 1 point each
    for (size_t i = 1; i < resampled.size(); i++) {
        EXPECT_EQ(1u, resampled[i].count);
        EXPECT_NEAR(200.0, resampled[i].avg_value, 0.1);
    }
}

// Test min/max aggregation
TEST_F(ResamplingTest, MinMaxAggregation) {
    uint64_t interval_us = 60 * 1000000ULL;  // 1 minute
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create oscillating data
    std::vector<MemRecord> records;
    int64_t base_ts_us = 1000000000;

    for (int i = 0; i < 120; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        // Oscillate between 0 and 100
        rec.value.f64_value = 50.0 + 50.0 * std::sin(i * 0.1);
        rec.quality = 192;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // Verify min/max are captured
    for (const auto& rp : resampled) {
        EXPECT_LE(rp.min_value, rp.avg_value);
        EXPECT_GE(rp.max_value, rp.avg_value);
        EXPECT_GE(rp.max_value, rp.min_value);

        // All values should be in [0, 100] range
        EXPECT_GE(rp.min_value, 0.0);
        EXPECT_LE(rp.max_value, 100.0);
    }
}

// Test empty input
TEST_F(ResamplingTest, EmptyInput) {
    uint64_t interval_us = 60 * 1000000ULL;
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    std::vector<MemRecord> records;
    std::vector<ResampledPoint> resampled;

    auto result = engine.resample(0, records, resampled);

    EXPECT_EQ(ResampleResult::ERROR_EMPTY_INPUT, result);
    EXPECT_FALSE(engine.getLastError().empty());
}

// Test invalid interval
TEST_F(ResamplingTest, InvalidInterval) {
    uint64_t interval_us = 0;  // Invalid
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    std::vector<MemRecord> records;
    MemRecord rec;
    rec.time_offset = 0;
    rec.value.f64_value = 100.0;
    rec.quality = 192;
    records.push_back(rec);

    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(0, records, resampled);

    EXPECT_EQ(ResampleResult::ERROR_INVALID_INTERVAL, result);
}

// Test quality averaging
TEST_F(ResamplingTest, QualityAveraging) {
    uint64_t interval_us = 60 * 1000000ULL;
    ResamplingEngine engine(interval_us, AggregationMethod::AVG);

    // Create data with varying quality
    std::vector<MemRecord> records;
    int64_t base_ts_us = 1000000000;

    for (int i = 0; i < 60; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = 100.0;
        // Quality alternates between 192 (good) and 128 (fair)
        rec.quality = (i % 2 == 0) ? 192 : 128;
        records.push_back(rec);
    }

    // Resample
    std::vector<ResampledPoint> resampled;
    auto result = engine.resample(base_ts_us, records, resampled);

    ASSERT_EQ(ResampleResult::SUCCESS, result);

    // Average quality should be around 160
    EXPECT_NEAR(160, resampled[0].quality, 5);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
