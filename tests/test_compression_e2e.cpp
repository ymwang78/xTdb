#include <gtest/gtest.h>
#include "xTdb/block_writer.h"
#include "xTdb/block_reader.h"
#include "xTdb/aligned_io.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/directory_builder.h"
#include <filesystem>
#include <cmath>

using namespace xtdb;
namespace fs = std::filesystem;

class CompressionE2ETest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/xtdb_compression_test.dat";

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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
