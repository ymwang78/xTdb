#include <gtest/gtest.h>
#include "xTdb/swinging_door_encoder.h"
#include "xTdb/swinging_door_decoder.h"
#include "xTdb/quantized_16_encoder.h"
#include "xTdb/quantized_16_decoder.h"
#include <vector>
#include <cmath>
#include <iostream>

using namespace xtdb;

class CompressionIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    void TearDown() override {
    }

    // Helper: Generate smooth sine wave data
    std::vector<MemRecord> generateSmoothData(size_t count, double amplitude = 100.0, double period = 1000.0) {
        std::vector<MemRecord> records;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i;  // milliseconds
            rec.value.f64_value = amplitude * std::sin(2.0 * M_PI * i / period);
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Generate noisy data
    std::vector<MemRecord> generateNoisyData(size_t count, double mean = 50.0, double noise = 5.0) {
        std::vector<MemRecord> records;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i;
            // Add random noise
            double noise_factor = (std::rand() % 100 - 50) / 50.0;
            rec.value.f64_value = mean + noise * noise_factor;
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Generate sparse data (with gaps)
    std::vector<MemRecord> generateSparseData(size_t count, size_t gap = 10) {
        std::vector<MemRecord> records;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * gap;  // Gaps between points
            rec.value.f64_value = 100.0 + i * 0.1;  // Slow linear trend
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Calculate compression ratio
    double calculateCompressionRatio(size_t original_count, size_t compressed_count) {
        if (compressed_count == 0) return 0.0;
        return static_cast<double>(original_count) / static_cast<double>(compressed_count);
    }
};

// Test 1: Swinging Door + Quantization on smooth data
TEST_F(CompressionIntegrationTest, SmoothDataCombinedCompression) {
    // Generate smooth sine wave (10000 points)
    auto records = generateSmoothData(10000, 100.0, 1000.0);
    int64_t base_ts_us = 1000000000;  // Base timestamp

    std::cout << "\n=== Smooth Data Combined Compression ===" << std::endl;
    std::cout << "Original points: " << records.size() << std::endl;

    // Step 1: Apply Swinging Door compression
    SwingingDoorEncoder sd_encoder(1.0, 1.0);  // tolerance=1.0, compression_factor=1.0
    std::vector<SwingingDoorEncoder::CompressedPoint> sd_compressed;
    auto sd_result = sd_encoder.encode(base_ts_us, records, sd_compressed);

    ASSERT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, sd_result);
    std::cout << "After Swinging Door: " << sd_compressed.size() << " points" << std::endl;
    double sd_ratio = calculateCompressionRatio(records.size(), sd_compressed.size());
    std::cout << "SD compression ratio: " << sd_ratio << ":1" << std::endl;

    // Expect high compression for smooth data (> 10:1)
    EXPECT_GT(sd_ratio, 10.0);

    // Step 2: Apply 16-bit quantization to original data (for comparison)
    Quantized16Encoder q16_encoder(-100.0, 100.0);
    std::vector<Quantized16Encoder::QuantizedPoint> quantized_values;
    auto q16_result = q16_encoder.encode(base_ts_us, records, quantized_values);

    ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, q16_result);

    // Calculate storage savings
    size_t original_bytes = records.size() * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
    size_t sd_bytes = sd_compressed.size() * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
    size_t quantized_bytes = quantized_values.size() * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));
    size_t combined_bytes = sd_compressed.size() * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));

    double sd_only_saving = 100.0 * (1.0 - static_cast<double>(sd_bytes) / original_bytes);
    double q16_only_saving = 100.0 * (1.0 - static_cast<double>(quantized_bytes) / original_bytes);
    double combined_saving = 100.0 * (1.0 - static_cast<double>(combined_bytes) / original_bytes);

    std::cout << "Original storage: " << original_bytes << " bytes" << std::endl;
    std::cout << "SD only: " << sd_bytes << " bytes (saving: " << sd_only_saving << "%)" << std::endl;
    std::cout << "Q16 only: " << quantized_bytes << " bytes (saving: " << q16_only_saving << "%)" << std::endl;
    std::cout << "Combined: " << combined_bytes << " bytes (saving: " << combined_saving << "%)" << std::endl;

    // Expect > 90% storage saving for smooth data
    EXPECT_GT(combined_saving, 90.0);

    // Step 3: Verify reconstruction accuracy
    SwingingDoorDecoder sd_decoder;

    // Test interpolation at various points
    double max_error = 0.0;
    for (size_t i = 0; i < records.size(); i += 100) {  // Sample every 100 points
        int64_t query_ts_us = base_ts_us + records[i].time_offset * 1000;
        SwingingDoorDecoder::DecodedPoint decoded;

        auto decode_result = sd_decoder.interpolate(base_ts_us, sd_compressed, query_ts_us, decoded);
        ASSERT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, decode_result);

        double error = std::abs(decoded.value - records[i].value.f64_value);
        max_error = std::max(max_error, error);
    }

    std::cout << "Max interpolation error: " << max_error << std::endl;
    EXPECT_LT(max_error, 3.0);  // Error should be within ~1.5x tolerance for curved data
}

// Test 2: Noisy data compression
TEST_F(CompressionIntegrationTest, NoisyDataCompression) {
    auto records = generateNoisyData(10000, 50.0, 5.0);
    int64_t base_ts_us = 1000000000;

    std::cout << "\n=== Noisy Data Compression ===" << std::endl;
    std::cout << "Original points: " << records.size() << std::endl;

    // Apply Swinging Door with larger tolerance for noisy data
    SwingingDoorEncoder sd_encoder(2.0, 1.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> sd_compressed;
    auto result = sd_encoder.encode(base_ts_us, records, sd_compressed);

    ASSERT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);
    std::cout << "After Swinging Door: " << sd_compressed.size() << " points" << std::endl;
    double sd_ratio = calculateCompressionRatio(records.size(), sd_compressed.size());
    std::cout << "SD compression ratio: " << sd_ratio << ":1" << std::endl;

    // Noisy data should still achieve reasonable compression (> 3:1)
    EXPECT_GT(sd_ratio, 3.0);

    // Calculate combined storage
    size_t original_bytes = records.size() * (sizeof(uint32_t) + sizeof(double) + sizeof(uint8_t));
    size_t combined_bytes = sd_compressed.size() * (sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint8_t));
    double combined_saving = 100.0 * (1.0 - static_cast<double>(combined_bytes) / original_bytes);

    std::cout << "Combined storage saving: " << combined_saving << "%" << std::endl;

    // Even noisy data should achieve > 70% storage saving
    EXPECT_GT(combined_saving, 70.0);
}

// Test 3: Sparse data compression
TEST_F(CompressionIntegrationTest, SparseDataCompression) {
    auto records = generateSparseData(1000, 10);
    int64_t base_ts_us = 1000000000;

    std::cout << "\n=== Sparse Data Compression ===" << std::endl;
    std::cout << "Original points: " << records.size() << std::endl;

    // Apply Swinging Door
    SwingingDoorEncoder sd_encoder(0.5, 1.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> sd_compressed;
    auto result = sd_encoder.encode(base_ts_us, records, sd_compressed);

    ASSERT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);
    std::cout << "After Swinging Door: " << sd_compressed.size() << " points" << std::endl;
    double sd_ratio = calculateCompressionRatio(records.size(), sd_compressed.size());
    std::cout << "SD compression ratio: " << sd_ratio << ":1" << std::endl;

    // Linear trend should compress extremely well (> 50:1)
    EXPECT_GT(sd_ratio, 50.0);
}

// Test 4: Quantization precision with different ranges
TEST_F(CompressionIntegrationTest, QuantizationPrecision) {
    std::cout << "\n=== Quantization Precision Test ===" << std::endl;

    // Create a single test record
    std::vector<MemRecord> records;
    MemRecord rec;
    rec.time_offset = 0;
    rec.value.f64_value = 5.123456;
    rec.quality = 192;
    records.push_back(rec);

    int64_t base_ts_us = 1000000000;

    // Test narrow range (0-10)
    Quantized16Encoder narrow_encoder(0.0, 10.0);
    Quantized16Decoder narrow_decoder(0.0, 10.0);

    std::vector<Quantized16Encoder::QuantizedPoint> quantized;
    narrow_encoder.encode(base_ts_us, records, quantized);

    std::vector<MemRecord> decoded_records;
    narrow_decoder.decode(base_ts_us, quantized, decoded_records);

    double test_value = rec.value.f64_value;
    double decoded_value = decoded_records[0].value.f64_value;
    double error = std::abs(decoded_value - test_value);
    double relative_error = error / test_value;

    std::cout << "Narrow range (0-10):" << std::endl;
    std::cout << "  Original: " << test_value << std::endl;
    std::cout << "  Decoded: " << decoded_value << std::endl;
    std::cout << "  Relative error: " << (relative_error * 100.0) << "%" << std::endl;

    EXPECT_LT(relative_error, 0.002);  // < 0.2%

    // Test wide range (0-1000)
    rec.value.f64_value = 512.345;
    records[0] = rec;

    Quantized16Encoder wide_encoder(0.0, 1000.0);
    Quantized16Decoder wide_decoder(0.0, 1000.0);

    quantized.clear();
    wide_encoder.encode(base_ts_us, records, quantized);

    decoded_records.clear();
    wide_decoder.decode(base_ts_us, quantized, decoded_records);

    test_value = rec.value.f64_value;
    decoded_value = decoded_records[0].value.f64_value;
    error = std::abs(decoded_value - test_value);
    relative_error = error / test_value;

    std::cout << "Wide range (0-1000):" << std::endl;
    std::cout << "  Original: " << test_value << std::endl;
    std::cout << "  Decoded: " << decoded_value << std::endl;
    std::cout << "  Relative error: " << (relative_error * 100.0) << "%" << std::endl;

    EXPECT_LT(relative_error, 0.002);  // < 0.2%
}

// Test 5: Combined compression with different data patterns
TEST_F(CompressionIntegrationTest, MixedDataPatterns) {
    std::cout << "\n=== Mixed Data Patterns ===" << std::endl;
    int64_t base_ts_us = 1000000000;

    // Create mixed data: smooth + step changes
    std::vector<MemRecord> records;

    // Segment 1: Smooth (0-1000)
    for (size_t i = 0; i < 1000; i++) {
        MemRecord rec;
        rec.time_offset = i;
        rec.value.f64_value = 50.0 + 10.0 * std::sin(2.0 * M_PI * i / 200.0);
        rec.quality = 192;
        records.push_back(rec);
    }

    // Segment 2: Step change
    for (size_t i = 1000; i < 1100; i++) {
        MemRecord rec;
        rec.time_offset = i;
        rec.value.f64_value = 100.0;  // Sudden jump
        rec.quality = 192;
        records.push_back(rec);
    }

    // Segment 3: Linear ramp
    for (size_t i = 1100; i < 2000; i++) {
        MemRecord rec;
        rec.time_offset = i;
        rec.value.f64_value = 100.0 + (i - 1100) * 0.05;
        rec.quality = 192;
        records.push_back(rec);
    }

    std::cout << "Original points: " << records.size() << std::endl;

    // Apply combined compression
    SwingingDoorEncoder sd_encoder(1.0, 1.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> sd_compressed;
    auto result = sd_encoder.encode(base_ts_us, records, sd_compressed);

    ASSERT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);
    std::cout << "After Swinging Door: " << sd_compressed.size() << " points" << std::endl;
    double sd_ratio = calculateCompressionRatio(records.size(), sd_compressed.size());
    std::cout << "Compression ratio: " << sd_ratio << ":1" << std::endl;

    // Mixed patterns should still achieve good compression (> 5:1)
    EXPECT_GT(sd_ratio, 5.0);

    // Verify step changes are captured
    SwingingDoorDecoder decoder;
    SwingingDoorDecoder::DecodedPoint decoded_1000, decoded_999;

    auto result_1000 = decoder.interpolate(base_ts_us, sd_compressed, base_ts_us + 1000000, decoded_1000);
    auto result_999 = decoder.interpolate(base_ts_us, sd_compressed, base_ts_us + 999000, decoded_999);

    ASSERT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, result_1000);
    ASSERT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, result_999);

    std::cout << "Value at t=999ms: " << decoded_999.value << std::endl;
    std::cout << "Value at t=1000ms: " << decoded_1000.value << std::endl;

    // Step change should be captured (values should be significantly different)
    // Note: Interpolation may smooth the transition, so we expect > 20.0 difference
    EXPECT_GT(std::abs(decoded_1000.value - decoded_999.value), 20.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
