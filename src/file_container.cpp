#include "xTdb/file_container.h"
#include "xTdb/platform_compat.h"
#include <cstring>
#include <cerrno>
#include <iostream>
#include <chrono>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace xtdb {

FileContainer::FileContainer(const std::string& path,
                             const ChunkLayout& layout,
                             bool direct_io,
                             bool read_only)
    : path_(path),
      layout_(layout),
      direct_io_(direct_io),
      read_only_(read_only),
      is_open_(false) {
}

FileContainer::~FileContainer() {
    close();
}

ContainerResult FileContainer::open(bool create_if_not_exists) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_open_) {
        setError("Container already open");
        return ContainerResult::ERR_ALREADY_OPEN;
    }

    // Check if file exists (but don't fail if stat fails, try opening anyway)
    struct stat st;
    bool exists = (stat(path_.c_str(), &st) == 0);

    // Open file using AlignedIO
    io_ = std::make_unique<AlignedIO>();
    IOResult io_result = io_->open(path_, create_if_not_exists, direct_io_);
    if (io_result != IOResult::SUCCESS) {
        if (!create_if_not_exists && !exists) {
            setError("Container file does not exist: " + path_);
        } else {
            setError("Failed to open file: " + io_->getLastError());
        }
        io_.reset();
        return ContainerResult::ERR_OPENFD_FAILED;
    }

    // Try to read and validate header first
    // This is the most reliable way to determine if this is an existing container
    ContainerResult header_result = readAndValidateHeader();
    if (header_result == ContainerResult::SUCCESS) {
        // Successfully read header, this is an existing container
        is_open_ = true;
        return ContainerResult::SUCCESS;
    }
    
    // Header read failed - check if this might be a new file
    // Only initialize if create_if_not_exists is true
    if (!create_if_not_exists) {
        // File exists but header is invalid - return error
        io_->close();
        io_.reset();
        setError("Container file exists but header is invalid: " + getLastError());
        return ContainerResult::ERR_INVALID_HEADER;
    }
    
    // Check file size to see if it's a new file
    int64_t file_size = io_->getFileSize();
    bool is_new_file = (file_size < 0 || file_size <= static_cast<int64_t>(kExtentSizeBytes));
    
    // If file seems new (small or can't get size), initialize it
    if (is_new_file) {
        ContainerResult result = initializeNewContainer();
        if (result != ContainerResult::SUCCESS) {
            io_->close();
            io_.reset();
            return result;
        }
    } else {
        // File exists and is large enough but header is invalid
        // This is an error - don't overwrite existing data
        io_->close();
        io_.reset();
        setError("Container file exists but header is invalid (file size: " + 
                 std::to_string(file_size) + " bytes)");
        return ContainerResult::ERR_INVALID_HEADER;
    }

    is_open_ = true;
    return ContainerResult::SUCCESS;
}

void FileContainer::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return;
    }

    if (io_) {
        io_->sync();  // Ensure all data is flushed
        io_->close();
        io_.reset();
    }

    is_open_ = false;
}

bool FileContainer::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

ContainerResult FileContainer::write(const void* buffer, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Container is read-only");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    IOResult io_result = io_->write(buffer, size, offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Write failed: " + io_->getLastError());
        return convertIOResult(io_result);
    }

    // Update statistics
    stats_.bytes_written += size;
    stats_.write_operations++;

    return ContainerResult::SUCCESS;
}

ContainerResult FileContainer::read(void* buffer, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    IOResult io_result = io_->read(buffer, size, offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Read failed: " + io_->getLastError());
        return convertIOResult(io_result);
    }

    // Update statistics
    stats_.bytes_read += size;
    stats_.read_operations++;

    return ContainerResult::SUCCESS;
}

ContainerResult FileContainer::sync() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    IOResult io_result = io_->sync();
    if (io_result != IOResult::SUCCESS) {
        setError("Sync failed: " + io_->getLastError());
        return ContainerResult::ERR_SYNC_FAILED;
    }

    stats_.sync_operations++;
    return ContainerResult::SUCCESS;
}

int64_t FileContainer::getCurrentSize() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return -1;
    }

    return io_->getFileSize();
}

ContainerResult FileContainer::preallocate(uint64_t size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Container is read-only");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    IOResult io_result = io_->preallocate(size);
    if (io_result != IOResult::SUCCESS) {
        setError("Preallocate failed: " + io_->getLastError());
        return ContainerResult::ERR_WRITE_FAILED;
    }

    return ContainerResult::SUCCESS;
}

int FileContainer::getFd() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_ || !io_) {
        return -1;
    }

    return io_->getFd();
}

ContainerResult FileContainer::initializeNewContainer() {
    // Create container header
    ContainerHeaderV12 header;
    // Constructor already sets magic, version, header_size

    // Calculate capacity (16 chunks by default)
    uint32_t num_chunks = 16;
    header.capacity_extents = (static_cast<uint64_t>(layout_.chunk_size_bytes) * num_chunks) / kExtentSizeBytes;
    header.chunk_size_extents = layout_.chunk_size_bytes / kExtentSizeBytes;
    header.block_size_extents = layout_.block_size_bytes / kExtentSizeBytes;
    header.layout = static_cast<uint8_t>(ContainerLayout::LAYOUT_RAW_FIXED);
    header.capacity_type = static_cast<uint8_t>(CapacityType::CAP_DYNAMIC);
    header.raw_block_class = 1;  // RAW16K

    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    header.created_ts_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    // Write header to extent 0
    AlignedBuffer header_buf(kExtentSizeBytes);
    std::memcpy(header_buf.data(), &header, sizeof(header));
    IOResult io_result = io_->write(header_buf.data(), kExtentSizeBytes, 0);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write container header: " + io_->getLastError());
        return ContainerResult::ERR_CREATE_FAILED;
    }

    // Sync to ensure header is written to disk
    io_result = io_->sync();
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to sync container header: " + io_->getLastError());
        return ContainerResult::ERR_CREATE_FAILED;
    }

    // Initialize WAL region (extent 1-256, 4 MB total)
    // Zero-fill the WAL region so it's ready for use
    AlignedBuffer zero_buf(kExtentSizeBytes);
    std::memset(zero_buf.data(), 0, kExtentSizeBytes);
    for (uint32_t i = 1; i <= 256; i++) {
        io_result = io_->write(zero_buf.data(), kExtentSizeBytes, i * kExtentSizeBytes);
        if (io_result != IOResult::SUCCESS) {
            setError("Failed to initialize WAL region: " + io_->getLastError());
            return ContainerResult::ERR_CREATE_FAILED;
        }
    }

    // Sync WAL region initialization
    io_result = io_->sync();
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to sync WAL region: " + io_->getLastError());
        return ContainerResult::ERR_CREATE_FAILED;
    }

    // Preallocate container space (optional, for better performance)
    uint64_t container_capacity = header.capacity_extents * kExtentSizeBytes;
    int fd = io_->getFd();
    if (fd >= 0) {
#ifdef __linux__
        // Use fallocate on Linux for efficient space allocation
        int fallocate_result = fallocate(fd, 0, 0, container_capacity);
        if (fallocate_result != 0) {
            // fallocate failed, try fallback method
            std::cerr << "[FileContainer] Warning: fallocate failed (errno=" << errno
                      << "), using fallback pre-allocation method" << std::endl;
            // Fallback: write a single extent at the end
            AlignedBuffer end_buf(kExtentSizeBytes);
            std::memset(end_buf.data(), 0, kExtentSizeBytes);
            io_result = io_->write(end_buf.data(), kExtentSizeBytes,
                                  container_capacity - kExtentSizeBytes);
            if (io_result != IOResult::SUCCESS) {
                std::cerr << "[FileContainer] Warning: Container pre-allocation failed, "
                          << "continuing without pre-allocation" << std::endl;
            }
        }
#else
        // Non-Linux: use fallback method
        AlignedBuffer end_buf(kExtentSizeBytes);
        std::memset(end_buf.data(), 0, kExtentSizeBytes);
        io_result = io_->write(end_buf.data(), kExtentSizeBytes,
                              container_capacity - kExtentSizeBytes);
        if (io_result != IOResult::SUCCESS) {
            std::cerr << "[FileContainer] Warning: Container pre-allocation failed, "
                      << "continuing without pre-allocation" << std::endl;
        }
#endif
    }

    // Store metadata
    std::memset(metadata_.db_instance_id, 0, 16);
    metadata_.layout = ContainerLayout::LAYOUT_RAW_FIXED;
    metadata_.capacity_type = CapacityType::CAP_DYNAMIC;
    metadata_.archive_level = ArchiveLevel::ARCHIVE_RAW;
    metadata_.capacity_extents = header.capacity_extents;
    metadata_.capacity_bytes = header.capacity_extents * kExtentSizeBytes;
    metadata_.chunk_size_extents = header.chunk_size_extents;
    metadata_.block_size_extents = header.block_size_extents;
    metadata_.created_ts_us = header.created_ts_us;

    return ContainerResult::SUCCESS;
}

ContainerResult FileContainer::readAndValidateHeader() {
    // Read header from extent 0
    AlignedBuffer header_buf(kExtentSizeBytes);
    IOResult io_result = io_->read(header_buf.data(), kExtentSizeBytes, 0);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read container header: " + io_->getLastError());
        return ContainerResult::ERR_INVALID_HEADER;
    }

    // Parse header
    ContainerHeaderV12 header;
    std::memcpy(&header, header_buf.data(), sizeof(header));

    // Validate magic number
    if (std::memcmp(header.magic, kContainerMagic, 8) != 0) {
        setError("Invalid container magic number");
        return ContainerResult::ERR_INVALID_HEADER;
    }

    // Validate version
    if (header.version != 0x0102) {
        setError("Unsupported container version");
        return ContainerResult::ERR_INVALID_HEADER;
    }

    // Store metadata
    std::memcpy(metadata_.db_instance_id, header.db_instance_id, 16);
    metadata_.layout = static_cast<ContainerLayout>(header.layout);
    metadata_.capacity_type = static_cast<CapacityType>(header.capacity_type);
    metadata_.archive_level = static_cast<ArchiveLevel>(header.archive_level);
    metadata_.capacity_extents = header.capacity_extents;
    metadata_.capacity_bytes = header.capacity_extents * kExtentSizeBytes;
    metadata_.chunk_size_extents = header.chunk_size_extents;
    metadata_.block_size_extents = header.block_size_extents;
    metadata_.created_ts_us = header.created_ts_us;

    return ContainerResult::SUCCESS;
}

void FileContainer::setError(const std::string& message) {
    last_error_ = message;
}

ContainerResult FileContainer::convertIOResult(IOResult io_result) {
    switch (io_result) {
        case IOResult::SUCCESS:
            return ContainerResult::SUCCESS;
        case IOResult::ERR_OPENFD_FAILED:
            return ContainerResult::ERR_OPENFD_FAILED;
        case IOResult::ERR_ALIGNMENT:
            return ContainerResult::ERR_INVALID_OFFSET;
        case IOResult::ERR_IO_FAILED:
            return ContainerResult::ERR_READ_FAILED;
        case IOResult::ERR_INVALID_FD:
            return ContainerResult::ERR_NOT_OPEN;
        case IOResult::ERR_PREALLOCATE_FAILED:
            return ContainerResult::ERR_WRITE_FAILED;
        default:
            return ContainerResult::ERR_READ_FAILED;
    }
}

}  // namespace xtdb
