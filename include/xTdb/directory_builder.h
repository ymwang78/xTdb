#ifndef XTDB_DIRECTORY_BUILDER_H_
#define XTDB_DIRECTORY_BUILDER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "layout_calculator.h"
#include <cstdint>
#include <vector>
#include <string>

namespace xtdb {

// ============================================================================
// Directory Builder - Manages BlockDirEntry array and block sealing
// ============================================================================

/// Directory build result
enum class DirBuildResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_INVALID_BLOCK,
    ERROR_BLOCK_SEALED,
    ERROR_CRC_FAILED
};

/// Directory builder for managing block metadata
class DirectoryBuilder {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param layout Chunk layout configuration
    /// @param chunk_offset Offset of the chunk in file
    DirectoryBuilder(AlignedIO* io, const ChunkLayout& layout, uint64_t chunk_offset);

    /// Destructor
    ~DirectoryBuilder();

    // Disable copy and move
    DirectoryBuilder(const DirectoryBuilder&) = delete;
    DirectoryBuilder& operator=(const DirectoryBuilder&) = delete;
    DirectoryBuilder(DirectoryBuilder&&) = delete;
    DirectoryBuilder& operator=(DirectoryBuilder&&) = delete;

    /// Initialize directory (allocate entries)
    /// @return DirBuildResult
    DirBuildResult initialize();

    /// Load existing directory from disk
    /// @return DirBuildResult
    DirBuildResult load();

    /// Seal a block (update BlockDirEntry with metadata)
    /// @param block_index Data block index (0-based)
    /// @param tag_id Tag ID
    /// @param start_ts_us Start timestamp (microseconds)
    /// @param end_ts_us End timestamp (microseconds)
    /// @param time_unit Time unit for records
    /// @param value_type Value type for records
    /// @param record_count Number of records in block
    /// @param data_crc32 CRC32 of block data
    /// @param encoding_type Encoding/compression type
    /// @param encoding_param1 Encoding parameter 1 (context-dependent)
    /// @param encoding_param2 Encoding parameter 2 (context-dependent)
    /// @return DirBuildResult
    DirBuildResult sealBlock(uint32_t block_index,
                            uint32_t tag_id,
                            int64_t start_ts_us,
                            int64_t end_ts_us,
                            TimeUnit time_unit,
                            ValueType value_type,
                            uint32_t record_count,
                            uint32_t data_crc32,
                            EncodingType encoding_type = EncodingType::ENC_RAW,
                            uint32_t encoding_param1 = 0,
                            uint32_t encoding_param2 = 0);

    /// Write directory to meta region
    /// @return DirBuildResult
    DirBuildResult writeDirectory();

    /// Get block directory entry (read-only)
    /// @param block_index Data block index
    /// @return Pointer to entry or nullptr if invalid
    const BlockDirEntryV16* getEntry(uint32_t block_index) const;

    /// Get number of sealed blocks
    uint32_t getSealedBlockCount() const { return sealed_block_count_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Set error message
    void setError(const std::string& message);

    AlignedIO* io_;                          // I/O interface (not owned)
    ChunkLayout layout_;                     // Chunk layout
    uint64_t chunk_offset_;                  // Chunk offset in file

    std::vector<BlockDirEntryV16> entries_;  // Block directory entries
    uint32_t sealed_block_count_;            // Number of sealed blocks

    std::string last_error_;                 // Last error message
};

}  // namespace xtdb

#endif  // XTDB_DIRECTORY_BUILDER_H_
