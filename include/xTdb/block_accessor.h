#ifndef XTDB_BLOCK_ACCESSOR_H_
#define XTDB_BLOCK_ACCESSOR_H_

#include "xTdb/compact_container.h"
#include "xTdb/file_container.h"
#include "xTdb/metadata_sync.h"
#include <memory>
#include <string>
#include <vector>

namespace xtdb {

// ============================================================================
// BlockAccessor - Transparent block access (RAW or COMPACT)
// ============================================================================

/// Access result codes
enum class AccessResult {
    SUCCESS = 0,
    ERR_BLOCK_NOT_FOUND,
    ERR_METADATA_QUERY_FAILED,
    ERR_RAW_READ_FAILED,
    ERR_COMPACT_READ_FAILED,
    ERR_DECOMPRESSION_FAILED,
    ERR_INVALID_CONTAINER
};

/// Block data with metadata
struct BlockData {
    uint32_t container_id;
    uint32_t chunk_id;
    uint32_t block_index;
    uint32_t tag_id;
    int64_t start_ts_us;
    int64_t end_ts_us;
    TimeUnit time_unit;
    ValueType value_type;
    uint32_t record_count;
    EncodingType encoding_type;
    bool is_compressed;            // True if from COMPACT
    std::vector<uint8_t> data;     // Decompressed data
};

/// Access statistics
struct AccessStats {
    uint64_t raw_reads;
    uint64_t compact_reads;
    uint64_t total_bytes_read;
    uint64_t total_bytes_decompressed;

    AccessStats()
        : raw_reads(0),
          compact_reads(0),
          total_bytes_read(0),
          total_bytes_decompressed(0) {}
};

/// BlockAccessor - Provides transparent access to blocks in RAW or COMPACT storage
class BlockAccessor {
public:
    /// Constructor
    /// @param raw_container RAW file container
    /// @param compact_container COMPACT container
    /// @param metadata_sync Metadata sync instance
    BlockAccessor(FileContainer* raw_container,
                 CompactContainer* compact_container,
                 MetadataSync* metadata_sync);

    /// Destructor
    ~BlockAccessor();

    // Disable copy and move
    BlockAccessor(const BlockAccessor&) = delete;
    BlockAccessor& operator=(const BlockAccessor&) = delete;
    BlockAccessor(BlockAccessor&&) = delete;
    BlockAccessor& operator=(BlockAccessor&&) = delete;

    // ========================================================================
    // Read Operations
    // ========================================================================

    /// Read a block transparently (automatically detects RAW or COMPACT)
    /// @param raw_container_id RAW container ID
    /// @param chunk_id Chunk ID
    /// @param block_index Block index
    /// @param raw_layout RAW chunk layout
    /// @param block_data Output: block data
    /// @return AccessResult
    AccessResult readBlock(uint32_t raw_container_id,
                          uint32_t chunk_id,
                          uint32_t block_index,
                          const ChunkLayout& raw_layout,
                          BlockData& block_data);

    /// Query blocks by tag and time range (returns both RAW and COMPACT)
    /// @param tag_id Tag ID to query
    /// @param start_ts_us Start timestamp (inclusive)
    /// @param end_ts_us End timestamp (inclusive)
    /// @param raw_layout RAW chunk layout
    /// @param results Output: matching blocks
    /// @return AccessResult
    AccessResult queryBlocksByTagAndTime(uint32_t tag_id,
                                        int64_t start_ts_us,
                                        int64_t end_ts_us,
                                        const ChunkLayout& raw_layout,
                                        std::vector<BlockData>& results);

    /// Get access statistics
    /// @return Access statistics
    const AccessStats& getStats() const { return stats_; }

    /// Get last error message
    /// @return Error message
    std::string getLastError() const { return last_error_; }

    /// Reset statistics
    void resetStats();

private:
    /// Read from RAW container
    AccessResult readFromRaw(uint32_t chunk_id,
                            uint32_t block_index,
                            const ChunkLayout& raw_layout,
                            const BlockQueryResult& metadata,
                            BlockData& block_data);

    /// Read from COMPACT container
    AccessResult readFromCompact(uint32_t compact_block_index,
                                const BlockQueryResult& metadata,
                                BlockData& block_data);

    /// Set error message
    void setError(const std::string& message);

    FileContainer* raw_container_;              // RAW container (not owned)
    CompactContainer* compact_container_;       // COMPACT container (not owned)
    MetadataSync* metadata_sync_;               // Metadata sync (not owned)

    AccessStats stats_;                         // Statistics
    std::string last_error_;                    // Last error message
};

}  // namespace xtdb

#endif  // XTDB_BLOCK_ACCESSOR_H_
