#ifndef XTDB_CHUNK_SEALER_H_
#define XTDB_CHUNK_SEALER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "state_mutator.h"
#include "layout_calculator.h"
#include <cstdint>
#include <string>

namespace xtdb {

// ============================================================================
// Chunk Sealer - Seals chunks and calculates SuperCRC
// ============================================================================

/// Chunk seal result
enum class SealResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_INVALID_CHUNK,
    ERROR_ALREADY_SEALED,
    ERROR_CRC_FAILED
};

/// Chunk sealer for finalizing chunk metadata
class ChunkSealer {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    /// @param mutator StateMutator for state transitions
    ChunkSealer(AlignedIO* io, StateMutator* mutator);

    /// Destructor
    ~ChunkSealer();

    // Disable copy and move
    ChunkSealer(const ChunkSealer&) = delete;
    ChunkSealer& operator=(const ChunkSealer&) = delete;
    ChunkSealer(ChunkSealer&&) = delete;
    ChunkSealer& operator=(ChunkSealer&&) = delete;

    /// Seal a chunk (calculate SuperCRC, update timestamps, clear SEALED bit)
    /// @param chunk_offset Offset of the chunk in file
    /// @param layout Chunk layout
    /// @param start_ts_us Start timestamp of chunk (microseconds)
    /// @param end_ts_us End timestamp of chunk (microseconds)
    /// @return SealResult
    SealResult sealChunk(uint64_t chunk_offset,
                        const ChunkLayout& layout,
                        int64_t start_ts_us,
                        int64_t end_ts_us);

    /// Calculate SuperCRC for a chunk
    /// @param chunk_offset Offset of the chunk in file
    /// @param layout Chunk layout
    /// @param super_crc32 Output: calculated SuperCRC
    /// @return SealResult
    SealResult calculateSuperCRC(uint64_t chunk_offset,
                                const ChunkLayout& layout,
                                uint32_t& super_crc32);

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Set error message
    void setError(const std::string& message);

    /// Calculate CRC32 for a buffer
    /// @param data Buffer data
    /// @param size Buffer size
    /// @return CRC32 value
    uint32_t calculateCRC32(const void* data, uint64_t size);

    AlignedIO* io_;                  // I/O interface (not owned)
    StateMutator* mutator_;          // State mutator (not owned)

    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_CHUNK_SEALER_H_
