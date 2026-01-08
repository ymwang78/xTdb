#ifndef XTDB_ROTATING_WAL_H_
#define XTDB_ROTATING_WAL_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "wal_writer.h"
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <set>

namespace xtdb {

// ============================================================================
// Rotating WAL - Multi-segment WAL with wear leveling
// ============================================================================

/// WAL segment information
struct WALSegment {
    uint32_t segment_id;           // Segment ID (0, 1, 2, ...)
    uint64_t start_offset;         // Start offset in file (bytes)
    uint64_t segment_size;         // Segment size in bytes (e.g., 64 MB)
    uint64_t write_position;       // Current write position within segment
    int64_t min_timestamp_us;      // Earliest timestamp in this segment
    int64_t max_timestamp_us;      // Latest timestamp in this segment
    uint64_t entry_count;          // Number of entries in this segment
    std::set<uint32_t> tag_ids;    // Tag IDs present in this segment

    WALSegment()
        : segment_id(0),
          start_offset(0),
          segment_size(0),
          write_position(0),
          min_timestamp_us(INT64_MAX),
          max_timestamp_us(INT64_MIN),
          entry_count(0) {}

    /// Check if segment is full
    bool isFull() const {
        return write_position >= segment_size;
    }

    /// Get available space in segment
    uint64_t getAvailableSpace() const {
        return segment_size > write_position ? (segment_size - write_position) : 0;
    }

    /// Reset segment (prepare for reuse)
    void reset() {
        write_position = 0;
        min_timestamp_us = INT64_MAX;
        max_timestamp_us = INT64_MIN;
        entry_count = 0;
        tag_ids.clear();
    }
};

/// Rotating WAL operation result
enum class RotatingWALResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_INVALID_ENTRY,
    ERROR_ALL_SEGMENTS_FULL,
    ERROR_CONTAINER_OPEN_FAILED,
    ERROR_SEGMENT_NOT_CLEARED,
    ERROR_CALLBACK_FAILED
};

/// Rotating WAL statistics
struct RotatingWALStats {
    uint64_t total_entries_written = 0;
    uint64_t total_bytes_written = 0;
    uint64_t segment_rotations = 0;
    uint64_t segment_flushes = 0;
    uint64_t sync_operations = 0;
};

/// Flush callback function type
/// Called when a segment is full and needs to be flushed
/// @param segment_id Segment ID that is full
/// @param tag_ids Set of tag IDs in the segment
/// @return true if flush succeeded, false otherwise
using SegmentFlushCallback = std::function<bool(uint32_t segment_id,
                                                 const std::set<uint32_t>& tag_ids)>;

/// Rotating WAL configuration
struct RotatingWALConfig {
    std::string wal_container_path;     // Path to WAL container file
    uint32_t num_segments;              // Number of segments (default: 4)
    uint64_t segment_size_bytes;        // Size per segment (default: 64 MB)
    bool auto_grow;                     // Allow automatic growth (default: false)
    uint32_t max_segments;              // Maximum segments if auto_grow (default: 8)
    bool direct_io;                     // Enable O_DIRECT (default: false)

    RotatingWALConfig()
        : wal_container_path("./data/wal_container.raw"),
          num_segments(4),
          segment_size_bytes(64 * 1024 * 1024),  // 64 MB
          auto_grow(false),
          max_segments(8),
          direct_io(false) {}
};

/// Rotating WAL - manages multiple segments with automatic rotation
class RotatingWAL {
public:
    /// Constructor
    /// @param config Rotating WAL configuration
    explicit RotatingWAL(const RotatingWALConfig& config);

    /// Destructor
    ~RotatingWAL();

    // Disable copy and move
    RotatingWAL(const RotatingWAL&) = delete;
    RotatingWAL& operator=(const RotatingWAL&) = delete;
    RotatingWAL(RotatingWAL&&) = delete;
    RotatingWAL& operator=(RotatingWAL&&) = delete;

    /// Open WAL container (create if not exists)
    /// @return RotatingWALResult
    RotatingWALResult open();

    /// Close WAL container
    void close();

    /// Check if WAL is open
    bool isOpen() const { return is_open_; }

    /// Append entry to WAL
    /// @param entry Entry to append
    /// @return RotatingWALResult
    RotatingWALResult append(const WALEntry& entry);

    /// Batch append entries to WAL (Phase 3)
    /// More efficient than individual append for multiple entries
    /// @param entries Vector of entries to append
    /// @return RotatingWALResult
    RotatingWALResult batchAppend(const std::vector<WALEntry>& entries);

    /// Sync WAL to disk (fsync)
    /// @return RotatingWALResult
    RotatingWALResult sync();

    /// Register flush callback
    /// Called when a segment is full and needs data flushed to disk
    /// @param callback Callback function
    void setFlushCallback(SegmentFlushCallback callback);

    /// Mark segment as cleared (after successful flush)
    /// This resets the segment and makes it available for reuse
    /// @param segment_id Segment ID to clear
    /// @return RotatingWALResult
    RotatingWALResult clearSegment(uint32_t segment_id);

    /// Get current segment ID
    uint32_t getCurrentSegmentId() const { return current_segment_id_; }

    /// Get segment information
    const WALSegment& getSegment(uint32_t segment_id) const;

    /// Get all segment information
    const std::vector<WALSegment>& getSegments() const { return segments_; }

    /// Get statistics
    const RotatingWALStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

    /// Get WAL usage ratio (0.0 - 1.0)
    /// Used for adaptive flush strategy
    double getUsageRatio() const;

private:
    /// Initialize WAL container (create header and segments)
    RotatingWALResult initializeContainer();

    /// Load WAL container (read header and segment info)
    RotatingWALResult loadContainer();

    /// Rotate to next segment
    /// Triggers flush callback for current segment
    /// @return RotatingWALResult
    RotatingWALResult rotateSegment();

    /// Grow WAL container (add new segment)
    /// Only works if auto_grow is enabled
    /// @return RotatingWALResult
    RotatingWALResult growContainer();

    /// Set error message
    void setError(const std::string& message);

    RotatingWALConfig config_;
    bool is_open_;
    std::string last_error_;

    // I/O
    std::unique_ptr<AlignedIO> io_;

    // Segments
    std::vector<WALSegment> segments_;
    uint32_t current_segment_id_;
    std::unique_ptr<WALWriter> current_writer_;

    // Callback
    SegmentFlushCallback flush_callback_;

    // Statistics
    RotatingWALStats stats_;
};

}  // namespace xtdb

#endif  // XTDB_ROTATING_WAL_H_
