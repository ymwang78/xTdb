#ifndef XTDB_WAL_WRITER_H_
#define XTDB_WAL_WRITER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include <cstdint>
#include <string>

namespace xtdb {

// ============================================================================
// WAL (Write-Ahead Log) Writer
// ============================================================================

/// WAL entry: (tag_id, timestamp, value)
#pragma pack(push, 1)
struct WALEntry {
    uint32_t tag_id;
    int64_t  timestamp_us;
    uint8_t  value_type;     // ValueType
    uint8_t  quality;        // Quality byte
    uint16_t reserved;

    union {
        bool     bool_value;
        int32_t  i32_value;
        float    f32_value;
        double   f64_value;
    } value;

    WALEntry() : tag_id(0), timestamp_us(0), value_type(0), quality(0), reserved(0) {
        value.f64_value = 0.0;
    }
};
#pragma pack(pop)

static_assert(sizeof(WALEntry) == 24, "WALEntry must be 24 bytes");

/// WAL operation result
enum class WALResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_INVALID_ENTRY,
    ERROR_FULL
};

/// WAL statistics
struct WALStats {
    uint64_t entries_written = 0;
    uint64_t bytes_written = 0;
    uint64_t sync_operations = 0;
};

class WALWriter {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param wal_offset Starting offset for WAL in file
    /// @param wal_size_bytes Maximum WAL size in bytes (must be extent-aligned)
    WALWriter(AlignedIO* io, uint64_t wal_offset, uint64_t wal_size_bytes);

    /// Destructor
    ~WALWriter();

    // Disable copy and move
    WALWriter(const WALWriter&) = delete;
    WALWriter& operator=(const WALWriter&) = delete;
    WALWriter(WALWriter&&) = delete;
    WALWriter& operator=(WALWriter&&) = delete;

    /// Append entry to WAL
    /// @param entry Entry to append
    /// @return WALResult
    WALResult append(const WALEntry& entry);

    /// Sync WAL to disk (fsync)
    /// @return WALResult
    WALResult sync();

    /// Reset WAL (clear all entries)
    /// @return WALResult
    WALResult reset();

    /// Get current write position
    uint64_t getCurrentOffset() const { return current_offset_; }

    /// Get available space
    uint64_t getAvailableSpace() const {
        return wal_size_bytes_ - (current_offset_ - wal_start_offset_);
    }

    /// Check if WAL is full
    bool isFull() const {
        return getAvailableSpace() < sizeof(WALEntry);
    }

    /// Get statistics
    const WALStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Flush buffer to disk
    WALResult flush();

    /// Set error message
    void setError(const std::string& message);

    AlignedIO* io_;                  // I/O interface (not owned)
    uint64_t wal_start_offset_;      // WAL start offset in file
    uint64_t wal_size_bytes_;        // Maximum WAL size
    uint64_t current_offset_;        // Current write position

    AlignedBuffer buffer_;           // Write buffer (16KB aligned)
    uint32_t buffer_used_;           // Bytes used in buffer

    WALStats stats_;                 // Statistics
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_WAL_WRITER_H_
