#include "xTdb/state_mutator.h"
#include <cassert>

namespace xtdb {

StateMutator::StateMutator(AlignedIO* io)
    : io_(io) {
    assert(io_ != nullptr);
    assert(io_->isOpen());
}

bool StateMutator::validateTransition(uint32_t old_value, uint32_t new_value) const {
    // Check that no bit transitions from 0->1
    // Valid transition: new_value must have all 0-bits that old_value has
    // Invalid if: (new_value & ~old_value) != 0
    uint32_t flipped_to_one = new_value & ~old_value;
    return flipped_to_one == 0;
}

void StateMutator::setError(const std::string& message) {
    last_error_ = message;
}

// ============================================================================
// Chunk State Mutations
// ============================================================================

MutateResult StateMutator::allocateChunk(uint64_t chunk_offset) {
    // Read current header
    AlignedBuffer buffer(sizeof(RawChunkHeaderV16));
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    RawChunkHeaderV16* header = reinterpret_cast<RawChunkHeaderV16*>(buffer.data());

    // Check if already allocated
    if (chunkIsAllocated(header->flags)) {
        setError("Chunk already allocated");
        return MutateResult::ERROR_ALREADY_SET;
    }

    // Clear ALLOCATED bit (1->0)
    uint32_t new_flags = chunkClearBit(header->flags, ChunkStateBit::CHB_ALLOCATED);

    // Validate transition
    if (!validateTransition(header->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    // Update flags
    header->flags = new_flags;

    // Write back (only the flags field in production; here we write full extent for simplicity)
    io_result = io_->write(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write chunk header: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::sealChunk(uint64_t chunk_offset,
                                     int64_t start_ts_us,
                                     int64_t end_ts_us,
                                     uint32_t super_crc32) {
    // Read current header
    AlignedBuffer buffer(sizeof(RawChunkHeaderV16));
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    RawChunkHeaderV16* header = reinterpret_cast<RawChunkHeaderV16*>(buffer.data());

    // Check if already sealed
    if (chunkIsSealed(header->flags)) {
        setError("Chunk already sealed");
        return MutateResult::ERROR_ALREADY_SET;
    }

    // Clear SEALED bit (1->0)
    uint32_t new_flags = chunkClearBit(header->flags, ChunkStateBit::CHB_SEALED);

    // Validate transition
    if (!validateTransition(header->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    // Update seal data
    header->flags = new_flags;
    header->start_ts_us = start_ts_us;
    header->end_ts_us = end_ts_us;
    header->super_crc32 = super_crc32;

    // Write back
    io_result = io_->write(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write chunk header: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::deprecateChunk(uint64_t chunk_offset) {
    // Read current header
    AlignedBuffer buffer(sizeof(RawChunkHeaderV16));
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    RawChunkHeaderV16* header = reinterpret_cast<RawChunkHeaderV16*>(buffer.data());

    // Check if already deprecated
    if (chunkIsDeprecated(header->flags)) {
        setError("Chunk already deprecated");
        return MutateResult::ERROR_ALREADY_SET;
    }

    // Clear DEPRECATED bit (1->0)
    uint32_t new_flags = chunkClearBit(header->flags, ChunkStateBit::CHB_DEPRECATED);

    // Validate transition
    if (!validateTransition(header->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    // Update flags
    header->flags = new_flags;

    // Write back
    io_result = io_->write(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write chunk header: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::markChunkFree(uint64_t chunk_offset) {
    // Read current header
    AlignedBuffer buffer(sizeof(RawChunkHeaderV16));
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    RawChunkHeaderV16* header = reinterpret_cast<RawChunkHeaderV16*>(buffer.data());

    // Clear FREE_MARK bit (1->0)
    uint32_t new_flags = chunkClearBit(header->flags, ChunkStateBit::CHB_FREE_MARK);

    // Validate transition
    if (!validateTransition(header->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    // Update flags
    header->flags = new_flags;

    // Write back
    io_result = io_->write(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write chunk header: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

// ============================================================================
// Block State Mutations
// ============================================================================

MutateResult StateMutator::sealBlock(uint64_t block_dir_entry_offset,
                                     int64_t end_ts_us,
                                     uint32_t record_count,
                                     uint32_t data_crc32) {
    // Read current entry (must be aligned to 16KB boundary)
    // Round down to extent boundary
    uint64_t extent_offset = (block_dir_entry_offset / kExtentSizeBytes) * kExtentSizeBytes;
    uint64_t offset_within_extent = block_dir_entry_offset - extent_offset;

    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    BlockDirEntryV16* entry = reinterpret_cast<BlockDirEntryV16*>(
        static_cast<char*>(buffer.data()) + offset_within_extent);

    // Check if already sealed
    if (blockIsSealed(entry->flags)) {
        setError("Block already sealed");
        return MutateResult::ERROR_ALREADY_SET;
    }

    // Clear SEALED bit (1->0)
    uint32_t new_flags = blockClearBit(entry->flags, BlockStateBit::BLB_SEALED);

    // Validate transition
    if (!validateTransition(entry->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    // Update seal data
    entry->flags = new_flags;
    entry->end_ts_us = end_ts_us;
    entry->record_count = record_count;
    entry->data_crc32 = data_crc32;

    // Write back
    io_result = io_->write(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::assertMonotonicTime(uint64_t block_dir_entry_offset) {
    uint64_t extent_offset = (block_dir_entry_offset / kExtentSizeBytes) * kExtentSizeBytes;
    uint64_t offset_within_extent = block_dir_entry_offset - extent_offset;

    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    BlockDirEntryV16* entry = reinterpret_cast<BlockDirEntryV16*>(
        static_cast<char*>(buffer.data()) + offset_within_extent);

    uint32_t new_flags = blockClearBit(entry->flags, BlockStateBit::BLB_MONOTONIC_TIME);

    if (!validateTransition(entry->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    entry->flags = new_flags;

    io_result = io_->write(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::assertNoTimeGap(uint64_t block_dir_entry_offset) {
    uint64_t extent_offset = (block_dir_entry_offset / kExtentSizeBytes) * kExtentSizeBytes;
    uint64_t offset_within_extent = block_dir_entry_offset - extent_offset;

    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    BlockDirEntryV16* entry = reinterpret_cast<BlockDirEntryV16*>(
        static_cast<char*>(buffer.data()) + offset_within_extent);

    uint32_t new_flags = blockClearBit(entry->flags, BlockStateBit::BLB_NO_TIME_GAP);

    if (!validateTransition(entry->flags, new_flags)) {
        setError("Invalid state transition: 0->1 bit flip detected");
        return MutateResult::ERROR_INVALID_TRANSITION;
    }

    entry->flags = new_flags;

    io_result = io_->write(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

// ============================================================================
// Initialization (Full Write)
// ============================================================================

MutateResult StateMutator::initChunkHeader(uint64_t chunk_offset,
                                          const RawChunkHeaderV16& header) {
    // Allocate aligned buffer and copy header
    AlignedBuffer buffer(kExtentSizeBytes);
    buffer.zero();
    std::memcpy(buffer.data(), &header, sizeof(RawChunkHeaderV16));

    // Write full extent
    IOResult io_result = io_->write(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to init chunk header: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

MutateResult StateMutator::initBlockDirEntry(uint64_t block_dir_entry_offset,
                                            const BlockDirEntryV16& entry) {
    // Round down to extent boundary
    uint64_t extent_offset = (block_dir_entry_offset / kExtentSizeBytes) * kExtentSizeBytes;
    uint64_t offset_within_extent = block_dir_entry_offset - extent_offset;

    // Read entire extent
    AlignedBuffer buffer(kExtentSizeBytes);
    buffer.zero();

    // Try to read existing data (may fail if new file)
    io_->read(buffer.data(), kExtentSizeBytes, extent_offset);

    // Copy entry to correct position
    std::memcpy(static_cast<char*>(buffer.data()) + offset_within_extent,
                &entry,
                sizeof(BlockDirEntryV16));

    // Write back
    IOResult io_result = io_->write(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to init block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_WRITE_FAILED;
    }

    return MutateResult::SUCCESS;
}

// ============================================================================
// Read Operations
// ============================================================================

MutateResult StateMutator::readChunkHeader(uint64_t chunk_offset,
                                          RawChunkHeaderV16& header) {
    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    std::memcpy(&header, buffer.data(), sizeof(RawChunkHeaderV16));
    return MutateResult::SUCCESS;
}

MutateResult StateMutator::readBlockDirEntry(uint64_t block_dir_entry_offset,
                                            BlockDirEntryV16& entry) {
    uint64_t extent_offset = (block_dir_entry_offset / kExtentSizeBytes) * kExtentSizeBytes;
    uint64_t offset_within_extent = block_dir_entry_offset - extent_offset;

    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult io_result = io_->read(buffer.data(), kExtentSizeBytes, extent_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read block dir entry: " + io_->getLastError());
        return MutateResult::ERROR_READ_FAILED;
    }

    std::memcpy(&entry,
                static_cast<char*>(buffer.data()) + offset_within_extent,
                sizeof(BlockDirEntryV16));
    return MutateResult::SUCCESS;
}

}  // namespace xtdb
