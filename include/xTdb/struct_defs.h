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
    uint16_t flags;

    uint64_t capacity_extents;       // CAP_FIXED: required; CAP_DYNAMIC: can be 0
    int64_t  created_ts_us;          // PostgreSQL epoch microseconds

    // RAW fixed parameters (only meaningful when layout=RAW_FIXED)
    uint8_t  raw_block_class;        // RawBlockClass (1=16K, 2=64K, 3=256K)
    uint8_t  reserved8[7];

    uint32_t chunk_size_extents;     // RAW: fixed chunk slot size (in extents)
    uint32_t block_size_extents;     // RAW: fixed block size (in extents)

    uint32_t header_crc32;           // CRC32(header without this field)
    uint32_t reserved0;

    // Padding to fill 16KB
    uint8_t  payload[kExtentSizeBytes - 8 - 2 - 2 - 16 - 1 - 1 - 2 - 8 - 8 - 1 - 7 - 4 - 4 - 4 - 4];

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
// BlockDirEntryV16 (48 bytes)
// ============================================================================

#pragma pack(push, 1)

struct BlockDirEntryV16 {
    uint32_t tag_id;

    uint8_t  value_type;             // ValueType
    uint8_t  time_unit;              // TimeUnit
    uint16_t record_size;

    uint32_t flags;                  // Active-low (init: kBlockFlagsInit)

    int64_t  start_ts_us;
    int64_t  end_ts_us;              // Sealed: write-once

    uint32_t record_count;           // Sealed: write-once (0xFFFFFFFF before seal)
    uint32_t data_crc32;             // Sealed: write-once (0xFFFFFFFF before seal)

    // Padding to reach 48 bytes
    uint8_t  padding[48 - 4 - 1 - 1 - 2 - 4 - 8 - 8 - 4 - 4];

    // Constructor: initialize to safe defaults
    BlockDirEntryV16() {
        std::memset(this, 0, sizeof(*this));
        flags = kBlockFlagsInit;
        start_ts_us = 0x7FFFFFFFFFFFFFFFll;
        end_ts_us = 0x7FFFFFFFFFFFFFFFll;
        record_count = 0xFFFFFFFFu;
        data_crc32 = 0xFFFFFFFFu;
    }
};

#pragma pack(pop)

// Verify struct size (must be 48 bytes as per design)
static_assert(sizeof(BlockDirEntryV16) == 48,
              "BlockDirEntryV16 must be exactly 48 bytes");

}  // namespace xtdb

#endif  // XTDB_STRUCT_DEFS_H_
