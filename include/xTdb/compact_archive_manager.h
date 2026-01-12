#ifndef XTDB_COMPACT_ARCHIVE_MANAGER_H_
#define XTDB_COMPACT_ARCHIVE_MANAGER_H_

#include "xTdb/compact_archiver.h"
#include "xTdb/compact_container.h"
#include "xTdb/file_container.h"
#include "xTdb/metadata_sync.h"
#include <memory>
#include <string>

namespace xtdb {

// ============================================================================
// CompactArchiveManager - Orchestrates RAW to COMPACT archiving with metadata
// ============================================================================

/// Archive manager result codes
enum class ArchiveManagerResult {
    SUCCESS = 0,
    ERR_METADATA_SYNC_FAILED,
    ERR_RAW_CONTAINER_FAILED,
    ERR_COMPACT_CONTAINER_FAILED,
    ERR_ARCHIVE_FAILED,
    ERR_NO_BLOCKS_TO_ARCHIVE
};

/// Archive statistics
struct ArchiveManagerStats {
    uint64_t blocks_found;
    uint64_t blocks_archived;
    uint64_t blocks_failed;
    uint64_t total_bytes_read;
    uint64_t total_bytes_written;
    double average_compression_ratio;

    ArchiveManagerStats()
        : blocks_found(0),
          blocks_archived(0),
          blocks_failed(0),
          total_bytes_read(0),
          total_bytes_written(0),
          average_compression_ratio(0.0) {}
};

/// CompactArchiveManager - Integrates CompactArchiver with MetadataSync
class CompactArchiveManager {
public:
    /// Constructor
    /// @param raw_container RAW container
    /// @param compact_container COMPACT container
    /// @param metadata_sync Metadata sync instance
    CompactArchiveManager(FileContainer* raw_container,
                         CompactContainer* compact_container,
                         MetadataSync* metadata_sync);

    /// Destructor
    ~CompactArchiveManager();

    // Disable copy and move
    CompactArchiveManager(const CompactArchiveManager&) = delete;
    CompactArchiveManager& operator=(const CompactArchiveManager&) = delete;
    CompactArchiveManager(CompactArchiveManager&&) = delete;
    CompactArchiveManager& operator=(CompactArchiveManager&&) = delete;

    // ========================================================================
    // Archive Operations
    // ========================================================================

    /// Archive old blocks from RAW to COMPACT
    /// @param raw_container_id RAW container ID
    /// @param compact_container_id COMPACT container ID
    /// @param min_age_seconds Minimum age of blocks to archive (seconds)
    /// @param raw_layout RAW chunk layout
    /// @return ArchiveManagerResult
    ArchiveManagerResult archiveOldBlocks(uint32_t raw_container_id,
                                         uint32_t compact_container_id,
                                         int64_t min_age_seconds,
                                         const ChunkLayout& raw_layout);

    /// Get archive statistics
    /// @return Archive statistics
    const ArchiveManagerStats& getStats() const { return stats_; }

    /// Get last error message
    /// @return Error message
    std::string getLastError() const { return last_error_; }

    /// Reset statistics
    void resetStats();

private:
    /// Set error message
    void setError(const std::string& message);

    FileContainer* raw_container_;              // RAW container (not owned)
    CompactContainer* compact_container_;       // COMPACT container (not owned)
    MetadataSync* metadata_sync_;               // Metadata sync (not owned)

    std::unique_ptr<CompactArchiver> archiver_; // Archive engine
    ArchiveManagerStats stats_;                 // Statistics
    std::string last_error_;                    // Last error message
};

}  // namespace xtdb

#endif  // XTDB_COMPACT_ARCHIVE_MANAGER_H_
