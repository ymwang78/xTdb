#include <gtest/gtest.h>
#include "xTdb/block_writer.h"
#include "xTdb/block_reader.h"
#include "xTdb/aligned_io.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/directory_builder.h"
#include "test_utils.h"
#include <filesystem>
#include <cmath>

using namespace xtdb;
namespace fs = std::filesystem;

class CompressionE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string temp_dir = get_temp_dir();
        test_file_ = join_path(temp_dir, "xtdb_compression_test.dat");

        // Clean up any existing test file
        if (fs::exists(test_file_)) {
            fs::remove(test_file_);
        }

        io_ = new AlignedIO();
        auto result = io_->open(test_file_, true);
        ASSERT_EQ(IOResult::SUCCESS, result);
    }

    void TearDown() override {
        if (io_) {
            io_->close();
            delete io_;
            io_ = nullptr;
        }

        // Clean up test file
        if (fs::exists(test_file_)) {
            fs::remove(test_file_);
        }
    }

    std::string test_file_;
    AlignedIO* io_;
};

// Test Swinging Door write and read
TEST_F(CompressionE2ETest, SwingingDoorWriteRead) {
    // Setup layout for 16KB blocks
    ChunkLayout layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);
    ASSERT_TRUE(LayoutCalculator::validateLayout(layout));

    // Create test data - linear values
    TagBuffer tag_buffer;
    tag_buffer.tag_id = 100;
    tag_buffer.value_type = ValueType::VT_F64;
    tag_buffer.time_unit = TimeUnit::TU_MS;
    tag_buffer.start_ts_us = 1000000;  // 1 second
    tag_buffer.encoding_type = EncodingType::ENC_SWINGING_DOOR;
    tag_buffer.encoding_tolerance = 1.0;
    tag_buffer.encoding_compression_factor = 1.0;

    // 100 linear points: 0, 10, 20, ..., 990
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;  // milliseconds
        rec.value.f64_value = 10.0 * i;
        rec.quality = 192;
        tag_buffer.records.push_back(rec);
    }

    // Write compressed data
    BlockWriter writer(io_, layout, 0);
    uint64_t chunk_offset = 0;
    uint32_t data_block_index = 0;

    auto write_result = writer.writeBlock(chunk_offset, data_block_index, tag_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, write_result);

    std::cout << "Original points: " << tag_buffer.records.size() << std::endl;

    // Setup directory entry for reading
    BlockDirEntryV16 dir_entry;
    dir_entry.tag_id = tag_buffer.tag_id;
    dir_entry.value_type = static_cast<uint8_t>(tag_buffer.value_type);
    dir_entry.encoding_type = static_cast<uint8_t>(tag_buffer.encoding_type);
    dir_entry.start_ts_us = tag_buffer.start_ts_us;
    dir_entry.end_ts_us = tag_buffer.start_ts_us + 99000;  // 99 seconds later
    dir_entry.record_count = static_cast<uint32_t>(tag_buffer.records.size());

    // Store encoding parameters
    float tolerance_f = static_cast<float>(tag_buffer.encoding_tolerance);
    float factor_f = static_cast<float>(tag_buffer.encoding_compression_factor);
    std::memcpy(&dir_entry.encoding_param1, &tolerance_f, 4);
    std::memcpy(&dir_entry.encoding_param2, &factor_f, 4);

    // Read compressed data
    BlockReader reader(io_, layout);
    std::vector<MemRecord> read_records;

    auto read_result = reader.readBlock(chunk_offset, data_block_index, dir_entry, read_records);
    ASSERT_EQ(ReadResult::SUCCESS, read_result);

    // Linear data should compress to 2 points (start and end)
    std::cout << "Compressed points: " << read_records.size() << std::endl;
    EXPECT_LE(read_records.size(), 3u);  // Should be 2 or 3 points
    EXPECT_GE(read_records.size(), 2u);

    // Verify first and last points
    EXPECT_EQ(0u, read_records[0].time_offset);
    EXPECT_DOUBLE_EQ(0.0, read_records[0].value.f64_value);

    EXPECT_EQ(99000u, read_records[read_records.size()-1].time_offset);
    EXPECT_DOUBLE_EQ(990.0, read_records[read_records.size()-1].value.f64_value);
}

// Test RAW vs Compressed comparison
TEST_F(CompressionE2ETest, RAWvsCompressed) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);

    // Create zigzag data
    TagBuffer raw_buffer, compressed_buffer;
    raw_buffer.tag_id = compressed_buffer.tag_id = 200;
    raw_buffer.value_type = compressed_buffer.value_type = ValueType::VT_F64;
    raw_buffer.time_unit = compressed_buffer.time_unit = TimeUnit::TU_MS;
    raw_buffer.start_ts_us = compressed_buffer.start_ts_us = 1000000;

    // RAW encoding
    raw_buffer.encoding_type = EncodingType::ENC_RAW;

    // Swinging Door with tolerance 10.0 (should handle zigzag between 50 and 60)
    compressed_buffer.encoding_type = EncodingType::ENC_SWINGING_DOOR;
    compressed_buffer.encoding_tolerance = 10.0;
    compressed_buffer.encoding_compression_factor = 1.0;

    // 100 zigzag points: 50, 60, 50, 60, ...
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = (i % 2 == 0) ? 50.0 : 60.0;
        rec.quality = 192;
        raw_buffer.records.push_back(rec);
        compressed_buffer.records.push_back(rec);
    }

    // Write RAW
    BlockWriter writer(io_, layout, 0);
    auto raw_write = writer.writeBlock(0, 0, raw_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, raw_write);

    // Write compressed
    auto comp_write = writer.writeBlock(0, 1, compressed_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, comp_write);

    // Read back and compare point counts
    BlockDirEntryV16 raw_entry, comp_entry;
    raw_entry.encoding_type = static_cast<uint8_t>(EncodingType::ENC_RAW);
    raw_entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    raw_entry.record_count = 100;

    comp_entry.encoding_type = static_cast<uint8_t>(EncodingType::ENC_SWINGING_DOOR);
    comp_entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    comp_entry.record_count = 100;

    BlockReader reader(io_, layout);
    std::vector<MemRecord> raw_read, comp_read;

    reader.readBlock(0, 0, raw_entry, raw_read);
    reader.readBlock(0, 1, comp_entry, comp_read);

    std::cout << "RAW points: " << raw_read.size() << std::endl;
    std::cout << "Compressed points: " << comp_read.size() << std::endl;
    std::cout << "Compression ratio: " << (double)raw_read.size() / comp_read.size() << "x" << std::endl;

    // Zigzag with tolerance 10 should compress significantly
    EXPECT_EQ(100u, raw_read.size());
    EXPECT_LT(comp_read.size(), 100u);  // Should be compressed
    EXPECT_GE(comp_read.size(), 2u);    // At least 2 points
}

// Test 16-bit Quantization write and read
TEST_F(CompressionE2ETest, Quantized16WriteRead) {
    // Setup layout for 16KB blocks
    ChunkLayout layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);
    ASSERT_TRUE(LayoutCalculator::validateLayout(layout));

    // Create test data - temperature values in range [20.0, 25.0]
    TagBuffer tag_buffer;
    tag_buffer.tag_id = 300;
    tag_buffer.value_type = ValueType::VT_F64;
    tag_buffer.time_unit = TimeUnit::TU_MS;
    tag_buffer.start_ts_us = 1000000;  // 1 second
    tag_buffer.encoding_type = EncodingType::ENC_QUANTIZED_16;
    tag_buffer.encoding_tolerance = 20.0;  // low_extreme
    tag_buffer.encoding_compression_factor = 25.0;  // high_extreme

    // 100 temperature values: 20.0, 20.05, 20.1, ..., 24.95
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;  // milliseconds
        rec.value.f64_value = 20.0 + (i * 0.05);
        rec.quality = 192;
        tag_buffer.records.push_back(rec);
    }

    // Write quantized data
    BlockWriter writer(io_, layout, 0);
    uint64_t chunk_offset = 0;
    uint32_t data_block_index = 0;

    auto write_result = writer.writeBlock(chunk_offset, data_block_index, tag_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, write_result);

    std::cout << "Original records: " << tag_buffer.records.size() << std::endl;

    // Setup directory entry for reading
    BlockDirEntryV16 dir_entry;
    dir_entry.tag_id = tag_buffer.tag_id;
    dir_entry.value_type = static_cast<uint8_t>(tag_buffer.value_type);
    dir_entry.encoding_type = static_cast<uint8_t>(tag_buffer.encoding_type);
    dir_entry.start_ts_us = tag_buffer.start_ts_us;
    dir_entry.end_ts_us = tag_buffer.start_ts_us + 99000;
    dir_entry.record_count = static_cast<uint32_t>(tag_buffer.records.size());

    // Store encoding parameters (low_extreme and high_extreme as floats)
    float low_extreme_f = static_cast<float>(tag_buffer.encoding_tolerance);
    float high_extreme_f = static_cast<float>(tag_buffer.encoding_compression_factor);
    std::memcpy(&dir_entry.encoding_param1, &low_extreme_f, 4);
    std::memcpy(&dir_entry.encoding_param2, &high_extreme_f, 4);

    // Read quantized data
    BlockReader reader(io_, layout);
    std::vector<MemRecord> read_records;

    auto read_result = reader.readBlock(chunk_offset, data_block_index, dir_entry, read_records);
    ASSERT_EQ(ReadResult::SUCCESS, read_result);

    // Verify all records are preserved
    std::cout << "Quantized records: " << read_records.size() << std::endl;
    EXPECT_EQ(tag_buffer.records.size(), read_records.size());

    // Verify precision loss is within acceptable range
    double max_error = 0.0;
    for (size_t i = 0; i < tag_buffer.records.size(); i++) {
        double original = tag_buffer.records[i].value.f64_value;
        double decoded = read_records[i].value.f64_value;
        double error = std::abs(original - decoded);
        max_error = std::max(max_error, error);

        EXPECT_EQ(tag_buffer.records[i].time_offset, read_records[i].time_offset);
        EXPECT_EQ(tag_buffer.records[i].quality, read_records[i].quality);
    }

    std::cout << "Max precision loss: " << max_error << " ("
              << (max_error / 5.0 * 100.0) << "% of range)" << std::endl;

    // Precision loss should be < 0.0015% of range (5 * 0.0015% = 0.000075)
    EXPECT_LT(max_error, 0.00008);
}

// Test RAW vs Quantized comparison
TEST_F(CompressionE2ETest, RAWvsQuantized) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);

    // Create sensor data with typical range [0.0, 100.0]
    TagBuffer raw_buffer, quantized_buffer;
    raw_buffer.tag_id = quantized_buffer.tag_id = 400;
    raw_buffer.value_type = quantized_buffer.value_type = ValueType::VT_F64;
    raw_buffer.time_unit = quantized_buffer.time_unit = TimeUnit::TU_MS;
    raw_buffer.start_ts_us = quantized_buffer.start_ts_us = 1000000;

    // RAW encoding
    raw_buffer.encoding_type = EncodingType::ENC_RAW;

    // Quantized encoding with range [0.0, 100.0]
    quantized_buffer.encoding_type = EncodingType::ENC_QUANTIZED_16;
    quantized_buffer.encoding_tolerance = 0.0;     // low_extreme
    quantized_buffer.encoding_compression_factor = 100.0;  // high_extreme

    // 100 sensor values: 0.0, 1.0, 2.0, ..., 99.0
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = static_cast<double>(i);
        rec.quality = 192;
        raw_buffer.records.push_back(rec);
        quantized_buffer.records.push_back(rec);
    }

    // Write RAW
    BlockWriter writer(io_, layout, 0);
    auto raw_write = writer.writeBlock(0, 0, raw_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, raw_write);

    // Write Quantized
    auto quant_write = writer.writeBlock(0, 1, quantized_buffer);
    ASSERT_EQ(BlockWriteResult::SUCCESS, quant_write);

    // Read back and compare storage efficiency
    BlockDirEntryV16 raw_entry, quant_entry;

    // RAW entry
    raw_entry.encoding_type = static_cast<uint8_t>(EncodingType::ENC_RAW);
    raw_entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    raw_entry.record_count = 100;

    // Quantized entry
    quant_entry.encoding_type = static_cast<uint8_t>(EncodingType::ENC_QUANTIZED_16);
    quant_entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    quant_entry.record_count = 100;
    quant_entry.start_ts_us = quantized_buffer.start_ts_us;

    // Store quantized parameters
    float low_f = 0.0f;
    float high_f = 100.0f;
    std::memcpy(&quant_entry.encoding_param1, &low_f, 4);
    std::memcpy(&quant_entry.encoding_param2, &high_f, 4);

    BlockReader reader(io_, layout);
    std::vector<MemRecord> raw_read, quant_read;

    reader.readBlock(0, 0, raw_entry, raw_read);
    reader.readBlock(0, 1, quant_entry, quant_read);

    std::cout << "RAW size: 13 bytes/record * 100 = " << (13 * 100) << " bytes" << std::endl;
    std::cout << "Quantized size: 7 bytes/record * 100 = " << (7 * 100) << " bytes" << std::endl;
    std::cout << "Storage reduction: " << (1.0 - (700.0 / 1300.0)) * 100.0 << "%" << std::endl;

    // Verify both read all records
    EXPECT_EQ(100u, raw_read.size());
    EXPECT_EQ(100u, quant_read.size());

    // Verify quantized precision
    double max_error = 0.0;
    for (size_t i = 0; i < raw_read.size(); i++) {
        double error = std::abs(raw_read[i].value.f64_value - quant_read[i].value.f64_value);
        max_error = std::max(max_error, error);
    }

    std::cout << "Max quantization error: " << max_error << " ("
              << (max_error / 100.0 * 100.0) << "% of range)" << std::endl;

    // Error should be minimal for this range
    EXPECT_LT(max_error, 0.002);  // < 0.002% of range
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
