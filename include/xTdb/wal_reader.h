#ifndef XTDB_WAL_READER_H_
#define XTDB_WAL_READER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "wal_writer.h"
#include <cstdint>
#include <string>

namespace xtdb {

// ============================================================================
// WAL (Write-Ahead Log) Reader
// ============================================================================

/// WAL reader statistics
struct WALReaderStats {
    uint64_t entries_read = 0;
    uint64_t bytes_read = 0;
    uint64_t corrupted_entries = 0;
};

class WALReader {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param wal_offset Starting offset for WAL in file
    /// @param wal_size_bytes Maximum WAL size in bytes (must be extent-aligned)
    WALReader(AlignedIO* io, uint64_t wal_offset, uint64_t wal_size_bytes);

    /// Destructor
    ~WALReader();

    // Disable copy and move
    WALReader(const WALReader&) = delete;
    WALReader& operator=(const WALReader&) = delete;
    WALReader(WALReader&&) = delete;
    WALReader& operator=(WALReader&&) = delete;

    /// Read next entry from WAL
    /// @param entry Output entry
    /// @return WALResult
    WALResult readNext(WALEntry& entry);

    /// Check if at end of WAL
    bool isEOF() const;

    /// Reset to beginning
    void reset();

    /// Get current read position
    uint64_t getCurrentOffset() const { return current_offset_; }

    /// Get statistics
    const WALReaderStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Load buffer from disk
    WALResult loadBuffer();

    /// Check if entry is valid (basic validation)
    bool isValidEntry(const WALEntry& entry) const;

    /// Set error message
    void setError(const std::string& message);

    AlignedIO* io_;                  // I/O interface (not owned)
    uint64_t wal_start_offset_;      // WAL start offset in file
    uint64_t wal_size_bytes_;        // Maximum WAL size
    uint64_t current_offset_;        // Current read position
    uint64_t buffer_file_offset_;    // File offset of current buffer

    AlignedBuffer buffer_;           // Read buffer (16KB aligned)
    uint32_t buffer_size_;           // Valid bytes in buffer
    uint32_t buffer_position_;       // Current position in buffer

    WALReaderStats stats_;           // Statistics
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_WAL_READER_H_
