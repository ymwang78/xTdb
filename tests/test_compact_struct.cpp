#include <gtest/gtest.h>
#include "xTdb/struct_defs.h"
#include "xTdb/compact_layout.h"
#include <cstring>

using namespace xtdb;

// ============================================================================
// Structure Size Tests
// ============================================================================

TEST(CompactStructTest, CompactChunkHeaderSize) {
    EXPECT_EQ(128u, sizeof(CompactChunkHeader));
    std::cout << "  CompactChunkHeader size: " << sizeof(CompactChunkHeader) << " bytes" << std::endl;
}

TEST(CompactStructTest, CompactBlockIndexSize) {
    EXPECT_EQ(80u, sizeof(CompactBlockIndex));
    std::cout << "  CompactBlockIndex size: " << sizeof(CompactBlockIndex) << " bytes" << std::endl;
}

// ============================================================================
// Structure Initialization Tests
// ============================================================================

TEST(CompactStructTest, CompactChunkHeaderInitialization) {
    CompactChunkHeader header;

    // Verify magic number
    EXPECT_EQ(0, std::memcmp(header.magic, kCompactChunkMagic, 8));

    // Verify version and size
    EXPECT_EQ(kCompactChunkVersion, header.version);
    EXPECT_EQ(sizeof(CompactChunkHeader), header.header_size);

    // Verify defaults
    EXPECT_EQ(0u, header.chunk_id);
    EXPECT_EQ(kChunkFlagsInit, header.flags);
    EXPECT_EQ(0u, header.block_count);
    EXPECT_EQ(static_cast<uint8_t>(CompressionType::COMP_ZSTD), header.compression_type);
    EXPECT_EQ(0u, header.index_offset);
    EXPECT_EQ(0u, header.data_offset);
    EXPECT_EQ(0u, header.total_compressed_size);
    EXPECT_EQ(0u, header.total_original_size);
    EXPECT_EQ(0, header.start_ts_us);
    EXPECT_EQ(0, header.end_ts_us);
}

TEST(CompactStructTest, CompactBlockIndexInitialization) {
    CompactBlockIndex index;

    // Verify defaults
    EXPECT_EQ(0u, index.tag_id);
    EXPECT_EQ(0u, index.original_block_index);
    EXPECT_EQ(0u, index.data_offset);
    EXPECT_EQ(0u, index.compressed_size);
    EXPECT_EQ(0u, index.original_size);
    EXPECT_EQ(0, index.start_ts_us);
    EXPECT_EQ(0, index.end_ts_us);
    EXPECT_EQ(0u, index.record_count);
    EXPECT_EQ(static_cast<uint8_t>(EncodingType::ENC_RAW), index.original_encoding);
    EXPECT_EQ(static_cast<uint8_t>(ValueType::VT_F64), index.value_type);
    EXPECT_EQ(static_cast<uint8_t>(TimeUnit::TU_US), index.time_unit);
}

// ============================================================================
// Helper Function Tests
// ============================================================================

TEST(CompactStructTest, CalculateCompactChunkSize) {
    uint32_t block_count = 10;
    uint64_t data_size = 16384;  // 16KB

    uint64_t total_size = calculateCompactChunkSize(block_count, data_size);

    uint64_t expected_size = sizeof(CompactChunkHeader) +
                            block_count * sizeof(CompactBlockIndex) +
                            data_size;

    EXPECT_EQ(expected_size, total_size);

    std::cout << "  Chunk with 10 blocks + 16KB data: " << total_size << " bytes" << std::endl;
}

TEST(CompactStructTest, GetCompactIndexOffset) {
    uint64_t offset = getCompactIndexOffset();
    EXPECT_EQ(sizeof(CompactChunkHeader), offset);
    EXPECT_EQ(128u, offset);
}

TEST(CompactStructTest, GetCompactDataOffset) {
    uint32_t block_count = 10;
    uint64_t offset = getCompactDataOffset(block_count);

    uint64_t expected = sizeof(CompactChunkHeader) +
                       block_count * sizeof(CompactBlockIndex);

    EXPECT_EQ(expected, offset);
    EXPECT_EQ(128u + 10u * 80u, offset);  // 128 + 800 = 928
}

TEST(CompactStructTest, ValidateCompactChunkHeader) {
    CompactChunkHeader header;

    // Valid header
    EXPECT_TRUE(validateCompactChunkHeader(header));

    // Invalid magic
    CompactChunkHeader bad_magic;
    bad_magic.magic[0] = 'A';
    EXPECT_FALSE(validateCompactChunkHeader(bad_magic));

    // Invalid version
    CompactChunkHeader bad_version;
    bad_version.version = 0xFFFF;
    EXPECT_FALSE(validateCompactChunkHeader(bad_version));

    // Invalid size
    CompactChunkHeader bad_size;
    bad_size.header_size = 256;
    EXPECT_FALSE(validateCompactChunkHeader(bad_size));
}

TEST(CompactStructTest, CalculateCompressionRatio) {
    EXPECT_DOUBLE_EQ(0.5, calculateCompressionRatio(1000, 500));
    EXPECT_DOUBLE_EQ(0.1, calculateCompressionRatio(1000, 100));
    EXPECT_DOUBLE_EQ(1.0, calculateCompressionRatio(1000, 1000));
    EXPECT_DOUBLE_EQ(0.0, calculateCompressionRatio(0, 500));
}

TEST(CompactStructTest, CalculateSpaceSavings) {
    EXPECT_DOUBLE_EQ(50.0, calculateSpaceSavings(1000, 500));
    EXPECT_DOUBLE_EQ(90.0, calculateSpaceSavings(1000, 100));
    EXPECT_DOUBLE_EQ(0.0, calculateSpaceSavings(1000, 1000));
    EXPECT_DOUBLE_EQ(0.0, calculateSpaceSavings(0, 500));
}

// ============================================================================
// Layout Calculator Tests
// ============================================================================

TEST(CompactLayoutTest, CalculateIndexSize) {
    EXPECT_EQ(80u, CompactLayoutCalculator::calculateIndexSize(1));
    EXPECT_EQ(800u, CompactLayoutCalculator::calculateIndexSize(10));
    EXPECT_EQ(8000u, CompactLayoutCalculator::calculateIndexSize(100));
}

TEST(CompactLayoutTest, EstimateCompressedSize) {
    // Test with default 30% compression ratio
    uint32_t size1 = CompactLayoutCalculator::estimateCompressedSize(1000, CompressionType::COMP_ZSTD);
    EXPECT_GT(size1, 0u);
    EXPECT_LE(size1, 1000u);  // Should not exceed original

    // Very small blocks
    uint32_t size2 = CompactLayoutCalculator::estimateCompressedSize(100, CompressionType::COMP_ZSTD);
    EXPECT_GT(size2, 0u);

    // Large blocks
    uint32_t size3 = CompactLayoutCalculator::estimateCompressedSize(16384, CompressionType::COMP_ZSTD);
    EXPECT_GT(size3, 0u);
    EXPECT_LE(size3, 16384u);

    std::cout << "  1KB block estimated: " << size1 << " bytes" << std::endl;
    std::cout << "  100B block estimated: " << size2 << " bytes" << std::endl;
    std::cout << "  16KB block estimated: " << size3 << " bytes" << std::endl;
}

TEST(CompactLayoutTest, CalculateTotalDataSize) {
    std::vector<CompactBlockInfo> blocks;
    blocks.emplace_back(1000, 0, 16384, 5000);
    blocks.emplace_back(1000, 1, 16384, 4800);
    blocks.emplace_back(1001, 0, 8192, 2500);

    uint64_t total = CompactLayoutCalculator::calculateTotalDataSize(blocks);
    EXPECT_EQ(5000u + 4800u + 2500u, total);
    EXPECT_EQ(12300u, total);
}

TEST(CompactLayoutTest, CalculateLayout) {
    std::vector<CompactBlockInfo> blocks;
    for (uint32_t i = 0; i < 10; i++) {
        blocks.emplace_back(1000 + i, i, 16384, 5000);
    }

    CompactChunkLayout layout;
    bool result = CompactLayoutCalculator::calculateLayout(blocks, layout);

    ASSERT_TRUE(result);
    EXPECT_EQ(10u, layout.block_count);
    EXPECT_EQ(128u, layout.header_size);
    EXPECT_EQ(800u, layout.index_size);  // 10 * 80
    EXPECT_EQ(50000u, layout.data_size);  // 10 * 5000
    EXPECT_EQ(50928u, layout.total_size);  // 128 + 800 + 50000

    EXPECT_EQ(128u, layout.index_offset);
    EXPECT_EQ(928u, layout.data_offset);  // 128 + 800

    std::cout << "  Layout for 10 blocks:" << std::endl;
    std::cout << "    Header: " << layout.header_size << " bytes" << std::endl;
    std::cout << "    Index: " << layout.index_size << " bytes" << std::endl;
    std::cout << "    Data: " << layout.data_size << " bytes" << std::endl;
    std::cout << "    Total: " << layout.total_size << " bytes" << std::endl;
}

TEST(CompactLayoutTest, CalculateLayoutEmpty) {
    std::vector<CompactBlockInfo> blocks;
    CompactChunkLayout layout;

    bool result = CompactLayoutCalculator::calculateLayout(blocks, layout);
    EXPECT_FALSE(result);  // Should fail with empty blocks
}

TEST(CompactLayoutTest, CalculateLayoutTooManyBlocks) {
    std::vector<CompactBlockInfo> blocks;
    uint32_t max_blocks = CompactLayoutCalculator::getMaxBlocksPerChunk();

    // Create one more than max
    for (uint32_t i = 0; i <= max_blocks; i++) {
        blocks.emplace_back(1000, i, 16384, 5000);
    }

    CompactChunkLayout layout;
    bool result = CompactLayoutCalculator::calculateLayout(blocks, layout);
    EXPECT_FALSE(result);  // Should fail with too many blocks
}

TEST(CompactLayoutTest, ValidateLayout) {
    CompactChunkLayout layout;
    layout.block_count = 10;
    layout.header_size = 128;
    layout.index_size = 800;
    layout.data_size = 50000;
    layout.total_size = 50928;
    layout.index_offset = 128;
    layout.data_offset = 928;

    EXPECT_TRUE(CompactLayoutCalculator::validateLayout(layout));

    // Test invalid cases
    CompactChunkLayout invalid1 = layout;
    invalid1.block_count = 0;
    EXPECT_FALSE(CompactLayoutCalculator::validateLayout(invalid1));

    CompactChunkLayout invalid2 = layout;
    invalid2.total_size = 1000;  // Inconsistent with components
    EXPECT_FALSE(CompactLayoutCalculator::validateLayout(invalid2));

    CompactChunkLayout invalid3 = layout;
    invalid3.index_offset = 256;  // Wrong offset
    EXPECT_FALSE(CompactLayoutCalculator::validateLayout(invalid3));
}

TEST(CompactLayoutTest, MaxLimits) {
    EXPECT_EQ(256ULL * 1024 * 1024, CompactLayoutCalculator::getMaxChunkSize());
    EXPECT_EQ(10000u, CompactLayoutCalculator::getMaxBlocksPerChunk());

    std::cout << "  Max chunk size: " << (CompactLayoutCalculator::getMaxChunkSize() / (1024*1024))
              << " MB" << std::endl;
    std::cout << "  Max blocks per chunk: " << CompactLayoutCalculator::getMaxBlocksPerChunk() << std::endl;
}

// ============================================================================
// Memory Layout Tests
// ============================================================================

TEST(CompactStructTest, MemoryLayoutAlignment) {
    CompactChunkHeader header;
    CompactBlockIndex index;

    // Test that structures are properly aligned
    EXPECT_EQ(0, reinterpret_cast<uintptr_t>(&header) % alignof(CompactChunkHeader));
    EXPECT_EQ(0, reinterpret_cast<uintptr_t>(&index) % alignof(CompactBlockIndex));

    std::cout << "  CompactChunkHeader alignment: " << alignof(CompactChunkHeader) << " bytes" << std::endl;
    std::cout << "  CompactBlockIndex alignment: " << alignof(CompactBlockIndex) << " bytes" << std::endl;
}

TEST(CompactStructTest, FieldOffsets) {
    CompactChunkHeader header;

    // Verify critical field offsets
    EXPECT_EQ(0, offsetof(CompactChunkHeader, magic));
    EXPECT_EQ(8, offsetof(CompactChunkHeader, version));
    EXPECT_EQ(12, offsetof(CompactChunkHeader, db_instance_id));
    EXPECT_EQ(28, offsetof(CompactChunkHeader, chunk_id));
    EXPECT_EQ(32, offsetof(CompactChunkHeader, flags));

    std::cout << "  chunk_id offset: " << offsetof(CompactChunkHeader, chunk_id) << std::endl;
    std::cout << "  block_count offset: " << offsetof(CompactChunkHeader, block_count) << std::endl;
    std::cout << "  index_offset offset: " << offsetof(CompactChunkHeader, index_offset) << std::endl;
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST(CompactStructTest, RoundTripChunkHeader) {
    CompactChunkHeader original;
    original.chunk_id = 42;
    original.block_count = 100;
    original.compression_type = static_cast<uint8_t>(CompressionType::COMP_ZSTD);
    original.index_offset = 128;
    original.data_offset = 8128;
    original.total_compressed_size = 50000;
    original.total_original_size = 100000;
    original.start_ts_us = 1000000;
    original.end_ts_us = 2000000;

    // Serialize to bytes
    uint8_t buffer[sizeof(CompactChunkHeader)];
    std::memcpy(buffer, &original, sizeof(CompactChunkHeader));

    // Deserialize
    CompactChunkHeader copy;
    std::memcpy(&copy, buffer, sizeof(CompactChunkHeader));

    // Verify
    EXPECT_EQ(original.chunk_id, copy.chunk_id);
    EXPECT_EQ(original.block_count, copy.block_count);
    EXPECT_EQ(original.compression_type, copy.compression_type);
    EXPECT_EQ(original.index_offset, copy.index_offset);
    EXPECT_EQ(original.data_offset, copy.data_offset);
    EXPECT_EQ(original.total_compressed_size, copy.total_compressed_size);
    EXPECT_EQ(original.total_original_size, copy.total_original_size);
    EXPECT_EQ(original.start_ts_us, copy.start_ts_us);
    EXPECT_EQ(original.end_ts_us, copy.end_ts_us);
}

TEST(CompactStructTest, RoundTripBlockIndex) {
    CompactBlockIndex original;
    original.tag_id = 1000;
    original.original_block_index = 5;
    original.data_offset = 1024;
    original.compressed_size = 4800;
    original.original_size = 16384;
    original.start_ts_us = 1000000;
    original.end_ts_us = 1100000;
    original.record_count = 1000;
    original.original_encoding = static_cast<uint8_t>(EncodingType::ENC_SWINGING_DOOR);

    // Serialize
    uint8_t buffer[sizeof(CompactBlockIndex)];
    std::memcpy(buffer, &original, sizeof(CompactBlockIndex));

    // Deserialize
    CompactBlockIndex copy;
    std::memcpy(&copy, buffer, sizeof(CompactBlockIndex));

    // Verify
    EXPECT_EQ(original.tag_id, copy.tag_id);
    EXPECT_EQ(original.original_block_index, copy.original_block_index);
    EXPECT_EQ(original.data_offset, copy.data_offset);
    EXPECT_EQ(original.compressed_size, copy.compressed_size);
    EXPECT_EQ(original.original_size, copy.original_size);
    EXPECT_EQ(original.start_ts_us, copy.start_ts_us);
    EXPECT_EQ(original.end_ts_us, copy.end_ts_us);
    EXPECT_EQ(original.record_count, copy.record_count);
    EXPECT_EQ(original.original_encoding, copy.original_encoding);
}
