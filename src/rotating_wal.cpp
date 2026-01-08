#include "xTdb/rotating_wal.h"
#include "xTdb/constants.h"
#include <cassert>
#include <cstring>
#include <algorithm>
#include <sys/stat.h>
#include <iostream>

namespace xtdb {

// ============================================================================
// WAL Container Header (stored at offset 0)
// ============================================================================

#pragma pack(push, 1)
struct WALContainerHeader {
    char magic[8];              // "XTDB-WAL"
    uint32_t version;           // Container version
    uint32_t num_segments;      // Number of segments
    uint64_t segment_size;      // Size per segment (bytes)
    uint32_t current_segment;   // Current active segment
    uint8_t reserved[4068];     // Padding to 4096 bytes (8+4+4+8+4+4068=4096)

    WALContainerHeader() {
        std::memcpy(magic, "XTDB-WAL", 8);
        version = 1;
        num_segments = 0;
        segment_size = 0;
        current_segment = 0;
        std::memset(reserved, 0, sizeof(reserved));
    }
};
#pragma pack(pop)

static_assert(sizeof(WALContainerHeader) == 4096, "WALContainerHeader must be 4096 bytes");

// ============================================================================
// RotatingWAL Implementation
// ============================================================================

RotatingWAL::RotatingWAL(const RotatingWALConfig& config)
    : config_(config),
      is_open_(false),
      current_segment_id_(0) {
    // Validate configuration
    assert(config_.num_segments > 0 && config_.num_segments <= config_.max_segments);
    assert(isExtentAligned(config_.segment_size_bytes));
}

RotatingWAL::~RotatingWAL() {
    close();
}

RotatingWALResult RotatingWAL::open() {
    if (is_open_) {
        setError("WAL already open");
        return RotatingWALResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    // Check if container exists
    struct stat st;
    bool exists = (stat(config_.wal_container_path.c_str(), &st) == 0);

    // Create AlignedIO
    io_ = std::make_unique<AlignedIO>();
    IOResult io_result = io_->open(config_.wal_container_path, true, config_.direct_io);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to open WAL container: " + io_->getLastError());
        return RotatingWALResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    RotatingWALResult result;
    if (!exists) {
        // Initialize new container
        result = initializeContainer();
    } else {
        // Load existing container
        result = loadContainer();
    }

    if (result != RotatingWALResult::SUCCESS) {
        io_->close();
        return result;
    }

    // Create writer for current segment
    assert(current_segment_id_ < segments_.size());
    WALSegment& segment = segments_[current_segment_id_];
    current_writer_ = std::make_unique<WALWriter>(
        io_.get(),
        segment.start_offset + segment.write_position,
        segment.getAvailableSpace()
    );

    is_open_ = true;
    return RotatingWALResult::SUCCESS;
}

void RotatingWAL::close() {
    if (!is_open_) {
        return;
    }

    // Sync before closing
    if (current_writer_) {
        current_writer_->sync();
        current_writer_.reset();
    }

    // Update container header with current state
    if (io_ && io_->isOpen()) {
        WALContainerHeader header;
        header.num_segments = static_cast<uint32_t>(segments_.size());
        header.segment_size = config_.segment_size_bytes;
        header.current_segment = current_segment_id_;

        AlignedBuffer header_buf(kExtentSizeBytes);
        std::memcpy(header_buf.data(), &header, sizeof(header));

        io_->write(header_buf.data(), kExtentSizeBytes, 0);
        io_->sync();
        io_->close();
    }

    segments_.clear();
    is_open_ = false;
}

RotatingWALResult RotatingWAL::initializeContainer() {
    // Create container header
    WALContainerHeader header;
    header.num_segments = config_.num_segments;
    header.segment_size = config_.segment_size_bytes;
    header.current_segment = 0;

    // Write header (first extent)
    AlignedBuffer header_buf(kExtentSizeBytes);
    std::memcpy(header_buf.data(), &header, sizeof(header));

    IOResult io_result = io_->write(header_buf.data(), kExtentSizeBytes, 0);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write WAL container header: " + io_->getLastError());
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Initialize segments
    segments_.resize(config_.num_segments);
    uint64_t offset = kExtentSizeBytes;  // Start after header

    for (uint32_t i = 0; i < config_.num_segments; i++) {
        WALSegment& segment = segments_[i];
        segment.segment_id = i;
        segment.start_offset = offset;
        segment.segment_size = config_.segment_size_bytes;
        segment.write_position = 0;
        segment.min_timestamp_us = INT64_MAX;
        segment.max_timestamp_us = INT64_MIN;
        segment.entry_count = 0;

        std::cerr << "[initContainer] Segment " << i << ": start=" << offset
                  << ", size=" << config_.segment_size_bytes << std::endl;

        // Zero-fill first extent of each segment
        // This marks the segment as empty (first entry will have tag_id=0)
        AlignedBuffer zero_buf(kExtentSizeBytes);
        std::memset(zero_buf.data(), 0, kExtentSizeBytes);
        io_result = io_->write(zero_buf.data(), kExtentSizeBytes, offset);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to initialize segment " + std::to_string(i));
            return RotatingWALResult::ERROR_IO_FAILED;
        }

        offset += config_.segment_size_bytes;
    }

    current_segment_id_ = 0;
    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::loadContainer() {
    // Read container header
    AlignedBuffer header_buf(kExtentSizeBytes);
    IOResult io_result = io_->read(header_buf.data(), kExtentSizeBytes, 0);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read WAL container header: " + io_->getLastError());
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    WALContainerHeader header;
    std::memcpy(&header, header_buf.data(), sizeof(header));

    // Verify magic
    if (std::memcmp(header.magic, "XTDB-WAL", 8) != 0) {
        setError("Invalid WAL container magic");
        return RotatingWALResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    // Verify version
    if (header.version != 1) {
        setError("Unsupported WAL container version: " + std::to_string(header.version));
        return RotatingWALResult::ERROR_CONTAINER_OPEN_FAILED;
    }

    // Load segments
    segments_.resize(header.num_segments);
    uint64_t offset = kExtentSizeBytes;

    for (uint32_t i = 0; i < header.num_segments; i++) {
        WALSegment& segment = segments_[i];
        segment.segment_id = i;
        segment.start_offset = offset;
        segment.segment_size = header.segment_size;

        // For now, we assume segments are empty on restart
        // In a production system, you'd scan each segment to find the write position
        // and reconstruct tag_ids set
        segment.write_position = 0;
        segment.min_timestamp_us = INT64_MAX;
        segment.max_timestamp_us = INT64_MIN;
        segment.entry_count = 0;

        offset += header.segment_size;
    }

    current_segment_id_ = header.current_segment;

    // Validate current_segment_id
    if (current_segment_id_ >= segments_.size()) {
        current_segment_id_ = 0;
    }

    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::append(const WALEntry& entry) {
    if (!is_open_) {
        setError("WAL not open");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Validate entry
    if (entry.tag_id == 0) {
        setError("Invalid entry: tag_id cannot be 0");
        return RotatingWALResult::ERROR_INVALID_ENTRY;
    }

    // Check if current segment has space
    WALSegment& segment = segments_[current_segment_id_];

    // If segment is full or close to full, rotate
    if (segment.getAvailableSpace() < sizeof(WALEntry) + kExtentSizeBytes) {
        RotatingWALResult result = rotateSegment();
        if (result != RotatingWALResult::SUCCESS) {
            return result;
        }
        // CRITICAL: Re-fetch segment reference after rotation
        // current_segment_id_ has changed, so we need to update the reference
        segment = segments_[current_segment_id_];
    }

    // Append to current writer
    WALResult wal_result = current_writer_->append(entry);
    if (wal_result != WALResult::SUCCESS) {
        setError("WAL write failed: " + current_writer_->getLastError());
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Update segment metadata
    uint64_t writer_offset = current_writer_->getCurrentOffset();

    // CRITICAL: Verify writer offset is within current segment bounds
    if (writer_offset < segment.start_offset ||
        writer_offset >= segment.start_offset + segment.segment_size) {
        std::cerr << "[append] FATAL: Writer offset " << writer_offset
                  << " is OUTSIDE current segment " << current_segment_id_ << " bounds"
                  << " [" << segment.start_offset << ", "
                  << (segment.start_offset + segment.segment_size) << ")" << std::endl;
        setError("Writer offset out of bounds");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    uint64_t new_write_pos = writer_offset - segment.start_offset;
    segment.write_position = new_write_pos;
    segment.entry_count++;
    segment.tag_ids.insert(entry.tag_id);

    if (entry.timestamp_us < segment.min_timestamp_us) {
        segment.min_timestamp_us = entry.timestamp_us;
    }
    if (entry.timestamp_us > segment.max_timestamp_us) {
        segment.max_timestamp_us = entry.timestamp_us;
    }

    // Update statistics
    stats_.total_entries_written++;
    stats_.total_bytes_written += sizeof(WALEntry);

    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::sync() {
    if (!is_open_ || !current_writer_) {
        setError("WAL not open");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    WALResult result = current_writer_->sync();
    if (result != WALResult::SUCCESS) {
        setError("WAL sync failed: " + current_writer_->getLastError());
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    stats_.sync_operations++;
    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::rotateSegment() {
    if (!is_open_) {
        setError("WAL not open");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Sync current writer
    if (current_writer_) {
        current_writer_->sync();
    }

    // Save old segment info for callback
    uint32_t old_segment_id = current_segment_id_;
    WALSegment& old_segment = segments_[old_segment_id];
    std::set<uint32_t> old_segment_tags = old_segment.tag_ids;

    // Find next available segment
    uint32_t next_segment_id = (current_segment_id_ + 1) % segments_.size();
    WALSegment& next_segment = segments_[next_segment_id];

    // Check if next segment is cleared
    if (next_segment.entry_count > 0 && next_segment.write_position > 0) {
        // Next segment not cleared yet
        if (config_.auto_grow && segments_.size() < config_.max_segments) {
            // Try to grow container
            RotatingWALResult result = growContainer();
            if (result != RotatingWALResult::SUCCESS) {
                setError("All segments full and cannot grow");
                return RotatingWALResult::ERROR_ALL_SEGMENTS_FULL;
            }
            next_segment_id = static_cast<uint32_t>(segments_.size() - 1);
        } else {
            setError("Next segment not cleared, cannot rotate");
            return RotatingWALResult::ERROR_SEGMENT_NOT_CLEARED;
        }
    }

    std::cerr << "[rotateSegment] Rotating from segment " << old_segment_id
              << " to segment " << next_segment_id << std::endl;
    std::cerr << "[rotateSegment] Next segment before callback: entries="
              << next_segment.entry_count << ", write_pos=" << next_segment.write_position << std::endl;

    // Switch to next segment
    current_segment_id_ = next_segment_id;

    // Trigger flush callback BEFORE creating new writer
    // This ensures the old segment is flushed first
    if (flush_callback_) {
        bool flush_success = flush_callback_(old_segment_id, old_segment_tags);
        if (!flush_success) {
            setError("Flush callback failed for segment " + std::to_string(old_segment_id));
            return RotatingWALResult::ERROR_CALLBACK_FAILED;
        }
        stats_.segment_flushes++;
    }

    // Re-fetch next_segment reference after callback (it may have been modified)
    WALSegment& updated_next_segment = segments_[next_segment_id];

    std::cerr << "[rotateSegment] Next segment after callback: entries="
              << updated_next_segment.entry_count << ", write_pos=" << updated_next_segment.write_position << std::endl;

    // IMPORTANT: After callback, next_segment should be cleared (write_position=0)
    // Create new writer for next segment starting from its current position
    current_writer_ = std::make_unique<WALWriter>(
        io_.get(),
        updated_next_segment.start_offset + updated_next_segment.write_position,
        updated_next_segment.getAvailableSpace()
    );

    std::cerr << "[rotateSegment] Created writer at offset "
              << (updated_next_segment.start_offset + updated_next_segment.write_position) << std::endl;

    stats_.segment_rotations++;
    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::clearSegment(uint32_t segment_id) {
    if (segment_id >= segments_.size()) {
        setError("Invalid segment ID: " + std::to_string(segment_id));
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Don't clear current active segment
    if (segment_id == current_segment_id_) {
        setError("Cannot clear current active segment");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    WALSegment& segment = segments_[segment_id];

    std::cerr << "[clearSegment] Before clear segment " << segment_id
              << ": id=" << segment.segment_id
              << ", start=" << segment.start_offset
              << ", size=" << segment.segment_size
              << ", entries=" << segment.entry_count << std::endl;

    //  Zero out first extent to mark as empty
    AlignedBuffer zero_buf(kExtentSizeBytes);
    std::memset(zero_buf.data(), 0, kExtentSizeBytes);
    IOResult result = io_->write(zero_buf.data(), kExtentSizeBytes, segment.start_offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to clear segment " + std::to_string(segment_id));
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // IMPORTANT: Reset segment metadata
    // We must preserve segment_id, start_offset, and segment_size
    uint32_t saved_id = segment.segment_id;
    uint64_t saved_start = segment.start_offset;
    uint64_t saved_size = segment.segment_size;

    segment.reset();

    // Restore fixed fields
    segment.segment_id = saved_id;
    segment.start_offset = saved_start;
    segment.segment_size = saved_size;

    std::cerr << "[clearSegment] After clear segment " << segment_id
              << ": id=" << segment.segment_id
              << ", start=" << segment.start_offset
              << ", size=" << segment.segment_size
              << ", entries=" << segment.entry_count << std::endl;

    return RotatingWALResult::SUCCESS;
}

RotatingWALResult RotatingWAL::growContainer() {
    if (!config_.auto_grow) {
        setError("Auto-grow not enabled");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    if (segments_.size() >= config_.max_segments) {
        setError("Maximum segments reached");
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    // Create new segment
    uint32_t new_segment_id = static_cast<uint32_t>(segments_.size());
    WALSegment new_segment;
    new_segment.segment_id = new_segment_id;

    // Calculate offset (after last segment)
    uint64_t offset = kExtentSizeBytes + (segments_.size() * config_.segment_size_bytes);
    new_segment.start_offset = offset;
    new_segment.segment_size = config_.segment_size_bytes;
    new_segment.write_position = 0;

    // Zero-fill first extent
    AlignedBuffer zero_buf(kExtentSizeBytes);
    std::memset(zero_buf.data(), 0, kExtentSizeBytes);
    IOResult result = io_->write(zero_buf.data(), kExtentSizeBytes, offset);
    if (result != IOResult::SUCCESS) {
        setError("Failed to grow container: " + io_->getLastError());
        return RotatingWALResult::ERROR_IO_FAILED;
    }

    segments_.push_back(new_segment);

    return RotatingWALResult::SUCCESS;
}

void RotatingWAL::setFlushCallback(SegmentFlushCallback callback) {
    flush_callback_ = std::move(callback);
}

const WALSegment& RotatingWAL::getSegment(uint32_t segment_id) const {
    assert(segment_id < segments_.size());
    return segments_[segment_id];
}

double RotatingWAL::getUsageRatio() const {
    if (segments_.empty()) {
        return 0.0;
    }

    uint64_t total_capacity = segments_.size() * config_.segment_size_bytes;
    uint64_t total_used = 0;

    for (const auto& segment : segments_) {
        total_used += segment.write_position;
    }

    return static_cast<double>(total_used) / static_cast<double>(total_capacity);
}

void RotatingWAL::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
