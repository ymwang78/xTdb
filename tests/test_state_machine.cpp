#include "xTdb/state_mutator.h"
#include <gtest/gtest.h>
#include <unistd.h>

using namespace xtdb;

// ============================================================================
// T4-BitFlip: Verify active-low state machine (1->0 transitions only)
// ============================================================================

class StateMachineTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/xtdb_state_test.dat";
        ::unlink(test_file_.c_str());

        // Open file
        io_ = std::make_unique<AlignedIO>();
        ASSERT_EQ(IOResult::SUCCESS, io_->open(test_file_, true, false));

        // Preallocate 1MB
        ASSERT_EQ(IOResult::SUCCESS, io_->preallocate(1024 * 1024));

        // Create mutator
        mutator_ = std::make_unique<StateMutator>(io_.get());
    }

    void TearDown() override {
        mutator_.reset();
        io_.reset();
        ::unlink(test_file_.c_str());
    }

    std::string test_file_;
    std::unique_ptr<AlignedIO> io_;
    std::unique_ptr<StateMutator> mutator_;
};

// Test 1: Initialize chunk header with all bits = 1
TEST_F(StateMachineTest, InitChunkHeader) {
    RawChunkHeaderV16 header;
    header.chunk_id = 42;
    header.chunk_size_extents = kDefaultChunkSizeExtents;
    header.block_size_extents = getBlockSizeExtents(RawBlockClass::RAW_16K);
    header.meta_blocks = 48;
    header.data_blocks = 16336;

    // Write at offset 0
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // Read back and verify
    RawChunkHeaderV16 read_header;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, read_header));

    EXPECT_EQ(0, std::memcmp(read_header.magic, kRawChunkMagic, 8));
    EXPECT_EQ(0x0106, read_header.version);
    EXPECT_EQ(42u, read_header.chunk_id);
    EXPECT_EQ(kChunkFlagsInit, read_header.flags)
        << "Initial flags should be 0xFFFFFFFF";
}

// Test 2: Allocate chunk (clear CHB_ALLOCATED bit)
TEST_F(StateMachineTest, AllocateChunk) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 1;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // Verify initial state: not allocated
    RawChunkHeaderV16 before;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, before));
    EXPECT_FALSE(chunkIsAllocated(before.flags)) << "Initial: not allocated";
    EXPECT_TRUE(chunkIsFree(before.flags)) << "Initial: FREE";

    // Allocate (clear bit 2)
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Read back and verify
    RawChunkHeaderV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, after));

    EXPECT_TRUE(chunkIsAllocated(after.flags)) << "After allocate: allocated";
    EXPECT_FALSE(chunkIsFree(after.flags)) << "After allocate: not FREE";

    // Verify bit transition: only bit 2 changed from 1->0
    uint32_t expected_flags = kChunkFlagsInit & ~(1u << 2);
    EXPECT_EQ(expected_flags, after.flags);

    // Verify no 0->1 transitions
    EXPECT_EQ(0u, after.flags & ~before.flags)
        << "No bits should transition from 0->1";
}

// Test 3: Seal chunk (clear CHB_SEALED bit)
TEST_F(StateMachineTest, SealChunk) {
    // Initialize and allocate chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 2;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Read state before seal
    RawChunkHeaderV16 before;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, before));
    EXPECT_FALSE(chunkIsSealed(before.flags)) << "Before seal: not sealed";

    // Seal chunk
    int64_t start_ts = 1000000;
    int64_t end_ts = 2000000;
    uint32_t crc = 0x12345678;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->sealChunk(0, start_ts, end_ts, crc));

    // Read back and verify
    RawChunkHeaderV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, after));

    EXPECT_TRUE(chunkIsSealed(after.flags)) << "After seal: sealed";
    EXPECT_EQ(start_ts, after.start_ts_us);
    EXPECT_EQ(end_ts, after.end_ts_us);
    EXPECT_EQ(crc, after.super_crc32);

    // Verify bit transition: only bit 1 changed from 1->0
    EXPECT_FALSE(chunkIsBitSet(after.flags, ChunkStateBit::CHB_SEALED));
    EXPECT_TRUE(chunkIsAllocated(after.flags)) << "Should still be allocated";

    // Verify no 0->1 transitions
    EXPECT_EQ(0u, after.flags & ~before.flags)
        << "No bits should transition from 0->1";
}

// Test 4: Deprecate chunk (clear CHB_DEPRECATED bit)
TEST_F(StateMachineTest, DeprecateChunk) {
    // Initialize, allocate, and seal chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 3;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->sealChunk(0, 1000000, 2000000, 0x12345678));

    // Read state before deprecate
    RawChunkHeaderV16 before;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, before));
    EXPECT_FALSE(chunkIsDeprecated(before.flags)) << "Before: not deprecated";

    // Deprecate chunk
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->deprecateChunk(0));

    // Read back and verify
    RawChunkHeaderV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, after));

    EXPECT_TRUE(chunkIsDeprecated(after.flags)) << "After deprecate: deprecated";
    EXPECT_TRUE(chunkIsAllocated(after.flags)) << "Should still be allocated";
    EXPECT_TRUE(chunkIsSealed(after.flags)) << "Should still be sealed";

    // Verify no 0->1 transitions
    EXPECT_EQ(0u, after.flags & ~before.flags)
        << "No bits should transition from 0->1";
}

// Test 5: Full chunk lifecycle
TEST_F(StateMachineTest, ChunkLifecycle) {
    RawChunkHeaderV16 header;
    header.chunk_id = 4;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // State 1: FREE (all bits = 1)
    RawChunkHeaderV16 state1;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, state1));
    EXPECT_EQ(kChunkFlagsInit, state1.flags);
    EXPECT_TRUE(chunkIsFree(state1.flags));

    // State 2: ALLOCATED (bit 2 cleared)
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));
    RawChunkHeaderV16 state2;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, state2));
    EXPECT_TRUE(chunkIsAllocated(state2.flags));
    EXPECT_FALSE(chunkIsFree(state2.flags));

    // State 3: SEALED (bit 1 cleared)
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->sealChunk(0, 1000000, 2000000, 0x12345678));
    RawChunkHeaderV16 state3;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, state3));
    EXPECT_TRUE(chunkIsSealed(state3.flags));

    // State 4: DEPRECATED (bit 0 cleared)
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->deprecateChunk(0));
    RawChunkHeaderV16 state4;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, state4));
    EXPECT_TRUE(chunkIsDeprecated(state4.flags));

    // State 5: FREE_MARK (bit 3 cleared)
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->markChunkFree(0));
    RawChunkHeaderV16 state5;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, state5));
    EXPECT_FALSE(chunkIsBitSet(state5.flags, ChunkStateBit::CHB_FREE_MARK));

    // Verify monotonic bit clearing (no 0->1 transitions throughout)
    EXPECT_EQ(0u, state2.flags & ~state1.flags);
    EXPECT_EQ(0u, state3.flags & ~state2.flags);
    EXPECT_EQ(0u, state4.flags & ~state3.flags);
    EXPECT_EQ(0u, state5.flags & ~state4.flags);
}

// Test 6: Initialize block directory entry
TEST_F(StateMachineTest, InitBlockDirEntry) {
    BlockDirEntryV16 entry;
    entry.tag_id = 100;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.time_unit = static_cast<uint8_t>(TimeUnit::TU_MS);
    entry.record_size = 16;
    entry.start_ts_us = 1000000;

    // Write at offset 0
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initBlockDirEntry(0, entry));

    // Read back and verify
    BlockDirEntryV16 read_entry;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readBlockDirEntry(0, read_entry));

    EXPECT_EQ(100u, read_entry.tag_id);
    EXPECT_EQ(kBlockFlagsInit, read_entry.flags)
        << "Initial flags should be 0xFFFFFFFF";
    EXPECT_EQ(0xFFFFFFFFu, read_entry.record_count)
        << "Initial record_count should be 0xFFFFFFFF";
}

// Test 7: Seal block (clear BLB_SEALED bit)
TEST_F(StateMachineTest, SealBlock) {
    // Initialize block entry
    BlockDirEntryV16 entry;
    entry.tag_id = 200;
    entry.start_ts_us = 1000000;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initBlockDirEntry(0, entry));

    // Read state before seal
    BlockDirEntryV16 before;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readBlockDirEntry(0, before));
    EXPECT_FALSE(blockIsSealed(before.flags)) << "Before seal: not sealed";

    // Seal block
    int64_t end_ts = 2000000;
    uint32_t record_count = 1000;
    uint32_t data_crc = 0xABCDEF12;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->sealBlock(0, end_ts, record_count, data_crc));

    // Read back and verify
    BlockDirEntryV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readBlockDirEntry(0, after));

    EXPECT_TRUE(blockIsSealed(after.flags)) << "After seal: sealed";
    EXPECT_EQ(end_ts, after.end_ts_us);
    EXPECT_EQ(record_count, after.record_count);
    EXPECT_EQ(data_crc, after.data_crc32);

    // Verify no 0->1 transitions
    EXPECT_EQ(0u, after.flags & ~before.flags)
        << "No bits should transition from 0->1";
}

// Test 8: Assert monotonic time property
TEST_F(StateMachineTest, AssertMonotonicTime) {
    BlockDirEntryV16 entry;
    entry.tag_id = 300;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initBlockDirEntry(0, entry));

    // Assert monotonic time
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->assertMonotonicTime(0));

    // Read back and verify
    BlockDirEntryV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readBlockDirEntry(0, after));

    EXPECT_FALSE(blockIsBitSet(after.flags, BlockStateBit::BLB_MONOTONIC_TIME))
        << "BLB_MONOTONIC_TIME bit should be cleared";
}

// Test 9: Assert no time gap property
TEST_F(StateMachineTest, AssertNoTimeGap) {
    BlockDirEntryV16 entry;
    entry.tag_id = 400;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initBlockDirEntry(0, entry));

    // Assert no time gap
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->assertNoTimeGap(0));

    // Read back and verify
    BlockDirEntryV16 after;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readBlockDirEntry(0, after));

    EXPECT_FALSE(blockIsBitSet(after.flags, BlockStateBit::BLB_NO_TIME_GAP))
        << "BLB_NO_TIME_GAP bit should be cleared";
}

// Test 10: Prevent double allocation
TEST_F(StateMachineTest, PreventDoubleAllocation) {
    RawChunkHeaderV16 header;
    header.chunk_id = 5;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // First allocation should succeed
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Second allocation should fail
    EXPECT_EQ(MutateResult::ERROR_ALREADY_SET, mutator_->allocateChunk(0));
}

// Test 11: Prevent double seal
TEST_F(StateMachineTest, PreventDoubleSeal) {
    RawChunkHeaderV16 header;
    header.chunk_id = 6;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // First seal should succeed
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->sealChunk(0, 1000000, 2000000, 0x12345678));

    // Second seal should fail
    EXPECT_EQ(MutateResult::ERROR_ALREADY_SET,
              mutator_->sealChunk(0, 1000000, 2000000, 0x12345678));
}

// Test 12: Multiple chunks at different offsets
TEST_F(StateMachineTest, MultipleChunks) {
    // Chunk 0 at offset 0
    RawChunkHeaderV16 header0;
    header0.chunk_id = 0;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header0));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Chunk 1 at offset 256MB
    uint64_t chunk1_offset = 256 * 1024 * 1024;
    RawChunkHeaderV16 header1;
    header1.chunk_id = 1;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->initChunkHeader(chunk1_offset, header1));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(chunk1_offset));

    // Verify chunk 0
    RawChunkHeaderV16 read0;
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->readChunkHeader(0, read0));
    EXPECT_EQ(0u, read0.chunk_id);
    EXPECT_TRUE(chunkIsAllocated(read0.flags));

    // Verify chunk 1
    RawChunkHeaderV16 read1;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->readChunkHeader(chunk1_offset, read1));
    EXPECT_EQ(1u, read1.chunk_id);
    EXPECT_TRUE(chunkIsAllocated(read1.flags));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
