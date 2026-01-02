#include "xTdb/directory_builder.h"
#include "xTdb/chunk_sealer.h"
#include "xTdb/block_writer.h"
#include "xTdb/state_mutator.h"
#include <gtest/gtest.h>
#include <unistd.h>

using namespace xtdb;

// ============================================================================
// T6-DirectoryIntegrity: Verify directory sealing and chunk finalization
// ============================================================================

class SealDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/xtdb_seal_test.dat";
        ::unlink(test_file_.c_str());

        // Open file
        io_ = std::make_unique<AlignedIO>();
        ASSERT_EQ(IOResult::SUCCESS, io_->open(test_file_, true, false));

        // Preallocate 256MB
        ASSERT_EQ(IOResult::SUCCESS,
                  io_->preallocate(256 * 1024 * 1024));

        // Calculate layout for RAW16K
        layout_ = LayoutCalculator::calculateLayout(
            RawBlockClass::RAW_16K,
            kDefaultChunkSizeExtents);

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
    ChunkLayout layout_;
};

// Test 1: DirectoryBuilder initialization
TEST_F(SealDirectoryTest, DirectoryBuilderInit) {
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());
    EXPECT_EQ(0u, dir_builder.getSealedBlockCount());
}

// Test 2: Seal single block
TEST_F(SealDirectoryTest, SealSingleBlock) {
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Seal block 0
    ASSERT_EQ(DirBuildResult::SUCCESS,
              dir_builder.sealBlock(0,                    // block_index
                                   100,                  // tag_id
                                   1000000,              // start_ts_us
                                   2000000,              // end_ts_us
                                   TimeUnit::TU_MS,      // time_unit
                                   ValueType::VT_F64,    // value_type
                                   50,                   // record_count
                                   0x12345678));         // data_crc32

    EXPECT_EQ(1u, dir_builder.getSealedBlockCount());

    // Verify entry
    const BlockDirEntryV16* entry = dir_builder.getEntry(0);
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(100u, entry->tag_id);
    EXPECT_EQ(1000000, entry->start_ts_us);
    EXPECT_EQ(2000000, entry->end_ts_us);
    EXPECT_EQ(static_cast<uint32_t>(TimeUnit::TU_MS), entry->time_unit);
    EXPECT_EQ(static_cast<uint32_t>(ValueType::VT_F64), entry->value_type);
    EXPECT_EQ(50u, entry->record_count);
    EXPECT_EQ(0x12345678u, entry->data_crc32);
}

// Test 3: Seal multiple blocks
TEST_F(SealDirectoryTest, SealMultipleBlocks) {
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Seal 10 blocks
    for (uint32_t i = 0; i < 10; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i,
                                       100 + i,
                                       1000000 + i * 10000,
                                       2000000 + i * 10000,
                                       TimeUnit::TU_MS,
                                       ValueType::VT_F64,
                                       50 + i,
                                       0x12345678 + i));
    }

    EXPECT_EQ(10u, dir_builder.getSealedBlockCount());

    // Verify entries
    for (uint32_t i = 0; i < 10; i++) {
        const BlockDirEntryV16* entry = dir_builder.getEntry(i);
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(100u + i, entry->tag_id);
        EXPECT_EQ(50u + i, entry->record_count);
    }
}

// Test 4: Write directory to disk
TEST_F(SealDirectoryTest, WriteDirectoryToDisk) {
    // Initialize chunk header
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // Build directory
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Seal some blocks
    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 100 + i, 1000000, 2000000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50, 0x12345678));
    }

    // Write directory
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // Read back directory and verify
    // Directory starts at block 1 (block 0 contains chunk header)
    uint64_t dir_offset = layout_.block_size_bytes;
    uint64_t dir_size = layout_.data_blocks * sizeof(BlockDirEntryV16);
    uint64_t buffer_size = alignToExtent(dir_size);

    AlignedBuffer buffer(buffer_size);
    ASSERT_EQ(IOResult::SUCCESS,
              io_->read(buffer.data(), buffer_size, dir_offset));

    // Verify first entry
    const BlockDirEntryV16* entries =
        static_cast<const BlockDirEntryV16*>(buffer.data());
    EXPECT_EQ(100u, entries[0].tag_id);
    EXPECT_EQ(50u, entries[0].record_count);
}

// Test 5: ChunkSealer SuperCRC calculation
TEST_F(SealDirectoryTest, SuperCRCCalculation) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));

    // Build and write directory
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 100 + i, 1000000, 2000000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50, 0x12345678));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // Calculate SuperCRC
    ChunkSealer sealer(io_.get(), mutator_.get());
    uint32_t super_crc32 = 0;
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.calculateSuperCRC(0, layout_, super_crc32));

    // SuperCRC should be non-zero
    EXPECT_NE(0u, super_crc32);
}

// Test 6: Full chunk seal
TEST_F(SealDirectoryTest, FullChunkSeal) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 5;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Build and write directory
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t i = 0; i < 10; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 100 + i, 1000000 + i * 1000,
                                       2000000 + i * 1000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       100, 0x12345678));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // Seal chunk
    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 2009000));

    // Verify chunk header
    RawChunkHeaderV16 sealed_header;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->readChunkHeader(0, sealed_header));

    EXPECT_EQ(5u, sealed_header.chunk_id);
    EXPECT_EQ(1000000, sealed_header.start_ts_us);
    EXPECT_EQ(2009000, sealed_header.end_ts_us);
    EXPECT_NE(0u, sealed_header.super_crc32);
    EXPECT_TRUE(chunkIsSealed(sealed_header.flags));  // Chunk is sealed
}

// Test 7: Integrated write-seal workflow
TEST_F(SealDirectoryTest, IntegratedWriteSealWorkflow) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Write data to blocks using BlockWriter
    BlockWriter writer(io_.get(), layout_);

    int64_t global_start_ts = 1000000;
    int64_t global_end_ts = 1000000;

    for (uint32_t block_idx = 0; block_idx < 100; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 100 + block_idx;
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.time_unit = TimeUnit::TU_MS;
        tag_buffer.start_ts_us = 1000000 + block_idx * 1000;

        // Add 50 records
        for (int i = 0; i < 50; i++) {
            MemRecord record;
            record.time_offset = i * 10;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 100 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));

        // Track global timestamps
        int64_t block_end_ts = tag_buffer.start_ts_us + 49 * 10 * 1000;  // MS to US
        if (block_end_ts > global_end_ts) {
            global_end_ts = block_end_ts;
        }
    }

    // Build directory
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Seal all 100 blocks
    for (uint32_t block_idx = 0; block_idx < 100; block_idx++) {
        int64_t block_start_ts = 1000000 + block_idx * 1000;
        int64_t block_end_ts = block_start_ts + 49 * 10 * 1000;  // MS to US

        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(block_idx,
                                       100 + block_idx,
                                       block_start_ts,
                                       block_end_ts,
                                       TimeUnit::TU_MS,
                                       ValueType::VT_F64,
                                       50,
                                       0));  // CRC32 placeholder
    }

    EXPECT_EQ(100u, dir_builder.getSealedBlockCount());

    // Write directory
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // Seal chunk
    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, global_start_ts, global_end_ts));

    // Verify chunk is sealed
    RawChunkHeaderV16 sealed_header;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator_->readChunkHeader(0, sealed_header));

    EXPECT_EQ(global_start_ts, sealed_header.start_ts_us);
    EXPECT_EQ(global_end_ts, sealed_header.end_ts_us);
    EXPECT_NE(0u, sealed_header.super_crc32);
    EXPECT_TRUE(chunkIsSealed(sealed_header.flags));  // Chunk is sealed

    // Verify we can read back directory
    const BlockDirEntryV16* entry = dir_builder.getEntry(50);
    ASSERT_NE(nullptr, entry);
    EXPECT_EQ(150u, entry->tag_id);
    EXPECT_EQ(50u, entry->record_count);
}

// Test 8: Double seal prevention
TEST_F(SealDirectoryTest, DoubleSealPrevention) {
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Seal block once
    ASSERT_EQ(DirBuildResult::SUCCESS,
              dir_builder.sealBlock(0, 100, 1000000, 2000000,
                                   TimeUnit::TU_MS, ValueType::VT_F64,
                                   50, 0x12345678));

    // Try to seal again
    EXPECT_EQ(DirBuildResult::ERROR_BLOCK_SEALED,
              dir_builder.sealBlock(0, 100, 1000000, 2000000,
                                   TimeUnit::TU_MS, ValueType::VT_F64,
                                   50, 0x12345678));
}

// Test 9: Invalid block index
TEST_F(SealDirectoryTest, InvalidBlockIndex) {
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    // Try to seal block beyond range
    EXPECT_EQ(DirBuildResult::ERROR_INVALID_BLOCK,
              dir_builder.sealBlock(layout_.data_blocks + 1,
                                   100, 1000000, 2000000,
                                   TimeUnit::TU_MS, ValueType::VT_F64,
                                   50, 0x12345678));
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
