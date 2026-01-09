#ifndef XTDB_ALIGNED_IO_H_
#define XTDB_ALIGNED_IO_H_

#include "constants.h"
#include <string>
#include <memory>
#include <cstdint>

namespace xtdb {

// ============================================================================
// AlignedIO: Enforces 16KB-aligned disk I/O operations
// ============================================================================

/// I/O operation result
enum class IOResult {
    SUCCESS = 0,
    ERR_OPENFD_FAILED,
    ERR_ALIGNMENT,      // Buffer or offset not 16KB-aligned
    ERR_IO_FAILED,
    ERR_INVALID_FD,
    ERR_PREALLOCATE_FAILED
};

/// I/O statistics
struct IOStats {
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_operations = 0;
    uint64_t read_operations = 0;
};

class AlignedIO {
public:
    /// Constructor
    AlignedIO();

    /// Destructor (closes file if open)
    ~AlignedIO();

    // Disable copy and move
    AlignedIO(const AlignedIO&) = delete;
    AlignedIO& operator=(const AlignedIO&) = delete;
    AlignedIO(AlignedIO&&) = delete;
    AlignedIO& operator=(AlignedIO&&) = delete;

    /// Open file for aligned I/O
    /// @param path File path
    /// @param create_if_not_exists Create file if it doesn't exist
    /// @param direct_io Enable O_DIRECT for direct I/O (bypass page cache)
    /// @return IOResult indicating success or error
    IOResult open(const std::string& path,
                  bool create_if_not_exists = true,
                  bool direct_io = false);

    /// Close file
    void close();

    /// Check if file is open
    bool isOpen() const { return fd_ >= 0; }

    /// Write data to file at specified offset
    /// CONSTRAINT: buffer must be 16KB-aligned, offset must be extent-aligned
    /// @param buffer Data to write (must be 16KB-aligned)
    /// @param size Size in bytes (must be extent-aligned)
    /// @param offset File offset in bytes (must be extent-aligned)
    /// @return IOResult indicating success or error
    IOResult write(const void* buffer, uint64_t size, uint64_t offset);

    /// Read data from file at specified offset
    /// CONSTRAINT: buffer must be 16KB-aligned, offset must be extent-aligned
    /// @param buffer Buffer to read into (must be 16KB-aligned)
    /// @param size Size in bytes (must be extent-aligned)
    /// @param offset File offset in bytes (must be extent-aligned)
    /// @return IOResult indicating success or error
    IOResult read(void* buffer, uint64_t size, uint64_t offset);

    /// Preallocate disk space (fallocate on Linux)
    /// Prevents fragmentation and ensures space is available
    /// @param size Size in bytes to preallocate (must be extent-aligned)
    /// @return IOResult indicating success or error
    IOResult preallocate(uint64_t size);

    /// Sync data to disk (fsync)
    /// @return IOResult indicating success or error
    IOResult sync();

    /// Get I/O statistics
    const IOStats& getStats() const { return stats_; }

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

    /// Get file descriptor (for advanced use)
    int getFd() const { return fd_; }

    /// Get current file size
    /// @return File size in bytes, or -1 on error
    int64_t getFileSize() const;

private:
    /// Validate alignment constraints
    /// @param buffer Buffer pointer
    /// @param size Size in bytes
    /// @param offset Offset in bytes
    /// @return true if all constraints satisfied
    bool validateAlignment(const void* buffer, uint64_t size, uint64_t offset) const;

    /// Set last error message
    void setError(const std::string& message);

    int fd_;                    // File descriptor (-1 if closed)
    std::string path_;          // File path
    bool direct_io_;            // O_DIRECT enabled
    IOStats stats_;             // I/O statistics
    std::string last_error_;    // Last error message
};

// ============================================================================
// AlignedBuffer: RAII wrapper for 16KB-aligned memory buffers
// ============================================================================

class AlignedBuffer {
public:
    /// Allocate aligned buffer
    /// @param size Size in bytes (will be rounded up to extent alignment)
    explicit AlignedBuffer(uint64_t size);

    /// Destructor (frees memory)
    ~AlignedBuffer();

    // Disable copy, enable move
    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&& other) noexcept;
    AlignedBuffer& operator=(AlignedBuffer&& other) noexcept;

    /// Get buffer pointer
    void* data() { return buffer_; }
    const void* data() const { return buffer_; }

    /// Get buffer size
    uint64_t size() const { return size_; }

    /// Zero out buffer
    void zero();

    /// Check if buffer is valid
    bool isValid() const { return buffer_ != nullptr; }

private:
    void* buffer_;   // Aligned buffer
    uint64_t size_;  // Buffer size (extent-aligned)
};

}  // namespace xtdb

#endif  // XTDB_ALIGNED_IO_H_
