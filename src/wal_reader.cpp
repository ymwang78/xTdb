#include "xTdb/wal_reader.h"
#include "xTdb/constants.h"
#include <cstring>
#include <iostream>

namespace xtdb {

WALReader::WALReader(AlignedIO* io, uint64_t wal_offset, uint64_t wal_size_bytes)
    : io_(io),
      wal_start_offset_(wal_offset),
      wal_size_bytes_(wal_size_bytes),
      current_offset_(wal_offset),
      buffer_file_offset_(wal_offset),
      buffer_(kExtentSizeBytes),
      buffer_size_(0),
      buffer_position_(0) {
}

WALReader::~WALReader() {
}

WALResult WALReader::readNext(WALEntry& entry) {
    // Check if we've reached the end
    if (current_offset_ >= wal_start_offset_ + wal_size_bytes_) {
        return WALResult::ERROR_INVALID_ENTRY;  // EOF
    }

    // Check if we need to load more data
    if (buffer_position_ + sizeof(WALEntry) > buffer_size_) {
        WALResult result = loadBuffer();
        if (result != WALResult::SUCCESS) {
            return result;
        }

        // Check again after loading
        if (buffer_position_ + sizeof(WALEntry) > buffer_size_) {
            // Not enough data for a complete entry
            return WALResult::ERROR_INVALID_ENTRY;
        }
    }

    // Read entry from buffer
    std::memcpy(&entry, static_cast<uint8_t*>(buffer_.data()) + buffer_position_, sizeof(WALEntry));

    // Validate entry
    if (!isValidEntry(entry)) {
        stats_.corrupted_entries++;
        setError("Corrupted WAL entry detected");
        return WALResult::ERROR_INVALID_ENTRY;
    }

    // Update positions
    buffer_position_ += sizeof(WALEntry);
    current_offset_ += sizeof(WALEntry);
    stats_.entries_read++;

    return WALResult::SUCCESS;
}

bool WALReader::isEOF() const {
    return current_offset_ >= wal_start_offset_ + wal_size_bytes_ ||
           buffer_position_ >= buffer_size_;
}

void WALReader::reset() {
    current_offset_ = wal_start_offset_;
    buffer_file_offset_ = wal_start_offset_;
    buffer_size_ = 0;
    buffer_position_ = 0;
}

WALResult WALReader::loadBuffer() {
    // Calculate how much to read
    uint64_t remaining = (wal_start_offset_ + wal_size_bytes_) - current_offset_;
    if (remaining == 0) {
        return WALResult::ERROR_INVALID_ENTRY;  // EOF
    }

    // Align to extent boundary for read
    uint64_t read_offset = (current_offset_ / kExtentSizeBytes) * kExtentSizeBytes;
    uint32_t offset_in_extent = current_offset_ % kExtentSizeBytes;

    // Read aligned block
    IOResult io_result = io_->read(buffer_.data(), kExtentSizeBytes, read_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read WAL from disk");
        return WALResult::ERROR_IO_FAILED;
    }

    // Update buffer state
    buffer_file_offset_ = read_offset;
    buffer_size_ = kExtentSizeBytes;
    buffer_position_ = offset_in_extent;
    stats_.bytes_read += kExtentSizeBytes;

    return WALResult::SUCCESS;
}

bool WALReader::isValidEntry(const WALEntry& entry) const {
    // Check for all-zero entry (empty slot)
    if (entry.tag_id == 0 && entry.timestamp_us == 0) {
        return false;
    }

    // Check for all-ones entry (uninitialized)
    if (entry.tag_id == 0xFFFFFFFF && entry.timestamp_us == -1) {
        return false;
    }

    // Basic sanity checks
    if (entry.timestamp_us < 0) {
        return false;
    }

    // Check value type is valid
    if (entry.value_type > static_cast<uint8_t>(ValueType::VT_F64)) {
        return false;
    }

    return true;
}

void WALReader::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
