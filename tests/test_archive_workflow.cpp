#include "xTdb/compact_archive_manager.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/aligned_io.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace xtdb;

class ArchiveWorkflowTest : public ::testing::Test {
protected:
    void SetUp() override {
        raw_path_ = "/tmp/test_archive_raw.dat";
        compact_path_ = "/tmp/test_archive_compact.dat";
        db_path_ = "/tmp/test_archive_metadata.db";

        // Remove test files if they exist
        std::filesystem::remove(raw_path_);
        std::filesystem::remove(compact_path_);
        std::filesystem::remove(db_path_);

        // Calculate layout
        layout_ = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);

        // Initialize metadata sync
        metadata_sync_ = std::make_unique<MetadataSync>(db_path_);
        ASSERT_EQ(SyncResult::SUCCESS, metadata_sync_->open());
        ASSERT_EQ(SyncResult::SUCCESS, metadata_sync_->initSchema());

        // Create and open RAW container
        raw_container_ = std::make_unique<FileContainer>(raw_path_, layout_, false, false);
        ASSERT_EQ(ContainerResult::SUCCESS, raw_container_->open(true));

        // Create and open COMPACT container
        compact_container_ = std::make_unique<CompactContainer>(
            compact_path_, layout_, CompressionType::COMP_ZSTD);
        ASSERT_EQ(ContainerResult::SUCCESS, compact_container_->open(true));
    }

    void TearDown() override {
        raw_container_.reset();
        compact_container_.reset();
        metadata_sync_->close();
        metadata_sync_.reset();

        std::filesystem::remove(raw_path_);
        std::filesystem::remove(compact_path_);
        std::filesystem::remove(db_path_);
    }

    // Helper: Write test data to RAW container and sync metadata
    void writeTestBlocks(int num_blocks, uint32_t tag_id) {
        // Use current time as base
        int64_t base_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        for (int i = 0; i < num_blocks; i++) {
            // Create test data
            AlignedBuffer block_data(layout_.block_size_bytes);
            uint8_t* data_ptr = static_cast<uint8_t*>(block_data.data());
            for (size_t j = 0; j < block_data.size(); j++) {
                data_ptr[j] = static_cast<uint8_t>((i + j) % 256);
            }

            // Write to RAW container
            uint32_t physical_block_index = layout_.meta_blocks + i;
            uint64_t block_offset = LayoutCalculator::calculateBlockOffset(
                0, physical_block_index, layout_);

            ContainerResult write_result = raw_container_->write(
                block_data.data(), block_data.size(), block_offset);
            ASSERT_EQ(ContainerResult::SUCCESS, write_result);

            // Prepare metadata insert
            sqlite3* db = nullptr;
            int rc = sqlite3_open(db_path_.c_str(), &db);
            ASSERT_EQ(SQLITE_OK, rc);

            const char* sql = R"(
                INSERT INTO blocks
                (chunk_id, block_index, tag_id, start_ts_us, end_ts_us,
                 time_unit, value_type, record_count, chunk_offset,
                 container_id, is_archived)
                VALUES (0, ?, ?, ?, ?, ?, ?, ?, 0, 0, 0);
            )";

            sqlite3_stmt* stmt = nullptr;
            rc = sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
            ASSERT_EQ(SQLITE_OK, rc);

            int64_t start_ts_us = base_time_us + i * 1000;
            int64_t end_ts_us = start_ts_us + 999;

            sqlite3_bind_int(stmt, 1, i);
            sqlite3_bind_int(stmt, 2, tag_id);
            sqlite3_bind_int64(stmt, 3, start_ts_us);
            sqlite3_bind_int64(stmt, 4, end_ts_us);
            sqlite3_bind_int(stmt, 5, static_cast<int>(TimeUnit::TU_US));
            sqlite3_bind_int(stmt, 6, static_cast<int>(ValueType::VT_F64));
            sqlite3_bind_int(stmt, 7, 100);

            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            sqlite3_close(db);

            ASSERT_EQ(SQLITE_DONE, rc);
        }
    }

    std::string raw_path_;
    std::string compact_path_;
    std::string db_path_;
    ChunkLayout layout_;

    std::unique_ptr<FileContainer> raw_container_;
    std::unique_ptr<CompactContainer> compact_container_;
    std::unique_ptr<MetadataSync> metadata_sync_;
};

TEST_F(ArchiveWorkflowTest, BasicArchiveWorkflow) {
    // Write 5 test blocks to RAW container
    writeTestBlocks(5, 1000);

    std::cout << "Waiting 2 seconds for blocks to age..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Create archive manager
    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    // Archive blocks older than 1 second
    ArchiveManagerResult result = archive_manager.archiveOldBlocks(
        0,  // raw_container_id
        1,  // compact_container_id
        1,  // min_age_seconds = 1 second
        layout_
    );

    EXPECT_EQ(ArchiveManagerResult::SUCCESS, result);

    // Check statistics
    const ArchiveManagerStats& stats = archive_manager.getStats();
    EXPECT_EQ(5u, stats.blocks_found);
    EXPECT_EQ(5u, stats.blocks_archived);
    EXPECT_EQ(0u, stats.blocks_failed);
    EXPECT_GT(stats.total_bytes_read, 0u);
    EXPECT_GT(stats.total_bytes_written, 0u);
    EXPECT_LT(stats.average_compression_ratio, 1.0);  // Should have compression

    std::cout << "Archive statistics:" << std::endl;
    std::cout << "  Blocks found: " << stats.blocks_found << std::endl;
    std::cout << "  Blocks archived: " << stats.blocks_archived << std::endl;
    std::cout << "  Compression ratio: " << stats.average_compression_ratio << std::endl;

    // Seal COMPACT container
    EXPECT_EQ(ContainerResult::SUCCESS, compact_container_->seal());

    // Verify COMPACT container has blocks
    EXPECT_EQ(5u, compact_container_->getBlockCount());

    // Verify metadata was updated (blocks should be marked as archived)
    std::vector<BlockQueryResult> archived_blocks;
    SyncResult sync_result = metadata_sync_->queryBlocksForArchive(
        0,  // raw_container_id
        0,  // min_age_seconds = 0 (all blocks)
        archived_blocks
    );

    EXPECT_EQ(SyncResult::SUCCESS, sync_result);
    EXPECT_EQ(0u, archived_blocks.size());  // No more blocks to archive
}

TEST_F(ArchiveWorkflowTest, NoBlocksToArchive) {
    // Write blocks but don't wait (they won't be old enough)
    writeTestBlocks(3, 1000);

    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    // Try to archive blocks older than 100 seconds (none should qualify)
    ArchiveManagerResult result = archive_manager.archiveOldBlocks(
        0, 1, 100, layout_);

    EXPECT_EQ(ArchiveManagerResult::ERR_NO_BLOCKS_TO_ARCHIVE, result);

    const ArchiveManagerStats& stats = archive_manager.getStats();
    EXPECT_EQ(0u, stats.blocks_found);
    EXPECT_EQ(0u, stats.blocks_archived);
}

TEST_F(ArchiveWorkflowTest, MultipleArchiveRuns) {
    // Write 10 blocks
    writeTestBlocks(10, 1000);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    // First archive run
    ArchiveManagerResult result = archive_manager.archiveOldBlocks(
        0, 1, 1, layout_);
    EXPECT_EQ(ArchiveManagerResult::SUCCESS, result);

    const ArchiveManagerStats& stats1 = archive_manager.getStats();
    EXPECT_EQ(10u, stats1.blocks_archived);

    // Second archive run (should find no blocks)
    result = archive_manager.archiveOldBlocks(0, 1, 1, layout_);
    EXPECT_EQ(ArchiveManagerResult::ERR_NO_BLOCKS_TO_ARCHIVE, result);

    // Add more blocks
    writeTestBlocks(5, 1001);
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Third archive run (should find the new blocks)
    result = archive_manager.archiveOldBlocks(0, 1, 1, layout_);
    EXPECT_EQ(ArchiveManagerResult::SUCCESS, result);

    const ArchiveManagerStats& stats3 = archive_manager.getStats();
    EXPECT_EQ(5u, stats3.blocks_archived);

    // Seal COMPACT container
    EXPECT_EQ(ContainerResult::SUCCESS, compact_container_->seal());

    // Total blocks in COMPACT container
    EXPECT_EQ(15u, compact_container_->getBlockCount());
}
