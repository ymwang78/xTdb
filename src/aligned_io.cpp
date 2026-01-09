#include "xTdb/aligned_io.h"
#include "xTdb/platform_compat.h"
#include <cerrno>
#include <cstring>
#include <cassert>
#include <stdexcept>

// For fallocate (Linux)
#ifdef __linux__
#include <linux/falloc.h>
#endif

#ifdef _WIN32
#include <io.h>
#include <stdio.h>
// Windows doesn't support O_DIRECT easily, so we disable it
#ifndef O_DIRECT
#define O_DIRECT 0
#endif
#endif

namespace xtdb {

// ============================================================================
// AlignedIO Implementation
// ============================================================================

AlignedIO::AlignedIO()
    : fd_(-1),
      direct_io_(false) {
}

AlignedIO::~AlignedIO() {
    close();
}

IOResult AlignedIO::open(const std::string& path,
                         bool create_if_not_exists,
                         bool direct_io) {
    if (fd_ >= 0) {
        close();
    }

    path_ = path;
    direct_io_ = direct_io;

    // Prepare flags
    int flags = O_RDWR;
    if (create_if_not_exists) {
        flags |= O_CREAT;
    }

#ifdef O_DIRECT
    if (direct_io_) {
        flags |= O_DIRECT;
    }
#endif

    // Open file
#ifdef _WIN32
    int mode = _S_IREAD | _S_IWRITE;
    fd_ = ::_open(path.c_str(), flags, mode);
#else
    fd_ = ::open(path.c_str(), flags, 0644);
#endif
    if (fd_ < 0) {
        setError("Failed to open file: " + std::string(strerror(errno)));
        return IOResult::ERROR_OPEN_FAILED;
    }

    return IOResult::SUCCESS;
}

void AlignedIO::close() {
    if (fd_ >= 0) {
#ifdef _WIN32
        ::_close(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }
}

bool AlignedIO::validateAlignment(const void* buffer,
                                  uint64_t size,
                                  uint64_t offset) const {
    // Check buffer alignment (16KB)
    uintptr_t buffer_addr = reinterpret_cast<uintptr_t>(buffer);
    if (buffer_addr % kExtentSizeBytes != 0) {
        return false;
    }

    // Check size alignment (extent-aligned)
    if (!isExtentAligned(size)) {
        return false;
    }

    // Check offset alignment (extent-aligned)
    if (!isExtentAligned(offset)) {
        return false;
    }

    return true;
}

IOResult AlignedIO::write(const void* buffer, uint64_t size, uint64_t offset) {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERROR_INVALID_FD;
    }

    // CRITICAL: Enforce alignment constraints
    if (!validateAlignment(buffer, size, offset)) {
        setError("Alignment constraint violated: buffer, size, and offset "
                "must be 16KB-aligned");
        return IOResult::ERROR_ALIGNMENT;
    }

    // Perform pwrite
#ifdef _WIN32
    if (::_lseek(fd_, offset, SEEK_SET) == -1) {
        setError("lseek failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }
    int written = ::_write(fd_, buffer, static_cast<unsigned int>(size));
#else
    ssize_t written = ::pwrite(fd_, buffer, size, offset);
#endif
    if (written < 0) {
        setError("pwrite failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }

    if (static_cast<uint64_t>(written) != size) {
        setError("Partial write: expected " + std::to_string(size) +
                " bytes, wrote " + std::to_string(written) + " bytes");
        return IOResult::ERROR_IO_FAILED;
    }

    // Update statistics
    stats_.bytes_written += size;
    stats_.write_operations++;

    return IOResult::SUCCESS;
}

IOResult AlignedIO::read(void* buffer, uint64_t size, uint64_t offset) {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERROR_INVALID_FD;
    }

    // CRITICAL: Enforce alignment constraints
    if (!validateAlignment(buffer, size, offset)) {
        setError("Alignment constraint violated: buffer, size, and offset "
                "must be 16KB-aligned");
        return IOResult::ERROR_ALIGNMENT;
    }

    // Perform pread
#ifdef _WIN32
    if (::_lseek(fd_, offset, SEEK_SET) == -1) {
        setError("lseek failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }
    int bytes_read = ::_read(fd_, buffer, static_cast<unsigned int>(size));
#else
    ssize_t bytes_read = ::pread(fd_, buffer, size, offset);
#endif
    if (bytes_read < 0) {
        setError("pread failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }

    if (static_cast<uint64_t>(bytes_read) != size) {
        setError("Partial read: expected " + std::to_string(size) +
                " bytes, read " + std::to_string(bytes_read) + " bytes");
        return IOResult::ERROR_IO_FAILED;
    }

    // Update statistics
    stats_.bytes_read += size;
    stats_.read_operations++;

    return IOResult::SUCCESS;
}

IOResult AlignedIO::preallocate(uint64_t size) {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERROR_INVALID_FD;
    }

    // Check alignment
    if (!isExtentAligned(size)) {
        setError("Preallocate size must be extent-aligned");
        return IOResult::ERROR_ALIGNMENT;
    }

#ifdef __linux__
    // Use fallocate on Linux
    int ret = ::fallocate(fd_, 0, 0, size);
    if (ret != 0) {
        setError("fallocate failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_PREALLOCATE_FAILED;
    }
#elif defined(_WIN32)
    // Windows: use SetEndOfFile after seeking
    if (::_lseek(fd_, size, SEEK_SET) == -1) {
        setError("lseek failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_PREALLOCATE_FAILED;
    }
    if (::_chsize(fd_, size) != 0) {
        setError("chsize failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_PREALLOCATE_FAILED;
    }
#else
    // Fallback: use ftruncate
    if (::ftruncate(fd_, size) != 0) {
        setError("ftruncate failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_PREALLOCATE_FAILED;
    }
#endif

    return IOResult::SUCCESS;
}

IOResult AlignedIO::sync() {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERROR_INVALID_FD;
    }

#ifdef _WIN32
    if (::_commit(fd_) != 0) {
        setError("commit failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }
#else
    if (::fsync(fd_) != 0) {
        setError("fsync failed: " + std::string(strerror(errno)));
        return IOResult::ERROR_IO_FAILED;
    }
#endif

    return IOResult::SUCCESS;
}

int64_t AlignedIO::getFileSize() const {
    if (fd_ < 0) {
        return -1;
    }

    struct stat st;
    if (::fstat(fd_, &st) != 0) {
        return -1;
    }

    return st.st_size;
}

void AlignedIO::setError(const std::string& message) {
    last_error_ = message;
}

// ============================================================================
// AlignedBuffer Implementation
// ============================================================================

AlignedBuffer::AlignedBuffer(uint64_t size)
    : buffer_(nullptr),
      size_(0) {

    // Round up to extent alignment
    if (!isExtentAligned(size)) {
        size = ((size + kExtentSizeBytes - 1) / kExtentSizeBytes) * kExtentSizeBytes;
    }

    // Allocate aligned memory (16KB alignment)
#ifdef _WIN32
    buffer_ = _aligned_malloc(size, kExtentSizeBytes);
    if (buffer_ == nullptr) {
        throw std::bad_alloc();
    }
#else
    int ret = ::posix_memalign(&buffer_, kExtentSizeBytes, size);
    if (ret != 0 || buffer_ == nullptr) {
        throw std::bad_alloc();
    }
#endif

    size_ = size;
}

AlignedBuffer::~AlignedBuffer() {
    if (buffer_ != nullptr) {
#ifdef _WIN32
        _aligned_free(buffer_);
#else
        ::free(buffer_);
#endif
        buffer_ = nullptr;
    }
}

AlignedBuffer::AlignedBuffer(AlignedBuffer&& other) noexcept
    : buffer_(other.buffer_),
      size_(other.size_) {
    other.buffer_ = nullptr;
    other.size_ = 0;
}

AlignedBuffer& AlignedBuffer::operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
        if (buffer_ != nullptr) {
#ifdef _WIN32
            _aligned_free(buffer_);
#else
            ::free(buffer_);
#endif
        }
        buffer_ = other.buffer_;
        size_ = other.size_;
        other.buffer_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

void AlignedBuffer::zero() {
    if (buffer_ != nullptr) {
        ::memset(buffer_, 0, size_);
    }
}

}  // namespace xtdb
