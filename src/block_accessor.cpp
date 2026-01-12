#include "xTdb/block_accessor.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/aligned_io.h"
#include <iostream>

namespace xtdb {

// ============================================================================
// Constructor & Destructor
// ============================================================================

BlockAccessor::BlockAccessor(FileContainer* raw_container,
                             CompactContainer* compact_container,
                             MetadataSync* metadata_sync)
    : raw_container_(raw_container),
      compact_container_(compact_container),
      metadata_sync_(metadata_sync),
      stats_(),
      last_error_() {
}

BlockAccessor::~BlockAccessor() {
}

// ============================================================================
// Read Operations
// ============================================================================

AccessResult BlockAccessor::readBlock(uint32_t raw_container_id,
                                     uint32_t chunk_id,
                                     uint32_t block_index,
                                     const ChunkLayout& raw_layout,
                                     BlockData& block_data) {
    // Query metadata to check archive status
    bool is_archived = false;
    uint32_t archived_to_container_id = 0;
    uint32_t archived_to_block_index = 0;

    SyncResult sync_result = metadata_sync_->queryBlockArchiveStatus(
        raw_container_id,
        chunk_id,
        block_index,
        is_archived,
        archived_to_container_id,
        archived_to_block_index
    );

    if (sync_result != SyncResult::SUCCESS) {
        setError("Failed to query block archive status: " + metadata_sync_->getLastError());
        return AccessResult::ERR_METADATA_QUERY_FAILED;
    }

    // Get block metadata
    BlockQueryResult metadata;
    sync_result = metadata_sync_->queryBlockMetadata(
        raw_container_id,
        chunk_id,
        block_index,
        metadata
    );

    if (sync_result != SyncResult::SUCCESS) {
        setError("Failed to query block metadata: " + metadata_sync_->getLastError());
        return AccessResult::ERR_METADATA_QUERY_FAILED;
    }

    // If block is archived, read from COMPACT
    if (is_archived) {
        return readFromCompact(archived_to_block_index, metadata, block_data);
    }

    // Block is not archived, read from RAW
    return readFromRaw(chunk_id, block_index, raw_layout, metadata, block_data);
}

AccessResult BlockAccessor::queryBlocksByTagAndTime(uint32_t tag_id,
                                                   int64_t start_ts_us,
                                                   int64_t end_ts_us,
                                                   const ChunkLayout& raw_layout,
                                                   std::vector<BlockData>& results) {
    results.clear();

    // Query metadata for RAW blocks in time range (container_id=0 only)
    std::vector<BlockQueryResult> query_results;
    SyncResult sync_result = metadata_sync_->queryBlocksByTagAndTime(
        tag_id, start_ts_us, end_ts_us, query_results);

    if (sync_result != SyncResult::SUCCESS) {
        setError("Metadata query failed: " + metadata_sync_->getLastError());
        return AccessResult::ERR_METADATA_QUERY_FAILED;
    }

    std::cout << "Found " << query_results.size() << " RAW blocks in time range" << std::endl;

    // Read each block (RAW or COMPACT based on archive status)
    for (const auto& metadata : query_results) {
        BlockData block_data;

        // Check if block is archived
        bool is_archived = false;
        uint32_t archived_to_container_id = 0;
        uint32_t archived_to_block_index = 0;

        sync_result = metadata_sync_->queryBlockArchiveStatus(
            0,  // RAW container ID
            metadata.chunk_id,
            metadata.block_index,
            is_archived,
            archived_to_container_id,
            archived_to_block_index
        );

        AccessResult result;
        if (sync_result == SyncResult::SUCCESS && is_archived) {
            // Read from COMPACT
            result = readFromCompact(archived_to_block_index, metadata, block_data);
        } else {
            // Read from RAW
            result = readFromRaw(
                metadata.chunk_id,
                metadata.block_index,
                raw_layout,
                metadata,
                block_data
            );
        }

        if (result == AccessResult::SUCCESS) {
            results.push_back(block_data);
        } else {
            std::cerr << "Failed to read block chunk=" << metadata.chunk_id
                     << " block_index=" << metadata.block_index << std::endl;
        }
    }

    std::cout << "Successfully read " << results.size() << " blocks" << std::endl;

    return AccessResult::SUCCESS;
}

void BlockAccessor::resetStats() {
    stats_ = AccessStats();
}

// ============================================================================
// Private Helper Methods
// ============================================================================

AccessResult BlockAccessor::readFromRaw(uint32_t chunk_id,
                                       uint32_t block_index,
                                       const ChunkLayout& raw_layout,
                                       const BlockQueryResult& metadata,
                                       BlockData& block_data) {
    // Calculate physical offset
    uint32_t physical_block_index = raw_layout.meta_blocks + block_index;
    uint64_t block_offset = LayoutCalculator::calculateBlockOffset(
        chunk_id, physical_block_index, raw_layout);

    // Read data using aligned buffer
    AlignedBuffer aligned_data(raw_layout.block_size_bytes);
    ContainerResult result = raw_container_->read(
        aligned_data.data(), aligned_data.size(), block_offset);

    if (result != ContainerResult::SUCCESS) {
        setError("RAW read failed: " + raw_container_->getLastError());
        return AccessResult::ERR_RAW_READ_FAILED;
    }

    // Copy to regular vector for block_data
    std::vector<uint8_t> data(raw_layout.block_size_bytes);
    std::memcpy(data.data(), aligned_data.data(), aligned_data.size());

    // Fill block data
    block_data.container_id = 0;  // RAW container
    block_data.chunk_id = chunk_id;
    block_data.block_index = block_index;
    block_data.tag_id = metadata.tag_id;
    block_data.start_ts_us = metadata.start_ts_us;
    block_data.end_ts_us = metadata.end_ts_us;
    block_data.time_unit = metadata.time_unit;
    block_data.value_type = metadata.value_type;
    block_data.record_count = metadata.record_count;
    block_data.encoding_type = EncodingType::ENC_RAW;  // TODO: Get from metadata
    block_data.is_compressed = false;
    block_data.data = data;

    stats_.raw_reads++;
    stats_.total_bytes_read += data.size();

    return AccessResult::SUCCESS;
}

AccessResult BlockAccessor::readFromCompact(uint32_t compact_block_index,
                                           const BlockQueryResult& metadata,
                                           BlockData& block_data) {
    // Read and decompress from COMPACT container
    std::vector<uint8_t> data(16384);  // Max block size
    uint32_t actual_size = 0;

    ContainerResult result = compact_container_->readBlock(
        compact_block_index,
        data.data(),
        data.size(),
        actual_size
    );

    if (result != ContainerResult::SUCCESS) {
        setError("COMPACT read failed: " + compact_container_->getLastError());
        return AccessResult::ERR_COMPACT_READ_FAILED;
    }

    // Resize to actual size
    data.resize(actual_size);

    // Fill block data
    block_data.container_id = 1;  // COMPACT container
    block_data.chunk_id = metadata.chunk_id;
    block_data.block_index = metadata.block_index;
    block_data.tag_id = metadata.tag_id;
    block_data.start_ts_us = metadata.start_ts_us;
    block_data.end_ts_us = metadata.end_ts_us;
    block_data.time_unit = metadata.time_unit;
    block_data.value_type = metadata.value_type;
    block_data.record_count = metadata.record_count;
    block_data.encoding_type = EncodingType::ENC_RAW;  // TODO: Get from metadata
    block_data.is_compressed = true;
    block_data.data = data;

    stats_.compact_reads++;
    stats_.total_bytes_read += actual_size;
    stats_.total_bytes_decompressed += actual_size;

    return AccessResult::SUCCESS;
}

void BlockAccessor::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
