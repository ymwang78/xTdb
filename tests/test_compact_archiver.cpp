#include "xTdb/compact_archiver.h"
#include "xTdb/file_container.h"
#include "xTdb/compact_container.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/aligned_io.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <cstring>

using namespace xtdb;

class CompactArchiverTest : public ::testing::Test {
protected:
    void SetUp() override {
        raw_path_ = "/tmp/test_archiver_raw.dat";
        compact_path_ = "/tmp/test_archiver_compact.dat";

        // Remove test files if they exist
        std::filesystem::remove(raw_path_);
        std::filesystem::remove(compact_path_);

        // Calculate layout using LayoutCalculator
        layout_ = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);
    }

    void TearDown() override {
        std::filesystem::remove(raw_path_);
        std::filesystem::remove(compact_path_);
    }

    std::string raw_path_;
    std::string compact_path_;
    ChunkLayout layout_;
};

TEST_F(CompactArchiverTest, ArchiveSingleBlock) {
    // Create and open RAW container
    FileContainer raw_container(raw_path_, layout_, false, false);
    ASSERT_EQ(ContainerResult::SUCCESS, raw_container.open(true));

    // Create test data (16KB of repeated pattern) in aligned buffer
    AlignedBuffer test_data(layout_.block_size_bytes);
    uint8_t* data_ptr = static_cast<uint8_t*>(test_data.data());
    for (size_t i = 0; i < test_data.size(); i++) {
        data_ptr[i] = static_cast<uint8_t>(i % 256);
    }

    // Write test data to RAW container
    // Use LayoutCalculator to get correct block-aligned offset
    // Data block 0 is at physical block index = meta_blocks + 0
    uint32_t physical_block_index = layout_.meta_blocks + 0;  // First data block
    uint64_t raw_block_offset = LayoutCalculator::calculateBlockOffset(
        0,  // chunk_id
        physical_block_index,
        layout_
    );

    ContainerResult write_result = raw_container.write(
        test_data.data(),
        test_data.size(),
        raw_block_offset
    );
    ASSERT_EQ(ContainerResult::SUCCESS, write_result);

    // Create and open COMPACT container
    CompactContainer compact_container(compact_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.open(true));

    // Archive the block
    CompactArchiver archiver;
    ArchiveResult result = archiver.archiveBlock(
        raw_container,
        layout_,  // raw_layout
        0,  // chunk_id
        0,  // block_index
        compact_container,
        1000,  // tag_id
        1000000,  // start_ts_us
        2000000,  // end_ts_us
        1000,  // record_count
        EncodingType::ENC_RAW,
        ValueType::VT_F64,
        TimeUnit::TU_US
    );

    ASSERT_EQ(ArchiveResult::SUCCESS, result);

    // Check statistics
    const ArchiveStats& stats = archiver.getStats();
    EXPECT_EQ(1u, stats.blocks_archived);
    EXPECT_EQ(layout_.block_size_bytes, stats.bytes_read);
    EXPECT_GT(stats.bytes_read, stats.bytes_written);  // Should be compressed
    EXPECT_LT(stats.compression_ratio, 1.0);  // Should have compression

    // Seal COMPACT container
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.seal());

    // Read back from COMPACT container
    std::vector<uint8_t> read_buffer(layout_.block_size_bytes);
    uint32_t actual_size = 0;
    ContainerResult read_result = compact_container.readBlock(
        0,  // block_index
        read_buffer.data(),
        read_buffer.size(),
        actual_size
    );

    ASSERT_EQ(ContainerResult::SUCCESS, read_result);
    EXPECT_EQ(test_data.size(), actual_size);
    EXPECT_EQ(0, std::memcmp(test_data.data(), read_buffer.data(), test_data.size()));

    raw_container.close();
    compact_container.close();
}

TEST_F(CompactArchiverTest, ArchiveMultipleBlocks) {
    // Create and open RAW container
    FileContainer raw_container(raw_path_, layout_, false, false);
    ASSERT_EQ(ContainerResult::SUCCESS, raw_container.open(true));

    const int num_blocks = 5;

    // Write multiple blocks to RAW container
    for (int i = 0; i < num_blocks; i++) {
        // Use LayoutCalculator to get correct block-aligned offset
        uint32_t physical_block_index = layout_.meta_blocks + i;
        uint64_t block_offset = LayoutCalculator::calculateBlockOffset(
            0,  // chunk_id
            physical_block_index,
            layout_
        );

        // Create slightly different data for each block in aligned buffer
        AlignedBuffer block_data(layout_.block_size_bytes);
        uint8_t* data_ptr = static_cast<uint8_t*>(block_data.data());
        for (size_t j = 0; j < block_data.size(); j++) {
            data_ptr[j] = static_cast<uint8_t>((i + j) % 256);
        }

        ContainerResult write_result = raw_container.write(
            block_data.data(),
            block_data.size(),
            block_offset
        );
        ASSERT_EQ(ContainerResult::SUCCESS, write_result);
    }

    // Create and open COMPACT container
    CompactContainer compact_container(compact_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.open(true));

    // Archive all blocks
    CompactArchiver archiver;
    for (int i = 0; i < num_blocks; i++) {
        ArchiveResult result = archiver.archiveBlock(
            raw_container,
            layout_,  // raw_layout
            0,  // chunk_id
            i,  // block_index
            compact_container,
            1000 + i,  // tag_id
            1000000 + i * 1000,  // start_ts_us
            2000000 + i * 1000,  // end_ts_us
            1000,  // record_count
            EncodingType::ENC_RAW,
            ValueType::VT_F64,
            TimeUnit::TU_US
        );
        ASSERT_EQ(ArchiveResult::SUCCESS, result);
    }

    // Check statistics
    const ArchiveStats& stats = archiver.getStats();
    EXPECT_EQ(static_cast<uint32_t>(num_blocks), compact_container.getBlockCount());
    EXPECT_EQ(static_cast<uint64_t>(num_blocks), stats.blocks_archived);

    // Seal COMPACT container
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.seal());

    // Read back and verify all blocks
    for (int i = 0; i < num_blocks; i++) {
        std::vector<uint8_t> read_buffer(layout_.block_size_bytes);
        uint32_t actual_size = 0;
        ContainerResult read_result = compact_container.readBlock(
            i,  // block_index
            read_buffer.data(),
            read_buffer.size(),
            actual_size
        );

        ASSERT_EQ(ContainerResult::SUCCESS, read_result);
        EXPECT_EQ(layout_.block_size_bytes, actual_size);

        // Verify data pattern
        for (size_t j = 0; j < actual_size; j++) {
            EXPECT_EQ(static_cast<uint8_t>((i + j) % 256), read_buffer[j])
                << "Mismatch at block " << i << " byte " << j;
        }
    }

    raw_container.close();
    compact_container.close();
}

TEST_F(CompactArchiverTest, ErrorHandling) {
    // Test with closed RAW container
    FileContainer raw_container(raw_path_, layout_, false, false);
    CompactContainer compact_container(compact_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.open(true));

    CompactArchiver archiver;
    ArchiveResult result = archiver.archiveBlock(
        raw_container,  // Not opened
        layout_,  // raw_layout
        0, 0,
        compact_container,
        1000, 1000000, 2000000, 1000,
        EncodingType::ENC_RAW,
        ValueType::VT_F64,
        TimeUnit::TU_US
    );

    EXPECT_EQ(ArchiveResult::ERR_RAW_CONTAINER_NOT_OPEN, result);
    EXPECT_FALSE(archiver.getLastError().empty());

    compact_container.close();
}

TEST_F(CompactArchiverTest, CompressionEffectiveness) {
    // Create RAW container with compressible data
    FileContainer raw_container(raw_path_, layout_, false, false);
    ASSERT_EQ(ContainerResult::SUCCESS, raw_container.open(true));

    // Create highly compressible data (all zeros) in aligned buffer
    AlignedBuffer compressible_data(layout_.block_size_bytes);
    std::memset(compressible_data.data(), 0, compressible_data.size());

    // Use LayoutCalculator to get correct block-aligned offset
    uint32_t physical_block_index = layout_.meta_blocks + 0;
    uint64_t raw_block_offset = LayoutCalculator::calculateBlockOffset(
        0,  // chunk_id
        physical_block_index,
        layout_
    );
    ASSERT_EQ(ContainerResult::SUCCESS,
              raw_container.write(compressible_data.data(), compressible_data.size(), raw_block_offset));

    // Archive to COMPACT
    CompactContainer compact_container(compact_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container.open(true));

    CompactArchiver archiver;
    ASSERT_EQ(ArchiveResult::SUCCESS,
              archiver.archiveBlock(raw_container, layout_, 0, 0, compact_container,
                                   1000, 1000000, 2000000, 1000,
                                   EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US));

    const ArchiveStats& stats = archiver.getStats();

    // For all-zero data, compression should be very effective
    double space_savings = 1.0 - stats.compression_ratio;
    std::cout << "Compression ratio: " << stats.compression_ratio << std::endl;
    std::cout << "Space savings: " << (space_savings * 100.0) << "%" << std::endl;

    EXPECT_LT(stats.compression_ratio, 0.1);  // Should compress to less than 10%
    EXPECT_GT(space_savings, 0.9);  // Should save more than 90% space

    raw_container.close();
    compact_container.close();
}
