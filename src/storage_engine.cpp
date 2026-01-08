#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include "xTdb/layout_calculator.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
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
    : config_(config), is_open_(false), next_io_index_(0), wal_entries_since_sync_(0) {
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

    // Step 2.5: Initialize parallel execution infrastructure
    // Create thread pool with hardware concurrency
    size_t num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
        num_threads = 8;  // Fallback to 8 threads
    }
    flush_pool_ = std::make_unique<ThreadPool>(num_threads);

    // Create per-thread I/O instances for parallel flush
    // Each thread needs its own file descriptor for parallel writes
    std::string container_path = config_.data_dir + "/container_0.raw";
    io_pool_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        auto thread_io = std::make_unique<AlignedIO>();
        IOResult io_result = thread_io->open(container_path, false, false);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to open per-thread I/O: " + thread_io->getLastError());
            return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
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

    is_open_ = true;
    return EngineResult::SUCCESS;
}

void StorageEngine::close() {
    if (!is_open_) {
        return;
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

        // Initialize WAL region (extent 1-256, 4 MB total)
        // Zero-fill the WAL region so it's ready for use
        AlignedBuffer zero_buf(kExtentSizeBytes);
        std::memset(zero_buf.data(), 0, kExtentSizeBytes);
        for (uint32_t i = 1; i <= 256; i++) {
            io_result = io_->write(zero_buf.data(), kExtentSizeBytes, i * kExtentSizeBytes);
            if (io_result != IOResult::SUCCESS) {
                setError("Failed to initialize WAL region: " + io_->getLastError());
                return EngineResult::ERROR_CONTAINER_OPEN_FAILED;
            }
        }

        // Pre-allocate container space for better performance
        // This ensures the entire container capacity is allocated upfront
        uint64_t container_capacity = header.capacity_extents * kExtentSizeBytes;
        int fd = io_->getFd();
        if (fd >= 0) {
#ifdef __linux__
            // Use fallocate on Linux for efficient space allocation
            int fallocate_result = fallocate(fd, 0, 0, container_capacity);
            if (fallocate_result != 0) {
                // fallocate failed, try fallback method
                std::cerr << "[StorageEngine] Warning: fallocate failed (errno=" << errno
                          << "), using fallback pre-allocation method" << std::endl;
                // Fallback: write a single byte at the end to extend file
                AlignedBuffer end_buf(kExtentSizeBytes);
                std::memset(end_buf.data(), 0, kExtentSizeBytes);
                io_result = io_->write(end_buf.data(), kExtentSizeBytes,
                                      container_capacity - kExtentSizeBytes);
                if (io_result != IOResult::SUCCESS) {
                    std::cerr << "[StorageEngine] Warning: Container pre-allocation failed, "
                              << "continuing without pre-allocation" << std::endl;
                }
            }
#else
            // Non-Linux: use fallback method (write at end)
            AlignedBuffer end_buf(kExtentSizeBytes);
            std::memset(end_buf.data(), 0, kExtentSizeBytes);
            io_result = io_->write(end_buf.data(), kExtentSizeBytes,
                                  container_capacity - kExtentSizeBytes);
            if (io_result != IOResult::SUCCESS) {
                std::cerr << "[StorageEngine] Warning: Container pre-allocation failed, "
                          << "continuing without pre-allocation" << std::endl;
            }
#endif
        }

        // Register container
        ContainerInfo info;
        info.container_id = 0;
        info.file_path = container_path;
        info.capacity_bytes = header.capacity_extents * kExtentSizeBytes;
        info.layout = config_.layout;
        containers_.push_back(info);
    }
    else {
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
        return EngineResult::ERROR_WAL_OPEN_FAILED;
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
    mutator_ = std::make_unique<StateMutator>(io_.get());

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
    if (!rotating_wal_ || !io_) {
        setError("Rotating WAL or I/O not initialized");
        return EngineResult::ERROR_WAL_OPEN_FAILED;
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

    // Verify container has minimum size (header + initial space)
    if (static_cast<uint64_t>(st.st_size) < kExtentSizeBytes) {
        setError("Container file too small");
        return EngineResult::ERROR_CONTAINER_HEADER_INVALID;
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
    WALEntry entry;
    entry.tag_id = tag_id;
    entry.timestamp_us = timestamp_us;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.value.f64_value = value;
    entry.quality = quality;

    // Write to Rotating WAL
    if (rotating_wal_) {
        RotatingWALResult wal_result = rotating_wal_->append(entry);
        if (wal_result != RotatingWALResult::SUCCESS) {
            setError("WAL append failed: " + rotating_wal_->getLastError());
            return EngineResult::ERROR_WAL_OPEN_FAILED;
        }

        // Periodic WAL sync (every 10000 entries for better performance)
        wal_entries_since_sync_++;
        if (wal_entries_since_sync_ >= 10000) {
            rotating_wal_->sync();
            wal_entries_since_sync_ = 0;
        }
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

EngineResult StorageEngine::flush() {
    if (!is_open_) {
        setError("Engine not open");
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
    }

    // Phase 2: Parallel flush implementation
    // Step 1: Collect non-empty buffers and prepare for parallel flush
    std::vector<std::pair<uint32_t, TagBuffer>> buffers_to_flush;
    {
        std::unique_lock<std::shared_mutex> buffers_lock(buffers_mutex_);
        for (auto& [tag_id, tag_buffer] : buffers_) {
            if (!tag_buffer.records.empty()) {
                // Make a copy of the buffer to flush
                buffers_to_flush.emplace_back(tag_id, tag_buffer);
                // Clear the original buffer immediately to allow new writes
                tag_buffer.records.clear();
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

        // Submit task to thread pool
        auto future = flush_pool_->submit([this, tag_id, tag_buffer, block_index,
                                           chunk_offset, thread_io]() -> WriteResult {
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

            // Calculate timestamp range
            result.block_start_ts = tag_buffer.start_ts_us;
            result.block_end_ts = tag_buffer.start_ts_us;
            if (!tag_buffer.records.empty()) {
                uint32_t max_offset = 0;
                for (const auto& rec : tag_buffer.records) {
                    if (rec.time_offset > max_offset) {
                        max_offset = rec.time_offset;
                    }
                }
                result.block_end_ts = tag_buffer.start_ts_us +
                                     static_cast<int64_t>(max_offset) * 1000;
            }

            // Store metadata for directory update
            result.time_unit = tag_buffer.time_unit;
            result.value_type = tag_buffer.value_type;
            result.record_count = static_cast<uint32_t>(tag_buffer.records.size());
            result.encoding_type = tag_buffer.encoding_type;

            // Convert encoding parameters
            float tolerance_f = static_cast<float>(tag_buffer.encoding_tolerance);
            float compression_factor_f = static_cast<float>(tag_buffer.encoding_compression_factor);
            std::memcpy(&result.encoding_param1, &tolerance_f, 4);
            std::memcpy(&result.encoding_param2, &compression_factor_f, 4);

            result.success = true;
            return result;
        });

        write_futures.push_back(std::move(future));
    }

    // Step 4: Wait for all writes to complete
    std::vector<WriteResult> write_results;
    write_results.reserve(write_futures.size());

    for (auto& future : write_futures) {
        write_results.push_back(future.get());
    }

    // Step 5: Check for errors
    for (const auto& result : write_results) {
        if (!result.success) {
            setError(result.error_msg);
            return EngineResult::ERROR_INVALID_DATA;
        }
    }

    // Step 6: Batch directory updates (single-threaded for now)
    if (!dir_builder_) {
        setError("Directory builder not initialized");
        return EngineResult::ERROR_INVALID_DATA;
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
            return EngineResult::ERROR_INVALID_DATA;
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
        return EngineResult::ERROR_INVALID_DATA;
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
        return EngineResult::ERROR_ENGINE_NOT_OPEN;
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

    // Step 2: Read from disk blocks
    // Acquire shared lock for thread-safe active_chunk reading
    uint64_t chunk_offset;
    uint32_t blocks_used;
    {
        std::lock_guard<std::mutex> lock(active_chunk_mutex_);
        chunk_offset = active_chunk_.chunk_offset;
        blocks_used = active_chunk_.blocks_used;
    }

    if (blocks_used > 0) {
        RawScanner scanner(io_.get());
        ScannedChunk scanned_chunk;

        ScanResult scan_result = scanner.scanChunk(chunk_offset,
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
                    chunk_offset,
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
    uint32_t data_crc32 = 0;
    BlockWriteResult write_result = writer.writeBlock(active_chunk_.chunk_offset,
                                                     data_block_index,
                                                     tag_buffer,
                                                     &data_crc32);
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

    return EngineResult::SUCCESS;
}

}  // namespace xtdb
