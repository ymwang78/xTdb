#include "xTdb/compact_archive_manager.h"
#include <iostream>

namespace xtdb {

// ============================================================================
// Constructor & Destructor
// ============================================================================

CompactArchiveManager::CompactArchiveManager(FileContainer* raw_container,
                                             CompactContainer* compact_container,
                                             MetadataSync* metadata_sync)
    : raw_container_(raw_container),
      compact_container_(compact_container),
      metadata_sync_(metadata_sync),
      archiver_(std::make_unique<CompactArchiver>()),
      stats_(),
      last_error_() {
}

CompactArchiveManager::~CompactArchiveManager() {
}

// ============================================================================
// Archive Operations
// ============================================================================

ArchiveManagerResult CompactArchiveManager::archiveOldBlocks(
        uint32_t raw_container_id,
        uint32_t compact_container_id,
        int64_t min_age_seconds,
        const ChunkLayout& raw_layout) {

    resetStats();

    // Query blocks ready for archiving
    std::vector<BlockQueryResult> blocks_to_archive;
    SyncResult sync_result = metadata_sync_->queryBlocksForArchive(
        raw_container_id, min_age_seconds, blocks_to_archive);

    if (sync_result != SyncResult::SUCCESS) {
        setError("Failed to query blocks for archive: " + metadata_sync_->getLastError());
        return ArchiveManagerResult::ERR_METADATA_SYNC_FAILED;
    }

    if (blocks_to_archive.empty()) {
        setError("No blocks found for archiving");
        return ArchiveManagerResult::ERR_NO_BLOCKS_TO_ARCHIVE;
    }

    stats_.blocks_found = blocks_to_archive.size();

    std::cout << "Found " << blocks_to_archive.size() << " blocks to archive" << std::endl;

    // Archive each block
    uint32_t compact_block_index = compact_container_->getBlockCount();

    for (const auto& block : blocks_to_archive) {
        // Archive block using CompactArchiver
        ArchiveResult archive_result = archiver_->archiveBlock(
            *raw_container_,
            raw_layout,
            block.chunk_id,
            block.block_index,
            *compact_container_,
            block.tag_id,
            block.start_ts_us,
            block.end_ts_us,
            block.record_count,
            EncodingType::ENC_RAW,  // TODO: Get from metadata
            block.value_type,
            block.time_unit
        );

        if (archive_result != ArchiveResult::SUCCESS) {
            std::cerr << "Failed to archive block chunk=" << block.chunk_id
                     << " block_index=" << block.block_index
                     << ": " << archiver_->getLastError() << std::endl;
            stats_.blocks_failed++;
            continue;
        }

        // Get compression stats
        const ArchiveStats& archive_stats = archiver_->getStats();
        uint32_t original_size = raw_layout.block_size_bytes;
        uint32_t compressed_size = archive_stats.bytes_written;

        // Sync COMPACT block to metadata
        sync_result = metadata_sync_->syncCompactBlock(
            compact_container_id,
            compact_block_index,
            block.tag_id,
            block.chunk_id,
            block.block_index,
            block.start_ts_us,
            block.end_ts_us,
            block.record_count,
            EncodingType::ENC_RAW,  // TODO: Get from metadata
            block.value_type,
            block.time_unit,
            original_size,
            compressed_size
        );

        if (sync_result != SyncResult::SUCCESS) {
            std::cerr << "Failed to sync COMPACT block metadata: "
                     << metadata_sync_->getLastError() << std::endl;
            stats_.blocks_failed++;
            continue;
        }

        // Mark RAW block as archived
        sync_result = metadata_sync_->markBlockAsArchived(
            raw_container_id,
            block.chunk_id,
            block.block_index,
            compact_container_id,
            compact_block_index
        );

        if (sync_result != SyncResult::SUCCESS) {
            std::cerr << "Failed to mark RAW block as archived: "
                     << metadata_sync_->getLastError() << std::endl;
            stats_.blocks_failed++;
            continue;
        }

        // Update statistics
        stats_.blocks_archived++;
        stats_.total_bytes_read += original_size;
        stats_.total_bytes_written += compressed_size;

        compact_block_index++;

        std::cout << "Archived block " << stats_.blocks_archived << "/"
                 << blocks_to_archive.size()
                 << " - Compression: " << (1.0 - (double)compressed_size / original_size) * 100.0
                 << "%" << std::endl;
    }

    // Calculate average compression ratio
    if (stats_.blocks_archived > 0) {
        stats_.average_compression_ratio =
            (double)stats_.total_bytes_written / stats_.total_bytes_read;
    }

    // Note: We don't seal the container here - caller should seal when done archiving
    // This allows multiple archiveOldBlocks() calls before sealing

    std::cout << "Archive complete: " << stats_.blocks_archived << " blocks archived, "
             << stats_.blocks_failed << " failed" << std::endl;
    if (stats_.blocks_archived > 0) {
        std::cout << "Average compression: " << (1.0 - stats_.average_compression_ratio) * 100.0
                 << "%" << std::endl;
    }

    return ArchiveManagerResult::SUCCESS;
}

void CompactArchiveManager::resetStats() {
    stats_ = ArchiveManagerStats();
    archiver_->resetStats();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

void CompactArchiveManager::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
