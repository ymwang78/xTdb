#ifndef XTDB_METADATA_SYNC_H_
#define XTDB_METADATA_SYNC_H_

#include "struct_defs.h"
#include "raw_scanner.h"
#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <sqlite3.h>

namespace xtdb {

// ============================================================================
// Metadata Sync - SQLite integration for metadata indexing
// ============================================================================

/// Sync result
enum class SyncResult {
    SUCCESS = 0,
    ERR_DB_OPEN_FAILED,
    ERR_DB_EXEC_FAILED,
    ERR_DB_PREPARE_FAILED,
    ERR_INVALID_DATA
};

/// Query result for blocks
struct BlockQueryResult {
    uint32_t chunk_id;
    uint32_t block_index;
    uint32_t tag_id;
    int64_t start_ts_us;
    int64_t end_ts_us;
    TimeUnit time_unit;
    ValueType value_type;
    uint32_t record_count;
    uint64_t chunk_offset;  // Physical offset in file
};

/// Metadata synchronization with SQLite
class MetadataSync {
public:
    /// Constructor
    /// @param db_path Path to SQLite database file
    MetadataSync(const std::string& db_path);

    /// Destructor
    ~MetadataSync();

    // Disable copy and move
    MetadataSync(const MetadataSync&) = delete;
    MetadataSync& operator=(const MetadataSync&) = delete;
    MetadataSync(MetadataSync&&) = delete;
    MetadataSync& operator=(MetadataSync&&) = delete;

    /// Open database connection
    /// @return SyncResult
    SyncResult open();

    /// Close database connection
    void close();

    /// Initialize database schema (create tables)
    /// @return SyncResult
    SyncResult initSchema();

    /// Sync a chunk to database
    /// @param chunk_offset Physical offset of chunk in file
    /// @param scanned_chunk Scanned chunk metadata
    /// @return SyncResult
    SyncResult syncChunk(uint64_t chunk_offset,
                        const ScannedChunk& scanned_chunk);

    /// Query blocks by tag ID
    /// @param tag_id Tag ID to query
    /// @param results Output: matching blocks
    /// @return SyncResult
    SyncResult queryBlocksByTag(uint32_t tag_id,
                               std::vector<BlockQueryResult>& results);

    /// Query blocks by time range
    /// @param start_ts_us Start timestamp (inclusive)
    /// @param end_ts_us End timestamp (inclusive)
    /// @param results Output: matching blocks
    /// @return SyncResult
    SyncResult queryBlocksByTimeRange(int64_t start_ts_us,
                                     int64_t end_ts_us,
                                     std::vector<BlockQueryResult>& results);

    /// Query blocks by tag and time range
    /// @param tag_id Tag ID to query
    /// @param start_ts_us Start timestamp (inclusive)
    /// @param end_ts_us End timestamp (inclusive)
    /// @param results Output: matching blocks
    /// @return SyncResult
    SyncResult queryBlocksByTagAndTime(uint32_t tag_id,
                                      int64_t start_ts_us,
                                      int64_t end_ts_us,
                                      std::vector<BlockQueryResult>& results);

    /// Get all distinct tag IDs
    /// @param tag_ids Output: all tag IDs
    /// @return SyncResult
    SyncResult getAllTags(std::vector<uint32_t>& tag_ids);

    // ========================================================================
    // Phase 10: Retention Service Support
    // ========================================================================

    /// Query sealed chunks within time range
    /// @param container_id Container ID
    /// @param min_end_ts Minimum end timestamp (0 = no minimum)
    /// @param max_end_ts Maximum end timestamp (cutoff for retention)
    /// @param callback Callback function called for each matching chunk
    /// @return SyncResult
    SyncResult querySealedChunks(uint32_t container_id,
                                int64_t min_end_ts,
                                int64_t max_end_ts,
                                std::function<void(uint32_t chunk_id,
                                                  uint64_t chunk_offset,
                                                  int64_t start_ts,
                                                  int64_t end_ts)> callback);

    /// Delete chunk metadata from database
    /// @param container_id Container ID
    /// @param chunk_id Chunk ID
    /// @return SyncResult
    SyncResult deleteChunk(uint32_t container_id, uint32_t chunk_id);

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Execute SQL statement
    /// @param sql SQL statement
    /// @return SyncResult
    SyncResult executeSql(const std::string& sql);

    /// Set error message
    void setError(const std::string& message);

    std::string db_path_;            // Database file path
    sqlite3* db_;                    // SQLite database handle
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_METADATA_SYNC_H_
