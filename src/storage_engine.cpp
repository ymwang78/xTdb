#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "xTdb/layout_calculator.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

namespace xtdb {

StorageEngine::StorageEngine(const EngineConfig& config)
    : config_(config), is_open_(false) {
    // Calculate layout based on block class (default: RAW_16K)
    config_.layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);
}

StorageEngine::~StorageEngine() {
    close();
}

EngineResult StorageEngine::open() {
    if (is_open_) {
        setError("Engine already open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Step 1: Connect to metadata (SQLite)
    EngineResult result = connectMetadata();
    if (result != EngineResult::SUCCESS) {
        return result;
    }

    // Step 2: Mount container files
    result = mountContainers();
    if (result != EngineResult::SUCCESS) {
        return result;
    }

    // Step 3: Restore active state
    result = restoreActiveState();
    if (result != EngineResult::SUCCESS) {
        return result;
    }

    // Step 4: Replay WAL
    result = replayWAL();
    if (result != EngineResult::SUCCESS) {
        return result;
    }

    is_open_ = true;
    return EngineResult::SUCCESS;
}

void StorageEngine::close() {
    if (!is_open_) {
        return;
    }

    // Close metadata
    if (metadata_) {
        metadata_->close();
        metadata_.reset();
    }

    // Close I/O
    if (io_) {
        io_->close();
        io_.reset();
    }

    containers_.clear();
    buffers_.clear();
    is_open_ = false;
}

EngineResult StorageEngine::connectMetadata() {
    // Create metadata sync instance
    metadata_ = std::make_unique<MetadataSync>(config_.db_path);

    // Open database
    SyncResult result = metadata_->open();
    if (result != SyncResult::SUCCESS) {
        setError("Failed to open metadata: " + metadata_->getLastError());
        return EngineResult::ERROR_METADATA_OPEN_FAILED;
    }

    // Initialize schema
    result = metadata_->initSchema();
    if (result != SyncResult::SUCCESS) {
        setError("Failed to init schema: " + metadata_->getLastError());
        return EngineResult::ERROR_METADATA_OPEN_FAILED;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::mountContainers() {
    // For this phase, we create a single container file
    std::string container_path = config_.data_dir + "/container_0.raw";

    // Check if container exists
    struct stat st;
    bool exists = (stat(container_path.c_str(), &st) == 0);

    if (!exists) {
        // Create new container file
        // Initialize I/O
        io_ = std::make_unique<AlignedIO>();
        IOResult io_result = io_->open(container_path, true, false);  // Create new, read/write
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to create container: " + io_->getLastError());
            return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
        }

        // Initialize container header using constructor
        ContainerHeaderV12 header;
        // Constructor already sets magic, version, header_size
        header.capacity_extents = (config_.layout.chunk_size_bytes * 16) / kExtentSizeBytes;  // 16 chunks
        header.chunk_size_extents = config_.layout.chunk_size_bytes / kExtentSizeBytes;
        header.block_size_extents = config_.layout.block_size_bytes / kExtentSizeBytes;
        header.layout = static_cast<uint8_t>(ContainerLayout::LAYOUT_RAW_FIXED);
        header.raw_block_class = 1;  // RAW16K

        // Write header
        AlignedBuffer header_buf(kExtentSizeBytes);
        std::memcpy(header_buf.data(), &header, sizeof(header));
        io_result = io_->write(header_buf.data(), kExtentSizeBytes, 0);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to write container header: " + io_->getLastError());
            return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
        }

        // Register container
        ContainerInfo info;
        info.container_id = 0;
        info.file_path = container_path;
        info.capacity_bytes = header.capacity_extents * kExtentSizeBytes;
        info.layout = config_.layout;
        containers_.push_back(info);

        return EngineResult::SUCCESS;
    }

    // Container exists, verify header
    io_ = std::make_unique<AlignedIO>();
    IOResult io_result = io_->open(container_path, false, false);  // Don't create, read/write
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to open container: " + io_->getLastError());
        return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    ContainerHeaderV12 header;
    EngineResult result = verifyContainerHeader(container_path, header);
    if (result != EngineResult::SUCCESS) {
        return result;
    }

    // Register container
    ContainerInfo info;
    info.container_id = 0;
    info.file_path = container_path;
    info.capacity_bytes = header.capacity_extents * kExtentSizeBytes;
    info.layout = config_.layout;
    containers_.push_back(info);

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::restoreActiveState() {
    // Initialize state mutator
    mutator_ = std::make_unique<StateMutator>(io_.get());

    // Check if we have any active chunks in SQLite
    // For now, we'll allocate a new chunk at offset kExtentSizeBytes (after container header)
    // This is simplified for Phase 7 - full implementation would query SQLite for active chunks

    // Initialize active chunk info
    active_chunk_.chunk_id = 42;  // Start with ID 42
    active_chunk_.chunk_offset = kExtentSizeBytes;  // After container header
    active_chunk_.blocks_used = 0;
    active_chunk_.blocks_total = config_.layout.data_blocks;
    active_chunk_.start_ts_us = 0;
    active_chunk_.end_ts_us = 0;

    // Check if chunk exists by reading header
    AlignedBuffer header_buf(config_.layout.block_size_bytes);
    IOResult result = io_->read(header_buf.data(),
                               config_.layout.block_size_bytes,
                               active_chunk_.chunk_offset);

    if (result == IOResult::SUCCESS) {
        // Chunk exists, read header
        RawChunkHeaderV16 chunk_header;
        std::memcpy(&chunk_header, header_buf.data(), sizeof(chunk_header));

        // Check if chunk is allocated
        if (std::memcmp(chunk_header.magic, kRawChunkMagic, 8) == 0 &&
            chunkIsAllocated(chunk_header.flags)) {
            // Chunk is allocated, restore state
            active_chunk_.chunk_id = chunk_header.chunk_id;
            active_chunk_.start_ts_us = chunk_header.start_ts_us;
            active_chunk_.end_ts_us = chunk_header.end_ts_us;

            // Count blocks by reading directory
            RawScanner scanner(io_.get());
            ScannedChunk scanned_chunk;
            ScanResult scan_result = scanner.scanChunk(active_chunk_.chunk_offset,
                                                      config_.layout,
                                                      scanned_chunk);
            if (scan_result == ScanResult::SUCCESS) {
                active_chunk_.blocks_used = scanned_chunk.blocks.size();
            }

            return EngineResult::SUCCESS;
        }
    }

    // No active chunk, allocate new one
    EngineResult alloc_result = allocateNewChunk(active_chunk_.chunk_offset);
    if (alloc_result != EngineResult::SUCCESS) {
        return alloc_result;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::replayWAL() {
    // WAL integration is complex and requires container-based storage
    // For Phase 7, we simplify by skipping WAL replay
    // WAL would be stored within the container file, not as a separate file
    // This will be fully implemented in Phase 8

    // TODO Phase 8: Implement full WAL replay with container-based storage
    // - Allocate WAL region in container
    // - Create WALWriter with proper offset/size
    // - Read and replay entries
    // - Truncate after successful replay

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::verifyContainerHeader(const std::string& container_path,
                                                 ContainerHeaderV12& header) {
    // Read header
    AlignedBuffer buf(kExtentSizeBytes);
    IOResult result = io_->read(buf.data(), kExtentSizeBytes, 0);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read container header: " + io_->getLastError());
        return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    // Copy header
    std::memcpy(&header, buf.data(), sizeof(header));

    // Verify magic
    if (std::memcmp(header.magic, kContainerMagic, 8) != 0) {
        setError("Invalid container magic");
        return EngineResult::ERROR_CONTAINER_HEADER_INVALID;
    }

    // Verify version (0x0102 for V12)
    if (header.version != 0x0102) {
        setError("Unsupported container version");
        return EngineResult::ERROR_CONTAINER_HEADER_INVALID;
    }

    // Verify file size
    struct stat st;
    if (stat(container_path.c_str(), &st) != 0) {
        setError("Failed to stat container file");
        return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    uint64_t capacity_bytes = header.capacity_extents * kExtentSizeBytes;
    if (static_cast<uint64_t>(st.st_size) < capacity_bytes) {
        setError("Container file truncated");
        return EngineResult::ERROR_CONTAINER_HEADER_INVALID;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::allocateNewChunk(uint64_t chunk_offset) {
    // Initialize chunk header using constructor
    RawChunkHeaderV16 header;
    // Constructor already sets magic, version, flags, etc.
    header.chunk_id = active_chunk_.chunk_id;
    header.chunk_size_extents = config_.layout.chunk_size_bytes / kExtentSizeBytes;
    header.block_size_extents = config_.layout.block_size_bytes / kExtentSizeBytes;
    header.meta_blocks = config_.layout.meta_blocks;
    header.data_blocks = config_.layout.data_blocks;
    // start_ts_us and end_ts_us already initialized by constructor

    // Write header
    AlignedBuffer header_buf(config_.layout.block_size_bytes);
    std::memset(header_buf.data(), 0xFF, config_.layout.block_size_bytes);
    std::memcpy(header_buf.data(), &header, sizeof(header));

    IOResult io_result = io_->write(header_buf.data(),
                                    config_.layout.block_size_bytes,
                                    chunk_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write chunk header: " + io_->getLastError());
        return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
    }

    // Initialize directory (all blocks unsealed)
    uint32_t dir_block_count = config_.layout.meta_blocks - 1;  // Exclude header block
    uint64_t dir_offset = chunk_offset + config_.layout.block_size_bytes;

    for (uint32_t i = 0; i < dir_block_count; i++) {
        AlignedBuffer dir_buf(config_.layout.block_size_bytes);
        std::memset(dir_buf.data(), 0xFF, config_.layout.block_size_bytes);

        io_result = io_->write(dir_buf.data(),
                              config_.layout.block_size_bytes,
                              dir_offset + i * config_.layout.block_size_bytes);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to write directory: " + io_->getLastError());
            return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
        }
    }

    // Set chunk as allocated (clear ALLOCATED bit)
    MutateResult mut_result = mutator_->allocateChunk(chunk_offset);
    if (mut_result != MutateResult::SUCCESS) {
        setError("Failed to set chunk state: " + mutator_->getLastError());
        return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
    }

    return EngineResult::SUCCESS;
}

void StorageEngine::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
