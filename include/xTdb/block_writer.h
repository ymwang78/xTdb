#ifndef XTDB_BLOCK_WRITER_H_
#define XTDB_BLOCK_WRITER_H_

#include "aligned_io.h"
#include "layout_calculator.h"
#include "mem_buffer.h"
#include <cstdint>
#include <string>

namespace xtdb {

// ============================================================================
// BlockWriter: High-throughput data block writer
// ============================================================================

/// Block write result
enum class BlockWriteResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_BUFFER_TOO_LARGE,
    ERROR_INVALID_LAYOUT,
    ERROR_INVALID_BLOCK_INDEX
};

/// Block write statistics
struct BlockWriteStats {
    uint64_t blocks_written = 0;
    uint64_t bytes_written = 0;
    uint64_t records_written = 0;
};

class BlockWriter {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param layout Chunk layout information
    /// @param container_base Base offset of container (after header)
    BlockWriter(AlignedIO* io,
                const ChunkLayout& layout,
                uint64_t container_base = kExtentSizeBytes);

    /// Write tag buffer to data block
    /// IMPORTANT: This writes to the data block ONLY.
    /// The BlockDirEntry is NOT updated (that's done in Seal phase).
    ///
    /// @param chunk_id Chunk ID
    /// @param data_block_index Data block index (0-based, relative to data region)
    /// @param tag_buffer Tag buffer with records
    /// @return BlockWriteResult
    BlockWriteResult writeBlock(uint32_t chunk_id,
                                uint32_t data_block_index,
                                const TagBuffer& tag_buffer);

    /// Calculate record size based on value type
    /// @param value_type Value type
    /// @return Record size in bytes (4 bytes header + value size)
    static uint16_t calculateRecordSize(ValueType value_type);

    /// Get statistics
    const BlockWriteStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Serialize records to buffer
    /// @param tag_buffer Tag buffer
    /// @param buffer Output buffer
    /// @param buffer_size Buffer size
    /// @return Number of bytes written
    uint64_t serializeRecords(const TagBuffer& tag_buffer,
                             void* buffer,
                             uint64_t buffer_size);

    /// Set error message
    void setError(const std::string& message);

    AlignedIO* io_;                  // I/O interface (not owned)
    ChunkLayout layout_;             // Chunk layout
    uint64_t container_base_;        // Container base offset

    BlockWriteStats stats_;          // Statistics
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_BLOCK_WRITER_H_
