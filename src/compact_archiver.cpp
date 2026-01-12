#include "xTdb/compact_archiver.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/aligned_io.h"
#include <cstring>
#include <vector>

namespace xtdb {

// ============================================================================
// Constructor & Destructor
// ============================================================================

CompactArchiver::CompactArchiver()
    : stats_(),
      last_error_() {
}

CompactArchiver::~CompactArchiver() {
}

// ============================================================================
// Archive Operations
// ============================================================================

ArchiveResult CompactArchiver::archiveBlock(FileContainer& raw_container,
                                           const ChunkLayout& raw_layout,
                                           uint32_t raw_chunk_id,
                                           uint32_t raw_block_index,
                                           CompactContainer& compact_container,
                                           uint32_t tag_id,
                                           int64_t start_ts_us,
                                           int64_t end_ts_us,
                                           uint32_t record_count,
                                           EncodingType original_encoding,
                                           ValueType value_type,
                                           TimeUnit time_unit) {
    // Validate containers are open
    if (!raw_container.isOpen()) {
        setError("RAW container is not open");
        return ArchiveResult::ERR_RAW_CONTAINER_NOT_OPEN;
    }

    if (!compact_container.isOpen()) {
        setError("COMPACT container is not open");
        return ArchiveResult::ERR_COMPACT_CONTAINER_NOT_OPEN;
    }

    // Validate block index
    if (raw_block_index >= raw_layout.data_blocks) {
        setError("Block index out of range");
        return ArchiveResult::ERR_BLOCK_NOT_FOUND;
    }

    // Calculate block offset in RAW container
    // raw_block_index is a data block index, need to add meta_blocks for physical block index
    uint32_t physical_block_index = raw_layout.meta_blocks + raw_block_index;
    uint64_t block_offset = LayoutCalculator::calculateBlockOffset(
        raw_chunk_id, physical_block_index, raw_layout);
    uint32_t block_size = raw_layout.block_size_bytes;

    // Allocate aligned buffer for RAW block data (FileContainer requires alignment)
    AlignedBuffer raw_block_data(block_size);

    // Read RAW block from container
    ContainerResult read_result = raw_container.read(
        raw_block_data.data(),
        block_size,
        block_offset
    );

    if (read_result != ContainerResult::SUCCESS) {
        setError("Failed to read RAW block: " + raw_container.getLastError());
        return ArchiveResult::ERR_READ_FAILED;
    }

    // Write to COMPACT container (compression happens inside writeBlock)
    ContainerResult write_result = compact_container.writeBlock(
        tag_id,
        raw_block_index,  // Use same block index for 1:1 mapping
        raw_block_data.data(),
        block_size,
        start_ts_us,
        end_ts_us,
        record_count,
        original_encoding,
        value_type,
        time_unit
    );

    if (write_result != ContainerResult::SUCCESS) {
        setError("Failed to write COMPACT block: " + compact_container.getLastError());
        return ArchiveResult::ERR_WRITE_FAILED;
    }

    // Update statistics
    stats_.blocks_archived++;
    stats_.bytes_read += block_size;
    stats_.total_original_size += block_size;

    // Get compression stats from compact container
    uint64_t total_original, total_compressed;
    double compression_ratio;
    compact_container.getCompressionStats(total_original, total_compressed, compression_ratio);

    stats_.bytes_written = total_compressed;
    stats_.total_compressed_size = total_compressed;
    stats_.compression_ratio = compression_ratio;

    return ArchiveResult::SUCCESS;
}

void CompactArchiver::resetStats() {
    stats_ = ArchiveStats();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

uint64_t CompactArchiver::calculateBlockOffset(uint32_t chunk_id,
                                              uint32_t block_index,
                                              const ChunkLayout& layout) const {
    // Deprecated: Use LayoutCalculator::calculateBlockOffset() instead
    // This method is kept for API compatibility
    return LayoutCalculator::calculateBlockOffset(chunk_id, block_index, layout);
}

void CompactArchiver::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
