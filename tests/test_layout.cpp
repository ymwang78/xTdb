#include "xTdb/layout_calculator.h"
#include <gtest/gtest.h>

using namespace xtdb;

// ============================================================================
// T2-OffsetMath: Unit test LayoutCalculator
// ============================================================================

class LayoutTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// Test 1: RAW16K layout calculation (256MB chunk)
TEST_F(LayoutTest, RAW16K_256MB) {
    // Calculate layout for RAW16K with 256MB chunk
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_16K,
        kDefaultChunkSizeExtents);  // 256MB

    // Verify basic parameters
    EXPECT_EQ(kDefaultChunkSizeExtents, layout.chunk_size_extents);  // 16384 extents
    EXPECT_EQ(1, layout.block_size_extents);  // 1 extent = 16KB
    EXPECT_EQ(256 * 1024 * 1024, layout.chunk_size_bytes);  // 256MB
    EXPECT_EQ(16384, layout.block_size_bytes);  // 16KB

    // Calculate blocks_per_chunk: 256MB / 16KB = 16384 blocks
    EXPECT_EQ(16384, layout.blocks_per_chunk);

    // meta_blocks calculation (Updated for 64-byte BlockDirEntryV16):
    // Initial estimate: data_blocks = 16384
    // meta_bytes = 128 (header) + 16384 * 64 (entries) = 1,048,704 bytes
    // meta_blocks = ceil(1,048,704 / 16,384) = 64 blocks
    // data_blocks = 16384 - 64 = 16320
    // Verify convergence:
    // meta_bytes = 128 + 16320 * 64 = 1,044,608 bytes
    // meta_blocks = ceil(1,044,608 / 16,384) = 64 blocks (converged)

    EXPECT_EQ(64, layout.meta_blocks) << "Meta blocks for RAW16K 256MB chunk";
    EXPECT_EQ(16320, layout.data_blocks) << "Data blocks for RAW16K 256MB chunk";

    // Verify sum
    EXPECT_EQ(layout.blocks_per_chunk, layout.meta_blocks + layout.data_blocks);

    // Validate layout
    EXPECT_TRUE(LayoutCalculator::validateLayout(layout));
}

// Test 2: RAW64K layout calculation (256MB chunk)
TEST_F(LayoutTest, RAW64K_256MB) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_64K,
        kDefaultChunkSizeExtents);

    // Verify basic parameters
    EXPECT_EQ(kDefaultChunkSizeExtents, layout.chunk_size_extents);
    EXPECT_EQ(4, layout.block_size_extents);  // 4 extents = 64KB
    EXPECT_EQ(256 * 1024 * 1024, layout.chunk_size_bytes);
    EXPECT_EQ(65536, layout.block_size_bytes);  // 64KB

    // blocks_per_chunk: 256MB / 64KB = 4096 blocks
    EXPECT_EQ(4096, layout.blocks_per_chunk);

    // meta_blocks calculation (Updated for 64-byte BlockDirEntryV16):
    // data_blocks estimate = 4096
    // meta_bytes = 128 + 4096 * 64 = 262,272 bytes
    // meta_blocks = ceil(262,272 / 65,536) = 4 blocks
    // data_blocks = 4096 - 4 = 4092

    EXPECT_EQ(4, layout.meta_blocks) << "Meta blocks for RAW64K 256MB chunk";
    EXPECT_EQ(4092, layout.data_blocks) << "Data blocks for RAW64K 256MB chunk";

    // Verify sum
    EXPECT_EQ(layout.blocks_per_chunk, layout.meta_blocks + layout.data_blocks);

    // Validate layout
    EXPECT_TRUE(LayoutCalculator::validateLayout(layout));
}

// Test 3: RAW256K layout calculation (256MB chunk)
TEST_F(LayoutTest, RAW256K_256MB) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_256K,
        kDefaultChunkSizeExtents);

    // Verify basic parameters
    EXPECT_EQ(kDefaultChunkSizeExtents, layout.chunk_size_extents);
    EXPECT_EQ(16, layout.block_size_extents);  // 16 extents = 256KB
    EXPECT_EQ(256 * 1024 * 1024, layout.chunk_size_bytes);
    EXPECT_EQ(262144, layout.block_size_bytes);  // 256KB

    // blocks_per_chunk: 256MB / 256KB = 1024 blocks
    EXPECT_EQ(1024, layout.blocks_per_chunk);

    // meta_blocks calculation:
    // data_blocks estimate = 1024
    // meta_bytes = 128 + 1024 * 48 = 49,280 bytes
    // meta_blocks = ceil(49,280 / 262,144) = 1 block
    // data_blocks = 1024 - 1 = 1023

    EXPECT_EQ(1, layout.meta_blocks) << "Meta blocks for RAW256K 256MB chunk";
    EXPECT_EQ(1023, layout.data_blocks) << "Data blocks for RAW256K 256MB chunk";

    // Verify sum
    EXPECT_EQ(layout.blocks_per_chunk, layout.meta_blocks + layout.data_blocks);

    // Validate layout
    EXPECT_TRUE(LayoutCalculator::validateLayout(layout));
}

// Test 4: Chunk offset calculation
TEST_F(LayoutTest, ChunkOffsetCalculation) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_16K,
        kDefaultChunkSizeExtents);

    uint64_t container_base = kExtentSizeBytes;  // 16KB (after container header)

    // Chunk 0 offset
    uint64_t offset0 = LayoutCalculator::calculateChunkOffset(0, layout, container_base);
    EXPECT_EQ(container_base, offset0);

    // Chunk 1 offset: base + 1 * 256MB
    uint64_t offset1 = LayoutCalculator::calculateChunkOffset(1, layout, container_base);
    EXPECT_EQ(container_base + 256 * 1024 * 1024, offset1);

    // Chunk 10 offset: base + 10 * 256MB
    uint64_t offset10 = LayoutCalculator::calculateChunkOffset(10, layout, container_base);
    EXPECT_EQ(container_base + 10 * 256ULL * 1024 * 1024, offset10);

    // Verify all offsets are extent-aligned
    EXPECT_TRUE(isExtentAligned(offset0));
    EXPECT_TRUE(isExtentAligned(offset1));
    EXPECT_TRUE(isExtentAligned(offset10));
}

// Test 5: Block offset calculation (meta blocks)
TEST_F(LayoutTest, MetaBlockOffsetCalculation) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_64K,
        kDefaultChunkSizeExtents);

    uint64_t container_base = kExtentSizeBytes;
    uint32_t chunk_id = 5;

    // Meta block 0 (chunk header block)
    uint64_t meta_block0 = LayoutCalculator::calculateBlockOffset(
        chunk_id, 0, layout, container_base);

    // Expected: container_base + chunk_id * chunk_size + 0 * block_size
    uint64_t chunk_base = container_base + chunk_id * layout.chunk_size_bytes;
    EXPECT_EQ(chunk_base, meta_block0);

    // Meta block 1
    uint64_t meta_block1 = LayoutCalculator::calculateBlockOffset(
        chunk_id, 1, layout, container_base);
    EXPECT_EQ(chunk_base + layout.block_size_bytes, meta_block1);

    // Verify alignment
    EXPECT_TRUE(isExtentAligned(meta_block0));
    EXPECT_TRUE(isExtentAligned(meta_block1));
}

// Test 6: Block offset calculation (data blocks)
TEST_F(LayoutTest, DataBlockOffsetCalculation) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_256K,
        kDefaultChunkSizeExtents);

    uint64_t container_base = kExtentSizeBytes;
    uint32_t chunk_id = 3;

    // First data block: block_index = meta_blocks
    uint32_t first_data_index = layout.meta_blocks;
    uint64_t first_data_offset = LayoutCalculator::calculateBlockOffset(
        chunk_id, first_data_index, layout, container_base);

    // Expected: chunk_base + meta_blocks * block_size
    uint64_t chunk_base = container_base + chunk_id * layout.chunk_size_bytes;
    uint64_t expected_data_offset = chunk_base + layout.meta_blocks * layout.block_size_bytes;
    EXPECT_EQ(expected_data_offset, first_data_offset);

    // Last data block: block_index = blocks_per_chunk - 1
    uint32_t last_block_index = layout.blocks_per_chunk - 1;
    uint64_t last_block_offset = LayoutCalculator::calculateBlockOffset(
        chunk_id, last_block_index, layout, container_base);

    // Verify it's still within chunk bounds
    EXPECT_LT(last_block_offset, chunk_base + layout.chunk_size_bytes);

    // Verify alignment
    EXPECT_TRUE(isExtentAligned(first_data_offset));
    EXPECT_TRUE(isExtentAligned(last_block_offset));
}

// Test 7: BlockDir offset within chunk
TEST_F(LayoutTest, BlockDirOffset) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_16K,
        kDefaultChunkSizeExtents);

    // BlockDir starts immediately after ChunkHeader (128 bytes)
    uint64_t dir_offset = LayoutCalculator::calculateBlockDirOffset(layout);
    EXPECT_EQ(kChunkHeaderSize, dir_offset);
}

// Test 8: Data region offset within chunk
TEST_F(LayoutTest, DataRegionOffset) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_64K,
        kDefaultChunkSizeExtents);

    // Data region starts after all meta blocks
    uint64_t data_offset = LayoutCalculator::calculateDataRegionOffset(layout);
    EXPECT_EQ(layout.meta_blocks * layout.block_size_bytes, data_offset);

    // Verify alignment
    EXPECT_TRUE(isExtentAligned(data_offset));
}

// Test 9: Edge case - invalid block class
TEST_F(LayoutTest, InvalidBlockClass) {
    EXPECT_THROW({
        LayoutCalculator::calculateLayout(
            static_cast<RawBlockClass>(99),
            kDefaultChunkSizeExtents);
    }, std::invalid_argument);
}

// Test 10: Edge case - chunk smaller than block
TEST_F(LayoutTest, ChunkSmallerThanBlock) {
    // Try to create 16KB chunk with 64KB blocks (invalid)
    EXPECT_THROW({
        LayoutCalculator::calculateLayout(
            RawBlockClass::RAW_64K,
            1);  // 1 extent = 16KB chunk
    }, std::invalid_argument);
}

// Test 11: Edge case - block index out of range
TEST_F(LayoutTest, BlockIndexOutOfRange) {
    ChunkLayout layout = LayoutCalculator::calculateLayout(
        RawBlockClass::RAW_16K,
        kDefaultChunkSizeExtents);

    uint64_t container_base = kExtentSizeBytes;

    // Try to access block beyond blocks_per_chunk
    EXPECT_THROW({
        LayoutCalculator::calculateBlockOffset(
            0,
            layout.blocks_per_chunk,  // Out of range
            layout,
            container_base);
    }, std::out_of_range);
}

// Test 12: Extent alignment helpers
TEST_F(LayoutTest, ExtentAlignmentHelpers) {
    // Test isExtentAligned
    EXPECT_TRUE(isExtentAligned(0));
    EXPECT_TRUE(isExtentAligned(kExtentSizeBytes));
    EXPECT_TRUE(isExtentAligned(kExtentSizeBytes * 2));
    EXPECT_FALSE(isExtentAligned(1));
    EXPECT_FALSE(isExtentAligned(kExtentSizeBytes - 1));
    EXPECT_FALSE(isExtentAligned(kExtentSizeBytes + 1));

    // Test extentToBytes
    EXPECT_EQ(0, extentToBytes(0));
    EXPECT_EQ(kExtentSizeBytes, extentToBytes(1));
    EXPECT_EQ(kExtentSizeBytes * 10, extentToBytes(10));

    // Test bytesToExtent
    EXPECT_EQ(0, bytesToExtent(0));
    EXPECT_EQ(1, bytesToExtent(kExtentSizeBytes));
    EXPECT_EQ(10, bytesToExtent(kExtentSizeBytes * 10));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
