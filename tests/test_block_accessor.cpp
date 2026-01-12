#include "xTdb/block_accessor.h"
#include "xTdb/compact_archive_manager.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/aligned_io.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace xtdb;

class BlockAccessorTest : public ::testing::Test {
protected:
    void SetUp() override {
        raw_path_ = "/tmp/test_block_accessor_raw.dat";
        compact_path_ = "/tmp/test_block_accessor_compact.dat";
        db_path_ = "/tmp/test_block_accessor_metadata.db";

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

        // Create block accessor
        block_accessor_ = std::make_unique<BlockAccessor>(
            raw_container_.get(),
            compact_container_.get(),
            metadata_sync_.get()
        );
    }

    void TearDown() override {
        // Reset in proper order
        block_accessor_.reset();
        raw_container_.reset();
        compact_container_.reset();

        // Close and reset metadata sync
        if (metadata_sync_) {
            metadata_sync_->close();
            metadata_sync_.reset();
        }

        // Small delay to ensure all file handles are closed
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // Remove test files
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
    std::unique_ptr<BlockAccessor> block_accessor_;
};

TEST_F(BlockAccessorTest, ReadFromRAW) {
    // Write 3 test blocks
    writeTestBlocks(3, 1000);

    // Read block 1 from RAW
    BlockData block_data;
    AccessResult result = block_accessor_->readBlock(
        0,  // raw_container_id
        0,  // chunk_id
        1,  // block_index
        layout_,
        block_data
    );

    if (result != AccessResult::SUCCESS) {
        std::cerr << "ERROR: " << block_accessor_->getLastError() << std::endl;
    }
    EXPECT_EQ(AccessResult::SUCCESS, result);
    EXPECT_EQ(0u, block_data.container_id);
    EXPECT_EQ(0u, block_data.chunk_id);
    EXPECT_EQ(1u, block_data.block_index);
    EXPECT_EQ(1000u, block_data.tag_id);
    EXPECT_FALSE(block_data.is_compressed);
    EXPECT_EQ(layout_.block_size_bytes, block_data.data.size());

    // Verify statistics
    const AccessStats& stats = block_accessor_->getStats();
    EXPECT_EQ(1u, stats.raw_reads);
    EXPECT_EQ(0u, stats.compact_reads);
    EXPECT_GT(stats.total_bytes_read, 0u);
}

TEST_F(BlockAccessorTest, ReadFromCOMPACT) {
    // Write 3 test blocks
    writeTestBlocks(3, 1000);

    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Archive blocks
    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    ArchiveManagerResult archive_result = archive_manager.archiveOldBlocks(
        0,  // raw_container_id
        1,  // compact_container_id
        1,  // min_age_seconds
        layout_
    );

    ASSERT_EQ(ArchiveManagerResult::SUCCESS, archive_result);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container_->seal());

    // Read block 1 (should come from COMPACT now)
    block_accessor_->resetStats();

    BlockData block_data;
    AccessResult result = block_accessor_->readBlock(
        0,  // raw_container_id
        0,  // chunk_id
        1,  // block_index
        layout_,
        block_data
    );

    EXPECT_EQ(AccessResult::SUCCESS, result);
    EXPECT_EQ(1u, block_data.container_id);  // COMPACT container
    EXPECT_EQ(0u, block_data.chunk_id);
    EXPECT_EQ(1u, block_data.block_index);
    EXPECT_EQ(1000u, block_data.tag_id);
    EXPECT_TRUE(block_data.is_compressed);
    EXPECT_GT(block_data.data.size(), 0u);

    // Verify statistics
    const AccessStats& stats = block_accessor_->getStats();
    EXPECT_EQ(0u, stats.raw_reads);
    EXPECT_EQ(1u, stats.compact_reads);
    EXPECT_GT(stats.total_bytes_read, 0u);
    EXPECT_GT(stats.total_bytes_decompressed, 0u);
}

TEST_F(BlockAccessorTest, QueryMixedBlocks) {
    // Write first 3 test blocks
    writeTestBlocks(3, 1000);

    // Wait to make them old
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Write 2 more recent blocks
    int64_t base_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (int i = 3; i < 5; i++) {
        AlignedBuffer block_data(layout_.block_size_bytes);
        uint8_t* data_ptr = static_cast<uint8_t*>(block_data.data());
        for (size_t j = 0; j < block_data.size(); j++) {
            data_ptr[j] = static_cast<uint8_t>((i + j) % 256);
        }

        uint32_t physical_block_index = layout_.meta_blocks + i;
        uint64_t block_offset = LayoutCalculator::calculateBlockOffset(
            0, physical_block_index, layout_);

        ContainerResult write_result = raw_container_->write(
            block_data.data(), block_data.size(), block_offset);
        ASSERT_EQ(ContainerResult::SUCCESS, write_result);

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
        sqlite3_bind_int(stmt, 2, 1000);
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

    // Archive blocks older than 1 second (should archive first 3 blocks only)
    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    ArchiveManagerResult archive_result = archive_manager.archiveOldBlocks(
        0, 1, 1, layout_);

    ASSERT_EQ(ArchiveManagerResult::SUCCESS, archive_result);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container_->seal());

    // Debug: Check what's in the database
    sqlite3* db = nullptr;
    int rc = sqlite3_open(db_path_.c_str(), &db);
    ASSERT_EQ(SQLITE_OK, rc);

    const char* debug_sql = "SELECT container_id, chunk_id, block_index, tag_id, is_archived FROM blocks ORDER BY container_id, block_index;";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db, debug_sql, -1, &stmt, nullptr);
    ASSERT_EQ(SQLITE_OK, rc);

    std::cout << "\n=== Database contents ===\n";
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        std::cout << "container=" << sqlite3_column_int(stmt, 0)
                  << " chunk=" << sqlite3_column_int(stmt, 1)
                  << " block=" << sqlite3_column_int(stmt, 2)
                  << " tag=" << sqlite3_column_int(stmt, 3)
                  << " archived=" << sqlite3_column_int(stmt, 4)
                  << std::endl;
    }
    std::cout << "========================\n\n";
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Query all blocks (should get mix of RAW and COMPACT)
    block_accessor_->resetStats();

    std::vector<BlockData> results;
    AccessResult result = block_accessor_->queryBlocksByTagAndTime(
        1000,  // tag_id
        0,     // start_ts
        INT64_MAX,  // end_ts
        layout_,
        results
    );

    EXPECT_EQ(AccessResult::SUCCESS, result);
    EXPECT_EQ(5u, results.size());

    // Verify some blocks came from each container
    int raw_count = 0;
    int compact_count = 0;

    for (const auto& block : results) {
        if (block.is_compressed) {
            compact_count++;
        } else {
            raw_count++;
        }
    }

    EXPECT_GT(compact_count, 0);  // At least some from COMPACT
    EXPECT_GT(raw_count, 0);      // At least some from RAW

    // Verify statistics
    const AccessStats& stats = block_accessor_->getStats();
    EXPECT_GT(stats.raw_reads, 0u);
    EXPECT_GT(stats.compact_reads, 0u);
    EXPECT_EQ(5u, stats.raw_reads + stats.compact_reads);
}

TEST_F(BlockAccessorTest, TransparentAccess) {
    // Write 3 test blocks
    writeTestBlocks(3, 1000);

    // Initially all blocks should be in RAW
    block_accessor_->resetStats();

    BlockData block_data;
    AccessResult result = block_accessor_->readBlock(0, 0, 0, layout_, block_data);
    EXPECT_EQ(AccessResult::SUCCESS, result);
    EXPECT_FALSE(block_data.is_compressed);

    const AccessStats& stats1 = block_accessor_->getStats();
    EXPECT_EQ(1u, stats1.raw_reads);
    EXPECT_EQ(0u, stats1.compact_reads);

    // Archive the blocks
    std::this_thread::sleep_for(std::chrono::seconds(2));

    CompactArchiveManager archive_manager(
        raw_container_.get(),
        compact_container_.get(),
        metadata_sync_.get()
    );

    ArchiveManagerResult archive_result = archive_manager.archiveOldBlocks(
        0, 1, 1, layout_);
    ASSERT_EQ(ArchiveManagerResult::SUCCESS, archive_result);
    ASSERT_EQ(ContainerResult::SUCCESS, compact_container_->seal());

    // Now same read should come from COMPACT
    block_accessor_->resetStats();

    result = block_accessor_->readBlock(0, 0, 0, layout_, block_data);
    EXPECT_EQ(AccessResult::SUCCESS, result);
    EXPECT_TRUE(block_data.is_compressed);

    const AccessStats& stats2 = block_accessor_->getStats();
    EXPECT_EQ(0u, stats2.raw_reads);
    EXPECT_EQ(1u, stats2.compact_reads);

    std::cout << "Transparent access test passed - block seamlessly moved from RAW to COMPACT" << std::endl;
}
