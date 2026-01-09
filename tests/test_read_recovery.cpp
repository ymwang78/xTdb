#include "xTdb/raw_scanner.h"
#include "xTdb/block_reader.h"
#include "xTdb/block_writer.h"
#include "xTdb/directory_builder.h"
#include "xTdb/chunk_sealer.h"
#include "xTdb/state_mutator.h"
#include "xTdb/platform_compat.h"
#include "test_utils.h"
#include <gtest/gtest.h>

using namespace xtdb;

// ============================================================================
// T7-DisasterRecovery & T8-PartialWrite: Read path and recovery tests
// ============================================================================

class ReadRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string temp_dir = get_temp_dir();
        test_file_ = join_path(temp_dir, "xtdb_recovery_test.dat");
        unlink_file(test_file_);

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
        unlink_file(test_file_);
    }

    std::string test_file_;
    std::unique_ptr<AlignedIO> io_;
    std::unique_ptr<StateMutator> mutator_;
    ChunkLayout layout_;
};

// Test 1: RawScanner basic scan
TEST_F(ReadRecoveryTest, RawScannerBasicScan) {
    // Initialize and seal a chunk with some data
    RawChunkHeaderV16 header;
    header.chunk_id = 100;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Build and seal 5 blocks
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t i = 0; i < 5; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 1000 + i, 1000000 + i * 10000,
                                       2000000 + i * 10000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50 + i, 0x12345678 + i));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // Seal chunk
    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 2040000));

    // Scan chunk
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    // Verify chunk metadata
    EXPECT_EQ(100u, scanned_chunk.chunk_id);
    EXPECT_EQ(1000000, scanned_chunk.start_ts_us);
    EXPECT_EQ(2040000, scanned_chunk.end_ts_us);
    EXPECT_TRUE(scanned_chunk.is_sealed);
    EXPECT_NE(0u, scanned_chunk.super_crc32);

    // Verify blocks
    EXPECT_EQ(5, scanned_chunk.blocks.size());
    for (size_t i = 0; i < scanned_chunk.blocks.size(); i++) {
        const ScannedBlock& block = scanned_chunk.blocks[i];
        EXPECT_EQ(i, block.block_index);
        EXPECT_EQ(1000u + i, block.tag_id);
        EXPECT_EQ(50u + i, block.record_count);
        EXPECT_TRUE(block.is_sealed);
    }
}

// Test 2: RawScanner verify chunk integrity
TEST_F(ReadRecoveryTest, RawScannerVerifyIntegrity) {
    // Initialize and seal a chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 100 + i, 1000000, 2000000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50, 0x12345678));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 2000000));

    // Verify integrity
    RawScanner scanner(io_.get());
    EXPECT_EQ(ScanResult::SUCCESS,
              scanner.verifyChunkIntegrity(0, layout_));
}

// Test 3: BlockReader read and parse records
TEST_F(ReadRecoveryTest, BlockReaderReadRecords) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Write data using BlockWriter
    BlockWriter writer(io_.get(), layout_);

    TagBuffer tag_buffer;
    tag_buffer.tag_id = 100;
    tag_buffer.value_type = ValueType::VT_F64;
    tag_buffer.time_unit = TimeUnit::TU_MS;
    tag_buffer.start_ts_us = 1000000;

    // Add 50 records
    for (int i = 0; i < 50; i++) {
        MemRecord record;
        record.time_offset = i * 10;
        record.quality = 0xC0;
        record.value.f64_value = 100.0 + i;
        tag_buffer.records.push_back(record);
    }

    ASSERT_EQ(BlockWriteResult::SUCCESS,
              writer.writeBlock(0, 0, tag_buffer));

    // Read back using BlockReader
    BlockReader reader(io_.get(), layout_);
    std::vector<MemRecord> read_records;

    ASSERT_EQ(ReadResult::SUCCESS,
              reader.readBlock(0, 0, 100, 1000000,
                             TimeUnit::TU_MS, ValueType::VT_F64, 50,
                             read_records));

    // Verify records
    ASSERT_EQ(50, read_records.size());

    for (int i = 0; i < 50; i++) {
        EXPECT_EQ(i * 10u, read_records[i].time_offset);
        EXPECT_EQ(0xC0, read_records[i].quality);
        EXPECT_DOUBLE_EQ(100.0 + i, read_records[i].value.f64_value);
    }

    // Verify statistics
    EXPECT_EQ(1u, reader.getStats().blocks_read);
    EXPECT_EQ(50u, reader.getStats().records_read);
}

// Test 4: BlockReader read multiple blocks
TEST_F(ReadRecoveryTest, BlockReaderMultipleBlocks) {
    // Initialize chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Write 10 blocks
    BlockWriter writer(io_.get(), layout_);

    for (uint32_t block_idx = 0; block_idx < 10; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 100 + block_idx;
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.time_unit = TimeUnit::TU_MS;
        tag_buffer.start_ts_us = 1000000 + block_idx * 100000;

        for (int i = 0; i < 20; i++) {
            MemRecord record;
            record.time_offset = i;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 100 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));
    }

    // Read back all blocks
    BlockReader reader(io_.get(), layout_);

    for (uint32_t block_idx = 0; block_idx < 10; block_idx++) {
        std::vector<MemRecord> records;
        ASSERT_EQ(ReadResult::SUCCESS,
                  reader.readBlock(0, block_idx, 100 + block_idx,
                                 1000000 + block_idx * 100000,
                                 TimeUnit::TU_MS, ValueType::VT_F64, 20,
                                 records));

        ASSERT_EQ(20, records.size());

        // Verify first and last record
        EXPECT_EQ(0u, records[0].time_offset);
        EXPECT_DOUBLE_EQ(static_cast<double>(block_idx * 100),
                        records[0].value.f64_value);

        EXPECT_EQ(19u, records[19].time_offset);
        EXPECT_DOUBLE_EQ(static_cast<double>(block_idx * 100 + 19),
                        records[19].value.f64_value);
    }

    // Verify statistics
    EXPECT_EQ(10u, reader.getStats().blocks_read);
    EXPECT_EQ(200u, reader.getStats().records_read);
}

// Test 5: T7-DisasterRecovery - Full write-seal-scan-read cycle
TEST_F(ReadRecoveryTest, T7_DisasterRecovery) {
    // Simulate a complete write-seal-recovery cycle

    // 1. Write phase
    RawChunkHeaderV16 header;
    header.chunk_id = 42;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Write 20 blocks with data
    BlockWriter writer(io_.get(), layout_);

    for (uint32_t block_idx = 0; block_idx < 20; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 1000 + block_idx;
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.time_unit = TimeUnit::TU_MS;
        tag_buffer.start_ts_us = 1000000 + block_idx * 50000;

        for (int i = 0; i < 30; i++) {
            MemRecord record;
            record.time_offset = i * 5;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 1000 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));
    }

    // 2. Seal phase
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t block_idx = 0; block_idx < 20; block_idx++) {
        int64_t start_ts = 1000000 + block_idx * 50000;
        int64_t end_ts = start_ts + 29 * 5 * 1000;  // 29 records * 5ms * 1000us

        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(block_idx, 1000 + block_idx,
                                       start_ts, end_ts,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       30, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 1000000 + 19 * 50000 + 145000));

    // 3. Recovery phase - Scan directory
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    EXPECT_EQ(42u, scanned_chunk.chunk_id);
    EXPECT_TRUE(scanned_chunk.is_sealed);
    EXPECT_EQ(20, scanned_chunk.blocks.size());

    // 4. Recovery phase - Read all data
    BlockReader reader(io_.get(), layout_);

    for (const auto& scanned_block : scanned_chunk.blocks) {
        std::vector<MemRecord> records;
        ASSERT_EQ(ReadResult::SUCCESS,
                  reader.readBlock(0, scanned_block.block_index,
                                 scanned_block.tag_id,
                                 scanned_block.start_ts_us,
                                 scanned_block.time_unit,
                                 scanned_block.value_type,
                                 scanned_block.record_count,
                                 records));

        ASSERT_EQ(scanned_block.record_count, records.size());

        // Verify data integrity
        uint32_t block_idx = scanned_block.block_index;
        for (size_t i = 0; i < records.size(); i++) {
            EXPECT_EQ(i * 5u, records[i].time_offset);
            EXPECT_EQ(0xC0, records[i].quality);
            EXPECT_DOUBLE_EQ(static_cast<double>(block_idx * 1000 + i),
                            records[i].value.f64_value);
        }
    }

    // Verify total recovery
    EXPECT_EQ(20u, reader.getStats().blocks_read);
    EXPECT_EQ(600u, reader.getStats().records_read);  // 20 blocks * 30 records
}

// Test 6: T8-PartialWrite - Handle unsealed blocks
TEST_F(ReadRecoveryTest, T8_PartialWrite) {
    // Simulate partial write scenario (some blocks sealed, some not)

    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    // Write 10 blocks
    BlockWriter writer(io_.get(), layout_);

    for (uint32_t block_idx = 0; block_idx < 10; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 100 + block_idx;
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.time_unit = TimeUnit::TU_MS;
        tag_buffer.start_ts_us = 1000000 + block_idx * 10000;

        for (int i = 0; i < 25; i++) {
            MemRecord record;
            record.time_offset = i;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 100 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));
    }

    // Seal only first 5 blocks (simulate crash during seal)
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t block_idx = 0; block_idx < 5; block_idx++) {
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(block_idx, 100 + block_idx,
                                       1000000 + block_idx * 10000,
                                       2000000 + block_idx * 10000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       25, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    // DON'T seal the chunk (simulating partial operation)

    // Scan - should find only 5 sealed blocks
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    EXPECT_FALSE(scanned_chunk.is_sealed);  // Chunk not sealed
    EXPECT_EQ(5, scanned_chunk.blocks.size());  // Only sealed blocks visible

    // Verify we can read the sealed blocks
    BlockReader reader(io_.get(), layout_);

    for (const auto& scanned_block : scanned_chunk.blocks) {
        std::vector<MemRecord> records;
        ASSERT_EQ(ReadResult::SUCCESS,
                  reader.readBlock(0, scanned_block.block_index,
                                 scanned_block.tag_id,
                                 scanned_block.start_ts_us,
                                 scanned_block.time_unit,
                                 scanned_block.value_type,
                                 scanned_block.record_count,
                                 records));

        EXPECT_EQ(25, records.size());
    }

    EXPECT_EQ(5u, reader.getStats().blocks_read);
    EXPECT_EQ(125u, reader.getStats().records_read);  // 5 blocks * 25 records
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
