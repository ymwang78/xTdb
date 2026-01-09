#ifndef XTDB_FILE_CONTAINER_H_
#define XTDB_FILE_CONTAINER_H_

#include "container.h"
#include "aligned_io.h"
#include <memory>
#include <string>
#include <mutex>

namespace xtdb {

// ============================================================================
// FileContainer - File-based container implementation
// ============================================================================

/// File-based container implementation
/// Stores data in regular files on the filesystem
class FileContainer : public IContainer {
public:
    /// Constructor
    /// @param path File path
    /// @param layout Chunk layout configuration
    /// @param direct_io Enable O_DIRECT for direct I/O
    /// @param read_only Open in read-only mode
    FileContainer(const std::string& path,
                  const ChunkLayout& layout,
                  bool direct_io = false,
                  bool read_only = false);

    /// Destructor
    ~FileContainer() override;

    // Disable copy and move
    FileContainer(const FileContainer&) = delete;
    FileContainer& operator=(const FileContainer&) = delete;
    FileContainer(FileContainer&&) = delete;
    FileContainer& operator=(FileContainer&&) = delete;

    // ========================================================================
    // IContainer Interface Implementation
    // ========================================================================

    ContainerResult open(bool create_if_not_exists = true) override;
    void close() override;
    bool isOpen() const override;

    ContainerResult write(const void* buffer, uint64_t size, uint64_t offset) override;
    ContainerResult read(void* buffer, uint64_t size, uint64_t offset) override;
    ContainerResult sync() override;

    ContainerType getType() const override { return ContainerType::FILE_BASED; }
    std::string getIdentifier() const override { return path_; }
    uint64_t getCapacity() const override { return metadata_.capacity_bytes; }
    int64_t getCurrentSize() const override;
    const ContainerMetadata& getMetadata() const override { return metadata_; }
    const ContainerStats& getStats() const override { return stats_; }
    std::string getLastError() const override { return last_error_; }

    ContainerResult preallocate(uint64_t size) override;
    bool supportsDynamicGrowth() const override { return true; }
    bool isReadOnly() const override { return read_only_; }

    // ========================================================================
    // FileContainer-specific Operations
    // ========================================================================

    /// Get file descriptor (for advanced use)
    /// @return File descriptor, or -1 if not open
    int getFd() const;

    /// Get underlying AlignedIO instance
    /// @return Pointer to AlignedIO, or nullptr if not open
    AlignedIO* getIO() { return io_.get(); }
    const AlignedIO* getIO() const { return io_.get(); }

private:
    /// Initialize new container (create header and initial structures)
    /// @return ContainerResult
    ContainerResult initializeNewContainer();

    /// Read and validate container header
    /// @return ContainerResult
    ContainerResult readAndValidateHeader();

    /// Set last error message
    /// @param message Error message
    void setError(const std::string& message);

    /// Convert IOResult to ContainerResult
    /// @param io_result IOResult from AlignedIO
    /// @return Corresponding ContainerResult
    ContainerResult convertIOResult(IOResult io_result);

    std::string path_;                          // File path
    ChunkLayout layout_;                        // Chunk layout configuration
    bool direct_io_;                            // O_DIRECT enabled
    bool read_only_;                            // Read-only mode
    bool is_open_;                              // Open state

    std::unique_ptr<AlignedIO> io_;             // Aligned I/O handler
    ContainerMetadata metadata_;                // Container metadata
    ContainerStats stats_;                      // I/O statistics
    std::string last_error_;                    // Last error message

    mutable std::mutex mutex_;                  // Thread-safety mutex
};

}  // namespace xtdb

#endif  // XTDB_FILE_CONTAINER_H_
