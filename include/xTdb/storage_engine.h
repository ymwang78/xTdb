#ifndef XTDB_STORAGE_ENGINE_H_
#define XTDB_STORAGE_ENGINE_H_

#include "aligned_io.h"
#include "metadata_sync.h"
#include "mem_buffer.h"
#include "block_writer.h"
#include "directory_builder.h"
#include "chunk_sealer.h"
#include "raw_scanner.h"
#include "block_reader.h"
#include "state_mutator.h"
#include "rotating_wal.h"
#include "wal_reader.h"
#include "thread_pool.h"
#include "struct_defs.h"
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>

namespace xtdb {

// ============================================================================
// Storage Engine - Global entry point for xTdb
// ============================================================================

/// Engine result codes
enum class EngineResult {
    SUCCESS = 0,
    ERROR_INVALID_PATH,
    ERROR_CONTAINER_OPEN_FAILED,
    ERROR_CONTAINER_HEADER_INVALID,
    ERROR_METADATA_OPEN_FAILED,
    ERROR_WAL_OPEN_FAILED,
    ERROR_CHUNK_ALLOCATION_FAILED,
    ERROR_STATE_RESTORATION_FAILED,
    ERROR_WAL_REPLAY_FAILED,
    ERROR_ENGINE_NOT_OPEN,
    ERROR_INVALID_DATA
};

/// Container information
struct ContainerInfo {
    uint32_t container_id;
    std::string file_path;
    uint64_t capacity_bytes;
    ChunkLayout layout;
};

/// Active chunk tracking
struct ActiveChunkInfo {
    uint32_t chunk_id;
    uint64_t chunk_offset;
    uint32_t blocks_used;    // Number of blocks currently written
    uint32_t blocks_total;   // Total data blocks available
    int64_t start_ts_us;
    int64_t end_ts_us;
};

/// Storage Engine configuration
struct EngineConfig {
    std::string data_dir;
    std::string db_path;
    ChunkLayout layout;
    int64_t retention_days;  // Data retention in days (0 = no retention limit)

    EngineConfig()
        : data_dir("./data"),
          db_path("./data/meta.db"),
          retention_days(0) {  // No retention limit by default
        // Default layout: 16KB blocks, 256MB chunk
        layout.block_size_bytes = 16384;
        layout.chunk_size_bytes = 256 * 1024 * 1024;
        layout.meta_blocks = 0;  // Will be calculated
        layout.data_blocks = 0;  // Will be calculated
    }
};

/// Storage Engine - manages all global state and coordinates operations
class StorageEngine {
public:
    /// Constructor
    explicit StorageEngine(const EngineConfig& config);

    /// Destructor
    ~StorageEngine();

    // Disable copy and move
    StorageEngine(const StorageEngine&) = delete;
    StorageEngine& operator=(const StorageEngine&) = delete;
    StorageEngine(StorageEngine&&) = delete;
    StorageEngine& operator=(StorageEngine&&) = delete;

    /// Open storage engine and perform bootstrap sequence
    /// @return EngineResult
    EngineResult open();

    /// Close storage engine
    void close();

    /// Check if engine is open
    bool isOpen() const { return is_open_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

    /// Get container information
    const std::vector<ContainerInfo>& getContainers() const { return containers_; }

    /// Get active chunk info (for testing/monitoring)
    const ActiveChunkInfo& getActiveChunk() const { return active_chunk_; }

    /// Get metadata sync (for testing)
    MetadataSync* getMetadataSync() { return metadata_.get(); }

    /// Write a single data point
    /// @param tag_id Tag ID
    /// @param timestamp_us Timestamp in microseconds
    /// @param value Value
    /// @param quality Quality byte
    /// @return EngineResult
    EngineResult writePoint(uint32_t tag_id,
                           int64_t timestamp_us,
                           double value,
                           uint8_t quality = 192);

    /// Flush buffers to disk
    /// @return EngineResult
    EngineResult flush();

    /// Get write statistics
    struct WriteStats {
        uint64_t points_written = 0;
        uint64_t blocks_flushed = 0;
        uint64_t chunks_sealed = 0;
        uint64_t chunks_allocated = 0;
    };
    const WriteStats& getWriteStats() const { return write_stats_; }

    /// Query result point
    struct QueryPoint {
        int64_t timestamp_us;
        double value;
        uint8_t quality;

        QueryPoint(int64_t ts, double val, uint8_t q)
            : timestamp_us(ts), value(val), quality(q) {}
    };

    /// Query data points by tag and time range
    /// @param tag_id Tag ID
    /// @param start_ts_us Start timestamp (microseconds, inclusive)
    /// @param end_ts_us End timestamp (microseconds, inclusive)
    /// @param results Output vector of query points
    /// @return EngineResult
    EngineResult queryPoints(uint32_t tag_id,
                            int64_t start_ts_us,
                            int64_t end_ts_us,
                            std::vector<QueryPoint>& results);

    /// Get read statistics
    struct ReadStats {
        uint64_t queries_executed = 0;
        uint64_t blocks_read = 0;
        uint64_t points_read_disk = 0;
        uint64_t points_read_memory = 0;
    };
    const ReadStats& getReadStats() const { return read_stats_; }

    // ========================================================================
    // Phase 10: Maintenance Services
    // ========================================================================

    /// Maintenance statistics
    struct MaintenanceStats {
        uint64_t chunks_deprecated = 0;  // Chunks marked as deprecated
        uint64_t chunks_freed = 0;        // Chunks marked as free
        uint64_t last_retention_run_ts = 0;  // Last retention service run timestamp
    };
    const MaintenanceStats& getMaintenanceStats() const { return maintenance_stats_; }

    /// Run retention service to deprecate old chunks
    /// Finds sealed chunks with end_ts < (now - retention_days) and marks them as deprecated
    /// @param current_time_us Current time in microseconds (for testing)
    /// @return EngineResult
    EngineResult runRetentionService(int64_t current_time_us = 0);

    /// Reclaim space from deprecated chunks
    /// Marks deprecated chunks as FREE (ready for reuse)
    /// @return EngineResult
    EngineResult reclaimDeprecatedChunks();

    /// Seal current active chunk
    /// Used for testing and graceful shutdown scenarios
    /// @return EngineResult
    EngineResult sealCurrentChunk();

private:
    /// Bootstrap step 1: Connect to SQLite
    EngineResult connectMetadata();

    /// Bootstrap step 2: Mount container files
    EngineResult mountContainers();

    /// Bootstrap step 3: Restore active state
    EngineResult restoreActiveState();

    /// Bootstrap step 4: Replay WAL
    EngineResult replayWAL();

    /// Verify container header
    /// @param container_path Path to container file
    /// @param header Output: container header
    /// @return EngineResult
    EngineResult verifyContainerHeader(const std::string& container_path,
                                      ContainerHeaderV12& header);

    /// Allocate new chunk (initialize header and directory)
    /// @param chunk_offset Chunk offset in container
    /// @return EngineResult
    EngineResult allocateNewChunk(uint64_t chunk_offset);

    /// Set error message
    void setError(const std::string& message);

    /// Handle WAL segment full callback
    /// @param segment_id Segment ID that is full
    /// @param tag_ids Tag IDs in the segment that need flushing
    /// @return true if flush succeeded, false otherwise
    bool handleSegmentFull(uint32_t segment_id, const std::set<uint32_t>& tag_ids);

    /// Flush single tag buffer to disk
    /// @param tag_id Tag ID to flush
    /// @param tag_buffer Tag buffer to flush
    /// @return EngineResult
    EngineResult flushSingleTag(uint32_t tag_id, TagBuffer& tag_buffer);

    /// Flush WAL batch for a tag (Phase 3)
    /// @param tag_id Tag ID to flush WAL batch
    /// @return EngineResult
    EngineResult flushWALBatch(uint32_t tag_id);

    EngineConfig config_;
    bool is_open_;
    std::string last_error_;

    // Core components
    std::unique_ptr<AlignedIO> io_;              // File I/O (main thread)
    std::unique_ptr<MetadataSync> metadata_;     // SQLite connection
    std::unique_ptr<StateMutator> mutator_;      // State machine
    std::unique_ptr<DirectoryBuilder> dir_builder_;  // Active chunk directory
    std::unique_ptr<RotatingWAL> rotating_wal_;  // Rotating WAL system
    std::unique_ptr<WALReader> wal_reader_;      // WAL reader

    // Parallel execution infrastructure (Phase 2)
    std::unique_ptr<ThreadPool> flush_pool_;     // Thread pool for parallel flush
    std::vector<std::unique_ptr<AlignedIO>> io_pool_;  // Per-thread I/O instances
    mutable std::shared_mutex buffers_mutex_;    // Reader-writer lock for buffers_
    std::mutex active_chunk_mutex_;              // Protect active_chunk_ updates
    std::atomic<size_t> next_io_index_;          // Round-robin I/O allocation

    // WAL batching infrastructure (Phase 3)
    std::unordered_map<uint32_t, std::vector<WALEntry>> wal_batches_;  // Per-tag WAL batch
    std::mutex wal_batch_mutex_;                 // Protect wal_batches_
    static constexpr size_t kWALBatchSize = 100; // Batch size threshold

    // Runtime state
    std::vector<ContainerInfo> containers_;      // All mounted containers
    ActiveChunkInfo active_chunk_;               // Current active chunk (protected by mutex)
    std::unordered_map<uint32_t, TagBuffer> buffers_;  // Tag -> MemBuffer (protected by mutex)
    WriteStats write_stats_;                     // Write statistics
    ReadStats read_stats_;                       // Read statistics
    MaintenanceStats maintenance_stats_;         // Maintenance statistics
    uint32_t wal_entries_since_sync_;            // Entries written since last WAL sync
};

}  // namespace xtdb

#endif  // XTDB_STORAGE_ENGINE_H_
