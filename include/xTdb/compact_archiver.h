#ifndef XTDB_COMPACT_ARCHIVER_H_
#define XTDB_COMPACT_ARCHIVER_H_

#include "xTdb/compact_container.h"
#include "xTdb/file_container.h"
#include "xTdb/struct_defs.h"
#include <memory>
#include <string>

namespace xtdb {

// ============================================================================
// CompactArchiver - RAW to COMPACT archive converter
// ============================================================================

/// Archive result codes
enum class ArchiveResult {
    SUCCESS = 0,
    ERR_RAW_CONTAINER_NOT_OPEN,
    ERR_COMPACT_CONTAINER_NOT_OPEN,
    ERR_BLOCK_NOT_FOUND,
    ERR_READ_FAILED,
    ERR_WRITE_FAILED,
    ERR_COMPRESSION_FAILED,
    ERR_INVALID_PARAMETERS
};

/// Archive statistics
struct ArchiveStats {
    uint64_t blocks_archived;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t total_original_size;
    uint64_t total_compressed_size;
    double compression_ratio;

    ArchiveStats()
        : blocks_archived(0),
          bytes_read(0),
          bytes_written(0),
          total_original_size(0),
          total_compressed_size(0),
          compression_ratio(0.0) {}
};

/// CompactArchiver - Converts RAW blocks to COMPACT blocks
/// 1:1 mapping: one RAW block → one COMPACT block
class CompactArchiver {
public:
    /// Constructor
    CompactArchiver();

    /// Destructor
    ~CompactArchiver();

    // Disable copy and move
    CompactArchiver(const CompactArchiver&) = delete;
    CompactArchiver& operator=(const CompactArchiver&) = delete;
    CompactArchiver(CompactArchiver&&) = delete;
    CompactArchiver& operator=(CompactArchiver&&) = delete;

    // ========================================================================
    // Archive Operations
    // ========================================================================

    /// Archive a single RAW block to COMPACT container
    /// @param raw_container RAW container to read from
    /// @param raw_layout RAW chunk layout
    /// @param raw_chunk_id RAW chunk ID
    /// @param raw_block_index RAW block index
    /// @param compact_container COMPACT container to write to
    /// @param tag_id Tag ID for the block
    /// @param start_ts_us Block start timestamp
    /// @param end_ts_us Block end timestamp
    /// @param record_count Number of records in block
    /// @param original_encoding Original encoding type
    /// @param value_type Value type
    /// @param time_unit Time unit
    /// @return ArchiveResult
    ArchiveResult archiveBlock(FileContainer& raw_container,
                               const ChunkLayout& raw_layout,
                               uint32_t raw_chunk_id,
                               uint32_t raw_block_index,
                               CompactContainer& compact_container,
                               uint32_t tag_id,
                               int64_t start_ts_us,
                               int64_t end_ts_us,
                               uint32_t record_count,
                               EncodingType original_encoding,
                               ValueType value_type,
                               TimeUnit time_unit);

    /// Get archive statistics
    /// @return Archive statistics
    const ArchiveStats& getStats() const { return stats_; }

    /// Get last error message
    /// @return Error message
    std::string getLastError() const { return last_error_; }

    /// Reset statistics
    void resetStats();

private:
    /// Calculate block offset in RAW container
    /// @param chunk_id Chunk ID
    /// @param block_index Block index within chunk
    /// @param layout Chunk layout
    /// @return Block offset in bytes
    uint64_t calculateBlockOffset(uint32_t chunk_id,
                                  uint32_t block_index,
                                  const ChunkLayout& layout) const;

    /// Set last error message
    /// @param message Error message
    void setError(const std::string& message);

    ArchiveStats stats_;        // Archive statistics
    std::string last_error_;    // Last error message
};

}  // namespace xtdb

#endif  // XTDB_COMPACT_ARCHIVER_H_
