#include <gtest/gtest.h>
#include "xTdb/compact_container.h"
#include <vector>
#include <cstring>
#include <random>
#include <filesystem>

using namespace xtdb;

class CompactContainerTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_path_ = "/tmp/test_compact_container.dat";

        // Remove test file if exists
        if (std::filesystem::exists(test_path_)) {
            std::filesystem::remove(test_path_);
        }

        // Setup layout
        layout_.block_size_bytes = 16384;  // 16KB
        layout_.chunk_size_bytes = 256 * 1024 * 1024;  // 256MB
        layout_.meta_blocks = 0;
        layout_.data_blocks = 0;

        // Generate test data
        generateTestData();
    }

    void TearDown() override {
        // Cleanup test file
        if (std::filesystem::exists(test_path_)) {
            std::filesystem::remove(test_path_);
        }
    }

    void generateTestData() {
        // Create test data with patterns for compression
        test_data_.resize(16384);

        // Fill with semi-repetitive pattern (good compression)
        for (size_t i = 0; i < test_data_.size(); i++) {
            test_data_[i] = static_cast<uint8_t>((i % 256) + (i / 1024));
        }

        // Random data (poor compression)
        random_data_.resize(16384);
        std::mt19937 rng(42);
        std::uniform_int_distribution<uint32_t> dist(0, 255);
        for (size_t i = 0; i < random_data_.size(); i++) {
            random_data_[i] = static_cast<uint8_t>(dist(rng));
        }
    }

    std::string test_path_;
    ChunkLayout layout_;
    std::vector<uint8_t> test_data_;
    std::vector<uint8_t> random_data_;
};

// ============================================================================
// Basic Operations Tests
// ============================================================================

TEST_F(CompactContainerTest, CreateAndOpen) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);

    ContainerResult result = container.open(true);
    ASSERT_EQ(ContainerResult::SUCCESS, result);
    EXPECT_TRUE(container.isOpen());
    EXPECT_FALSE(container.isSealed());
    EXPECT_EQ(ContainerType::FILE_BASED, container.getType());
    EXPECT_EQ(test_path_, container.getIdentifier());

    container.close();
    EXPECT_FALSE(container.isOpen());
}

TEST_F(CompactContainerTest, OpenExisting) {
    // Create container
    {
        CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
        ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));
        container.close();
    }

    // Open existing
    {
        CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
        ContainerResult result = container.open(false);
        ASSERT_EQ(ContainerResult::SUCCESS, result);
        EXPECT_TRUE(container.isOpen());
        container.close();
    }
}

// ============================================================================
// Block Write/Read Tests
// ============================================================================

TEST_F(CompactContainerTest, WriteAndReadSingleBlock) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write block
    ContainerResult result = container.writeBlock(
        1000,                              // tag_id
        0,                                 // block_index
        test_data_.data(),
        test_data_.size(),
        1000000,                           // start_ts_us
        2000000,                           // end_ts_us
        1000,                              // record_count
        EncodingType::ENC_RAW,
        ValueType::VT_F64,
        TimeUnit::TU_US
    );
    ASSERT_EQ(ContainerResult::SUCCESS, result);
    EXPECT_EQ(1u, container.getBlockCount());

    // Seal container
    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());
    EXPECT_TRUE(container.isSealed());

    // Read block back
    std::vector<uint8_t> read_buffer(test_data_.size());
    uint32_t actual_size = 0;
    result = container.readBlock(0, read_buffer.data(), read_buffer.size(), actual_size);
    ASSERT_EQ(ContainerResult::SUCCESS, result);
    EXPECT_EQ(test_data_.size(), actual_size);
    EXPECT_EQ(0, std::memcmp(test_data_.data(), read_buffer.data(), test_data_.size()));

    container.close();
}

TEST_F(CompactContainerTest, WriteAndReadMultipleBlocks) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    const int num_blocks = 10;

    // Write multiple blocks
    for (int i = 0; i < num_blocks; i++) {
        ContainerResult result = container.writeBlock(
            1000 + i,                      // tag_id
            i,                             // block_index
            test_data_.data(),
            test_data_.size(),
            1000000 + i * 1000,            // start_ts_us
            2000000 + i * 1000,            // end_ts_us
            1000,
            EncodingType::ENC_RAW,
            ValueType::VT_F64,
            TimeUnit::TU_US
        );
        ASSERT_EQ(ContainerResult::SUCCESS, result);
    }

    EXPECT_EQ(static_cast<uint32_t>(num_blocks), container.getBlockCount());

    // Seal
    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());

    // Read all blocks back
    for (int i = 0; i < num_blocks; i++) {
        std::vector<uint8_t> read_buffer(test_data_.size());
        uint32_t actual_size = 0;
        ContainerResult result = container.readBlock(i, read_buffer.data(), read_buffer.size(), actual_size);
        if (result != ContainerResult::SUCCESS) {
            std::cerr << "Block " << i << " read failed: " << container.getLastError() << std::endl;
        }
        ASSERT_EQ(ContainerResult::SUCCESS, result);
        EXPECT_EQ(test_data_.size(), actual_size);
        EXPECT_EQ(0, std::memcmp(test_data_.data(), read_buffer.data(), test_data_.size()));
    }

    container.close();
}

TEST_F(CompactContainerTest, ReopenAndRead) {
    const int num_blocks = 5;

    // Write blocks and seal
    {
        CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
        ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

        for (int i = 0; i < num_blocks; i++) {
            ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
                1000 + i, i, test_data_.data(), test_data_.size(),
                1000000 + i * 1000, 2000000 + i * 1000, 1000,
                EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
            ));
        }

        ASSERT_EQ(ContainerResult::SUCCESS, container.seal());
        container.close();
    }

    // Reopen and read
    {
        CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
        ASSERT_EQ(ContainerResult::SUCCESS, container.open(false));
        EXPECT_TRUE(container.isSealed());
        EXPECT_EQ(static_cast<uint32_t>(num_blocks), container.getBlockCount());

        for (int i = 0; i < num_blocks; i++) {
            std::vector<uint8_t> read_buffer(test_data_.size());
            uint32_t actual_size = 0;
            ContainerResult result = container.readBlock(i, read_buffer.data(), read_buffer.size(), actual_size);
            ASSERT_EQ(ContainerResult::SUCCESS, result);
            EXPECT_EQ(test_data_.size(), actual_size);
            EXPECT_EQ(0, std::memcmp(test_data_.data(), read_buffer.data(), test_data_.size()));
        }

        container.close();
    }
}

// ============================================================================
// Compression Tests
// ============================================================================

TEST_F(CompactContainerTest, CompressionEffectiveness) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write repetitive data (should compress well)
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 0, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    ));

    // Check compression stats
    uint64_t total_original, total_compressed;
    double compression_ratio;
    container.getCompressionStats(total_original, total_compressed, compression_ratio);

    EXPECT_EQ(test_data_.size(), total_original);
    EXPECT_GT(total_compressed, 0u);
    EXPECT_LT(total_compressed, total_original);  // Should be compressed
    EXPECT_GT(compression_ratio, 0.0);
    EXPECT_LT(compression_ratio, 1.0);  // Good compression

    std::cout << "  Original size: " << total_original << " bytes" << std::endl;
    std::cout << "  Compressed size: " << total_compressed << " bytes" << std::endl;
    std::cout << "  Compression ratio: " << compression_ratio << std::endl;
    std::cout << "  Space savings: " << calculateSpaceSavings(total_original, total_compressed) << "%" << std::endl;

    container.close();
}

TEST_F(CompactContainerTest, RandomDataCompression) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write random data (poor compression)
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 0, random_data_.data(), random_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    ));

    uint64_t total_original, total_compressed;
    double compression_ratio;
    container.getCompressionStats(total_original, total_compressed, compression_ratio);

    // Random data won't compress much
    EXPECT_EQ(random_data_.size(), total_original);
    EXPECT_GT(compression_ratio, 0.8);  // Should be close to 1.0 (minimal compression)

    std::cout << "  Random data compression ratio: " << compression_ratio << std::endl;

    container.close();
}

// ============================================================================
// Block Index Tests
// ============================================================================

TEST_F(CompactContainerTest, GetBlockIndex) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write block
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 5, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_SWINGING_DOOR, ValueType::VT_F64, TimeUnit::TU_US
    ));

    // Get block index
    const CompactBlockIndex* index = container.getBlockIndex(0);
    ASSERT_NE(nullptr, index);
    EXPECT_EQ(1000u, index->tag_id);
    EXPECT_EQ(5u, index->original_block_index);
    EXPECT_EQ(test_data_.size(), index->original_size);
    EXPECT_GT(index->compressed_size, 0u);
    EXPECT_EQ(1000000, index->start_ts_us);
    EXPECT_EQ(2000000, index->end_ts_us);
    EXPECT_EQ(1000u, index->record_count);
    EXPECT_EQ(static_cast<uint8_t>(EncodingType::ENC_SWINGING_DOOR), index->original_encoding);
    EXPECT_EQ(static_cast<uint8_t>(ValueType::VT_F64), index->value_type);
    EXPECT_EQ(static_cast<uint8_t>(TimeUnit::TU_US), index->time_unit);

    container.close();
}

TEST_F(CompactContainerTest, GetBlockIndexOutOfRange) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // No blocks written
    const CompactBlockIndex* index = container.getBlockIndex(0);
    EXPECT_EQ(nullptr, index);

    container.close();
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(CompactContainerTest, WriteToSealedContainer) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write and seal
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 0, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    ));
    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());

    // Try to write after seal
    ContainerResult result = container.writeBlock(
        1001, 1, test_data_.data(), test_data_.size(),
        2000000, 3000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    );
    EXPECT_NE(ContainerResult::SUCCESS, result);

    container.close();
}

TEST_F(CompactContainerTest, ReadInvalidBlock) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write one block
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 0, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    ));
    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());

    // Try to read non-existent block
    std::vector<uint8_t> read_buffer(test_data_.size());
    uint32_t actual_size = 0;
    ContainerResult result = container.readBlock(999, read_buffer.data(), read_buffer.size(), actual_size);
    EXPECT_NE(ContainerResult::SUCCESS, result);

    container.close();
}

TEST_F(CompactContainerTest, ReadWithSmallBuffer) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write block
    ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
        1000, 0, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    ));
    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());

    // Try to read with buffer too small
    std::vector<uint8_t> small_buffer(100);
    uint32_t actual_size = 0;
    ContainerResult result = container.readBlock(0, small_buffer.data(), small_buffer.size(), actual_size);
    EXPECT_NE(ContainerResult::SUCCESS, result);

    container.close();
}

TEST_F(CompactContainerTest, OperationsOnClosedContainer) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);

    // Try operations without opening
    ContainerResult result = container.writeBlock(
        1000, 0, test_data_.data(), test_data_.size(),
        1000000, 2000000, 1000,
        EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
    );
    EXPECT_EQ(ContainerResult::ERR_NOT_OPEN, result);

    std::vector<uint8_t> read_buffer(test_data_.size());
    uint32_t actual_size = 0;
    result = container.readBlock(0, read_buffer.data(), read_buffer.size(), actual_size);
    EXPECT_EQ(ContainerResult::ERR_NOT_OPEN, result);
}

// ============================================================================
// Metadata Tests
// ============================================================================

TEST_F(CompactContainerTest, ContainerMetadata) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    const ContainerMetadata& metadata = container.getMetadata();
    EXPECT_EQ(ContainerLayout::LAYOUT_COMPACT_VAR, metadata.layout);
    EXPECT_EQ(CapacityType::CAP_DYNAMIC, metadata.capacity_type);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, metadata.archive_level);  // COMPACT uses ARCHIVE_RAW
    EXPECT_GT(metadata.capacity_bytes, 0u);
    EXPECT_GT(metadata.created_ts_us, 0);

    container.close();
}

TEST_F(CompactContainerTest, IOStatistics) {
    CompactContainer container(test_path_, layout_, CompressionType::COMP_ZSTD);
    ASSERT_EQ(ContainerResult::SUCCESS, container.open(true));

    // Write blocks
    for (int i = 0; i < 3; i++) {
        ASSERT_EQ(ContainerResult::SUCCESS, container.writeBlock(
            1000 + i, i, test_data_.data(), test_data_.size(),
            1000000 + i * 1000, 2000000 + i * 1000, 1000,
            EncodingType::ENC_RAW, ValueType::VT_F64, TimeUnit::TU_US
        ));
    }

    ASSERT_EQ(ContainerResult::SUCCESS, container.seal());

    // Check stats
    const ContainerStats& stats = container.getStats();
    EXPECT_GT(stats.bytes_written, 0u);
    EXPECT_EQ(3u, stats.write_operations);
    EXPECT_GE(stats.sync_operations, 1u);  // At least one from seal()

    // Read blocks
    for (int i = 0; i < 3; i++) {
        std::vector<uint8_t> read_buffer(test_data_.size());
        uint32_t actual_size = 0;
        ASSERT_EQ(ContainerResult::SUCCESS, container.readBlock(i, read_buffer.data(), read_buffer.size(), actual_size));
    }

    const ContainerStats& stats2 = container.getStats();
    EXPECT_GT(stats2.bytes_read, 0u);
    EXPECT_EQ(3u, stats2.read_operations);

    container.close();
}
