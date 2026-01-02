#ifndef XTDB_RAW_SCANNER_H_
#define XTDB_RAW_SCANNER_H_

#include "struct_defs.h"
#include "aligned_io.h"
#include "layout_calculator.h"
#include <cstdint>
#include <vector>
#include <string>

namespace xtdb {

// ============================================================================
// Raw Scanner - Offline chunk scanning without SQLite dependency
// ============================================================================

/// Scan result
enum class ScanResult {
    SUCCESS = 0,
    ERROR_IO_FAILED,
    ERROR_INVALID_CHUNK,
    ERROR_NOT_SEALED,
    ERROR_CRC_MISMATCH
};

/// Scanned block metadata
struct ScannedBlock {
    uint32_t block_index;            // Data block index
    uint32_t tag_id;                 // Tag ID
    int64_t start_ts_us;             // Start timestamp
    int64_t end_ts_us;               // End timestamp
    TimeUnit time_unit;              // Time unit
    ValueType value_type;            // Value type
    uint32_t record_count;           // Number of records
    uint32_t data_crc32;             // Data CRC32
    bool is_sealed;                  // Whether block is sealed
};

/// Scanned chunk metadata
struct ScannedChunk {
    uint32_t chunk_id;               // Chunk ID
    int64_t start_ts_us;             // Chunk start timestamp
    int64_t end_ts_us;               // Chunk end timestamp
    uint32_t super_crc32;            // SuperCRC
    bool is_sealed;                  // Whether chunk is sealed
    std::vector<ScannedBlock> blocks; // All blocks in chunk
};

/// Raw scanner for chunk inspection
class RawScanner {
public:
    /// Constructor
    /// @param io AlignedIO instance (must be open)
    RawScanner(AlignedIO* io);

    /// Destructor
    ~RawScanner();

    // Disable copy and move
    RawScanner(const RawScanner&) = delete;
    RawScanner& operator=(const RawScanner&) = delete;
    RawScanner(RawScanner&&) = delete;
    RawScanner& operator=(RawScanner&&) = delete;

    /// Scan a chunk and extract all metadata
    /// @param chunk_offset Offset of the chunk in file
    /// @param layout Chunk layout
    /// @param chunk Output: scanned chunk metadata
    /// @return ScanResult
    ScanResult scanChunk(uint64_t chunk_offset,
                        const ChunkLayout& layout,
                        ScannedChunk& chunk);

    /// Verify chunk integrity (SuperCRC)
    /// @param chunk_offset Offset of the chunk in file
    /// @param layout Chunk layout
    /// @return ScanResult
    ScanResult verifyChunkIntegrity(uint64_t chunk_offset,
                                   const ChunkLayout& layout);

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Read chunk header
    /// @param chunk_offset Chunk offset
    /// @param header Output: chunk header
    /// @return ScanResult
    ScanResult readChunkHeader(uint64_t chunk_offset,
                              RawChunkHeaderV16& header);

    /// Read block directory
    /// @param chunk_offset Chunk offset
    /// @param layout Chunk layout
    /// @param entries Output: directory entries
    /// @return ScanResult
    ScanResult readBlockDirectory(uint64_t chunk_offset,
                                 const ChunkLayout& layout,
                                 std::vector<BlockDirEntryV16>& entries);

    /// Calculate SuperCRC for verification
    /// @param chunk_offset Chunk offset
    /// @param layout Chunk layout
    /// @param super_crc32 Output: calculated SuperCRC
    /// @return ScanResult
    ScanResult calculateSuperCRC(uint64_t chunk_offset,
                                const ChunkLayout& layout,
                                uint32_t& super_crc32);

    /// Set error message
    void setError(const std::string& message);

    /// Calculate CRC32 for a buffer
    uint32_t calculateCRC32(const void* data, uint64_t size);

    AlignedIO* io_;                  // I/O interface (not owned)
    std::string last_error_;         // Last error message
};

}  // namespace xtdb

#endif  // XTDB_RAW_SCANNER_H_
