#include "xTdb/layout_calculator.h"
#include <cassert>
#include <stdexcept>

namespace xtdb {

ChunkLayout LayoutCalculator::calculateLayout(
    RawBlockClass block_class,
    uint32_t chunk_size_extents) {

    ChunkLayout layout{};

    // Basic parameters
    layout.chunk_size_extents = chunk_size_extents;
    layout.block_size_extents = getBlockSizeExtents(block_class);
    layout.chunk_size_bytes = extentToBytes(chunk_size_extents);
    layout.block_size_bytes = getBlockSizeBytes(block_class);

    // Validate inputs
    if (layout.block_size_extents == 0 || layout.block_size_bytes == 0) {
        throw std::invalid_argument("Invalid block class");
    }

    if (layout.chunk_size_bytes < layout.block_size_bytes) {
        throw std::invalid_argument("Chunk size must be >= block size");
    }

    if (!isExtentAligned(layout.chunk_size_bytes) ||
        !isExtentAligned(layout.block_size_bytes)) {
        throw std::invalid_argument("Sizes must be extent-aligned");
    }

    // Calculate total blocks per chunk
    layout.blocks_per_chunk =
        static_cast<uint32_t>(layout.chunk_size_bytes / layout.block_size_bytes);

    // Iteratively solve for meta_blocks and data_blocks
    // This is necessary because meta_blocks depends on data_blocks,
    // but data_blocks = blocks_per_chunk - meta_blocks

    uint32_t data_blocks_estimate = layout.blocks_per_chunk;
    uint32_t meta_blocks = 0;

    // Iterate until converged (usually converges in 1-2 iterations)
    for (int iter = 0; iter < 10; ++iter) {
        meta_blocks = calculateMetaBlocks(data_blocks_estimate,
                                          layout.block_size_bytes);
        uint32_t data_blocks_new = layout.blocks_per_chunk - meta_blocks;

        if (data_blocks_new == data_blocks_estimate) {
            break;  // Converged
        }

        data_blocks_estimate = data_blocks_new;
    }

    layout.meta_blocks = meta_blocks;
    layout.data_blocks = layout.blocks_per_chunk - meta_blocks;

    // Sanity check
    if (layout.meta_blocks == 0 || layout.data_blocks == 0) {
        throw std::runtime_error("Invalid layout: zero meta_blocks or data_blocks");
    }

    return layout;
}

uint32_t LayoutCalculator::calculateMetaBlocks(
    uint32_t data_blocks_estimate,
    uint64_t block_size_bytes) {

    // meta_bytes = sizeof(ChunkHeader) + data_blocks * sizeof(BlockDirEntry)
    uint64_t meta_bytes = kChunkHeaderSize +
                          data_blocks_estimate * kBlockDirEntrySize;

    // meta_blocks = ceil(meta_bytes / block_size_bytes)
    uint32_t meta_blocks =
        static_cast<uint32_t>((meta_bytes + block_size_bytes - 1) / block_size_bytes);

    return meta_blocks;
}

uint64_t LayoutCalculator::calculateChunkOffset(
    uint32_t chunk_id,
    const ChunkLayout& layout,
    uint64_t container_base) {

    // chunk_offset = container_base + chunk_id * chunk_size_bytes
    return container_base + static_cast<uint64_t>(chunk_id) * layout.chunk_size_bytes;
}

uint64_t LayoutCalculator::calculateBlockOffset(
    uint32_t chunk_id,
    uint32_t block_index,
    const ChunkLayout& layout,
    uint64_t container_base) {

    // Validate block_index
    if (block_index >= layout.blocks_per_chunk) {
        throw std::out_of_range("block_index out of range");
    }

    // chunk_base = container_base + chunk_id * chunk_size_bytes
    uint64_t chunk_base = calculateChunkOffset(chunk_id, layout, container_base);

    // block_offset = chunk_base + block_index * block_size_bytes
    uint64_t block_offset = chunk_base +
                            static_cast<uint64_t>(block_index) * layout.block_size_bytes;

    // Ensure extent-aligned
    assert(isExtentAligned(block_offset));

    return block_offset;
}

uint64_t LayoutCalculator::calculateBlockDirOffset(const ChunkLayout& layout) {
    (void)layout;  // Unused but kept for API consistency
    // BlockDir starts immediately after ChunkHeader
    return kChunkHeaderSize;
}

uint64_t LayoutCalculator::calculateDataRegionOffset(const ChunkLayout& layout) {
    // Data region starts after all meta blocks
    return layout.meta_blocks * layout.block_size_bytes;
}

bool LayoutCalculator::validateLayout(const ChunkLayout& layout) {
    // Check extent alignment
    if (!isExtentAligned(layout.chunk_size_bytes) ||
        !isExtentAligned(layout.block_size_bytes)) {
        return false;
    }

    // Check blocks sum
    if (layout.meta_blocks + layout.data_blocks != layout.blocks_per_chunk) {
        return false;
    }

    // Check meta blocks can hold directory
    uint64_t required_meta_bytes = kChunkHeaderSize +
                                   layout.data_blocks * kBlockDirEntrySize;
    uint64_t available_meta_bytes = layout.meta_blocks * layout.block_size_bytes;

    if (available_meta_bytes < required_meta_bytes) {
        return false;
    }

    // Check non-zero values
    if (layout.meta_blocks == 0 || layout.data_blocks == 0) {
        return false;
    }

    return true;
}

}  // namespace xtdb
