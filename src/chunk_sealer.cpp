#include "xTdb/chunk_sealer.h"
#include "xTdb/constants.h"
#include <cstring>

namespace xtdb {

// ============================================================================
// CRC32 Implementation (Simple polynomial-based)
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

uint32_t ChunkSealer::calculateCRC32(const void* data, uint64_t size) {
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
// ChunkSealer Implementation
// ============================================================================

ChunkSealer::ChunkSealer(AlignedIO* io, StateMutator* mutator)
    : io_(io), mutator_(mutator) {
}

ChunkSealer::~ChunkSealer() {
}

SealResult ChunkSealer::calculateSuperCRC(uint64_t chunk_offset,
                                         const ChunkLayout& layout,
                                         uint32_t& super_crc32) {
    // Calculate directory size
    uint64_t dir_size_bytes = layout.data_blocks * sizeof(BlockDirEntryV16);
    uint64_t buffer_size = alignToExtent(dir_size_bytes);

    // Calculate directory offset (starts at block 1 in meta region)
    // Block 0 contains chunk header, directory starts at block 1
    uint64_t dir_offset = chunk_offset + layout.block_size_bytes;

    // Read directory
    AlignedBuffer buffer(buffer_size);
    IOResult result = io_->read(buffer.data(), buffer_size, dir_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read directory: " + io_->getLastError());
        return SealResult::ERROR_IO_FAILED;
    }

    // Calculate CRC32 over the directory entries only (not padding)
    super_crc32 = calculateCRC32(buffer.data(), dir_size_bytes);

    return SealResult::SUCCESS;
}

SealResult ChunkSealer::sealChunk(uint64_t chunk_offset,
                                 const ChunkLayout& layout,
                                 int64_t start_ts_us,
                                 int64_t end_ts_us) {
    // Read chunk header
    RawChunkHeaderV16 header;
    MutateResult read_result = mutator_->readChunkHeader(chunk_offset, header);
    if (read_result != MutateResult::SUCCESS) {
        setError("Failed to read chunk header: " + mutator_->getLastError());
        return SealResult::ERROR_IO_FAILED;
    }

    // Check if already sealed
    if (chunkIsSealed(header.flags)) {
        setError("Chunk already sealed");
        return SealResult::ERROR_ALREADY_SEALED;
    }

    // Calculate SuperCRC
    uint32_t super_crc32 = 0;
    SealResult crc_result = calculateSuperCRC(chunk_offset, layout, super_crc32);
    if (crc_result != SealResult::SUCCESS) {
        return crc_result;
    }

    // Update chunk header via StateMutator
    MutateResult seal_result = mutator_->sealChunk(chunk_offset,
                                                   start_ts_us,
                                                   end_ts_us,
                                                   super_crc32);
    if (seal_result != MutateResult::SUCCESS) {
        setError("Failed to seal chunk: " + mutator_->getLastError());
        return SealResult::ERROR_IO_FAILED;
    }

    return SealResult::SUCCESS;
}

void ChunkSealer::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
