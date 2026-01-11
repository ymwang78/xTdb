#ifndef XTDB_STRUCT_DEFS_H_
#define XTDB_STRUCT_DEFS_H_

#include "constants.h"
#include <cstdint>
#include <cstring>

namespace xtdb {

// ============================================================================
// Magic Numbers (no null terminator, exactly 8 bytes)
// ============================================================================

constexpr char kContainerMagic[8] = {'X','T','S','D','B','C','O','N'};
constexpr char kRawChunkMagic[8] = {'X','T','S','R','A','W','C','K'};

// ============================================================================
// Container Enums
// ============================================================================

enum class ContainerLayout : uint8_t {
    LAYOUT_RAW_FIXED   = 1,
    LAYOUT_COMPACT_VAR = 2
};

enum class CapacityType : uint8_t {
    CAP_DYNAMIC = 1,
    CAP_FIXED   = 2
};

// ============================================================================
// Archive Level (for multi-resolution storage)
// ============================================================================

enum class ArchiveLevel : uint8_t {
    ARCHIVE_RAW          = 0,  // Raw high-frequency data
    ARCHIVE_RESAMPLED_1M = 1,  // 1-minute resampled data
    ARCHIVE_RESAMPLED_1H = 2,  // 1-hour resampled data
    ARCHIVE_AGGREGATED   = 3,  // Aggregated statistical data
    ARCHIVE_RESERVED_4   = 4,  // Reserved for future use
    ARCHIVE_RESERVED_5   = 5   // Reserved for future use
};

// ============================================================================
// ContainerHeaderV12 (16KB fixed)
// ============================================================================

#pragma pack(push, 1)

struct ContainerHeaderV12 {
    char     magic[8];               // "XTSDBCON"
    uint16_t version;                // = 0x0102
    uint16_t header_size;            // = EXTENT_SIZE_BYTES (16384)

    uint8_t  db_instance_id[16];     // Must match db_meta['instance_id']

    uint8_t  layout;                 // ContainerLayout: RAW_FIXED / COMPACT_VAR
    uint8_t  capacity_type;          // CapacityType: DYNAMIC / FIXED
    uint8_t  archive_level;          // ArchiveLevel: RAW / RESAMPLED / AGGREGATED
    uint8_t  reserved_u8;            // Reserved for alignment
    uint16_t flags;

    uint64_t capacity_extents;       // CAP_FIXED: required; CAP_DYNAMIC: can be 0
    int64_t  created_ts_us;          // PostgreSQL epoch microseconds

    // RAW fixed parameters (only meaningful when layout=RAW_FIXED)
    uint8_t  raw_block_class;        // RawBlockClass (1=16K, 2=64K, 3=256K)
    uint8_t  reserved8[7];

    uint32_t chunk_size_extents;     // RAW: fixed chunk slot size (in extents)
    uint32_t block_size_extents;     // RAW: fixed block size (in extents)

    // Archive parameters (for multi-resolution storage)
    uint32_t resampling_interval_us; // Resampling interval in microseconds (0=no resampling)
    uint32_t reserved_archive_1;     // Reserved for future archive features

    uint32_t header_crc32;           // CRC32(header without this field)
    uint32_t reserved0;

    // Padding to fill 16KB
    uint8_t  payload[kExtentSizeBytes - 8 - 2 - 2 - 16 - 1 - 1 - 1 - 1 - 2 - 8 - 8 - 1 - 7 - 4 - 4 - 4 - 4 - 4 - 4];

    // Constructor: initialize to safe defaults
    ContainerHeaderV12() {
        std::memset(this, 0, sizeof(*this));
        std::memcpy(magic, kContainerMagic, 8);
        version = 0x0102;
        header_size = kExtentSizeBytes;
    }
};

#pragma pack(pop)

// Verify struct size
static_assert(sizeof(ContainerHeaderV12) == kExtentSizeBytes,
              "ContainerHeaderV12 must be exactly 16KB");

// ============================================================================
// Chunk State Bits (Active-Low / Write-Once)
// ============================================================================

enum class ChunkStateBit : uint8_t {
    CHB_DEPRECATED = 0,  // 1=not deprecated, 0=deprecated
    CHB_SEALED     = 1,  // 1=not sealed, 0=sealed
    CHB_ALLOCATED  = 2,  // 1=not allocated (FREE candidate), 0=allocated
    CHB_FREE_MARK  = 3   // 1=not marked FREE, 0=marked FREE (for tools)
};

// Initial flags value (all bits set to 1)
constexpr uint32_t kChunkFlagsInit = 0xFFFFFFFFu;

// Helper: Clear specific bit (1->0 transition only)
inline uint32_t chunkClearBit(uint32_t flags, ChunkStateBit bit) {
    return flags & ~(1u << static_cast<uint32_t>(bit));
}

// Helper: Check if bit is set (returns true if bit=1)
inline bool chunkIsBitSet(uint32_t flags, ChunkStateBit bit) {
    return (flags & (1u << static_cast<uint32_t>(bit))) != 0;
}

// State predicates (based on active-low encoding)
inline bool chunkIsAllocated(uint32_t flags) {
    return !chunkIsBitSet(flags, ChunkStateBit::CHB_ALLOCATED);  // bit=0 means allocated
}

inline bool chunkIsSealed(uint32_t flags) {
    return !chunkIsBitSet(flags, ChunkStateBit::CHB_SEALED);  // bit=0 means sealed
}

inline bool chunkIsDeprecated(uint32_t flags) {
    return !chunkIsBitSet(flags, ChunkStateBit::CHB_DEPRECATED);  // bit=0 means deprecated
}

inline bool chunkIsFree(uint32_t flags) {
    // FREE means all critical bits are still 1 (not allocated)
    return chunkIsBitSet(flags, ChunkStateBit::CHB_ALLOCATED);
}

// ============================================================================
// RawChunkHeaderV16
// ============================================================================

#pragma pack(push, 1)

struct RawChunkHeaderV16 {
    char     magic[8];               // "XTSRAWCK"
    uint16_t version;                // = 0x0106
    uint16_t header_size;            // sizeof(RawChunkHeaderV16)

    uint8_t  db_instance_id[16];

    uint32_t chunk_id;               // Slot ID
    uint32_t flags;                  // Active-low state bits (init: kChunkFlagsInit)

    uint32_t chunk_size_extents;     // Redundant (for offline scan validation)
    uint32_t block_size_extents;     // Redundant (for offline scan validation)

    uint32_t meta_blocks;            // Number of meta blocks (>=1)
    uint32_t data_blocks;            // Number of data blocks

    int64_t  start_ts_us;            // Sealed: min timestamp in chunk
    int64_t  end_ts_us;              // Sealed: max timestamp in chunk

    uint32_t super_crc32;            // CRC32(meta region), sealed
    uint32_t reserved0;

    // Padding to reach 128 bytes
    uint8_t  padding[128 - 8 - 2 - 2 - 16 - 4 - 4 - 4 - 4 - 4 - 4 - 8 - 8 - 4 - 4];

    // Constructor: initialize to safe defaults
    RawChunkHeaderV16() {
        std::memset(this, 0, sizeof(*this));
        std::memcpy(magic, kRawChunkMagic, 8);
        version = 0x0106;
        header_size = sizeof(RawChunkHeaderV16);
        flags = kChunkFlagsInit;  // All bits = 1
        start_ts_us = 0x7FFFFFFFFFFFFFFFll;  // Max int64
        end_ts_us = 0x7FFFFFFFFFFFFFFFll;
        super_crc32 = 0xFFFFFFFFu;
    }
};

#pragma pack(pop)

// Verify struct size (should be 128 bytes as per design)
static_assert(sizeof(RawChunkHeaderV16) == 128,
              "RawChunkHeaderV16 must be exactly 128 bytes");

// ============================================================================
// Block State Bits (Active-Low / Write-Once)
// ============================================================================

enum class BlockStateBit : uint8_t {
    BLB_SEALED         = 0,  // 1=not sealed, 0=sealed
    BLB_MONOTONIC_TIME = 1,  // 1=not asserted, 0=asserted monotonic time
    BLB_NO_TIME_GAP    = 2   // 1=not asserted, 0=asserted no time gaps
};

// Initial block flags value
constexpr uint32_t kBlockFlagsInit = 0xFFFFFFFFu;

// Helper: Clear specific bit (1->0 transition only)
inline uint32_t blockClearBit(uint32_t flags, BlockStateBit bit) {
    return flags & ~(1u << static_cast<uint32_t>(bit));
}

// Helper: Check if bit is set
inline bool blockIsBitSet(uint32_t flags, BlockStateBit bit) {
    return (flags & (1u << static_cast<uint32_t>(bit))) != 0;
}

// State predicates
inline bool blockIsSealed(uint32_t flags) {
    return !blockIsBitSet(flags, BlockStateBit::BLB_SEALED);
}

// ============================================================================
// Value Type & Time Unit Enums
// ============================================================================

enum class ValueType : uint8_t {
    VT_BOOL  = 1,
    VT_I32   = 2,
    VT_F32   = 3,
    VT_F64   = 4
};

enum class TimeUnit : uint8_t {
    TU_100MS = 1,
    TU_10MS  = 2,
    TU_MS    = 3,
    TU_100US = 4,
    TU_10US  = 5,
    TU_US    = 6
};

// ============================================================================
// Encoding Type (for compression and quantization)
// ============================================================================

enum class EncodingType : uint8_t {
    ENC_RAW              = 0,  // No compression, direct storage
    ENC_SWINGING_DOOR    = 1,  // Swinging Door / Constrained Slope algorithm
    ENC_QUANTIZED_16     = 2,  // 16-bit quantization (for fixed-range values)
    ENC_GORILLA          = 3,  // Gorilla / XOR compression
    ENC_DELTA_OF_DELTA   = 4,  // Delta-of-Delta compression
    ENC_RESERVED_5       = 5,  // Reserved for future use
    ENC_RESERVED_6       = 6,  // Reserved for future use
    ENC_RESERVED_7       = 7   // Reserved for future use
};

// ============================================================================
// CompressionType - General-purpose compression for COMPACT archive
// ============================================================================

enum class CompressionType : uint8_t {
    COMP_NONE  = 0,  // No compression
    COMP_ZSTD  = 1,  // Zstandard compression (recommended)
    COMP_LZ4   = 2,  // LZ4 compression (fast)
    COMP_ZLIB  = 3,  // Zlib/Gzip compression (compatible)
    COMP_RESERVED_4 = 4,  // Reserved for future use
    COMP_RESERVED_5 = 5,  // Reserved for future use
    COMP_RESERVED_6 = 6,  // Reserved for future use
    COMP_RESERVED_7 = 7   // Reserved for future use
};

// ============================================================================
// COMPACT Archive Structures
// ============================================================================

// COMPACT Chunk Magic Number
constexpr char kCompactChunkMagic[8] = {'X', 'T', 'C', 'P', 'T', 'C', 'K', '\0'};  // "XTCPTCK"

// COMPACT Chunk Header Version
constexpr uint16_t kCompactChunkVersion = 0x0001;  // Version 1.0

#pragma pack(push, 1)

/// Compact Chunk Header (128 bytes, same size as RawChunkHeaderV16 for alignment)
/// Located at the beginning of each COMPACT chunk
struct CompactChunkHeader {
    char     magic[8];               // "XTCPTCK" - COMPACT chunk identifier
    uint16_t version;                // = 0x0001 (Version 1.0)
    uint16_t header_size;            // sizeof(CompactChunkHeader) = 128

    uint8_t  db_instance_id[16];     // Database instance ID (for validation)

    uint32_t chunk_id;               // Chunk ID (corresponds to RAW chunk_id)
    uint32_t flags;                  // Chunk state flags (same as RawChunkHeaderV16)

    uint32_t block_count;            // Number of compressed blocks in this chunk
    uint8_t  compression_type;       // CompressionType used for all blocks
    uint8_t  reserved_u8[3];         // Reserved for alignment

    uint64_t index_offset;           // Offset to block index array (from chunk start)
    uint64_t data_offset;            // Offset to compressed data region (from chunk start)

    uint64_t total_compressed_size;  // Total size of all compressed blocks (bytes)
    uint64_t total_original_size;    // Total size before compression (bytes)

    int64_t  start_ts_us;            // Minimum timestamp in chunk
    int64_t  end_ts_us;              // Maximum timestamp in chunk

    uint32_t super_crc32;            // CRC32 of index region (for integrity)
    uint32_t reserved1;

    // Padding to reach 128 bytes
    uint8_t  padding[128 - 8 - 2 - 2 - 16 - 4 - 4 - 4 - 4 - 8 - 8 - 8 - 8 - 8 - 8 - 4 - 4];

    // Constructor: initialize to safe defaults
    CompactChunkHeader() {
        std::memcpy(magic, kCompactChunkMagic, 8);
        version = kCompactChunkVersion;
        header_size = sizeof(CompactChunkHeader);
        std::memset(db_instance_id, 0, 16);
        chunk_id = 0;
        flags = kChunkFlagsInit;
        block_count = 0;
        compression_type = static_cast<uint8_t>(CompressionType::COMP_ZSTD);
        std::memset(reserved_u8, 0, 3);
        index_offset = 0;
        data_offset = 0;
        total_compressed_size = 0;
        total_original_size = 0;
        start_ts_us = 0;
        end_ts_us = 0;
        super_crc32 = 0;
        reserved1 = 0;
        std::memset(padding, 0, sizeof(padding));
    }
};

// Verify struct size
static_assert(sizeof(CompactChunkHeader) == 128,
              "CompactChunkHeader must be exactly 128 bytes");

/// Compact Block Index Entry (80 bytes)
/// One entry per compressed block, stored in the index region
struct CompactBlockIndex {
    uint32_t tag_id;                 // Tag ID
    uint32_t original_block_index;   // Original RAW block index

    uint64_t data_offset;            // Offset to compressed data (from chunk data_offset)
    uint32_t compressed_size;        // Compressed block size (bytes)
    uint32_t original_size;          // Original RAW block size (bytes)

    int64_t  start_ts_us;            // Block start timestamp
    int64_t  end_ts_us;              // Block end timestamp

    uint32_t record_count;           // Number of records in block
    uint8_t  original_encoding;      // Original EncodingType (from RAW block)
    uint8_t  value_type;             // ValueType (VT_BOOL/I32/F32/F64)
    uint8_t  time_unit;              // TimeUnit
    uint8_t  reserved_u8;            // Reserved for alignment

    uint32_t block_crc32;            // CRC32 of compressed data
    uint32_t reserved1;

    // Padding to reach 80 bytes
    uint8_t  padding[80 - 4 - 4 - 8 - 4 - 4 - 8 - 8 - 4 - 4 - 4 - 4];

    // Constructor
    CompactBlockIndex() {
        tag_id = 0;
        original_block_index = 0;
        data_offset = 0;
        compressed_size = 0;
        original_size = 0;
        start_ts_us = 0;
        end_ts_us = 0;
        record_count = 0;
        original_encoding = static_cast<uint8_t>(EncodingType::ENC_RAW);
        value_type = static_cast<uint8_t>(ValueType::VT_F64);
        time_unit = static_cast<uint8_t>(TimeUnit::TU_US);
        reserved_u8 = 0;
        block_crc32 = 0;
        reserved1 = 0;
        std::memset(padding, 0, sizeof(padding));
    }
};

// Verify struct size
static_assert(sizeof(CompactBlockIndex) == 80,
              "CompactBlockIndex must be exactly 80 bytes");

#pragma pack(pop)

// ============================================================================
// COMPACT Helper Functions
// ============================================================================

/// Calculate COMPACT chunk size (in bytes)
/// @param block_count Number of blocks in chunk
/// @return Total chunk size including header, index, and data regions
inline uint64_t calculateCompactChunkSize(uint32_t block_count, uint64_t total_data_size) {
    uint64_t header_size = sizeof(CompactChunkHeader);
    uint64_t index_size = static_cast<uint64_t>(block_count) * sizeof(CompactBlockIndex);
    return header_size + index_size + total_data_size;
}

/// Get index region offset (right after header)
inline uint64_t getCompactIndexOffset() {
    return sizeof(CompactChunkHeader);
}

/// Get data region offset
/// @param block_count Number of blocks
/// @return Offset to data region
inline uint64_t getCompactDataOffset(uint32_t block_count) {
    return sizeof(CompactChunkHeader) +
           static_cast<uint64_t>(block_count) * sizeof(CompactBlockIndex);
}

/// Validate COMPACT chunk header
/// @param header Chunk header to validate
/// @return true if valid
inline bool validateCompactChunkHeader(const CompactChunkHeader& header) {
    if (std::memcmp(header.magic, kCompactChunkMagic, 8) != 0) {
        return false;
    }
    if (header.version != kCompactChunkVersion) {
        return false;
    }
    if (header.header_size != sizeof(CompactChunkHeader)) {
        return false;
    }
    return true;
}

/// Calculate compression ratio
/// @param original_size Original size before compression
/// @param compressed_size Compressed size
/// @return Compression ratio (0.0 to 1.0, lower is better)
inline double calculateCompressionRatio(uint64_t original_size, uint64_t compressed_size) {
    if (original_size == 0) {
        return 0.0;
    }
    return static_cast<double>(compressed_size) / static_cast<double>(original_size);
}

/// Calculate space savings
/// @param original_size Original size before compression
/// @param compressed_size Compressed size
/// @return Space savings percentage (0.0 to 100.0)
inline double calculateSpaceSavings(uint64_t original_size, uint64_t compressed_size) {
    if (original_size == 0) {
        return 0.0;
    }
    return 100.0 * (1.0 - calculateCompressionRatio(original_size, compressed_size));
}

// ============================================================================
// BlockDirEntryV16 (64 bytes, expanded from 48 bytes)
// ============================================================================

#pragma pack(push, 1)

struct BlockDirEntryV16 {
    uint32_t tag_id;

    uint8_t  value_type;             // ValueType: VT_BOOL / VT_I32 / VT_F32 / VT_F64
    uint8_t  time_unit;              // TimeUnit: TU_100MS / TU_10MS / TU_MS / etc.
    uint8_t  encoding_type;          // EncodingType: compression/quantization method
    uint8_t  reserved_u8;            // Reserved for alignment

    uint16_t record_size;            // Record size in bytes (depends on value_type)
    uint16_t reserved_u16;           // Reserved for future use

    uint32_t flags;                  // Active-low state bits (init: kBlockFlagsInit)

    int64_t  start_ts_us;            // Start timestamp (microseconds)
    int64_t  end_ts_us;              // End timestamp (sealed: write-once)

    uint32_t record_count;           // Number of records (sealed: write-once, 0xFFFFFFFF before seal)
    uint32_t data_crc32;             // Data CRC32 (sealed: write-once, 0xFFFFFFFF before seal)

    // Encoding-specific parameters (interpretation depends on encoding_type)
    // - ENC_SWINGING_DOOR: param1 = tolerance (float bits), param2 = compression_factor (float bits)
    // - ENC_QUANTIZED_16: param1 = low_extreme (float bits), param2 = high_extreme (float bits)
    // - ENC_RAW: unused (set to 0)
    uint32_t encoding_param1;        // Encoding parameter 1 (context-dependent)
    uint32_t encoding_param2;        // Encoding parameter 2 (context-dependent)

    // Reserved for future extensions (to reach 64 bytes)
    uint32_t reserved_u32_1;         // Reserved for future use
    uint32_t reserved_u32_2;         // Reserved for future use
    uint32_t reserved_u32_3;         // Reserved for future use
    uint32_t reserved_u32_4;         // Reserved for future use

    // Constructor: initialize to safe defaults
    BlockDirEntryV16() {
        std::memset(this, 0, sizeof(*this));
        flags = kBlockFlagsInit;
        start_ts_us = 0x7FFFFFFFFFFFFFFFll;
        end_ts_us = 0x7FFFFFFFFFFFFFFFll;
        record_count = 0xFFFFFFFFu;
        data_crc32 = 0xFFFFFFFFu;
        encoding_param1 = 0;
        encoding_param2 = 0;
        reserved_u32_1 = 0;
        reserved_u32_2 = 0;
        reserved_u32_3 = 0;
        reserved_u32_4 = 0;
    }
};

#pragma pack(pop)

// Verify struct size (expanded to 64 bytes for future compatibility)
static_assert(sizeof(BlockDirEntryV16) == 64,
              "BlockDirEntryV16 must be exactly 64 bytes");

}  // namespace xtdb

#endif  // XTDB_STRUCT_DEFS_H_
