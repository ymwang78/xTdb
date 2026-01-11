#ifndef XTDB_COMPACT_LAYOUT_H_
#define XTDB_COMPACT_LAYOUT_H_

#include "xTdb/struct_defs.h"
#include <vector>
#include <cstdint>

namespace xtdb {

// ============================================================================
// Compact Chunk Layout Calculator
// ============================================================================

/// Layout information for a COMPACT chunk
struct CompactChunkLayout {
    uint32_t block_count;            // Number of blocks
    uint64_t header_size;            // Size of CompactChunkHeader
    uint64_t index_size;             // Size of index region
    uint64_t data_size;              // Size of data region
    uint64_t total_size;             // Total chunk size

    uint64_t index_offset;           // Offset to index region
    uint64_t data_offset;            // Offset to data region

    CompactChunkLayout()
        : block_count(0),
          header_size(0),
          index_size(0),
          data_size(0),
          total_size(0),
          index_offset(0),
          data_offset(0) {}
};

/// Block information for COMPACT archive planning
struct CompactBlockInfo {
    uint32_t tag_id;
    uint32_t original_block_index;
    uint32_t original_size;
    uint32_t estimated_compressed_size;

    CompactBlockInfo()
        : tag_id(0),
          original_block_index(0),
          original_size(0),
          estimated_compressed_size(0) {}

    CompactBlockInfo(uint32_t tag, uint32_t block_idx, uint32_t orig_size, uint32_t est_compressed)
        : tag_id(tag),
          original_block_index(block_idx),
          original_size(orig_size),
          estimated_compressed_size(est_compressed) {}
};

/// COMPACT Chunk Layout Calculator
class CompactLayoutCalculator {
public:
    /// Calculate layout for a COMPACT chunk
    /// @param blocks List of blocks to be archived
    /// @param layout Output: calculated layout
    /// @return true if successful
    static bool calculateLayout(const std::vector<CompactBlockInfo>& blocks,
                               CompactChunkLayout& layout);

    /// Estimate compressed size
    /// @param original_size Original block size
    /// @param compression_type Compression algorithm
    /// @param compression_ratio Expected compression ratio (0.0-1.0)
    /// @return Estimated compressed size
    static uint32_t estimateCompressedSize(uint32_t original_size,
                                          CompressionType compression_type,
                                          double compression_ratio = 0.3);

    /// Calculate total data size from blocks
    /// @param blocks List of blocks
    /// @return Total data size (sum of compressed sizes)
    static uint64_t calculateTotalDataSize(const std::vector<CompactBlockInfo>& blocks);

    /// Calculate index region size
    /// @param block_count Number of blocks
    /// @return Index region size in bytes
    static uint64_t calculateIndexSize(uint32_t block_count);

    /// Validate layout consistency
    /// @param layout Layout to validate
    /// @return true if valid
    static bool validateLayout(const CompactChunkLayout& layout);

    /// Get recommended chunk size limit
    /// @return Maximum recommended chunk size (bytes)
    static constexpr uint64_t getMaxChunkSize() {
        // Limit to 256 MB per chunk for safety
        return 256ULL * 1024 * 1024;
    }

    /// Get maximum blocks per chunk
    /// @return Maximum recommended blocks per chunk
    static constexpr uint32_t getMaxBlocksPerChunk() {
        // Limit to 10000 blocks per chunk
        return 10000;
    }
};

// ============================================================================
// Inline Implementations
// ============================================================================

inline uint64_t CompactLayoutCalculator::calculateIndexSize(uint32_t block_count) {
    return static_cast<uint64_t>(block_count) * sizeof(CompactBlockIndex);
}

inline uint64_t CompactLayoutCalculator::calculateTotalDataSize(
    const std::vector<CompactBlockInfo>& blocks)
{
    uint64_t total = 0;
    for (const auto& block : blocks) {
        total += block.estimated_compressed_size;
    }
    return total;
}

inline uint32_t CompactLayoutCalculator::estimateCompressedSize(
    uint32_t original_size,
    CompressionType /* compression_type */,
    double compression_ratio)
{
    // Apply compression ratio estimate
    uint32_t estimated = static_cast<uint32_t>(original_size * compression_ratio);

    // Add safety margin for compression overhead (zstd frame headers, etc.)
    const uint32_t overhead = 128;  // Conservative estimate
    estimated += overhead;

    // Ensure minimum size
    if (estimated < overhead) {
        estimated = overhead;
    }

    // Cap at original size (worst case: no compression)
    if (estimated > original_size) {
        estimated = original_size;
    }

    return estimated;
}

inline bool CompactLayoutCalculator::calculateLayout(
    const std::vector<CompactBlockInfo>& blocks,
    CompactChunkLayout& layout)
{
    if (blocks.empty()) {
        return false;
    }

    if (blocks.size() > getMaxBlocksPerChunk()) {
        return false;
    }

    layout.block_count = static_cast<uint32_t>(blocks.size());
    layout.header_size = sizeof(CompactChunkHeader);
    layout.index_size = calculateIndexSize(layout.block_count);
    layout.data_size = calculateTotalDataSize(blocks);
    layout.total_size = layout.header_size + layout.index_size + layout.data_size;

    layout.index_offset = layout.header_size;
    layout.data_offset = layout.header_size + layout.index_size;

    // Validate total size
    if (layout.total_size > getMaxChunkSize()) {
        return false;
    }

    return true;
}

inline bool CompactLayoutCalculator::validateLayout(const CompactChunkLayout& layout) {
    // Check non-zero values
    if (layout.block_count == 0 || layout.total_size == 0) {
        return false;
    }

    // Check size consistency
    uint64_t expected_total = layout.header_size + layout.index_size + layout.data_size;
    if (layout.total_size != expected_total) {
        return false;
    }

    // Check offset consistency
    if (layout.index_offset != layout.header_size) {
        return false;
    }

    if (layout.data_offset != layout.header_size + layout.index_size) {
        return false;
    }

    // Check limits
    if (layout.block_count > getMaxBlocksPerChunk()) {
        return false;
    }

    if (layout.total_size > getMaxChunkSize()) {
        return false;
    }

    return true;
}

}  // namespace xtdb

#endif  // XTDB_COMPACT_LAYOUT_H_
