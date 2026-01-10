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
#include <windows.h>
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
#ifdef _WIN32
    flags |= O_BINARY;  // Always use binary mode on Windows
#endif

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
        return IOResult::ERR_OPENFD_FAILED;
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
        return IOResult::ERR_INVALID_FD;
    }

    // CRITICAL: Enforce alignment constraints
    if (!validateAlignment(buffer, size, offset)) {
        setError("Alignment constraint violated: buffer, size, and offset "
                "must be 16KB-aligned");
        return IOResult::ERR_ALIGNMENT;
    }

    // Perform pwrite
#ifdef _WIN32
    // On Windows, use WriteFile API for better control and reliability
    HANDLE hFile = reinterpret_cast<HANDLE>(::_get_osfhandle(fd_));
    if (hFile == INVALID_HANDLE_VALUE) {
        setError("Invalid file handle");
        return IOResult::ERR_INVALID_FD;
    }
    
    // Ensure file is large enough before writing
    int64_t current_size = getFileSize();
    uint64_t required_size = offset + size;
    if (current_size >= 0 && static_cast<uint64_t>(current_size) < required_size) {
        // Extend file using SetFilePointerEx and SetEndOfFile
        LARGE_INTEGER liSize;
        liSize.QuadPart = static_cast<LONGLONG>(required_size);
        if (::SetFilePointerEx(hFile, liSize, nullptr, FILE_BEGIN) == 0) {
            setError("SetFilePointerEx for extension failed: " + std::to_string(::GetLastError()));
            return IOResult::ERR_IO_FAILED;
        }
        if (::SetEndOfFile(hFile) == 0) {
            setError("SetEndOfFile failed: " + std::to_string(::GetLastError()));
            return IOResult::ERR_IO_FAILED;
        }
    }
    
    // Set file pointer to write position
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(hFile, liOffset, nullptr, FILE_BEGIN) == 0) {
        setError("SetFilePointerEx failed: " + std::to_string(::GetLastError()));
        return IOResult::ERR_IO_FAILED;
    }
    
    // Write data using WriteFile
    DWORD bytes_written = 0;
    DWORD size_to_write = static_cast<DWORD>(size);
    if (size_to_write != size) {
        setError("Size too large for Windows WriteFile");
        return IOResult::ERR_IO_FAILED;
    }
    
    if (::WriteFile(hFile, buffer, size_to_write, &bytes_written, nullptr) == 0) {
        setError("WriteFile failed: " + std::to_string(::GetLastError()));
        return IOResult::ERR_IO_FAILED;
    }
    
    int written = static_cast<int>(bytes_written);
#else
    ssize_t written = ::pwrite(fd_, buffer, size, offset);
#endif
    if (written < 0) {
        setError("pwrite failed: " + std::string(strerror(errno)));
        return IOResult::ERR_IO_FAILED;
    }

    if (static_cast<uint64_t>(written) != size) {
        setError("Partial write: expected " + std::to_string(size) +
                " bytes, wrote " + std::to_string(written) + " bytes");
        return IOResult::ERR_IO_FAILED;
    }

    // Update statistics
    stats_.bytes_written += size;
    stats_.write_operations++;

    return IOResult::SUCCESS;
}

IOResult AlignedIO::read(void* buffer, uint64_t size, uint64_t offset) {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERR_INVALID_FD;
    }

    // CRITICAL: Enforce alignment constraints
    if (!validateAlignment(buffer, size, offset)) {
        setError("Alignment constraint violated: buffer, size, and offset "
                "must be 16KB-aligned");
        return IOResult::ERR_ALIGNMENT;
    }

    // Perform pread
#ifdef _WIN32
    // On Windows, use ReadFile API for better control
    HANDLE hFile = reinterpret_cast<HANDLE>(::_get_osfhandle(fd_));
    if (hFile == INVALID_HANDLE_VALUE) {
        setError("Invalid file handle");
        return IOResult::ERR_INVALID_FD;
    }
    
    // Set file pointer
    LARGE_INTEGER liOffset;
    liOffset.QuadPart = static_cast<LONGLONG>(offset);
    if (::SetFilePointerEx(hFile, liOffset, nullptr, FILE_BEGIN) == 0) {
        setError("SetFilePointerEx failed: " + std::to_string(::GetLastError()));
        return IOResult::ERR_IO_FAILED;
    }
    
    // Read data
    DWORD bytes_read = 0;
    DWORD size_to_read = static_cast<DWORD>(size);
    if (size_to_read != size) {
        setError("Size too large for Windows ReadFile");
        return IOResult::ERR_IO_FAILED;
    }
    
    if (::ReadFile(hFile, buffer, size_to_read, &bytes_read, nullptr) == 0) {
        setError("ReadFile failed: " + std::to_string(::GetLastError()));
        return IOResult::ERR_IO_FAILED;
    }
    
    if (bytes_read != size_to_read) {
        setError("Partial read: expected " + std::to_string(size_to_read) +
                " bytes, read " + std::to_string(bytes_read) + " bytes");
        return IOResult::ERR_IO_FAILED;
    }
#else
    ssize_t bytes_read = ::pread(fd_, buffer, size, offset);
    if (bytes_read < 0) {
        setError("pread failed: " + std::string(strerror(errno)));
        return IOResult::ERR_IO_FAILED;
    }

    if (static_cast<uint64_t>(bytes_read) != size) {
        setError("Partial read: expected " + std::to_string(size) +
                " bytes, read " + std::to_string(bytes_read) + " bytes");
        return IOResult::ERR_IO_FAILED;
    }
#endif

    // Update statistics
    stats_.bytes_read += size;
    stats_.read_operations++;

    return IOResult::SUCCESS;
}

IOResult AlignedIO::preallocate(uint64_t size) {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERR_INVALID_FD;
    }

    // Check alignment
    if (!isExtentAligned(size)) {
        setError("Preallocate size must be extent-aligned");
        return IOResult::ERR_ALIGNMENT;
    }

#ifdef __linux__
    // Use fallocate on Linux
    int ret = ::fallocate(fd_, 0, 0, size);
    if (ret != 0) {
        setError("fallocate failed: " + std::string(strerror(errno)));
        return IOResult::ERR_PREALLOCATE_FAILED;
    }
#elif defined(_WIN32)
    // Windows: use SetEndOfFile after seeking
    if (::_lseeki64(fd_, size, SEEK_SET) == -1) {
        setError("lseek failed: " + std::string(strerror(errno)));
        return IOResult::ERR_PREALLOCATE_FAILED;
    }
    if (::_chsize_s(fd_, size) != 0) {
        setError("chsize failed: " + std::string(strerror(errno)));
        return IOResult::ERR_PREALLOCATE_FAILED;
    }
#else
    // Fallback: use ftruncate
    if (::ftruncate(fd_, size) != 0) {
        setError("ftruncate failed: " + std::string(strerror(errno)));
        return IOResult::ERR_PREALLOCATE_FAILED;
    }
#endif

    return IOResult::SUCCESS;
}

IOResult AlignedIO::sync() {
    if (fd_ < 0) {
        setError("File not open");
        return IOResult::ERR_INVALID_FD;
    }

#ifdef _WIN32
    // First commit any buffered data
    if (::_commit(fd_) != 0) {
        setError("commit failed: " + std::string(strerror(errno)));
        return IOResult::ERR_IO_FAILED;
    }
    
    // Then use FlushFileBuffers to ensure data is written to disk
    HANDLE hFile = reinterpret_cast<HANDLE>(::_get_osfhandle(fd_));
    if (hFile == INVALID_HANDLE_VALUE) {
        setError("Invalid file handle for flush");
        return IOResult::ERR_INVALID_FD;
    }
    
    if (::FlushFileBuffers(hFile) == 0) {
        setError("FlushFileBuffers failed: " + std::to_string(::GetLastError()));
        return IOResult::ERR_IO_FAILED;
    }
#else
    if (::fsync(fd_) != 0) {
        setError("fsync failed: " + std::string(strerror(errno)));
        return IOResult::ERR_IO_FAILED;
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
