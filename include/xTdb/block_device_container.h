#ifndef XTDB_BLOCK_DEVICE_CONTAINER_H_
#define XTDB_BLOCK_DEVICE_CONTAINER_H_

#include "container.h"
#include "aligned_io.h"
#include <memory>
#include <string>
#include <mutex>

namespace xtdb {

// ============================================================================
// BlockDeviceContainer - Block device container implementation
// ============================================================================

/// Block device container implementation
/// Stores data on raw block devices (e.g., /dev/sdb1, /dev/nvme0n1p1)
class BlockDeviceContainer : public IContainer {
public:
    /// Constructor
    /// @param device_path Block device path (e.g., /dev/sdb1)
    /// @param layout Chunk layout configuration
    /// @param read_only Open in read-only mode
    /// @param test_mode Allow using regular files for testing (bypasses block device check)
    BlockDeviceContainer(const std::string& device_path,
                         const ChunkLayout& layout,
                         bool read_only = false,
                         bool test_mode = false);

    /// Destructor
    ~BlockDeviceContainer() override;

    // Disable copy and move
    BlockDeviceContainer(const BlockDeviceContainer&) = delete;
    BlockDeviceContainer& operator=(const BlockDeviceContainer&) = delete;
    BlockDeviceContainer(BlockDeviceContainer&&) = delete;
    BlockDeviceContainer& operator=(BlockDeviceContainer&&) = delete;

    // ========================================================================
    // IContainer Interface Implementation
    // ========================================================================

    ContainerResult open(bool create_if_not_exists = true) override;
    void close() override;
    bool isOpen() const override;

    ContainerResult write(const void* buffer, uint64_t size, uint64_t offset) override;
    ContainerResult read(void* buffer, uint64_t size, uint64_t offset) override;
    ContainerResult sync() override;

    ContainerType getType() const override { return ContainerType::BLOCK_DEVICE; }
    std::string getIdentifier() const override { return device_path_; }
    uint64_t getCapacity() const override { return device_capacity_; }
    int64_t getCurrentSize() const override { return device_capacity_; }
    const ContainerMetadata& getMetadata() const override { return metadata_; }
    const ContainerStats& getStats() const override { return stats_; }
    std::string getLastError() const override { return last_error_; }

    ContainerResult preallocate(uint64_t size) override;
    bool supportsDynamicGrowth() const override { return false; }  // Fixed capacity
    bool isReadOnly() const override { return read_only_; }

    // ========================================================================
    // BlockDeviceContainer-specific Operations
    // ========================================================================

    /// Get device capacity in bytes
    /// @return Device capacity in bytes
    uint64_t getDeviceCapacity() const { return device_capacity_; }

    /// Get block size of device
    /// @return Block size in bytes
    uint64_t getDeviceBlockSize() const { return device_block_size_; }

    /// Check if device is a valid block device
    /// @param device_path Device path
    /// @return true if valid block device
    static bool isBlockDevice(const std::string& device_path);

    /// Get file descriptor (for advanced use)
    /// @return File descriptor, or -1 if not open
    int getFd() const;

    /// Get underlying AlignedIO instance (for StorageEngine integration)
    /// @return Pointer to AlignedIO, or nullptr if not open
    AlignedIO* getIO() { return io_.get(); }
    const AlignedIO* getIO() const { return io_.get(); }

private:
    /// Detect block device capacity and properties
    /// @return ContainerResult
    ContainerResult detectDeviceProperties();

    /// Initialize new container on block device (create header)
    /// @return ContainerResult
    ContainerResult initializeNewContainer();

    /// Read and validate container header
    /// @return ContainerResult
    ContainerResult readAndValidateHeader();

    /// Set last error message
    /// @param message Error message
    void setError(const std::string& message);

    std::string device_path_;                   // Block device path
    ChunkLayout layout_;                        // Chunk layout configuration
    bool read_only_;                            // Read-only mode
    bool test_mode_;                            // Test mode (allow regular files)
    bool is_open_;                              // Open state

    std::unique_ptr<AlignedIO> io_;             // Aligned I/O handler (for StorageEngine compatibility)
    int fd_;                                    // File descriptor (direct access)
    uint64_t device_capacity_;                  // Device capacity in bytes
    uint64_t device_block_size_;                // Device block size

    ContainerMetadata metadata_;                // Container metadata
    ContainerStats stats_;                      // I/O statistics
    std::string last_error_;                    // Last error message

    mutable std::mutex mutex_;                  // Thread-safety mutex
};

}  // namespace xtdb

#endif  // XTDB_BLOCK_DEVICE_CONTAINER_H_
