#include "xTdb/directory_builder.h"
#include "xTdb/constants.h"
#include <cstring>

namespace xtdb {

DirectoryBuilder::DirectoryBuilder(AlignedIO* io,
                                 const ChunkLayout& layout,
                                 uint64_t chunk_offset)
    : io_(io),
      layout_(layout),
      chunk_offset_(chunk_offset),
      sealed_block_count_(0) {
}

DirectoryBuilder::~DirectoryBuilder() {
}

DirBuildResult DirectoryBuilder::initialize() {
    // Allocate entries for all data blocks
    // BlockDirEntryV16 constructor handles initialization
    entries_.resize(layout_.data_blocks);

    sealed_block_count_ = 0;
    return DirBuildResult::SUCCESS;
}

DirBuildResult DirectoryBuilder::sealBlock(uint32_t block_index,
                                          uint32_t tag_id,
                                          int64_t start_ts_us,
                                          int64_t end_ts_us,
                                          TimeUnit time_unit,
                                          ValueType value_type,
                                          uint32_t record_count,
                                          uint32_t data_crc32) {
    // Validate block index
    if (block_index >= layout_.data_blocks) {
        setError("Invalid block index");
        return DirBuildResult::ERROR_INVALID_BLOCK;
    }

    // Check if already sealed
    BlockDirEntryV16& entry = entries_[block_index];
    if (entry.record_count != 0xFFFFFFFFu) {
        setError("Block already sealed");
        return DirBuildResult::ERROR_BLOCK_SEALED;
    }

    // Update entry
    entry.tag_id = tag_id;
    entry.start_ts_us = start_ts_us;
    entry.end_ts_us = end_ts_us;
    entry.time_unit = static_cast<uint8_t>(time_unit);
    entry.value_type = static_cast<uint8_t>(value_type);
    entry.record_count = record_count;
    entry.data_crc32 = data_crc32;

    // Note: padding is already zeroed by constructor

    sealed_block_count_++;
    return DirBuildResult::SUCCESS;
}

DirBuildResult DirectoryBuilder::writeDirectory() {
    // Calculate directory size
    uint64_t dir_size_bytes = entries_.size() * sizeof(BlockDirEntryV16);

    // Calculate directory offset (starts at block 1 in meta region)
    // Block 0 contains chunk header, directory starts at block 1
    uint64_t dir_offset = chunk_offset_ + layout_.block_size_bytes;

    // Create aligned buffer for directory
    // Round up to extent alignment
    uint64_t buffer_size = alignToExtent(dir_size_bytes);
    AlignedBuffer buffer(buffer_size);

    // Copy entries to buffer
    std::memcpy(buffer.data(), entries_.data(), dir_size_bytes);

    // Zero padding
    if (buffer_size > dir_size_bytes) {
        std::memset(static_cast<char*>(buffer.data()) + dir_size_bytes,
                   0,
                   buffer_size - dir_size_bytes);
    }

    // Write to disk
    IOResult result = io_->write(buffer.data(), buffer_size, dir_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to write directory: " + io_->getLastError());
        return DirBuildResult::ERROR_IO_FAILED;
    }

    return DirBuildResult::SUCCESS;
}

const BlockDirEntryV16* DirectoryBuilder::getEntry(uint32_t block_index) const {
    if (block_index >= entries_.size()) {
        return nullptr;
    }
    return &entries_[block_index];
}

void DirectoryBuilder::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
