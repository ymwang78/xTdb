#include <gtest/gtest.h>
#include "xTdb/quantized_16_encoder.h"
#include "xTdb/quantized_16_decoder.h"
#include <cmath>

using namespace xtdb;

class Quantized16Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup common test data
    }

    void TearDown() override {
    }
};

// Test basic encoding and decoding round-trip
TEST_F(Quantized16Test, BasicRoundTrip) {
    // Range: [0.0, 100.0]
    Quantized16Encoder encoder(0.0, 100.0);
    Quantized16Decoder decoder(0.0, 100.0);

    // Create test data
    std::vector<MemRecord> input_records;
    for (int i = 0; i <= 100; i += 10) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = static_cast<double>(i);
        rec.quality = 192;
        input_records.push_back(rec);
    }

    // Encode
    std::vector<Quantized16Encoder::QuantizedPoint> quantized;
    auto encode_result = encoder.encode(1000000, input_records, quantized);
    ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, encode_result);
    EXPECT_EQ(input_records.size(), quantized.size());

    // Decode
    std::vector<MemRecord> output_records;
    auto decode_result = decoder.decode(1000000, quantized, output_records);
    ASSERT_EQ(Quantized16Decoder::DecodeResult::SUCCESS, decode_result);
    EXPECT_EQ(input_records.size(), output_records.size());

    // Verify precision
    double max_error = decoder.getMaxPrecisionLoss();
    std::cout << "Max precision loss: " << max_error << " (<0.0015% of range)" << std::endl;

    for (size_t i = 0; i < input_records.size(); i++) {
        double original = input_records[i].value.f64_value;
        double decoded = output_records[i].value.f64_value;
        double error = std::abs(original - decoded);

        EXPECT_LE(error, max_error * 1.01);  // Allow 1% tolerance on max error
        EXPECT_EQ(input_records[i].time_offset, output_records[i].time_offset);
        EXPECT_EQ(input_records[i].quality, output_records[i].quality);
    }
}

// Test precision with large value range
TEST_F(Quantized16Test, LargeRange) {
    // Range: [0.0, 10000.0]
    Quantized16Encoder encoder(0.0, 10000.0);
    Quantized16Decoder decoder(0.0, 10000.0);

    // Test values at various points in the range
    std::vector<double> test_values = {0.0, 100.0, 1000.0, 5000.0, 9000.0, 10000.0};

    for (double val : test_values) {
        std::vector<MemRecord> input;
        MemRecord rec;
        rec.time_offset = 0;
        rec.value.f64_value = val;
        rec.quality = 192;
        input.push_back(rec);

        // Encode
        std::vector<Quantized16Encoder::QuantizedPoint> quantized;
        auto encode_result = encoder.encode(0, input, quantized);
        ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, encode_result);

        // Decode
        std::vector<MemRecord> output;
        auto decode_result = decoder.decode(0, quantized, output);
        ASSERT_EQ(Quantized16Decoder::DecodeResult::SUCCESS, decode_result);

        // Verify
        double error = std::abs(val - output[0].value.f64_value);
        double max_error = decoder.getMaxPrecisionLoss();
        EXPECT_LE(error, max_error * 1.01);
    }
}

// Test precision with small value range
TEST_F(Quantized16Test, SmallRange) {
    // Range: [20.0, 25.0] (typical temperature range)
    Quantized16Encoder encoder(20.0, 25.0);
    Quantized16Decoder decoder(20.0, 25.0);

    // Test fine-grained values
    std::vector<MemRecord> input;
    for (double val = 20.0; val <= 25.0; val += 0.1) {
        MemRecord rec;
        rec.time_offset = static_cast<uint32_t>((val - 20.0) * 1000);
        rec.value.f64_value = val;
        rec.quality = 192;
        input.push_back(rec);
    }

    // Encode
    std::vector<Quantized16Encoder::QuantizedPoint> quantized;
    auto encode_result = encoder.encode(0, input, quantized);
    ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, encode_result);

    // Decode
    std::vector<MemRecord> output;
    auto decode_result = decoder.decode(0, quantized, output);
    ASSERT_EQ(Quantized16Decoder::DecodeResult::SUCCESS, decode_result);

    // Verify precision
    double max_error = decoder.getMaxPrecisionLoss();
    std::cout << "Small range max precision loss: " << max_error << std::endl;
    EXPECT_LT(max_error, 0.00004);  // Should be very small for 5-unit range

    for (size_t i = 0; i < input.size(); i++) {
        double error = std::abs(input[i].value.f64_value - output[i].value.f64_value);
        EXPECT_LE(error, max_error * 1.01);
    }
}

// Test out-of-range value detection
TEST_F(Quantized16Test, OutOfRangeDetection) {
    Quantized16Encoder encoder(0.0, 100.0);

    // Test value below range
    std::vector<MemRecord> input1;
    MemRecord rec1;
    rec1.time_offset = 0;
    rec1.value.f64_value = -10.0;
    rec1.quality = 192;
    input1.push_back(rec1);

    std::vector<Quantized16Encoder::QuantizedPoint> quantized1;
    auto result1 = encoder.encode(0, input1, quantized1);
    EXPECT_EQ(Quantized16Encoder::EncodeResult::ERROR_VALUE_OUT_OF_RANGE, result1);
    EXPECT_FALSE(encoder.getLastError().empty());

    // Test value above range
    std::vector<MemRecord> input2;
    MemRecord rec2;
    rec2.time_offset = 0;
    rec2.value.f64_value = 110.0;
    rec2.quality = 192;
    input2.push_back(rec2);

    std::vector<Quantized16Encoder::QuantizedPoint> quantized2;
    auto result2 = encoder.encode(0, input2, quantized2);
    EXPECT_EQ(Quantized16Encoder::EncodeResult::ERROR_VALUE_OUT_OF_RANGE, result2);
}

// Test compression ratio calculation
TEST_F(Quantized16Test, CompressionRatio) {
    Quantized16Encoder encoder(0.0, 100.0);

    // Create 100 records
    std::vector<MemRecord> input;
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = static_cast<double>(i);
        rec.quality = 192;
        input.push_back(rec);
    }

    // Encode
    std::vector<Quantized16Encoder::QuantizedPoint> quantized;
    auto result = encoder.encode(0, input, quantized);
    ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, result);

    // Verify compression ratio
    double ratio = encoder.getCompressionRatio();
    std::cout << "Compression ratio: " << ratio << "x" << std::endl;

    // Expected: 13 bytes (original) / 7 bytes (compressed) = 1.857x
    EXPECT_NEAR(ratio, 1.857, 0.01);
}

// Test boundary values
TEST_F(Quantized16Test, BoundaryValues) {
    Quantized16Encoder encoder(0.0, 100.0);
    Quantized16Decoder decoder(0.0, 100.0);

    // Test exact boundary values
    std::vector<double> boundary_values = {0.0, 100.0};

    for (double val : boundary_values) {
        std::vector<MemRecord> input;
        MemRecord rec;
        rec.time_offset = 0;
        rec.value.f64_value = val;
        rec.quality = 192;
        input.push_back(rec);

        // Encode
        std::vector<Quantized16Encoder::QuantizedPoint> quantized;
        auto encode_result = encoder.encode(0, input, quantized);
        ASSERT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, encode_result);

        // Decode
        std::vector<MemRecord> output;
        auto decode_result = decoder.decode(0, quantized, output);
        ASSERT_EQ(Quantized16Decoder::DecodeResult::SUCCESS, decode_result);

        // Boundary values should be exact
        EXPECT_NEAR(val, output[0].value.f64_value, 0.001);
    }
}

// Test empty input
TEST_F(Quantized16Test, EmptyInput) {
    Quantized16Encoder encoder(0.0, 100.0);
    Quantized16Decoder decoder(0.0, 100.0);

    std::vector<MemRecord> input;
    std::vector<Quantized16Encoder::QuantizedPoint> quantized;

    // Encode empty
    auto encode_result = encoder.encode(0, input, quantized);
    EXPECT_EQ(Quantized16Encoder::EncodeResult::SUCCESS, encode_result);
    EXPECT_TRUE(quantized.empty());

    // Decode empty
    std::vector<MemRecord> output;
    auto decode_result = decoder.decode(0, quantized, output);
    EXPECT_EQ(Quantized16Decoder::DecodeResult::SUCCESS, decode_result);
    EXPECT_TRUE(output.empty());
}

// Test invalid range
TEST_F(Quantized16Test, InvalidRange) {
    // High < Low
    Quantized16Encoder encoder(100.0, 0.0);

    std::vector<MemRecord> input;
    MemRecord rec;
    rec.time_offset = 0;
    rec.value.f64_value = 50.0;
    rec.quality = 192;
    input.push_back(rec);

    std::vector<Quantized16Encoder::QuantizedPoint> quantized;
    auto result = encoder.encode(0, input, quantized);
    EXPECT_EQ(Quantized16Encoder::EncodeResult::ERROR_INVALID_RANGE, result);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
