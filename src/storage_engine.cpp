#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "xTdb/layout_calculator.h"
#include "xTdb/file_container.h"
#include "xTdb/block_device_container.h"
#include "xTdb/platform_compat.h"
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <chrono>
#ifdef __linux__
#include <linux/falloc.h>
#endif

namespace xtdb {

StorageEngine::StorageEngine(const EngineConfig& config)
    : config_(config), is_open_(false), io_(nullptr), next_io_index_(0),
      wal_flush_running_(false), wal_entries_since_sync_(0) {
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
        return EngineResult::ERR_ENGINE_NOT_OPEN;
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

    // Step 2.5: Initialize parallel execution infrastructure
    // Create thread pool with hardware concurrency
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
        num_threads = 8;  // Fallback to 8 threads
    }
    flush_pool_ = std::make_unique<ThreadPool>(num_threads);

    // Create per-thread I/O instances for parallel flush
    // Each thread needs its own file descriptor for parallel writes
    // Get container path from the writable container
    IContainer* writable_container = container_manager_->getWritableContainer();
    if (!writable_container) {
        setError("No writable container available for thread pool");
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }
    std::string container_path = writable_container->getIdentifier();

    io_pool_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        auto thread_io = std::make_unique<AlignedIO>();
        IOResult io_result = thread_io->open(container_path, false, false);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to open per-thread I/O: " + thread_io->getLastError());
            return EngineResult::ERR_CONTAINER_OPEN_FAILED;
        }
        io_pool_.push_back(std::move(thread_io));
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

    // Step 5: Start async WAL flush thread (Phase 4)
    startAsyncWALFlush();

    is_open_ = true;
    return EngineResult::SUCCESS;
}

void StorageEngine::close() {
    if (!is_open_) {
        return;
    }

    // Phase 4: Stop async WAL flush thread
    stopAsyncWALFlush();

    // Phase 3: Flush all pending WAL batches before closing
    std::vector<uint32_t> tags_with_wal_batches;
    {
        std::lock_guard<std::mutex> lock(wal_batch_mutex_);
        for (const auto& [tag_id, batch] : wal_batches_) {
            if (!batch.empty()) {
                tags_with_wal_batches.push_back(tag_id);
            }
        }
    }

    for (uint32_t tag_id : tags_with_wal_batches) {
        flushWALBatch(tag_id);  // Ignore errors on close
    }

    // Sync and close rotating WAL before closing I/O
    if (rotating_wal_) {
        rotating_wal_->sync();
        rotating_wal_->close();
        rotating_wal_.reset();
    }

    // Clear other components BEFORE closing I/O
    wal_reader_.reset();
    dir_builder_.reset();
    mutator_.reset();

    // Close metadata
    if (metadata_) {
        metadata_->close();
        metadata_.reset();
    }

    // Clear io_ pointer (it's borrowed from container, don't close/delete)
    io_ = nullptr;

    // Close per-thread I/O pool
    for (auto& thread_io : io_pool_) {
        if (thread_io) {
            thread_io->close();
        }
    }
    io_pool_.clear();

    // Close ContainerManager (this closes all containers)
    if (container_manager_) {
        container_manager_->close();
        container_manager_.reset();
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
        return EngineResult::ERR_METADATA_OPEN_FAILED;
    }

    // Initialize schema
    result = metadata_->initSchema();
    if (result != SyncResult::SUCCESS) {
        setError("Failed to init schema: " + metadata_->getLastError());
        return EngineResult::ERR_METADATA_OPEN_FAILED;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::mountContainers() {
    // Build ContainerManager configuration from EngineConfig
    ManagerConfig manager_config;
    manager_config.rollover_strategy = config_.rollover_strategy;
    manager_config.rollover_size_bytes = config_.rollover_size_bytes;
    manager_config.rollover_time_hours = config_.rollover_time_hours;
    manager_config.name_pattern = config_.container_name_pattern;

    // Create container configuration
    ContainerConfig container_config;
    container_config.type = config_.container_type;
    container_config.layout = config_.layout;
    container_config.create_if_not_exists = true;
    container_config.direct_io = config_.direct_io;
    container_config.read_only = false;
    container_config.test_mode = config_.block_device_test_mode;

    // Set container path based on type
    if (config_.container_type == ContainerType::BLOCK_DEVICE) {
        // Use block device path
        if (config_.block_device_path.empty()) {
            setError("Block device path not specified");
            return EngineResult::ERR_CONTAINER_OPEN_FAILED;
        }
        container_config.path = config_.block_device_path;
    } else {
        // Use file-based path with pattern
        // Use platform-independent path separator
        container_config.path = config_.data_dir + PATH_SEPARATOR + config_.container_name_pattern;
        // Replace {index} with 0 for initial container
        size_t pos = container_config.path.find("{index}");
        if (pos != std::string::npos) {
            container_config.path.replace(pos, 7, "0");
        }
    }

    // Add container to manager config
    manager_config.containers.push_back(container_config);

    // Create and initialize ContainerManager
    container_manager_ = std::make_unique<ContainerManager>(manager_config);
    ManagerResult mgr_result = container_manager_->initialize();
    if (mgr_result != ManagerResult::SUCCESS) {
        setError("Failed to initialize container manager: " + container_manager_->getLastError());
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    // Get writable container and register it
    IContainer* writable_container = container_manager_->getWritableContainer();
    if (!writable_container) {
        setError("No writable container available");
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    // Register container info for backward compatibility
    ContainerInfo info;
    info.container_id = 0;
    info.file_path = writable_container->getIdentifier();
    info.capacity_bytes = writable_container->getCapacity();
    info.layout = config_.layout;
    containers_.push_back(info);

    // Set up io_ pointer for backward compatibility with existing code
    // Both container types now provide AlignedIO interface
    if (config_.container_type == ContainerType::FILE_BASED) {
        FileContainer* file_container = dynamic_cast<FileContainer*>(writable_container);
        if (file_container) {
            io_ = file_container->getIO();
        } else {
            setError("Failed to cast container to FileContainer");
            return EngineResult::ERR_CONTAINER_OPEN_FAILED;
        }
    } else if (config_.container_type == ContainerType::BLOCK_DEVICE) {
        BlockDeviceContainer* block_container = dynamic_cast<BlockDeviceContainer*>(writable_container);
        if (block_container) {
            io_ = block_container->getIO();
        } else {
            setError("Failed to cast container to BlockDeviceContainer");
            return EngineResult::ERR_CONTAINER_OPEN_FAILED;
        }
    } else {
        setError("Unknown container type");
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    if (!io_) {
        setError("Failed to get I/O interface from container");
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    // Initialize Rotating WAL (independent container)
    RotatingWALConfig wal_config;
    wal_config.wal_container_path = config_.data_dir + "/wal_container.raw";
    wal_config.num_segments = 4;              // 4 segments for rotation
    wal_config.segment_size_bytes = 64 * 1024 * 1024;  // 64 MB per segment
    wal_config.auto_grow = false;
    wal_config.max_segments = 8;
    wal_config.direct_io = false;

    rotating_wal_ = std::make_unique<RotatingWAL>(wal_config);

    RotatingWALResult wal_result = rotating_wal_->open();
    if (wal_result != RotatingWALResult::SUCCESS) {
        setError("Failed to open rotating WAL: " + rotating_wal_->getLastError());
        return EngineResult::ERR_WAL_OPEN_FAILED;
    }

    // Set flush callback for WAL segment rotation
    rotating_wal_->setFlushCallback(
        [this](uint32_t segment_id, const std::set<uint32_t>& tag_ids) {
            return handleSegmentFull(segment_id, tag_ids);
        }
    );

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::restoreActiveState() {
    // Initialize state mutator
    mutator_ = std::make_unique<StateMutator>(io_);

    // Check if we have any active chunks in SQLite
    // For now, we'll allocate a new chunk at offset kExtentSizeBytes (after container header)
    // This is simplified for Phase 7 - full implementation would query SQLite for active chunks

    // Initialize active chunk info
    // First chunk starts after WAL region (extent 0 = header, extents 1-256 = WAL, extent 257+ = data)
    active_chunk_.chunk_id = 0;
    active_chunk_.chunk_offset = 257 * kExtentSizeBytes;  // After container header and WAL region
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
            RawScanner scanner(io_);
            ScannedChunk scanned_chunk;
            ScanResult scan_result = scanner.scanChunk(active_chunk_.chunk_offset,
                                                      config_.layout,
                                                      scanned_chunk);
            if (scan_result == ScanResult::SUCCESS) {
                active_chunk_.blocks_used = (uint32_t)scanned_chunk.blocks.size();
            }

            // Create DirectoryBuilder and load existing directory from disk
            dir_builder_ = std::make_unique<DirectoryBuilder>(io_,
                                                              config_.layout,
                                                              active_chunk_.chunk_offset);
            DirBuildResult dir_result = dir_builder_->load();
            if (dir_result != DirBuildResult::SUCCESS) {
                setError("Failed to load directory: " + dir_builder_->getLastError());
                return EngineResult::ERR_STATE_RESTORATION_FAILED;
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
    if (!rotating_wal_ || !io_) {
        setError("Rotating WAL or I/O not initialized");
        return EngineResult::ERR_WAL_OPEN_FAILED;
    }

    // Scan all segments to find entries
    const std::vector<WALSegment>& segments = rotating_wal_->getSegments();

    // Collect entries from all segments
    std::unordered_map<uint32_t, std::vector<WALEntry>> entries_by_tag;
    uint64_t total_entries_replayed = 0;

    // Get AlignedIO for WAL container
    std::string wal_path = config_.data_dir + "/wal_container.raw";
    AlignedIO wal_io;
    IOResult io_result = wal_io.open(wal_path, false, false);  // Read-only
    if (io_result != IOResult::SUCCESS) {
        // If WAL container doesn't exist or can't be opened, skip replay
        return EngineResult::SUCCESS;
    }

    for (const auto& segment : segments) {
        // Skip empty segments
        if (segment.entry_count == 0 || segment.write_position == 0) {
            continue;
        }

        // Create WAL reader for this segment
        WALReader reader(&wal_io, segment.start_offset, segment.segment_size);

        // Read all entries from this segment
        WALEntry entry;
        while (reader.readNext(entry) == WALResult::SUCCESS) {
            // Group entries by tag_id
            entries_by_tag[entry.tag_id].push_back(entry);
            total_entries_replayed++;
        }
    }

    // If no entries found, nothing to replay
    if (total_entries_replayed == 0) {
        return EngineResult::SUCCESS;
    }

    // Reconstruct and flush tag buffers
    for (auto& [tag_id, entries] : entries_by_tag) {
        // Sort entries by timestamp for correct ordering
        std::sort(entries.begin(), entries.end(),
                  [](const WALEntry& a, const WALEntry& b) {
                      return a.timestamp_us < b.timestamp_us;
                  });

        // Reconstruct tag buffer
        for (const auto& entry : entries) {
            // Extract value based on value_type
            double value_double = 0.0;
            ValueType vt = static_cast<ValueType>(entry.value_type);
            switch (vt) {
                case ValueType::VT_BOOL:
                    value_double = entry.value.bool_value ? 1.0 : 0.0;
                    break;
                case ValueType::VT_I32:
                    value_double = static_cast<double>(entry.value.i32_value);
                    break;
                case ValueType::VT_F32:
                    value_double = static_cast<double>(entry.value.f32_value);
                    break;
                case ValueType::VT_F64:
                    value_double = entry.value.f64_value;
                    break;
            }

            // Use writePoint to replay each entry
            EngineResult result = writePoint(tag_id, entry.timestamp_us,
                                            value_double, entry.quality);
            if (result != EngineResult::SUCCESS) {
                // Log error but continue with other entries
                std::cerr << "[StorageEngine] Warning: Failed to replay WAL entry for tag "
                          << tag_id << " at timestamp " << entry.timestamp_us << std::endl;
            }
        }
    }

    // Flush all pending tag buffers after replay
    EngineResult flush_result = flush();
    if (flush_result != EngineResult::SUCCESS) {
        setError("Failed to flush buffers after WAL replay");
        return flush_result;
    }

    std::cout << "[StorageEngine] WAL replay completed: " << total_entries_replayed
              << " entries replayed for " << entries_by_tag.size() << " tags" << std::endl;

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::verifyContainerHeader(const std::string& container_path,
                                                 ContainerHeaderV12& header) {
    // Read header
    AlignedBuffer buf(kExtentSizeBytes);
    IOResult result = io_->read(buf.data(), kExtentSizeBytes, 0);
    if (result != IOResult::SUCCESS) {
        setError("Failed to read container header: " + io_->getLastError());
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    // Copy header
    std::memcpy(&header, buf.data(), sizeof(header));

    // Verify magic
    if (std::memcmp(header.magic, kContainerMagic, 8) != 0) {
        setError("Invalid container magic");
        return EngineResult::ERR_CONTAINER_HEADER_INVALID;
    }

    // Verify version (0x0102 for V12)
    if (header.version != 0x0102) {
        setError("Unsupported container version");
        return EngineResult::ERR_CONTAINER_HEADER_INVALID;
    }

    // Verify file size - check that file has at least the header
    struct stat st;
    if (stat(container_path.c_str(), &st) != 0) {
        setError("Failed to stat container file");
        return EngineResult::ERR_CONTAINER_OPEN_FAILED;
    }

    // Verify container has minimum size (header + initial space)
    if (static_cast<uint64_t>(st.st_size) < kExtentSizeBytes) {
        setError("Container file too small");
        return EngineResult::ERR_CONTAINER_HEADER_INVALID;
    }

    // Calculate expected container capacity
    uint64_t expected_capacity = header.capacity_extents * kExtentSizeBytes;

    // Check if file size matches expected capacity
    // Allow some tolerance for filesystem overhead
    if (static_cast<uint64_t>(st.st_size) < expected_capacity) {
        // File is smaller than expected capacity
        // This could happen if container was not properly pre-allocated
        // For now, log a warning but don't fail
        std::cerr << "[StorageEngine] Warning: Container file size ("
                  << st.st_size << " bytes) is smaller than declared capacity ("
                  << expected_capacity << " bytes). Container may not be pre-allocated."
                  << std::endl;
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
        return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
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
            return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
        }
    }

    // Set chunk as allocated (clear ALLOCATED bit)
    MutateResult mut_result = mutator_->allocateChunk(chunk_offset);
    if (mut_result != MutateResult::SUCCESS) {
        setError("Failed to set chunk state: " + mutator_->getLastError());
        return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
    }

    // Create DirectoryBuilder for this chunk
    dir_builder_ = std::make_unique<DirectoryBuilder>(io_,
                                                      config_.layout,
                                                      chunk_offset);
    DirBuildResult dir_result = dir_builder_->initialize();
    if (dir_result != DirBuildResult::SUCCESS) {
        setError("Failed to initialize directory: " + dir_builder_->getLastError());
        return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::writePoint(uint32_t tag_id,
                                       int64_t timestamp_us,
                                       double value,
                                       uint8_t quality) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
    }

    // Step 1: WAL Batch Buffering (Phase 3)
    // Instead of directly appending to WAL, batch entries per-tag
    WALEntry entry;
    entry.tag_id = tag_id;
    entry.timestamp_us = timestamp_us;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.value.f64_value = value;
    entry.quality = quality;

    // Add to per-tag WAL batch
    // Phase 4: Use async flush - notify background thread instead of blocking
    bool should_notify_flush = false;
    if (rotating_wal_) {
        std::lock_guard<std::mutex> wal_lock(wal_batch_mutex_);
        wal_batches_[tag_id].push_back(entry);

        // Check if batch is full (threshold: 100 entries)
        // Background thread will flush at 50% threshold, but we also notify at 100%
        if (wal_batches_[tag_id].size() >= kWALBatchSize) {
            should_notify_flush = true;
        }
    }

    // Notify background flush thread (non-blocking)
    if (should_notify_flush) {
        std::lock_guard<std::mutex> lock(wal_flush_mutex_);
        wal_flush_cv_.notify_one();
    }

    // Step 2: Add point to memory buffer
    // Acquire unique lock for thread-safe buffer modification
    std::unique_lock<std::shared_mutex> lock(buffers_mutex_);

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

    // Check if buffer needs flush before releasing lock
    bool needs_flush = tag_buffer.records.size() >= 1000;
    lock.unlock();  // Release lock before flush

    // Step 3: Check if buffer needs flush (threshold: 1000 records or ~16KB)
    if (needs_flush) {
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

// New writePoint with TagConfig support
EngineResult StorageEngine::writePoint(const TagConfig* config,
                                      int64_t timestamp_us,
                                      double value,
                                      uint8_t quality) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
    }

    if (!config) {
        setError("Tag configuration pointer is null");
        return EngineResult::ERR_INVALID_DATA;
    }

    uint32_t tag_id = config->tag_id;

    // Step 1: Write to WAL with batching
    if (rotating_wal_) {
        WALEntry entry;
        entry.tag_id = tag_id;
        entry.timestamp_us = timestamp_us;
        entry.value_type = static_cast<uint8_t>(config->value_type);
        entry.quality = quality;
        entry.value.f64_value = value;  // Assuming double for now

        // Add to batch
        bool should_notify_flush = false;
        {
            std::lock_guard<std::mutex> lock(wal_batch_mutex_);
            wal_batches_[tag_id].push_back(entry);

            // Trigger flush when batch is full
            if (wal_batches_[tag_id].size() >= kWALBatchSize) {
                should_notify_flush = true;
            }
        }

        // Notify background flush thread (non-blocking)
        if (should_notify_flush) {
            std::lock_guard<std::mutex> lock(wal_flush_mutex_);
            wal_flush_cv_.notify_one();
        }
    }

    // Step 2: Add point to memory buffer
    std::unique_lock<std::shared_mutex> lock(buffers_mutex_);

    // Find or create TagBuffer for this tag
    auto it = buffers_.find(tag_id);
    if (it == buffers_.end()) {
        // Create new buffer with configuration from upper layer
        TagBuffer new_buffer;
        new_buffer.tag_id = tag_id;
        new_buffer.value_type = config->value_type;
        new_buffer.time_unit = config->time_unit;
        new_buffer.encoding_type = config->encoding_type;
        new_buffer.encoding_tolerance = config->encoding_param1;
        new_buffer.encoding_compression_factor = config->encoding_param2;
        new_buffer.start_ts_us = timestamp_us;

        buffers_[tag_id] = new_buffer;
        it = buffers_.find(tag_id);

        // Optional: Log tag creation with name if provided (for debugging)
        if (config->tag_name != nullptr) {
            // Debug logging could be added here if needed
            // printf("Created buffer for tag %u: %s\n", tag_id, config->tag_name);
        }
    }

    // Add record to buffer
    TagBuffer& tag_buffer = it->second;

    // Create MemRecord
    MemRecord record;
    record.time_offset = static_cast<uint32_t>((timestamp_us - tag_buffer.start_ts_us) / 1000);
    record.quality = quality;
    record.value.f64_value = value;
    tag_buffer.records.push_back(record);

    write_stats_.points_written++;

    // Check if buffer needs flush before releasing lock
    bool needs_flush = tag_buffer.records.size() >= 1000;
    lock.unlock();

    // Step 3: Check if buffer needs flush
    if (needs_flush) {
        EngineResult flush_result = flush();
        if (flush_result != EngineResult::SUCCESS) {
            return flush_result;
        }
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::flushWALBatch(uint32_t tag_id) {
    if (!rotating_wal_) {
        return EngineResult::SUCCESS;  // No WAL, skip
    }

    // Get batch entries for this tag
    std::vector<WALEntry> batch_to_flush;
    {
        std::lock_guard<std::mutex> lock(wal_batch_mutex_);
        auto it = wal_batches_.find(tag_id);
        if (it != wal_batches_.end() && !it->second.empty()) {
            batch_to_flush = std::move(it->second);
            it->second.clear();  // Clear the batch
        }
    }

    if (batch_to_flush.empty()) {
        return EngineResult::SUCCESS;
    }

    // Batch append to WAL
    RotatingWALResult wal_result = rotating_wal_->batchAppend(batch_to_flush);
    if (wal_result != RotatingWALResult::SUCCESS) {
        setError("WAL batch append failed: " + rotating_wal_->getLastError());
        return EngineResult::ERR_WAL_OPEN_FAILED;
    }

    // Periodic WAL sync (every 10000 entries for better performance)
    wal_entries_since_sync_ += (uint32_t)batch_to_flush.size();
    if (wal_entries_since_sync_ >= 10000) {
        rotating_wal_->sync();
        wal_entries_since_sync_ = 0;
    }

    return EngineResult::SUCCESS;
}

void StorageEngine::startAsyncWALFlush() {
    wal_flush_running_.store(true);
    wal_flush_thread_ = std::make_unique<std::thread>(&StorageEngine::walFlushThreadFunc, this);
}

void StorageEngine::stopAsyncWALFlush() {
    if (!wal_flush_thread_) {
        return;
    }

    // Signal thread to stop
    wal_flush_running_.store(false);

    // Wake up thread if sleeping
    {
        std::lock_guard<std::mutex> lock(wal_flush_mutex_);
        wal_flush_cv_.notify_one();
    }

    // Wait for thread to finish
    if (wal_flush_thread_->joinable()) {
        wal_flush_thread_->join();
    }

    wal_flush_thread_.reset();
}

void StorageEngine::walFlushThreadFunc() {
    // Phase 4: Background WAL flush thread
    // Periodically checks for batches that need flushing
    while (wal_flush_running_.load()) {
        // Wait for 10ms or until notified
        {
            std::unique_lock<std::mutex> lock(wal_flush_mutex_);
            wal_flush_cv_.wait_for(lock, std::chrono::milliseconds(10));
        }

        if (!wal_flush_running_.load()) {
            break;
        }

        // Collect tags with batches >= kAsyncWALThreshold (50% of max)
        std::vector<uint32_t> tags_to_flush;
        {
            std::lock_guard<std::mutex> lock(wal_batch_mutex_);
            for (const auto& [tag_id, batch] : wal_batches_) {
                if (batch.size() >= kAsyncWALThreshold) {
                    tags_to_flush.push_back(tag_id);
                }
            }
        }

        // Flush batches asynchronously
        for (uint32_t tag_id : tags_to_flush) {
            flushWALBatch(tag_id);  // Ignore errors in background thread
        }
    }
}

EngineResult StorageEngine::flush() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
    }

    // Phase 3: Flush all pending WAL batches before flushing buffers
    std::vector<uint32_t> tags_with_wal_batches;
    {
        std::lock_guard<std::mutex> lock(wal_batch_mutex_);
        for (const auto& [tag_id, batch] : wal_batches_) {
            if (!batch.empty()) {
                tags_with_wal_batches.push_back(tag_id);
            }
        }
    }

    for (uint32_t tag_id : tags_with_wal_batches) {
        EngineResult wal_result = flushWALBatch(tag_id);
        if (wal_result != EngineResult::SUCCESS) {
            return wal_result;
        }
    }

    // Phase 2: Parallel flush implementation
    // Step 1: Collect non-empty buffers and prepare for parallel flush
    std::vector<std::pair<uint32_t, TagBuffer>> buffers_to_flush;
    buffers_to_flush.reserve(buffers_.size());  // Pre-allocate to avoid reallocation during emplace_back
    {
        std::unique_lock<std::shared_mutex> buffers_lock(buffers_mutex_);
        for (auto& [tag_id, tag_buffer] : buffers_) {
            if (!tag_buffer.records.empty()) {
                // Move the buffer to avoid expensive copy of large vector
                // This avoids copying large std::vector<MemRecord> which can cause
                // memory issues during vector reallocation
                buffers_to_flush.emplace_back(tag_id, std::move(tag_buffer));
                // Re-initialize the moved-from buffer for reuse
                tag_buffer = TagBuffer();
                tag_buffer.tag_id = tag_id;  // Restore tag_id for future use
            }
        }
    }  // Release buffers lock

    if (buffers_to_flush.empty()) {
        return EngineResult::SUCCESS;
    }

    // Step 2: Check if we need to allocate more space
    {
        std::lock_guard<std::mutex> chunk_lock(active_chunk_mutex_);
        if (active_chunk_.blocks_used >= active_chunk_.blocks_total) {
            // Chunk is full, need to seal and allocate new one

            // Ensure directory is written to disk before sealing
            if (dir_builder_) {
                DirBuildResult dir_result = dir_builder_->writeDirectory();
                if (dir_result != DirBuildResult::SUCCESS) {
                    setError("Failed to write directory before sealing: " + dir_builder_->getLastError());
                    return EngineResult::ERR_INVALID_DATA;
                }
                // Sync directory to disk
                IOResult sync_result = io_->sync();
                if (sync_result != IOResult::SUCCESS) {
                    setError("Failed to sync directory before sealing: " + io_->getLastError());
                    return EngineResult::ERR_INVALID_DATA;
                }
            }

            // Seal current chunk
            ChunkSealer sealer(io_, mutator_.get());
            int64_t final_start_ts = active_chunk_.start_ts_us;
            int64_t final_end_ts = active_chunk_.end_ts_us;

            SealResult seal_result = sealer.sealChunk(active_chunk_.chunk_offset,
                                                     config_.layout,
                                                     final_start_ts,
                                                     final_end_ts);
            if (seal_result != SealResult::SUCCESS) {
                setError("Failed to seal chunk: " + sealer.getLastError());
                return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
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
    }  // Release chunk lock

    // Step 3: Submit parallel block write tasks
    // Structure to collect write results for batch directory update
    struct WriteResult {
        uint32_t tag_id;
        uint32_t block_index;
        uint32_t data_crc32;
        int64_t block_start_ts;
        int64_t block_end_ts;
        TimeUnit time_unit;
        ValueType value_type;
        uint32_t record_count;
        EncodingType encoding_type;
        uint32_t encoding_param1;
        uint32_t encoding_param2;
        bool success;
        std::string error_msg;
    };

    std::vector<std::future<WriteResult>> write_futures;
    write_futures.reserve(buffers_to_flush.size());

    // Submit each buffer flush as a parallel task
    for (size_t i = 0; i < buffers_to_flush.size(); ++i) {
        auto& [tag_id, tag_buffer] = buffers_to_flush[i];

        // Allocate block index (thread-safe)
        uint32_t block_index;
        uint64_t chunk_offset;
        {
            std::lock_guard<std::mutex> chunk_lock(active_chunk_mutex_);
            block_index = active_chunk_.blocks_used++;
            chunk_offset = active_chunk_.chunk_offset;
        }

        // Get per-thread I/O instance (round-robin)
        size_t io_index = next_io_index_.fetch_add(1) % io_pool_.size();
        AlignedIO* thread_io = io_pool_[io_index].get();

        // Extract metadata before moving tag_buffer (needed for result)
        int64_t start_ts_us = tag_buffer.start_ts_us;
        TimeUnit time_unit = tag_buffer.time_unit;
        ValueType value_type = tag_buffer.value_type;
        EncodingType encoding_type = tag_buffer.encoding_type;
        double encoding_tolerance = tag_buffer.encoding_tolerance;
        double encoding_compression_factor = tag_buffer.encoding_compression_factor;
        size_t record_count = tag_buffer.records.size();
        
        // Calculate max time offset before moving (needed for block_end_ts)
        uint32_t max_offset = 0;
        if (!tag_buffer.records.empty()) {
            for (const auto& rec : tag_buffer.records) {
                if (rec.time_offset > max_offset) {
                    max_offset = rec.time_offset;
                }
            }
        }

        // Submit task to thread pool - MOVE tag_buffer to avoid expensive copy
        // This prevents memory allocation failures when buffers contain large vectors
        auto future = flush_pool_->submit([this, tag_id, tag_buffer = std::move(tag_buffer),
                                           block_index, chunk_offset, thread_io,
                                           start_ts_us, time_unit, value_type, encoding_type,
                                           encoding_tolerance, encoding_compression_factor,
                                           record_count, max_offset]() -> WriteResult {
            WriteResult result;
            result.tag_id = tag_id;
            result.block_index = block_index;
            result.success = false;

            // Write block to disk using per-thread BlockWriter
            BlockWriter writer(thread_io, config_.layout, kExtentSizeBytes);

            BlockWriteResult write_result = writer.writeBlock(chunk_offset,
                                                             block_index,
                                                             tag_buffer,
                                                             &result.data_crc32);
            if (write_result != BlockWriteResult::SUCCESS) {
                result.error_msg = "Failed to write block: " + writer.getLastError();
                return result;
            }

            // Calculate timestamp range (using captured values)
            result.block_start_ts = start_ts_us;
            result.block_end_ts = start_ts_us + static_cast<int64_t>(max_offset) * 1000;

            // Store metadata for directory update (using captured values)
            result.time_unit = time_unit;
            result.value_type = value_type;
            result.record_count = static_cast<uint32_t>(record_count);
            result.encoding_type = encoding_type;

            // Convert encoding parameters
            float tolerance_f = static_cast<float>(encoding_tolerance);
            float compression_factor_f = static_cast<float>(encoding_compression_factor);
            std::memcpy(&result.encoding_param1, &tolerance_f, 4);
            std::memcpy(&result.encoding_param2, &compression_factor_f, 4);

            result.success = true;
            return result;
        });

        write_futures.push_back(std::move(future));
    }

    // Step 4: Wait for all writes to complete (with timeout to prevent deadlock)
    std::vector<WriteResult> write_results;
    write_results.reserve(write_futures.size());

    for (auto& future : write_futures) {
        // Use wait_for with timeout to detect deadlock
        auto status = future.wait_for(std::chrono::seconds(30));
        if (status == std::future_status::timeout) {
            setError("Timeout waiting for flush task completion - possible deadlock");
            return EngineResult::ERR_INVALID_DATA;
        }
        write_results.push_back(future.get());
    }

    // Step 5: Check for errors
    for (const auto& result : write_results) {
        if (!result.success) {
            setError(result.error_msg);
            return EngineResult::ERR_INVALID_DATA;
        }
    }

    // Step 6: Batch directory updates (single-threaded for now)
    if (!dir_builder_) {
        setError("Directory builder not initialized");
        return EngineResult::ERR_INVALID_DATA;
    }

    for (const auto& result : write_results) {
        DirBuildResult dir_result = dir_builder_->sealBlock(
            result.block_index,
            result.tag_id,
            result.block_start_ts,
            result.block_end_ts,
            result.time_unit,
            result.value_type,
            result.record_count,
            result.data_crc32,
            result.encoding_type,
            result.encoding_param1,
            result.encoding_param2
        );

        if (dir_result != DirBuildResult::SUCCESS) {
            setError("Failed to seal block in directory: " + dir_builder_->getLastError());
            return EngineResult::ERR_INVALID_DATA;
        }

        // Update stats
        write_stats_.blocks_flushed++;

        // Update active chunk timestamp tracking
        {
            std::lock_guard<std::mutex> chunk_lock(active_chunk_mutex_);
            if (active_chunk_.start_ts_us == 0) {
                active_chunk_.start_ts_us = result.block_start_ts;
            }
            if (result.block_end_ts > active_chunk_.end_ts_us) {
                active_chunk_.end_ts_us = result.block_end_ts;
            }
        }
    }

    // Write directory to disk once (batch update)
    DirBuildResult dir_result = dir_builder_->writeDirectory();
    if (dir_result != DirBuildResult::SUCCESS) {
        setError("Failed to write directory: " + dir_builder_->getLastError());
        return EngineResult::ERR_INVALID_DATA;
    }

    // Note: With rotating WAL, segment clearing is handled by
    // the handleSegmentFull callback during rotation.
    // No manual WAL reset needed here.

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::queryPoints(uint32_t tag_id,
                                        int64_t start_ts_us,
                                        int64_t end_ts_us,
                                        std::vector<QueryPoint>& results) {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
    }

    results.clear();
    read_stats_.queries_executed++;

    // Step 1: Read from memory buffers (unflushed data)
    // Acquire shared lock for thread-safe buffer reading
    {
        std::shared_lock<std::shared_mutex> lock(buffers_mutex_);
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
    }  // Release buffers lock before disk I/O

    // Step 2: Read from disk blocks (Phase 4: Parallel)
    // Acquire shared lock for thread-safe active_chunk reading
    uint64_t chunk_offset;
    uint32_t blocks_used;
    {
        std::lock_guard<std::mutex> lock(active_chunk_mutex_);
        chunk_offset = active_chunk_.chunk_offset;
        blocks_used = active_chunk_.blocks_used;
    }

    if (blocks_used > 0) {
        // Step 2.1: Scan directory to find matching blocks
        RawScanner scanner(io_);
        ScannedChunk scanned_chunk;

        ScanResult scan_result = scanner.scanChunk(chunk_offset,
                                                   config_.layout,
                                                   scanned_chunk);

        if (scan_result == ScanResult::SUCCESS) {
            // Step 2.2: Filter blocks by tag and time range
            std::vector<ScannedBlock> blocks_to_read;
            for (const auto& block_info : scanned_chunk.blocks) {
                if (block_info.tag_id != tag_id) {
                    continue;
                }

                if (block_info.end_ts_us < start_ts_us ||
                    block_info.start_ts_us > end_ts_us) {
                    continue;
                }

                blocks_to_read.push_back(block_info);
            }

            // Step 2.3: Parallel block reading using thread pool
            if (!blocks_to_read.empty()) {
                struct BlockReadResult {
                    bool success;
                    std::vector<QueryPoint> points;
                    std::string error_msg;
                };

                std::vector<std::future<BlockReadResult>> read_futures;
                read_futures.reserve(blocks_to_read.size());

                for (const auto& block_info : blocks_to_read) {
                    // Get per-thread I/O instance
                    size_t io_index = next_io_index_.fetch_add(1) % io_pool_.size();
                    AlignedIO* thread_io = io_pool_[io_index].get();

                    // Submit block read task
                    auto future = flush_pool_->submit([this, chunk_offset, block_info,
                                                       thread_io, start_ts_us, end_ts_us]() -> BlockReadResult {
                        BlockReadResult result;
                        result.success = false;

                        // Read block using per-thread BlockReader
                        BlockReader reader(thread_io, config_.layout);

                        std::vector<MemRecord> records;
                        ReadResult read_result = reader.readBlock(
                            chunk_offset,
                            block_info.block_index,
                            block_info.tag_id,
                            block_info.start_ts_us,
                            block_info.time_unit,
                            block_info.value_type,
                            block_info.record_count,
                            records
                        );

                        if (read_result != ReadResult::SUCCESS) {
                            result.error_msg = "Failed to read block";
                            return result;
                        }

                        // Filter and convert records to QueryPoint
                        for (const auto& record : records) {
                            int64_t timestamp_us = block_info.start_ts_us +
                                                 static_cast<int64_t>(record.time_offset) * 1000;

                            if (timestamp_us >= start_ts_us && timestamp_us <= end_ts_us) {
                                result.points.emplace_back(timestamp_us,
                                                          record.value.f64_value,
                                                          record.quality);
                            }
                        }

                        result.success = true;
                        return result;
                    });

                    read_futures.push_back(std::move(future));
                }

                // Step 2.4: Wait for all reads and aggregate results
                for (auto& future : read_futures) {
                    BlockReadResult read_result = future.get();

                    if (read_result.success) {
                        read_stats_.blocks_read++;
                        read_stats_.points_read_disk += read_result.points.size();

                        // Append results
                        results.insert(results.end(),
                                      read_result.points.begin(),
                                      read_result.points.end());
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
        return EngineResult::ERR_ENGINE_NOT_OPEN;
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
        return EngineResult::ERR_METADATA_OPEN_FAILED;
    }

    maintenance_stats_.last_retention_run_ts = current_time_us;
    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::reclaimDeprecatedChunks() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
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
            // Mark chunk as free for reuse
            MutateResult result = mutator_->markChunkFree(chunk_offset);
            if (result == MutateResult::SUCCESS) {
                maintenance_stats_.chunks_freed++;
            } else {
                std::cerr << "[StorageEngine] Failed to free deprecated chunk at offset "
                          << chunk_offset << ": " << mutator_->getLastError() << std::endl;
            }
        }

        // Move to next chunk
        chunk_offset += config_.layout.chunk_size_bytes;
    }

    return EngineResult::SUCCESS;
}

EngineResult StorageEngine::sealCurrentChunk() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERR_ENGINE_NOT_OPEN;
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
    ChunkSealer sealer(io_, mutator_.get());
    int64_t final_start_ts = active_chunk_.start_ts_us;
    int64_t final_end_ts = active_chunk_.end_ts_us;

    SealResult seal_result = sealer.sealChunk(active_chunk_.chunk_offset,
                                             config_.layout,
                                             final_start_ts,
                                             final_end_ts);
    if (seal_result != SealResult::SUCCESS) {
        setError("Failed to seal chunk: " + sealer.getLastError());
        return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
    }

    write_stats_.chunks_sealed++;

    // Sync chunk metadata to SQLite
    RawScanner scanner(io_);
    ScannedChunk scanned_chunk;
    ScanResult scan_result = scanner.scanChunk(active_chunk_.chunk_offset,
                                              config_.layout,
                                              scanned_chunk);
    if (scan_result == ScanResult::SUCCESS) {
        SyncResult sync_result = metadata_->syncChunk(active_chunk_.chunk_offset,
                                                     scanned_chunk);
        if (sync_result != SyncResult::SUCCESS) {
            setError("Failed to sync chunk metadata: " + metadata_->getLastError());
            return EngineResult::ERR_METADATA_OPEN_FAILED;
        }
    }

    return EngineResult::SUCCESS;
}

bool StorageEngine::handleSegmentFull(uint32_t segment_id,
                                       const std::set<uint32_t>& tag_ids) {
    // Flush all buffers for the tags in this segment
    for (uint32_t tag_id : tag_ids) {
        auto it = buffers_.find(tag_id);
        if (it == buffers_.end()) {
            continue;  // Tag buffer doesn't exist, skip
        }

        TagBuffer& tag_buffer = it->second;

        // If buffer has data, flush it to disk
        if (!tag_buffer.records.empty()) {
            EngineResult result = flushSingleTag(tag_id, tag_buffer);
            if (result != EngineResult::SUCCESS) {
                std::cerr << "[StorageEngine] Failed to flush tag " << tag_id
                          << " during segment rotation" << std::endl;
                return false;  // Flush failed
            }
        }
    }

    // Clear the WAL segment after successful flush
    RotatingWALResult result = rotating_wal_->clearSegment(segment_id);
    if (result != RotatingWALResult::SUCCESS) {
        std::cerr << "[StorageEngine] Failed to clear WAL segment " << segment_id
                  << ": " << rotating_wal_->getLastError() << std::endl;
        return false;
    }

    return true;  // Success
}

EngineResult StorageEngine::flushSingleTag(uint32_t tag_id, TagBuffer& tag_buffer) {
    // Check if buffer has data
    if (tag_buffer.records.empty()) {
        return EngineResult::SUCCESS;
    }

    // Check if we need to roll (allocate new chunk)
    if (active_chunk_.blocks_used >= active_chunk_.blocks_total) {
        // Chunk is full, need to seal and allocate new one

        // Ensure directory is written to disk before sealing
        if (dir_builder_) {
            DirBuildResult dir_result = dir_builder_->writeDirectory();
            if (dir_result != DirBuildResult::SUCCESS) {
                setError("Failed to write directory before sealing: " + dir_builder_->getLastError());
                return EngineResult::ERR_INVALID_DATA;
            }
            // Sync directory to disk
            IOResult sync_result = io_->sync();
            if (sync_result != IOResult::SUCCESS) {
                setError("Failed to sync directory before sealing: " + io_->getLastError());
                return EngineResult::ERR_INVALID_DATA;
            }
        }

        // Seal current chunk
        ChunkSealer sealer(io_, mutator_.get());
        int64_t final_start_ts = active_chunk_.start_ts_us;
        int64_t final_end_ts = active_chunk_.end_ts_us;

        SealResult seal_result = sealer.sealChunk(active_chunk_.chunk_offset,
                                                 config_.layout,
                                                 final_start_ts,
                                                 final_end_ts);
        if (seal_result != SealResult::SUCCESS) {
            setError("Failed to seal chunk: " + sealer.getLastError());
            return EngineResult::ERR_CHUNK_ALLOCATION_FAILED;
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
    BlockWriter writer(io_, config_.layout, kExtentSizeBytes);

    uint32_t data_block_index = active_chunk_.blocks_used;
    uint32_t data_crc32 = 0;
    BlockWriteResult write_result = writer.writeBlock(active_chunk_.chunk_offset,
                                                     data_block_index,
                                                     tag_buffer,
                                                     &data_crc32);
    if (write_result != BlockWriteResult::SUCCESS) {
        setError("Failed to write block: " + writer.getLastError());
        return EngineResult::ERR_INVALID_DATA;
    }

    write_stats_.blocks_flushed++;

    // Update directory entry using persistent dir_builder
    if (!dir_builder_) {
        setError("Directory builder not initialized");
        return EngineResult::ERR_INVALID_DATA;
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

    // Convert encoding parameters to uint32_t (store as float bits)
    float tolerance_f = static_cast<float>(tag_buffer.encoding_tolerance);
    float compression_factor_f = static_cast<float>(tag_buffer.encoding_compression_factor);
    uint32_t encoding_param1, encoding_param2;
    std::memcpy(&encoding_param1, &tolerance_f, 4);
    std::memcpy(&encoding_param2, &compression_factor_f, 4);

    DirBuildResult dir_result = dir_builder_->sealBlock(
        data_block_index,
        tag_id,
        block_start_ts,
        block_end_ts,
        tag_buffer.time_unit,
        tag_buffer.value_type,
        static_cast<uint32_t>(tag_buffer.records.size()),
        data_crc32,  // Use calculated CRC32
        tag_buffer.encoding_type,
        encoding_param1,
        encoding_param2
    );

    if (dir_result != DirBuildResult::SUCCESS) {
        setError("Failed to seal block in directory: " + dir_builder_->getLastError());
        return EngineResult::ERR_INVALID_DATA;
    }

    // Write directory to disk
    dir_result = dir_builder_->writeDirectory();
    if (dir_result != DirBuildResult::SUCCESS) {
        setError("Failed to write directory: " + dir_builder_->getLastError());
        return EngineResult::ERR_INVALID_DATA;
    }

    // Update active chunk tracking
    active_chunk_.blocks_used++;
    if (active_chunk_.start_ts_us == 0) {
        active_chunk_.start_ts_us = block_start_ts;
    }
    active_chunk_.end_ts_us = block_end_ts;

    // Clear buffer after successful write
    tag_buffer.records.clear();

    return EngineResult::SUCCESS;
}

}  // namespace xtdb
