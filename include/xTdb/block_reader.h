#ifndef XTDB_BLOCK_READER_H_
#define XTDB_BLOCK_READER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "layout_calculator.h"
#include "mem_buffer.h"
#include "swinging_door_decoder.h"
#include "quantized_16_decoder.h"
#include <cstdint>
#include <vector>
#include <string>

namespace xtdb {

// ============================================================================
// Block Reader - Read and parse data blocks
// ============================================================================

/// Read result
enum class ReadResult {
    SUCCESS = 0,
    ERR_IO_FAILED,
    ERR_INVALID_BLOCK,
    ERR_CRC_MISMATCH,
    ERR_PARSE_FAILED
};

/// Read statistics
struct ReadStats {
    uint64_t blocks_read = 0;
    uint64_t bytes_read = 0;
    uint64_t records_read = 0;
};

/// Block reader for parsing data blocks
class BlockReader {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param layout Chunk layout configuration
    BlockReader(AlignedIO* io, const ChunkLayout& layout);

    /// Destructor
    ~BlockReader();

    // Disable copy and move
    BlockReader(const BlockReader&) = delete;
    BlockReader& operator=(const BlockReader&) = delete;
    BlockReader(BlockReader&&) = delete;
    BlockReader& operator=(BlockReader&&) = delete;

    /// Read a data block and parse records
    /// @param chunk_offset Offset of the chunk in file
    /// @param block_index Data block index (0-based)
    /// @param tag_id Expected tag ID
    /// @param start_ts_us Base timestamp (microseconds)
    /// @param time_unit Time unit for records
    /// @param value_type Value type for records
    /// @param record_count Expected number of records
    /// @param records Output: parsed records
    /// @return ReadResult
    ReadResult readBlock(uint64_t chunk_offset,
                        uint32_t block_index,
                        uint32_t tag_id,
                        int64_t start_ts_us,
                        TimeUnit time_unit,
                        ValueType value_type,
                        uint32_t record_count,
                        std::vector<MemRecord>& records);

    /// Read a data block with encoding support
    /// @param chunk_offset Offset of the chunk in file
    /// @param block_index Data block index (0-based)
    /// @param dir_entry Block directory entry with encoding metadata
    /// @param records Output: parsed records
    /// @return ReadResult
    ReadResult readBlock(uint64_t chunk_offset,
                        uint32_t block_index,
                        const BlockDirEntryV16& dir_entry,
                        std::vector<MemRecord>& records);

    /// Verify block data integrity (CRC32)
    /// @param chunk_offset Offset of the chunk in file
    /// @param block_index Data block index
    /// @param expected_crc32 Expected CRC32 value
    /// @return ReadResult
    ReadResult verifyBlockIntegrity(uint64_t chunk_offset,
                                   uint32_t block_index,
                                   uint32_t expected_crc32);

    /// Get statistics
    const ReadStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Parse records from raw buffer
    /// @param data Raw data buffer
    /// @param size Buffer size
    /// @param value_type Value type
    /// @param record_count Expected record count
    /// @param records Output: parsed records
    /// @return ReadResult
    ReadResult parseRecords(const void* data,
                           uint64_t size,
                           ValueType value_type,
                           uint32_t record_count,
                           std::vector<MemRecord>& records);

    /// Calculate record size based on value type
    /// @param value_type Value type
    /// @return Record size in bytes
    uint32_t getRecordSize(ValueType value_type) const;

    /// Calculate CRC32 for a buffer
    uint32_t calculateCRC32(const void* data, uint64_t size);

    /// Set error message
    void setError(const std::string& message);

    AlignedIO* io_;                  // I/O interface (not owned)
    ChunkLayout layout_;             // Chunk layout

    ReadStats stats_;                // Statistics
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_BLOCK_READER_H_
