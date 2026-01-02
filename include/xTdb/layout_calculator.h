#ifndef XTDB_LAYOUT_CALCULATOR_H_
#define XTDB_LAYOUT_CALCULATOR_H_

#include "constants.h"
#include <cstdint>
#include <cmath>

namespace xtdb {

// ============================================================================
// LayoutCalculator: Pure function library for offset/layout calculations
// ============================================================================

/// Chunk layout information
struct ChunkLayout {
    uint32_t chunk_size_extents;   // Total chunk size (in extents)
    uint32_t block_size_extents;   // Block size (in extents)
    uint32_t blocks_per_chunk;     // Total blocks in chunk
    uint32_t meta_blocks;          // Number of metadata blocks
    uint32_t data_blocks;          // Number of data blocks
    uint64_t chunk_size_bytes;     // Total chunk size (in bytes)
    uint64_t block_size_bytes;     // Block size (in bytes)
};

/// Block directory entry size (from design V1.6 Section 7.2)
constexpr uint32_t kBlockDirEntrySize = 48u;  // sizeof(BlockDirEntryV16)

/// Chunk header size (from design V1.6 Section 6)
constexpr uint32_t kChunkHeaderSize = 128u;   // sizeof(RawChunkHeaderV16)

class LayoutCalculator {
public:
    /// Calculate chunk layout given block class and chunk size
    /// @param block_class RAW block class (16K/64K/256K)
    /// @param chunk_size_extents Chunk size in extents (default: 256MB)
    /// @return ChunkLayout structure with all calculated fields
    static ChunkLayout calculateLayout(
        RawBlockClass block_class,
        uint32_t chunk_size_extents = kDefaultChunkSizeExtents);

    /// Calculate physical byte offset for a specific block within a chunk
    /// @param chunk_id Chunk slot ID
    /// @param block_index Block index within chunk (0-based)
    /// @param layout Chunk layout information
    /// @param container_base Base offset of container file (usually 16KB after header)
    /// @return Physical byte offset for the block
    static uint64_t calculateBlockOffset(
        uint32_t chunk_id,
        uint32_t block_index,
        const ChunkLayout& layout,
        uint64_t container_base = kExtentSizeBytes);

    /// Calculate physical byte offset for a specific chunk
    /// @param chunk_id Chunk slot ID
    /// @param layout Chunk layout information
    /// @param container_base Base offset of container file
    /// @return Physical byte offset for the chunk
    static uint64_t calculateChunkOffset(
        uint32_t chunk_id,
        const ChunkLayout& layout,
        uint64_t container_base = kExtentSizeBytes);

    /// Calculate BlockDir region offset within a chunk
    /// @param layout Chunk layout information
    /// @return Offset (in bytes) from chunk base to BlockDir region
    static uint64_t calculateBlockDirOffset(const ChunkLayout& layout);

    /// Calculate data region starting offset within a chunk
    /// @param layout Chunk layout information
    /// @return Offset (in bytes) from chunk base to data region
    static uint64_t calculateDataRegionOffset(const ChunkLayout& layout);

    /// Validate that layout parameters are consistent and aligned
    /// @param layout Layout to validate
    /// @return true if valid, false otherwise
    static bool validateLayout(const ChunkLayout& layout);

private:
    /// Calculate number of meta blocks needed for directory
    /// Formula from design V1.6 Section 7.3:
    ///   meta_bytes = sizeof(ChunkHeader) + data_blocks * sizeof(BlockDirEntry)
    ///   meta_blocks = ceil(meta_bytes / block_size_bytes)
    static uint32_t calculateMetaBlocks(
        uint32_t data_blocks_estimate,
        uint64_t block_size_bytes);
};

}  // namespace xtdb

#endif  // XTDB_LAYOUT_CALCULATOR_H_
