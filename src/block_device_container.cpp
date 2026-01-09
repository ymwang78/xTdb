#include "xTdb/block_device_container.h"
#include "xTdb/platform_compat.h"
#include <cstring>
#include <iostream>
// Include chrono after platform_compat.h to avoid Windows.h macro conflicts
#include <chrono>
#include <iomanip>
#include <sstream>

#ifdef __linux__
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace xtdb {

BlockDeviceContainer::BlockDeviceContainer(const std::string& device_path,
                                           const ChunkLayout& layout,
                                           bool read_only,
                                           bool test_mode)
    : device_path_(device_path),
      layout_(layout),
      read_only_(read_only),
      test_mode_(test_mode),
      is_open_(false),
      fd_(-1),
      device_capacity_(0),
      device_block_size_(4096) {  // Default 4KB block size
}

BlockDeviceContainer::~BlockDeviceContainer() {
    close();
}

ContainerResult BlockDeviceContainer::open(bool create_if_not_exists) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_open_) {
        setError("Container already open");
        return ContainerResult::ERROR_ALREADY_OPEN;
    }

    // On Windows, allow using regular files to simulate block devices
#ifdef _WIN32
    // Windows doesn't support real block devices, so use test mode
    bool is_block_dev = false;
    bool effective_test_mode = true;  // Always use test mode on Windows
#else
    // Verify device is a block device (skip check in test mode)
    bool is_block_dev = isBlockDevice(device_path_);
    if (!is_block_dev && !test_mode_) {
        setError("Not a block device: " + device_path_);
        return ContainerResult::ERROR_DEVICE_NOT_FOUND;
    }
    bool effective_test_mode = test_mode_;
#endif

    // Open device
    int flags = read_only_ ? O_RDONLY : O_RDWR;
    // Use O_DIRECT only for real block devices (not in test mode with regular files)
    // On Windows, never use O_DIRECT as it's not supported with regular files
#ifndef _WIN32
    if (is_block_dev && !effective_test_mode) {
        flags |= O_DIRECT;
    }
#endif

#ifdef _WIN32
    int mode = _S_IREAD | (read_only_ ? 0 : _S_IWRITE);
    fd_ = ::_open(device_path_.c_str(), flags, mode);
#else
    fd_ = ::open(device_path_.c_str(), flags);
#endif
    if (fd_ < 0) {
        setError("Failed to open block device: " + device_path_ + " (errno=" + std::to_string(errno) + ")");
        if (errno == EACCES || errno == EPERM) {
            return ContainerResult::ERROR_INSUFFICIENT_PERMISSIONS;
        }
        return ContainerResult::ERROR_OPEN_FAILED;
    }

    // Detect device properties
    ContainerResult result = detectDeviceProperties();
    if (result != ContainerResult::SUCCESS) {
        ::_close(fd_);
        fd_ = -1;
        return result;
    }

    // Try to read existing header
    AlignedBuffer header_buf(kExtentSizeBytes);
#ifdef _WIN32
    if (::_lseek(fd_, 0, SEEK_SET) == -1) {
        setError("Failed to seek device header: " + std::string(strerror(errno)));
        ::_close(fd_);
        fd_ = -1;
        return ContainerResult::ERROR_READ_FAILED;
    }
    ssize_t bytes_read = ::_read(fd_, header_buf.data(), static_cast<unsigned int>(kExtentSizeBytes));
#else
    ssize_t bytes_read = ::pread(fd_, header_buf.data(), kExtentSizeBytes, 0);
#endif
    if (bytes_read < 0) {
        setError("Failed to read device header: " + std::string(strerror(errno)));
        ::_close(fd_);
        fd_ = -1;
        return ContainerResult::ERROR_READ_FAILED;
    }

    // Check if header is valid
    ContainerHeaderV12 header;
    std::memcpy(&header, header_buf.data(), sizeof(header));

    bool has_valid_header = (std::memcmp(header.magic, kContainerMagic, 8) == 0);

    if (has_valid_header) {
        // Valid header exists - read and validate
        result = readAndValidateHeader();
        if (result != ContainerResult::SUCCESS) {
            ::_close(fd_);
            fd_ = -1;
            return result;
        }
    } else {
        // No valid header - initialize if allowed
        if (create_if_not_exists && !read_only_) {
            result = initializeNewContainer();
            if (result != ContainerResult::SUCCESS) {
                ::_close(fd_);
                fd_ = -1;
                return result;
            }
        } else {
            setError("Device has no valid container header");
            ::_close(fd_);
            fd_ = -1;
            return ContainerResult::ERROR_INVALID_HEADER;
        }
    }

    // Create AlignedIO instance for StorageEngine compatibility
    io_ = std::make_unique<AlignedIO>();
    // Use O_DIRECT only for real block devices (not in test mode)
    // On Windows, never use direct I/O with regular files
#ifndef _WIN32
    bool use_direct_io = (is_block_dev && !effective_test_mode);
#else
    bool use_direct_io = false;  // Windows doesn't support O_DIRECT with regular files
#endif
    IOResult io_result = io_->open(device_path_, create_if_not_exists, use_direct_io);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to open AlignedIO for device: " + io_->getLastError());
        ::_close(fd_);
        fd_ = -1;
        io_.reset();
        return ContainerResult::ERROR_OPEN_FAILED;
    }

    is_open_ = true;
    return ContainerResult::SUCCESS;
}

void BlockDeviceContainer::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return;
    }

    // Close AlignedIO first
    if (io_) {
        io_->close();
        io_.reset();
    }

    if (fd_ >= 0) {
#ifdef _WIN32
        ::_commit(fd_);  // Ensure all data is flushed
#else
        ::fsync(fd_);  // Ensure all data is flushed
#endif
        ::_close(fd_);
        fd_ = -1;
    }

    is_open_ = false;
}

bool BlockDeviceContainer::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_open_;
}

ContainerResult BlockDeviceContainer::write(const void* buffer, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERROR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Container is read-only");
        return ContainerResult::ERROR_WRITE_FAILED;
    }

    // Validate alignment (16KB for xTdb)
    if ((reinterpret_cast<uintptr_t>(buffer) % kExtentSizeBytes) != 0) {
        setError("Buffer not 16KB-aligned");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    if ((size % kExtentSizeBytes) != 0) {
        setError("Size not extent-aligned");
        return ContainerResult::ERROR_INVALID_SIZE;
    }

    if ((offset % kExtentSizeBytes) != 0) {
        setError("Offset not extent-aligned");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    // Check bounds
    if (offset + size > device_capacity_) {
        setError("Write exceeds device capacity");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    // Perform write
#ifdef _WIN32
    if (::_lseek(fd_, offset, SEEK_SET) == -1) {
        setError("Failed to seek: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_WRITE_FAILED;
    }
    ssize_t bytes_written = ::_write(fd_, buffer, static_cast<unsigned int>(size));
#else
    ssize_t bytes_written = ::pwrite(fd_, buffer, size, offset);
#endif
    if (bytes_written < 0 || static_cast<uint64_t>(bytes_written) != size) {
        setError("Write failed: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_WRITE_FAILED;
    }

    // Update statistics
    stats_.bytes_written += size;
    stats_.write_operations++;

    return ContainerResult::SUCCESS;
}

ContainerResult BlockDeviceContainer::read(void* buffer, uint64_t size, uint64_t offset) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERROR_NOT_OPEN;
    }

    // Validate alignment (16KB for xTdb)
    if ((reinterpret_cast<uintptr_t>(buffer) % kExtentSizeBytes) != 0) {
        setError("Buffer not 16KB-aligned");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    if ((size % kExtentSizeBytes) != 0) {
        setError("Size not extent-aligned");
        return ContainerResult::ERROR_INVALID_SIZE;
    }

    if ((offset % kExtentSizeBytes) != 0) {
        setError("Offset not extent-aligned");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    // Check bounds
    if (offset + size > device_capacity_) {
        setError("Read exceeds device capacity");
        return ContainerResult::ERROR_INVALID_OFFSET;
    }

    // Perform read
#ifdef _WIN32
    if (::_lseek(fd_, offset, SEEK_SET) == -1) {
        setError("Failed to seek: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_READ_FAILED;
    }
    ssize_t bytes_read = ::_read(fd_, buffer, static_cast<unsigned int>(size));
#else
    ssize_t bytes_read = ::pread(fd_, buffer, size, offset);
#endif
    if (bytes_read < 0 || static_cast<uint64_t>(bytes_read) != size) {
        setError("Read failed: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_READ_FAILED;
    }

    // Update statistics
    stats_.bytes_read += size;
    stats_.read_operations++;

    return ContainerResult::SUCCESS;
}

ContainerResult BlockDeviceContainer::sync() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        setError("Container not open");
        return ContainerResult::ERROR_NOT_OPEN;
    }

    if (::fsync(fd_) < 0) {
        setError("Sync failed: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_SYNC_FAILED;
    }

    stats_.sync_operations++;
    return ContainerResult::SUCCESS;
}

ContainerResult BlockDeviceContainer::preallocate(uint64_t size) {
    // Block devices are already fully allocated, no-op
    (void)size;
    return ContainerResult::SUCCESS;
}

int BlockDeviceContainer::getFd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return fd_;
}

bool BlockDeviceContainer::isBlockDevice(const std::string& device_path) {
    struct stat st;
    if (stat(device_path.c_str(), &st) != 0) {
        return false;
    }

#ifdef _WIN32
    // On Windows, block devices are not supported in the same way
    (void)device_path;
    return false;
#else
    return S_ISBLK(st.st_mode);
#endif
}

ContainerResult BlockDeviceContainer::detectDeviceProperties() {
#ifdef __linux__
    uint64_t size_bytes = 0;

    if (test_mode_) {
        // Test mode: Get file size using fstat
        struct stat st;
        if (fstat(fd_, &st) < 0) {
            setError("Failed to get file size: " + std::string(strerror(errno)));
            return ContainerResult::ERROR_DEVICE_NOT_FOUND;
        }
        size_bytes = st.st_size;
        device_block_size_ = 4096;  // Use default 4KB for regular files
        std::cout << "[BlockDeviceContainer] Test mode: Using regular file, size="
                  << size_bytes << " bytes" << std::endl;
    } else {
        // Production mode: Get device size using BLKGETSIZE64
        if (ioctl(fd_, BLKGETSIZE64, &size_bytes) < 0) {
            setError("Failed to get device size: " + std::string(strerror(errno)));
            return ContainerResult::ERROR_DEVICE_NOT_FOUND;
        }

        // Get device block size
        int block_size = 0;
        if (ioctl(fd_, BLKSSZGET, &block_size) < 0) {
            // If failed, use default 4KB
            std::cerr << "[BlockDeviceContainer] Warning: Failed to get block size, using default 4KB" << std::endl;
            device_block_size_ = 4096;
        } else {
            device_block_size_ = block_size;
        }
    }

    device_capacity_ = size_bytes;

    // Verify device capacity is sufficient
    uint64_t min_capacity = layout_.chunk_size_bytes;  // At least 1 chunk
    if (device_capacity_ < min_capacity) {
        setError("Device capacity too small: " + std::to_string(device_capacity_) +
                 " bytes (minimum: " + std::to_string(min_capacity) + " bytes)");
        return ContainerResult::ERROR_DEVICE_NOT_FOUND;
    }

    return ContainerResult::SUCCESS;
#else
    setError("Block device support is only available on Linux");
    return ContainerResult::ERROR_DEVICE_NOT_FOUND;
#endif
}

ContainerResult BlockDeviceContainer::initializeNewContainer() {
    // Create container header
    ContainerHeaderV12 header;
    // Constructor already sets magic, version, header_size

    // Calculate capacity based on device size
    uint64_t usable_capacity = (device_capacity_ / kExtentSizeBytes) * kExtentSizeBytes;
    header.capacity_extents = usable_capacity / kExtentSizeBytes;
    header.chunk_size_extents = layout_.chunk_size_bytes / kExtentSizeBytes;
    header.block_size_extents = layout_.block_size_bytes / kExtentSizeBytes;
    header.layout = static_cast<uint8_t>(ContainerLayout::LAYOUT_RAW_FIXED);
    header.capacity_type = static_cast<uint8_t>(CapacityType::CAP_FIXED);  // Block device is fixed
    header.raw_block_class = 1;  // RAW16K

    // Get current timestamp
    // Use GetSystemTimePreciseAsFileTime on Windows to avoid chrono macro conflicts
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // Convert from 100-nanosecond intervals to microseconds
    header.created_ts_us = uli.QuadPart / 10;
#else
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    header.created_ts_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
#endif

    // Write header to extent 0
    AlignedBuffer header_buf(kExtentSizeBytes);
    std::memcpy(header_buf.data(), &header, sizeof(header));

#ifdef _WIN32
    if (::_lseek(fd_, 0, SEEK_SET) == -1) {
        setError("Failed to seek: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_CREATE_FAILED;
    }
    ssize_t bytes_written = ::_write(fd_, header_buf.data(), static_cast<unsigned int>(kExtentSizeBytes));
#else
    ssize_t bytes_written = ::pwrite(fd_, header_buf.data(), kExtentSizeBytes, 0);
#endif
    if (bytes_written < 0 || bytes_written != kExtentSizeBytes) {
        setError("Failed to write container header: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_CREATE_FAILED;
    }

    // Initialize WAL region (extent 1-256, 4 MB total)
    AlignedBuffer zero_buf(kExtentSizeBytes);
    std::memset(zero_buf.data(), 0, kExtentSizeBytes);
    for (uint32_t i = 1; i <= 256; i++) {
#ifdef _WIN32
        if (::_lseek(fd_, i * kExtentSizeBytes, SEEK_SET) == -1) {
            setError("Failed to seek: " + std::string(strerror(errno)));
            return ContainerResult::ERROR_CREATE_FAILED;
        }
        bytes_written = ::_write(fd_, zero_buf.data(), static_cast<unsigned int>(kExtentSizeBytes));
#else
        bytes_written = ::pwrite(fd_, zero_buf.data(), kExtentSizeBytes, i * kExtentSizeBytes);
#endif
        if (bytes_written < 0 || bytes_written != kExtentSizeBytes) {
            setError("Failed to initialize WAL region: " + std::string(strerror(errno)));
            return ContainerResult::ERROR_CREATE_FAILED;
        }
    }

    // Sync to ensure metadata is written
#ifdef _WIN32
    ::_commit(fd_);
#else
    ::fsync(fd_);
#endif

    // Store metadata
    std::memset(metadata_.db_instance_id, 0, 16);
    metadata_.layout = ContainerLayout::LAYOUT_RAW_FIXED;
    metadata_.capacity_type = CapacityType::CAP_FIXED;
    metadata_.archive_level = ArchiveLevel::ARCHIVE_RAW;
    metadata_.capacity_extents = header.capacity_extents;
    metadata_.capacity_bytes = usable_capacity;
    metadata_.chunk_size_extents = header.chunk_size_extents;
    metadata_.block_size_extents = header.block_size_extents;
    metadata_.created_ts_us = header.created_ts_us;

    return ContainerResult::SUCCESS;
}

ContainerResult BlockDeviceContainer::readAndValidateHeader() {
    // Read header from extent 0
    AlignedBuffer header_buf(kExtentSizeBytes);
#ifdef _WIN32
    if (::_lseek(fd_, 0, SEEK_SET) == -1) {
        setError("Failed to seek: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_INVALID_HEADER;
    }
    ssize_t bytes_read = ::_read(fd_, header_buf.data(), static_cast<unsigned int>(kExtentSizeBytes));
#else
    ssize_t bytes_read = ::pread(fd_, header_buf.data(), kExtentSizeBytes, 0);
#endif
    if (bytes_read < 0 || bytes_read != kExtentSizeBytes) {
        setError("Failed to read container header: " + std::string(strerror(errno)));
        return ContainerResult::ERROR_INVALID_HEADER;
    }

    // Parse header
    ContainerHeaderV12 header;
    std::memcpy(&header, header_buf.data(), sizeof(header));

    // Validate magic number
    if (std::memcmp(header.magic, kContainerMagic, 8) != 0) {
        setError("Invalid container magic number");
        return ContainerResult::ERROR_INVALID_HEADER;
    }

    // Validate version
    if (header.version != 0x0102) {
        setError("Unsupported container version");
        return ContainerResult::ERROR_INVALID_HEADER;
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

void BlockDeviceContainer::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
