#ifndef XTDB_STATE_MUTATOR_H_
#define XTDB_STATE_MUTATOR_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include <string>

namespace xtdb {

// ============================================================================
// StateMutator: SSD-friendly state machine for chunks and blocks
// ============================================================================

/// State mutation result
enum class MutateResult {
    SUCCESS = 0,
    ERR_IO_FAILED,
    ERR_INVALID_TRANSITION,  // Attempted 0->1 bit flip
    ERR_ALREADY_SET,         // State already in target state
    ERR_INVALID_OFFSET,
    ERR_READ_FAILED,
    ERR_WRITE_FAILED
};

class StateMutator {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    explicit StateMutator(AlignedIO* io);

    // ========================================================================
    // Chunk State Mutations (Active-Low: 1->0 only)
    // ========================================================================

    /// Allocate chunk (clear CHB_ALLOCATED bit)
    /// @param chunk_offset Physical byte offset of chunk header
    /// @return MutateResult
    MutateResult allocateChunk(uint64_t chunk_offset);

    /// Seal chunk (clear CHB_SEALED bit)
    /// @param chunk_offset Physical byte offset of chunk header
    /// @param start_ts_us Min timestamp in chunk
    /// @param end_ts_us Max timestamp in chunk
    /// @param super_crc32 CRC32 of meta region
    /// @return MutateResult
    MutateResult sealChunk(uint64_t chunk_offset,
                          int64_t start_ts_us,
                          int64_t end_ts_us,
                          uint32_t super_crc32);

    /// Deprecate chunk (clear CHB_DEPRECATED bit)
    /// @param chunk_offset Physical byte offset of chunk header
    /// @return MutateResult
    MutateResult deprecateChunk(uint64_t chunk_offset);

    /// Mark chunk as free (clear CHB_FREE_MARK bit)
    /// @param chunk_offset Physical byte offset of chunk header
    /// @return MutateResult
    MutateResult markChunkFree(uint64_t chunk_offset);

    // ========================================================================
    // Block State Mutations (Active-Low: 1->0 only)
    // ========================================================================

    /// Seal block (clear BLB_SEALED bit and write seal data)
    /// @param block_dir_entry_offset Physical byte offset of BlockDirEntry
    /// @param end_ts_us Max timestamp in block
    /// @param record_count Number of records in block
    /// @param data_crc32 CRC32 of data block
    /// @return MutateResult
    MutateResult sealBlock(uint64_t block_dir_entry_offset,
                          int64_t end_ts_us,
                          uint32_t record_count,
                          uint32_t data_crc32);

    /// Assert monotonic time property (clear BLB_MONOTONIC_TIME bit)
    /// @param block_dir_entry_offset Physical byte offset of BlockDirEntry
    /// @return MutateResult
    MutateResult assertMonotonicTime(uint64_t block_dir_entry_offset);

    /// Assert no time gaps property (clear BLB_NO_TIME_GAP bit)
    /// @param block_dir_entry_offset Physical byte offset of BlockDirEntry
    /// @return MutateResult
    MutateResult assertNoTimeGap(uint64_t block_dir_entry_offset);

    // ========================================================================
    // Chunk Header Initialization (Full Write)
    // ========================================================================

    /// Initialize new chunk header (full write, all bits = 1)
    /// @param chunk_offset Physical byte offset
    /// @param header Pre-filled header (flags should be kChunkFlagsInit)
    /// @return MutateResult
    MutateResult initChunkHeader(uint64_t chunk_offset,
                                const RawChunkHeaderV16& header);

    /// Initialize new block directory entry (full write, all bits = 1)
    /// @param block_dir_entry_offset Physical byte offset
    /// @param entry Pre-filled entry (flags should be kBlockFlagsInit)
    /// @return MutateResult
    MutateResult initBlockDirEntry(uint64_t block_dir_entry_offset,
                                  const BlockDirEntryV16& entry);

    // ========================================================================
    // Read Operations (for verification)
    // ========================================================================

    /// Read chunk header
    /// @param chunk_offset Physical byte offset
    /// @param header Output header
    /// @return MutateResult
    MutateResult readChunkHeader(uint64_t chunk_offset,
                                RawChunkHeaderV16& header);

    /// Read block directory entry
    /// @param block_dir_entry_offset Physical byte offset
    /// @param entry Output entry
    /// @return MutateResult
    MutateResult readBlockDirEntry(uint64_t block_dir_entry_offset,
                                  BlockDirEntryV16& entry);

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Validate that bit transition is 1->0 (not 0->1)
    /// @param old_value Old value
    /// @param new_value New value
    /// @return true if valid transition
    bool validateTransition(uint32_t old_value, uint32_t new_value) const;

    /// Set last error message
    void setError(const std::string& message);

    AlignedIO* io_;              // I/O interface (not owned)
    std::string last_error_;     // Last error message
};

}  // namespace xtdb

#endif  // XTDB_STATE_MUTATOR_H_
