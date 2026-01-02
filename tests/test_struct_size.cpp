#include "xTdb/struct_defs.h"
#include <gtest/gtest.h>
#include <cstring>

using namespace xtdb;

// ============================================================================
// T3-StructSize: Verify struct sizes match design specification
// ============================================================================

class StructSizeTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

// Test 1: ContainerHeaderV12 must be exactly 16KB
TEST_F(StructSizeTest, ContainerHeaderSize) {
    EXPECT_EQ(kExtentSizeBytes, sizeof(ContainerHeaderV12))
        << "ContainerHeaderV12 must be exactly 16KB (one extent)";
}

// Test 2: RawChunkHeaderV16 must be exactly 128 bytes
TEST_F(StructSizeTest, RawChunkHeaderSize) {
    EXPECT_EQ(128u, sizeof(RawChunkHeaderV16))
        << "RawChunkHeaderV16 must be exactly 128 bytes";
}

// Test 3: BlockDirEntryV16 must be exactly 48 bytes
TEST_F(StructSizeTest, BlockDirEntrySize) {
    EXPECT_EQ(48u, sizeof(BlockDirEntryV16))
        << "BlockDirEntryV16 must be exactly 48 bytes";
}

// Test 4: Verify ContainerHeaderV12 field offsets
TEST_F(StructSizeTest, ContainerHeaderLayout) {
    ContainerHeaderV12 header;

    // Check magic at offset 0
    EXPECT_EQ(0, offsetof(ContainerHeaderV12, magic));
    EXPECT_EQ(8, sizeof(header.magic));

    // Check version at offset 8
    EXPECT_EQ(8, offsetof(ContainerHeaderV12, version));
    EXPECT_EQ(2, sizeof(header.version));

    // Check header_size at offset 10
    EXPECT_EQ(10, offsetof(ContainerHeaderV12, header_size));
    EXPECT_EQ(2, sizeof(header.header_size));
}

// Test 5: Verify RawChunkHeaderV16 field offsets
TEST_F(StructSizeTest, RawChunkHeaderLayout) {
    RawChunkHeaderV16 header;

    // Check magic at offset 0
    EXPECT_EQ(0, offsetof(RawChunkHeaderV16, magic));
    EXPECT_EQ(8, sizeof(header.magic));

    // Check version at offset 8
    EXPECT_EQ(8, offsetof(RawChunkHeaderV16, version));

    // Check chunk_id
    EXPECT_EQ(28, offsetof(RawChunkHeaderV16, chunk_id));

    // Check flags (critical for state machine)
    EXPECT_EQ(32, offsetof(RawChunkHeaderV16, flags));
    EXPECT_EQ(4, sizeof(header.flags));
}

// Test 6: Verify BlockDirEntryV16 field offsets
TEST_F(StructSizeTest, BlockDirEntryLayout) {
    BlockDirEntryV16 entry;

    // Check tag_id at offset 0
    EXPECT_EQ(0, offsetof(BlockDirEntryV16, tag_id));
    EXPECT_EQ(4, sizeof(entry.tag_id));

    // Check flags at offset 8
    EXPECT_EQ(8, offsetof(BlockDirEntryV16, flags));
    EXPECT_EQ(4, sizeof(entry.flags));

    // Check timestamps
    EXPECT_EQ(12, offsetof(BlockDirEntryV16, start_ts_us));
    EXPECT_EQ(20, offsetof(BlockDirEntryV16, end_ts_us));
}

// Test 7: ContainerHeaderV12 initialization
TEST_F(StructSizeTest, ContainerHeaderInit) {
    ContainerHeaderV12 header;

    // Check magic
    EXPECT_EQ(0, std::memcmp(header.magic, kContainerMagic, 8))
        << "Magic should be 'XTSDBCON'";

    // Check version
    EXPECT_EQ(0x0102, header.version);

    // Check header_size
    EXPECT_EQ(kExtentSizeBytes, header.header_size);
}

// Test 8: RawChunkHeaderV16 initialization
TEST_F(StructSizeTest, RawChunkHeaderInit) {
    RawChunkHeaderV16 header;

    // Check magic
    EXPECT_EQ(0, std::memcmp(header.magic, kRawChunkMagic, 8))
        << "Magic should be 'XTSRAWCK'";

    // Check version
    EXPECT_EQ(0x0106, header.version);

    // Check flags initialization (all bits = 1)
    EXPECT_EQ(kChunkFlagsInit, header.flags)
        << "Flags should be initialized to 0xFFFFFFFF";

    // Check timestamp initialization
    EXPECT_EQ(0x7FFFFFFFFFFFFFFFll, header.start_ts_us)
        << "start_ts_us should be initialized to max int64";
    EXPECT_EQ(0x7FFFFFFFFFFFFFFFll, header.end_ts_us)
        << "end_ts_us should be initialized to max int64";

    // Check super_crc32 initialization
    EXPECT_EQ(0xFFFFFFFFu, header.super_crc32)
        << "super_crc32 should be initialized to 0xFFFFFFFF";
}

// Test 9: BlockDirEntryV16 initialization
TEST_F(StructSizeTest, BlockDirEntryInit) {
    BlockDirEntryV16 entry;

    // Check flags initialization (all bits = 1)
    EXPECT_EQ(kBlockFlagsInit, entry.flags)
        << "Flags should be initialized to 0xFFFFFFFF";

    // Check timestamp initialization
    EXPECT_EQ(0x7FFFFFFFFFFFFFFFll, entry.start_ts_us);
    EXPECT_EQ(0x7FFFFFFFFFFFFFFFll, entry.end_ts_us);

    // Check record_count initialization
    EXPECT_EQ(0xFFFFFFFFu, entry.record_count)
        << "record_count should be initialized to 0xFFFFFFFF";

    // Check data_crc32 initialization
    EXPECT_EQ(0xFFFFFFFFu, entry.data_crc32)
        << "data_crc32 should be initialized to 0xFFFFFFFF";
}

// Test 10: Memory alignment verification
TEST_F(StructSizeTest, MemoryAlignment) {
    // ContainerHeaderV12 should be 16KB-aligned
    // Note: Stack alignment may not be 16KB, but this tests struct size

    // RawChunkHeaderV16 size should be compatible with extent writes
    EXPECT_EQ(0, sizeof(RawChunkHeaderV16) % 16)
        << "RawChunkHeaderV16 size should be multiple of 16 bytes";

    // BlockDirEntryV16 size should be compatible with extent writes
    EXPECT_EQ(0, sizeof(BlockDirEntryV16) % 16)
        << "BlockDirEntryV16 size should be multiple of 16 bytes";
}

// Test 11: Chunk state bit helpers
TEST_F(StructSizeTest, ChunkStateBitHelpers) {
    // Test initial state (all bits = 1)
    uint32_t flags = kChunkFlagsInit;

    EXPECT_TRUE(chunkIsBitSet(flags, ChunkStateBit::CHB_ALLOCATED));
    EXPECT_TRUE(chunkIsBitSet(flags, ChunkStateBit::CHB_SEALED));
    EXPECT_TRUE(chunkIsBitSet(flags, ChunkStateBit::CHB_DEPRECATED));
    EXPECT_TRUE(chunkIsBitSet(flags, ChunkStateBit::CHB_FREE_MARK));

    // Initial state predicates
    EXPECT_FALSE(chunkIsAllocated(flags)) << "Initial state: not allocated";
    EXPECT_FALSE(chunkIsSealed(flags)) << "Initial state: not sealed";
    EXPECT_FALSE(chunkIsDeprecated(flags)) << "Initial state: not deprecated";
    EXPECT_TRUE(chunkIsFree(flags)) << "Initial state: FREE";

    // Test bit clearing
    flags = chunkClearBit(flags, ChunkStateBit::CHB_ALLOCATED);
    EXPECT_FALSE(chunkIsBitSet(flags, ChunkStateBit::CHB_ALLOCATED));
    EXPECT_TRUE(chunkIsAllocated(flags)) << "After clearing ALLOCATED: allocated";
    EXPECT_FALSE(chunkIsFree(flags)) << "After clearing ALLOCATED: not FREE";
}

// Test 12: Block state bit helpers
TEST_F(StructSizeTest, BlockStateBitHelpers) {
    // Test initial state (all bits = 1)
    uint32_t flags = kBlockFlagsInit;

    EXPECT_TRUE(blockIsBitSet(flags, BlockStateBit::BLB_SEALED));
    EXPECT_TRUE(blockIsBitSet(flags, BlockStateBit::BLB_MONOTONIC_TIME));
    EXPECT_TRUE(blockIsBitSet(flags, BlockStateBit::BLB_NO_TIME_GAP));

    // Initial state predicates
    EXPECT_FALSE(blockIsSealed(flags)) << "Initial state: not sealed";

    // Test bit clearing
    flags = blockClearBit(flags, BlockStateBit::BLB_SEALED);
    EXPECT_FALSE(blockIsBitSet(flags, BlockStateBit::BLB_SEALED));
    EXPECT_TRUE(blockIsSealed(flags)) << "After clearing SEALED: sealed";
}

// Test 13: Enum class sizes
TEST_F(StructSizeTest, EnumSizes) {
    EXPECT_EQ(1, sizeof(ContainerLayout));
    EXPECT_EQ(1, sizeof(CapacityType));
    EXPECT_EQ(1, sizeof(RawBlockClass));
    EXPECT_EQ(1, sizeof(ValueType));
    EXPECT_EQ(1, sizeof(TimeUnit));
    EXPECT_EQ(1, sizeof(ChunkStateBit));
    EXPECT_EQ(1, sizeof(BlockStateBit));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
