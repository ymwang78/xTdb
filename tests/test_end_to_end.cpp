#include "xTdb/metadata_sync.h"
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
// T9-EndToEnd: Complete write-seal-sync-query workflow
// ============================================================================

class EndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string temp_dir = get_temp_dir();
        test_file_ = join_path(temp_dir, "xtdb_e2e_test.dat");
        test_db_ = join_path(temp_dir, "xtdb_e2e_test.db");

        unlink_file(test_file_);
        unlink_file(test_db_);

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
        unlink_file(test_db_);
    }

    std::string test_file_;
    std::string test_db_;
    std::unique_ptr<AlignedIO> io_;
    std::unique_ptr<StateMutator> mutator_;
    ChunkLayout layout_;
};

// Test 1: MetadataSync basic operations
TEST_F(EndToEndTest, MetadataSyncBasicOps) {
    MetadataSync sync(test_db_);

    // Open database
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());

    // Initialize schema
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());

    // Close
    sync.close();
}

// Test 2: Sync a chunk to database
TEST_F(EndToEndTest, SyncChunkToDatabase) {
    // Create and seal a chunk
    RawChunkHeaderV16 header;
    header.chunk_id = 100;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

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

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 2040000));

    // Scan chunk
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    // Sync to database
    MetadataSync sync(test_db_);
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());
    ASSERT_EQ(SyncResult::SUCCESS, sync.syncChunk(0, scanned_chunk));

    sync.close();
}

// Test 3: Query blocks by tag
TEST_F(EndToEndTest, QueryBlocksByTag) {
    // Setup: Create chunk with multiple tags
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

    // Create blocks with tags 100, 101, 102 (repeated)
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t tag_id = 100 + (i % 3);
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, tag_id, 1000000 + i * 10000,
                                       2000000 + i * 10000,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 2090000));

    // Scan and sync
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    MetadataSync sync(test_db_);
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());
    ASSERT_EQ(SyncResult::SUCCESS, sync.syncChunk(0, scanned_chunk));

    // Query by tag
    std::vector<BlockQueryResult> results;
    ASSERT_EQ(SyncResult::SUCCESS, sync.queryBlocksByTag(100, results));

    // Tag 100 appears at indices 0, 3, 6, 9
    EXPECT_EQ(4, results.size());
    for (const auto& result : results) {
        EXPECT_EQ(100u, result.tag_id);
    }

    // Query tag 101
    results.clear();
    ASSERT_EQ(SyncResult::SUCCESS, sync.queryBlocksByTag(101, results));
    EXPECT_EQ(3, results.size());  // Indices 1, 4, 7

    // Query tag 102
    results.clear();
    ASSERT_EQ(SyncResult::SUCCESS, sync.queryBlocksByTag(102, results));
    EXPECT_EQ(3, results.size());  // Indices 2, 5, 8

    sync.close();
}

// Test 4: Query blocks by time range
TEST_F(EndToEndTest, QueryBlocksByTimeRange) {
    // Setup: Create chunk with time-ordered blocks
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

    for (uint32_t i = 0; i < 10; i++) {
        int64_t start_ts = 1000000 + i * 100000;
        int64_t end_ts = start_ts + 50000;
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, 100, start_ts, end_ts,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       50, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 1950000));

    // Scan and sync
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    MetadataSync sync(test_db_);
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());
    ASSERT_EQ(SyncResult::SUCCESS, sync.syncChunk(0, scanned_chunk));

    // Query time range: 1200000 - 1400000
    std::vector<BlockQueryResult> results;
    ASSERT_EQ(SyncResult::SUCCESS,
              sync.queryBlocksByTimeRange(1200000, 1400000, results));

    // Should match blocks 2, 3, 4 (overlapping range)
    EXPECT_GE(results.size(), 3);

    sync.close();
}

// Test 5: T9-EndToEnd - Complete workflow
TEST_F(EndToEndTest, T9_CompleteWorkflow) {
    // Phase 1: Write data to multiple blocks
    RawChunkHeaderV16 header;
    header.chunk_id = 42;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator_->initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator_->allocateChunk(0));

    BlockWriter writer(io_.get(), layout_);

    // Write 15 blocks with 3 different tags
    for (uint32_t block_idx = 0; block_idx < 15; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 1000 + (block_idx % 3);  // Tags: 1000, 1001, 1002
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.time_unit = TimeUnit::TU_MS;
        tag_buffer.start_ts_us = 1000000 + block_idx * 60000;

        for (int i = 0; i < 40; i++) {
            MemRecord record;
            record.time_offset = i * 10;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 1000 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));
    }

    // Phase 2: Seal blocks and chunk
    DirectoryBuilder dir_builder(io_.get(), layout_, 0);
    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.initialize());

    for (uint32_t block_idx = 0; block_idx < 15; block_idx++) {
        int64_t start_ts = 1000000 + block_idx * 60000;
        int64_t end_ts = start_ts + 39 * 10 * 1000;  // 39 records * 10ms * 1000us

        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(block_idx,
                                       1000 + (block_idx % 3),
                                       start_ts, end_ts,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       40, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 1000000 + 14 * 60000 + 390000));

    // Phase 3: Scan chunk
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    EXPECT_EQ(42u, scanned_chunk.chunk_id);
    EXPECT_TRUE(scanned_chunk.is_sealed);
    EXPECT_EQ(15, scanned_chunk.blocks.size());

    // Phase 4: Sync to database
    MetadataSync sync(test_db_);
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());
    ASSERT_EQ(SyncResult::SUCCESS, sync.syncChunk(0, scanned_chunk));

    // Phase 5: Query by tag and read data
    std::vector<BlockQueryResult> query_results;
    ASSERT_EQ(SyncResult::SUCCESS,
              sync.queryBlocksByTag(1000, query_results));

    // Tag 1000 appears at blocks 0, 3, 6, 9, 12 (5 blocks)
    EXPECT_EQ(5, query_results.size());

    // Phase 6: Read actual data using BlockReader
    BlockReader reader(io_.get(), layout_);

    for (const auto& query_result : query_results) {
        std::vector<MemRecord> records;
        ASSERT_EQ(ReadResult::SUCCESS,
                  reader.readBlock(query_result.chunk_offset,
                                 query_result.block_index,
                                 query_result.tag_id,
                                 query_result.start_ts_us,
                                 query_result.time_unit,
                                 query_result.value_type,
                                 query_result.record_count,
                                 records));

        ASSERT_EQ(40, records.size());

        // Verify data integrity
        uint32_t expected_block_idx = query_result.block_index;
        for (size_t i = 0; i < records.size(); i++) {
            EXPECT_EQ(i * 10u, records[i].time_offset);
            EXPECT_EQ(0xC0, records[i].quality);
            EXPECT_DOUBLE_EQ(static_cast<double>(expected_block_idx * 1000 + i),
                            records[i].value.f64_value);
        }
    }

    EXPECT_EQ(5u, reader.getStats().blocks_read);
    EXPECT_EQ(200u, reader.getStats().records_read);  // 5 blocks * 40 records

    // Phase 7: Get all tags
    std::vector<uint32_t> all_tags;
    ASSERT_EQ(SyncResult::SUCCESS, sync.getAllTags(all_tags));
    EXPECT_EQ(3, all_tags.size());
    EXPECT_EQ(1000u, all_tags[0]);
    EXPECT_EQ(1001u, all_tags[1]);
    EXPECT_EQ(1002u, all_tags[2]);

    sync.close();
}

// Test 6: Query by tag and time range
TEST_F(EndToEndTest, QueryByTagAndTimeRange) {
    // Setup
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

    // Create blocks: tag 100 and 101, alternating, with time progression
    for (uint32_t i = 0; i < 10; i++) {
        uint32_t tag_id = 100 + (i % 2);
        int64_t start_ts = 1000000 + i * 50000;
        int64_t end_ts = start_ts + 25000;
        ASSERT_EQ(DirBuildResult::SUCCESS,
                  dir_builder.sealBlock(i, tag_id, start_ts, end_ts,
                                       TimeUnit::TU_MS, ValueType::VT_F64,
                                       30, 0));
    }

    ASSERT_EQ(DirBuildResult::SUCCESS, dir_builder.writeDirectory());

    ChunkSealer sealer(io_.get(), mutator_.get());
    ASSERT_EQ(SealResult::SUCCESS,
              sealer.sealChunk(0, layout_, 1000000, 1475000));

    // Scan and sync
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ASSERT_EQ(ScanResult::SUCCESS,
              scanner.scanChunk(0, layout_, scanned_chunk));

    MetadataSync sync(test_db_);
    ASSERT_EQ(SyncResult::SUCCESS, sync.open());
    ASSERT_EQ(SyncResult::SUCCESS, sync.initSchema());
    ASSERT_EQ(SyncResult::SUCCESS, sync.syncChunk(0, scanned_chunk));

    // Query: tag 100, time range 1100000-1300000
    std::vector<BlockQueryResult> results;
    ASSERT_EQ(SyncResult::SUCCESS,
              sync.queryBlocksByTagAndTime(100, 1100000, 1300000, results));

    // Tag 100 at blocks 0, 2, 4, 6, 8
    // Time ranges: 1000000-1025000, 1100000-1125000, 1200000-1225000, ...
    // Should match blocks 2, 4 (and possibly 0, 6 depending on overlap)
    EXPECT_GE(results.size(), 2);

    for (const auto& result : results) {
        EXPECT_EQ(100u, result.tag_id);
        // Verify time overlap
        EXPECT_TRUE(result.start_ts_us <= 1300000 && result.end_ts_us >= 1100000);
    }

    sync.close();
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
