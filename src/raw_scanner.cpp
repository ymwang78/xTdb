#include "xTdb/raw_scanner.h"
#include "xTdb/constants.h"
#include <cstring>

namespace xtdb {

// ============================================================================
// CRC32 Implementation (reuse from ChunkSealer)
// ============================================================================

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

uint32_t RawScanner::calculateCRC32(const void* data, uint64_t size) {
    init_crc32_table();

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;

    for (uint64_t i = 0; i < size; i++) {
        uint8_t index = (crc ^ ptr[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }

    return crc ^ 0xFFFFFFFFu;
}

// ============================================================================
// RawScanner Implementation
// ============================================================================

RawScanner::RawScanner(AlignedIO* io)
    : io_(io) {
}

RawScanner::~RawScanner() {
}

ScanResult RawScanner::readChunkHeader(uint64_t chunk_offset,
                                      RawChunkHeaderV16& header) {
    // Read chunk header (first 16KB extent)
    AlignedBuffer buffer(kExtentSizeBytes);
    IOResult result = io_->read(buffer.data(), kExtentSizeBytes, chunk_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read chunk header: " + io_->getLastError());
        return ScanResult::ERROR_IO_FAILED;
    }

    // Copy header
    std::memcpy(&header, buffer.data(), sizeof(RawChunkHeaderV16));

    // Verify magic
    if (std::memcmp(header.magic, kRawChunkMagic, 8) != 0) {
        setError("Invalid chunk magic");
        return ScanResult::ERROR_INVALID_CHUNK;
    }

    // Verify version
    if (header.version != 0x0106) {
        setError("Unsupported chunk version");
        return ScanResult::ERROR_INVALID_CHUNK;
    }

    return ScanResult::SUCCESS;
}

ScanResult RawScanner::readBlockDirectory(uint64_t chunk_offset,
                                         const ChunkLayout& layout,
                                         std::vector<BlockDirEntryV16>& entries) {
    // Calculate directory size
    uint64_t dir_size_bytes = layout.data_blocks * sizeof(BlockDirEntryV16);
    uint64_t buffer_size = alignToExtent(dir_size_bytes);

    // Calculate directory offset (starts right after chunk header)
    uint64_t dir_offset = chunk_offset + kChunkHeaderSize;

    // Read directory
    AlignedBuffer buffer(buffer_size);
    IOResult result = io_->read(buffer.data(), buffer_size, dir_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read block directory: " + io_->getLastError());
        return ScanResult::ERROR_IO_FAILED;
    }

    // Copy entries
    entries.resize(layout.data_blocks);
    std::memcpy(entries.data(), buffer.data(), dir_size_bytes);

    return ScanResult::SUCCESS;
}

ScanResult RawScanner::calculateSuperCRC(uint64_t chunk_offset,
                                        const ChunkLayout& layout,
                                        uint32_t& super_crc32) {
    // Calculate directory size
    uint64_t dir_size_bytes = layout.data_blocks * sizeof(BlockDirEntryV16);
    uint64_t buffer_size = alignToExtent(dir_size_bytes);

    // Calculate directory offset
    uint64_t dir_offset = chunk_offset + layout.block_size_bytes;

    // Read directory
    AlignedBuffer buffer(buffer_size);
    IOResult result = io_->read(buffer.data(), buffer_size, dir_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read directory for CRC: " + io_->getLastError());
        return ScanResult::ERROR_IO_FAILED;
    }

    // Calculate CRC32 over directory entries only (not padding)
    super_crc32 = calculateCRC32(buffer.data(), dir_size_bytes);

    return ScanResult::SUCCESS;
}

ScanResult RawScanner::scanChunk(uint64_t chunk_offset,
                                const ChunkLayout& layout,
                                ScannedChunk& chunk) {
    // Read chunk header
    RawChunkHeaderV16 header;
    ScanResult result = readChunkHeader(chunk_offset, header);
    if (result != ScanResult::SUCCESS) {
        return result;
    }

    // Extract chunk metadata
    chunk.chunk_id = header.chunk_id;
    chunk.start_ts_us = header.start_ts_us;
    chunk.end_ts_us = header.end_ts_us;
    chunk.super_crc32 = header.super_crc32;
    chunk.is_sealed = chunkIsSealed(header.flags);

    // Read block directory
    std::vector<BlockDirEntryV16> entries;
    result = readBlockDirectory(chunk_offset, layout, entries);
    if (result != ScanResult::SUCCESS) {
        return result;
    }

    // Convert entries to ScannedBlock
    chunk.blocks.clear();
    for (uint32_t i = 0; i < entries.size(); i++) {
        const BlockDirEntryV16& entry = entries[i];

        // Check if block is sealed (record_count != 0xFFFFFFFF)
        if (entry.record_count == 0xFFFFFFFFu) {
            continue;  // Skip unsealed blocks
        }

        ScannedBlock block;
        block.block_index = i;
        block.tag_id = entry.tag_id;
        block.start_ts_us = entry.start_ts_us;
        block.end_ts_us = entry.end_ts_us;
        block.time_unit = static_cast<TimeUnit>(entry.time_unit);
        block.value_type = static_cast<ValueType>(entry.value_type);
        block.record_count = entry.record_count;
        block.data_crc32 = entry.data_crc32;
        block.is_sealed = true;

        chunk.blocks.push_back(block);
    }

    return ScanResult::SUCCESS;
}

ScanResult RawScanner::verifyChunkIntegrity(uint64_t chunk_offset,
                                           const ChunkLayout& layout) {
    // Read chunk header
    RawChunkHeaderV16 header;
    ScanResult result = readChunkHeader(chunk_offset, header);
    if (result != ScanResult::SUCCESS) {
        return result;
    }

    // Check if chunk is sealed
    if (!chunkIsSealed(header.flags)) {
        setError("Chunk is not sealed");
        return ScanResult::ERROR_NOT_SEALED;
    }

    // Calculate SuperCRC
    uint32_t calculated_crc = 0;
    result = calculateSuperCRC(chunk_offset, layout, calculated_crc);
    if (result != ScanResult::SUCCESS) {
        return result;
    }

    // Verify CRC
    if (calculated_crc != header.super_crc32) {
        setError("SuperCRC mismatch");
        return ScanResult::ERROR_CRC_MISMATCH;
    }

    return ScanResult::SUCCESS;
}

void RawScanner::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
