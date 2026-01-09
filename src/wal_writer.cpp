#include "xTdb/wal_writer.h"
#include <cassert>
#include <cstring>

namespace xtdb {

WALWriter::WALWriter(AlignedIO* io, uint64_t wal_offset, uint64_t wal_size_bytes)
    : io_(io),
      wal_start_offset_(wal_offset),
      wal_size_bytes_(wal_size_bytes),
      current_offset_(wal_offset),
      buffer_(kExtentSizeBytes),
      buffer_used_(0) {

    assert(io_ != nullptr);
    assert(io_->isOpen());
    assert(isExtentAligned(wal_offset));
    assert(isExtentAligned(wal_size_bytes));
}

WALWriter::~WALWriter() {
    // Flush any remaining data
    if (buffer_used_ > 0) {
        flush();
    }
}

void WALWriter::setError(const std::string& message) {
    last_error_ = message;
}

WALResult WALWriter::append(const WALEntry& entry) {
    // Validate entry
    if (entry.tag_id == 0) {
        setError("Invalid entry: tag_id cannot be 0");
        return WALResult::ERR_INVALID_ENTRY;
    }

    // Check if WAL is full
    if (isFull()) {
        setError("WAL is full");
        return WALResult::ERR_FULL;
    }

    // Check if buffer has space for this entry
    if (buffer_used_ + sizeof(WALEntry) > buffer_.size()) {
        // Flush current buffer
        WALResult result = flush();
        if (result != WALResult::SUCCESS) {
            return result;
        }
    }

    // Copy entry to buffer
    std::memcpy(static_cast<char*>(buffer_.data()) + buffer_used_,
                &entry,
                sizeof(WALEntry));
    buffer_used_ += sizeof(WALEntry);

    // Update statistics
    stats_.entries_written++;

    return WALResult::SUCCESS;
}

WALResult WALWriter::flush() {
    if (buffer_used_ == 0) {
        return WALResult::SUCCESS;  // Nothing to flush
    }

    // Pad to extent boundary
    uint32_t padded_size = ((buffer_used_ + kExtentSizeBytes - 1) / kExtentSizeBytes) * kExtentSizeBytes;

    // Zero out padding
    if (padded_size > buffer_used_) {
        std::memset(static_cast<char*>(buffer_.data()) + buffer_used_,
                    0,
                    padded_size - buffer_used_);
    }

    // Write to disk
    IOResult io_result = io_->write(buffer_.data(), padded_size, current_offset_);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write WAL: " + io_->getLastError());
        return WALResult::ERR_IO_FAILED;
    }

    // Update offsets
    current_offset_ += padded_size;
    stats_.bytes_written += padded_size;

    // Reset buffer
    buffer_used_ = 0;

    return WALResult::SUCCESS;
}

WALResult WALWriter::sync() {
    // Flush buffer first
    WALResult result = flush();
    if (result != WALResult::SUCCESS) {
        return result;
    }

    // Sync to disk
    IOResult io_result = io_->sync();
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to sync WAL: " + io_->getLastError());
        return WALResult::ERR_IO_FAILED;
    }

    stats_.sync_operations++;
    return WALResult::SUCCESS;
}

WALResult WALWriter::reset() {
    // Flush any pending data
    WALResult result = flush();
    if (result != WALResult::SUCCESS) {
        return result;
    }

    // Zero out the first extent of WAL to mark it as empty
    // This ensures that on restart, WAL replay will immediately see an invalid entry
    AlignedBuffer zero_buf(kExtentSizeBytes);
    std::memset(zero_buf.data(), 0, kExtentSizeBytes);
    IOResult io_result = io_->write(zero_buf.data(), kExtentSizeBytes, wal_start_offset_);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to clear WAL region");
        return WALResult::ERR_IO_FAILED;
    }

    // Reset write position
    current_offset_ = wal_start_offset_;
    buffer_used_ = 0;

    // Note: We don't reset statistics

    return WALResult::SUCCESS;
}

}  // namespace xtdb
