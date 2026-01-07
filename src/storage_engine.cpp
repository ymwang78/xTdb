#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "xTdb/layout_calculator.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <chrono>

namespace xtdb {

StorageEngine::StorageEngine(const EngineConfig& config)
    : config_(config), is_open_(false) {
    // If layout not specified by user, calculate default layout based on RAW_16K
    if (config_.layout.block_size_bytes == 0 || config_.layout.chunk_size_bytes == 0) {
        config_.layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K);
    }
    // Otherwise, recalculate meta/data blocks for user-specified sizes
    else if (config_.layout.meta_blocks == 0 || config_.layout.data_blocks == 0) {
        // Convert chunk_size_bytes to extents
        uint32_t chunk_size_extents = static_cast<uint32_t>(
            config_.layout.chunk_size_bytes / kExtentSizeBytes);
        config_.layout = LayoutCalculator::calculateLayout(RawBlockClass::RAW_16K,
                                                           chunk_size_extents);
    }
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

    // Clear directory builder and mutator BEFORE closing I/O
    // (they hold raw pointers to io_)
    dir_builder_.reset();
    mutator_.reset();

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
    // First chunk at offset kExtentSizeBytes corresponds to chunk_id = 0
    active_chunk_.chunk_id = 0;
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

            // Create DirectoryBuilder and load existing directory from disk
            dir_builder_ = std::make_unique<DirectoryBuilder>(io_.get(),
                                                              config_.layout,
                                                              active_chunk_.chunk_offset);
            DirBuildResult dir_result = dir_builder_->load();
            if (dir_result != DirBuildResult::SUCCESS) {
                setError("Failed to load directory: " + dir_builder_->getLastError());
                return EngineResult::ERROR_STATE_RESTORATION_FAILED;
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

    // Verify file size - check that file has at least the header
    struct stat st;
    if (stat(container_path.c_str(), &st) != 0) {
        setError("Failed to stat container file");
        return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    // TODO Phase 10: Implement proper container pre-allocation
    // For now, just verify the file exists and has a header
    if (static_cast<uint64_t>(st.st_size) < kExtentSizeBytes) {
        setError("Container file too small");
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

    // Create DirectoryBuilder for this chunk
    dir_builder_ = std::make_unique<DirectoryBuilder>(io_.get(),
                                                      config_.layout,
                                                      chunk_offset);
    DirBuildResult dir_result = dir_builder_->initialize();
    if (dir_result != DirBuildResult::SUCCESS) {
        setError("Failed to initialize directory: " + dir_builder_->getLastError());
        return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::writePoint(uint32_t tag_id,
                                       int64_t timestamp_us,
                                       double value,
                                       uint8_t quality) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Step 1: WAL Append (for crash recovery)
    // TODO Phase 8: Implement WAL append
    // For now, we skip WAL and focus on buffer management

    // Step 2: Add point to memory buffer through WAL entry
    WALEntry entry;
    entry.tag_id = tag_id;
    entry.timestamp_us = timestamp_us;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.value.f64_value = value;
    entry.quality = quality;

    // Find or create MemBuffer for this tag
    auto it = buffers_.find(tag_id);
    if (it == buffers_.end()) {
        // Create new buffer
        TagBuffer new_buffer;
        new_buffer.tag_id = tag_id;
        new_buffer.value_type = ValueType::VT_F64;
        new_buffer.time_unit = TimeUnit::TU_MS;
        new_buffer.start_ts_us = timestamp_us;
        buffers_[tag_id] = new_buffer;
        it = buffers_.find(tag_id);
    }

    // Add record to buffer
    TagBuffer& tag_buffer = it->second;

    // Create MemRecord
    MemRecord record;
    record.time_offset = static_cast<uint32_t>((timestamp_us - tag_buffer.start_ts_us) / 1000);  // Convert to ms
    record.quality = quality;
    record.value.f64_value = value;
    tag_buffer.records.push_back(record);

    write_stats_.points_written++;

    // Step 3: Check if buffer needs flush (threshold: 1000 records or ~16KB)
    if (tag_buffer.records.size() >= 1000) {
        // Trigger flush for this buffer
        // For Phase 8 simplification, we flush synchronously
        // In production, this should be async via thread pool
        EngineResult flush_result = flush();
        if (flush_result != EngineResult::SUCCESS) {
            return flush_result;
        }
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::flush() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Flush all non-empty buffers
    for (auto& [tag_id, tag_buffer] : buffers_) {
        if (tag_buffer.records.empty()) {
            continue;
        }

        // Check if we need to roll (allocate new chunk)
        if (active_chunk_.blocks_used >= active_chunk_.blocks_total) {
            // Chunk is full, need to seal and allocate new one

            // Seal current chunk
            ChunkSealer sealer(io_.get(), mutator_.get());
            int64_t final_start_ts = active_chunk_.start_ts_us;
            int64_t final_end_ts = active_chunk_.end_ts_us;

            SealResult seal_result = sealer.sealChunk(active_chunk_.chunk_offset,
                                                     config_.layout,
                                                     final_start_ts,
                                                     final_end_ts);
            if (seal_result != SealResult::SUCCESS) {
                setError("Failed to seal chunk: " + sealer.getLastError());
                return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
            }

            write_stats_.chunks_sealed++;

            // Allocate new chunk at next position
            uint64_t new_chunk_offset = active_chunk_.chunk_offset +
                                       config_.layout.chunk_size_bytes;

            // Update active chunk info before allocation
            active_chunk_.chunk_id++;
            active_chunk_.chunk_offset = new_chunk_offset;
            active_chunk_.blocks_used = 0;
            active_chunk_.start_ts_us = 0;
            active_chunk_.end_ts_us = 0;

            EngineResult alloc_result = allocateNewChunk(new_chunk_offset);
            if (alloc_result != EngineResult::SUCCESS) {
                return alloc_result;
            }

            write_stats_.chunks_allocated++;
        }

        // Write block to disk using BlockWriter
        BlockWriter writer(io_.get(), config_.layout, kExtentSizeBytes);

        uint32_t data_block_index = active_chunk_.blocks_used;
        BlockWriteResult write_result = writer.writeBlock(active_chunk_.chunk_offset,
                                                         data_block_index,
                                                         tag_buffer);
        if (write_result != BlockWriteResult::SUCCESS) {
            setError("Failed to write block: " + writer.getLastError());
            return EngineResult::ERROR_INVALID_DATA;
        }

        write_stats_.blocks_flushed++;

        // Update directory entry using persistent dir_builder
        if (!dir_builder_) {
            setError("Directory builder not initialized");
            return EngineResult::ERROR_INVALID_DATA;
        }

        // Get timestamp range from tag_buffer
        int64_t block_start_ts = tag_buffer.start_ts_us;
        int64_t block_end_ts = tag_buffer.start_ts_us;
        if (!tag_buffer.records.empty()) {
            // Find the maximum time_offset to calculate end_ts
            uint32_t max_offset = 0;
            for (const auto& rec : tag_buffer.records) {
                if (rec.time_offset > max_offset) {
                    max_offset = rec.time_offset;
                }
            }
            block_end_ts = tag_buffer.start_ts_us + static_cast<int64_t>(max_offset) * 1000;
        }

        DirBuildResult dir_result = dir_builder_->sealBlock(
            data_block_index,
            tag_id,
            block_start_ts,
            block_end_ts,
            tag_buffer.time_unit,
            tag_buffer.value_type,
            static_cast<uint32_t>(tag_buffer.records.size()),
            0  // TODO: Calculate CRC32
        );

        if (dir_result != DirBuildResult::SUCCESS) {
            setError("Failed to seal block in directory: " + dir_builder_->getLastError());
            return EngineResult::ERROR_INVALID_DATA;
        }

        // Write directory to disk
        dir_result = dir_builder_->writeDirectory();
        if (dir_result != DirBuildResult::SUCCESS) {
            setError("Failed to write directory: " + dir_builder_->getLastError());
            return EngineResult::ERROR_INVALID_DATA;
        }

        // Update active chunk tracking
        active_chunk_.blocks_used++;
        if (active_chunk_.start_ts_us == 0) {
            active_chunk_.start_ts_us = block_start_ts;
        }
        active_chunk_.end_ts_us = block_end_ts;

        // Clear buffer after successful write
        tag_buffer.records.clear();
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::queryPoints(uint32_t tag_id,
                                        int64_t start_ts_us,
                                        int64_t end_ts_us,
                                        std::vector<QueryPoint>& results) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    results.clear();
    read_stats_.queries_executed++;

    // Step 1: Read from memory buffers (unflushed data)
    auto it = buffers_.find(tag_id);
    if (it != buffers_.end()) {
        const TagBuffer& tag_buffer = it->second;
        for (const auto& record : tag_buffer.records) {
            int64_t timestamp_us = tag_buffer.start_ts_us +
                                  static_cast<int64_t>(record.time_offset) * 1000;
            if (timestamp_us >= start_ts_us && timestamp_us <= end_ts_us) {
                results.emplace_back(timestamp_us, record.value.f64_value, record.quality);
                read_stats_.points_read_memory++;
            }
        }
    }

    // Step 2: Read from disk blocks
    if (active_chunk_.blocks_used > 0) {
        RawScanner scanner(io_.get());
        ScannedChunk scanned_chunk;

        ScanResult scan_result = scanner.scanChunk(active_chunk_.chunk_offset,
                                                   config_.layout,
                                                   scanned_chunk);

        if (scan_result == ScanResult::SUCCESS) {
            BlockReader reader(io_.get(), config_.layout);

            for (const auto& block_info : scanned_chunk.blocks) {
                if (block_info.tag_id != tag_id) {
                    continue;
                }

                if (block_info.end_ts_us < start_ts_us ||
                    block_info.start_ts_us > end_ts_us) {
                    continue;
                }

                std::vector<MemRecord> records;
                ReadResult read_result = reader.readBlock(
                    active_chunk_.chunk_offset,
                    block_info.block_index,
                    block_info.tag_id,
                    block_info.start_ts_us,
                    block_info.time_unit,
                    block_info.value_type,
                    block_info.record_count,
                    records
                );

                if (read_result == ReadResult::SUCCESS) {
                    read_stats_.blocks_read++;

                    for (const auto& record : records) {
                        int64_t timestamp_us = block_info.start_ts_us +
                                             static_cast<int64_t>(record.time_offset) * 1000;

                        if (timestamp_us >= start_ts_us && timestamp_us <= end_ts_us) {
                            results.emplace_back(timestamp_us,
                                               record.value.f64_value,
                                               record.quality);
                            read_stats_.points_read_disk++;
                        }
                    }
                }
            }
        }
    }

    // Step 3: Sort results by timestamp
    std::sort(results.begin(), results.end(),
              [](const QueryPoint& a, const QueryPoint& b) {
                  return a.timestamp_us < b.timestamp_us;
              });

    return EngineResult::SUCCESS;
}

void StorageEngine::setError(const std::string& message) {
    last_error_ = message;
}

// ============================================================================
// Phase 10: Maintenance Services
// ============================================================================

EngineResult StorageEngine::runRetentionService(int64_t current_time_us) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // If retention is disabled (0 days), skip
    if (config_.retention_days == 0) {
        return EngineResult::SUCCESS;
    }

    // Use provided time or current system time
    if (current_time_us == 0) {
        current_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    // Calculate cutoff time
    int64_t retention_us = config_.retention_days * 24LL * 3600LL * 1000000LL;  // days to microseconds
    int64_t cutoff_time_us = current_time_us - retention_us;

    // Query SQLite for sealed chunks older than cutoff
    SyncResult result = metadata_->querySealedChunks(
        0,  // container_id
        0,  // min_end_ts (no minimum)
        cutoff_time_us,  // max_end_ts (cutoff)
        [this](uint32_t chunk_id, uint64_t chunk_offset, int64_t /*start_ts*/, int64_t /*end_ts*/) {
            // Deprecate this chunk
            MutateResult mut_result = mutator_->deprecateChunk(chunk_offset);
            if (mut_result == MutateResult::SUCCESS) {
                maintenance_stats_.chunks_deprecated++;

                // Delete chunk metadata from SQLite
                metadata_->deleteChunk(0, chunk_id);
            }
        }
    );

    if (result != SyncResult::SUCCESS) {
        setError("Failed to query chunks for retention: " + metadata_->getLastError());
        return EngineResult::ERROR_METADATA_OPEN_FAILED;
    }

    maintenance_stats_.last_retention_run_ts = current_time_us;
    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::reclaimDeprecatedChunks() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Scan container for deprecated chunks
    // In production, this would query SQLite for DEPRECATED chunks
    // For now, we scan the physical file

    uint64_t chunk_offset = kExtentSizeBytes;  // Start after container header
    uint64_t container_size = containers_[0].capacity_bytes;

    while (chunk_offset < container_size) {
        // Read chunk header
        AlignedBuffer header_buf(config_.layout.block_size_bytes);
        IOResult io_result = io_->read(header_buf.data(),
                                       config_.layout.block_size_bytes,
                                       chunk_offset);

        if (io_result != IOResult::SUCCESS) {
            break;  // End of valid chunks
        }

        RawChunkHeaderV16 chunk_header;
        std::memcpy(&chunk_header, header_buf.data(), sizeof(chunk_header));

        // Check if chunk is deprecated
        if (std::memcmp(chunk_header.magic, kRawChunkMagic, 8) == 0 &&
            chunkIsDeprecated(chunk_header.flags)) {
            // TODO: Implement freeChunk() in StateMutator to mark as FREE
            // For now, we just count deprecated chunks found
            maintenance_stats_.chunks_freed++;
        }

        // Move to next chunk
        chunk_offset += config_.layout.chunk_size_bytes;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::sealCurrentChunk() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Check if there's an active chunk with data
    if (active_chunk_.blocks_used == 0) {
        // No data written, nothing to seal
        return EngineResult::SUCCESS;
    }

    // Flush any pending buffers first
    EngineResult flush_result = flush();
    if (flush_result != EngineResult::SUCCESS) {
        return flush_result;
    }

    // Seal the chunk using ChunkSealer
    ChunkSealer sealer(io_.get(), mutator_.get());
    int64_t final_start_ts = active_chunk_.start_ts_us;
    int64_t final_end_ts = active_chunk_.end_ts_us;

    SealResult seal_result = sealer.sealChunk(active_chunk_.chunk_offset,
                                             config_.layout,
                                             final_start_ts,
                                             final_end_ts);
    if (seal_result != SealResult::SUCCESS) {
        setError("Failed to seal chunk: " + sealer.getLastError());
        return EngineResult::ERROR_CHUNK_ALLOCATION_FAILED;
    }

    write_stats_.chunks_sealed++;

    // Sync chunk metadata to SQLite
    RawScanner scanner(io_.get());
    ScannedChunk scanned_chunk;
    ScanResult scan_result = scanner.scanChunk(active_chunk_.chunk_offset,
                                              config_.layout,
                                              scanned_chunk);
    if (scan_result == ScanResult::SUCCESS) {
        SyncResult sync_result = metadata_->syncChunk(active_chunk_.chunk_offset,
                                                     scanned_chunk);
        if (sync_result != SyncResult::SUCCESS) {
            setError("Failed to sync chunk metadata: " + metadata_->getLastError());
            return EngineResult::ERROR_METADATA_OPEN_FAILED;
        }
    }

    return EngineResult::SUCCESS;
}

}  // namespace xtdb
