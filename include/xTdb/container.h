#ifndef XTDB_CONTAINER_H_
#define XTDB_CONTAINER_H_

#include "constants.h"
#include "struct_defs.h"
#include "layout_calculator.h"
#include <string>
#include <cstdint>
#include <memory>

namespace xtdb {

// ============================================================================
// Container Type and Strategy Enums
// ============================================================================

/// Container type: file-based or block device
enum class ContainerType : uint8_t {
    FILE_BASED = 1,      // Regular file on filesystem
    BLOCK_DEVICE = 2     // Raw block device (e.g., /dev/sdb1)
};

/// Container rollover strategy (for file-based containers)
enum class RolloverStrategy : uint8_t {
    NONE = 0,            // Single file, no rollover
    DAILY = 1,           // New file every day
    SIZE_BASED = 2,      // New file when size limit reached
    TIME_BASED = 3       // New file at specified time intervals
};

/// Container operation result codes
enum class ContainerResult {
    SUCCESS = 0,
    ERR_OPENFD_FAILED,
    ERR_CREATE_FAILED,
    ERR_READ_FAILED,
    ERR_WRITE_FAILED,
    ERR_SYNC_FAILED,
    ERR_INVALID_HEADER,
    ERR_INVALID_OFFSET,
    ERR_INVALID_SIZE,
    ERR_NOT_OPEN,
    ERR_ALREADY_OPEN,
    ERR_DEVICE_NOT_FOUND,
    ERR_INSUFFICIENT_PERMISSIONS
};

/// Container metadata (from header)
struct ContainerMetadata {
    uint8_t db_instance_id[16];     // Database instance ID
    ContainerLayout layout;          // RAW_FIXED or COMPACT_VAR
    CapacityType capacity_type;      // DYNAMIC or FIXED
    ArchiveLevel archive_level;      // RAW, RESAMPLED, etc.
    uint64_t capacity_extents;       // Total capacity in extents
    uint64_t capacity_bytes;         // Total capacity in bytes
    uint32_t chunk_size_extents;     // Chunk size in extents
    uint32_t block_size_extents;     // Block size in extents
    int64_t created_ts_us;           // Creation timestamp

    ContainerMetadata() {
        std::memset(db_instance_id, 0, 16);
        layout = ContainerLayout::LAYOUT_RAW_FIXED;
        capacity_type = CapacityType::CAP_DYNAMIC;
        archive_level = ArchiveLevel::ARCHIVE_RAW;
        capacity_extents = 0;
        capacity_bytes = 0;
        chunk_size_extents = 0;
        block_size_extents = 0;
        created_ts_us = 0;
    }
};

/// Container I/O statistics
struct ContainerStats {
    uint64_t bytes_written = 0;
    uint64_t bytes_read = 0;
    uint64_t write_operations = 0;
    uint64_t read_operations = 0;
    uint64_t sync_operations = 0;
};

// ============================================================================
// IContainer - Abstract Container Interface
// ============================================================================

/// Abstract interface for all container types
/// Provides unified access to file-based and block device containers
class IContainer {
public:
    virtual ~IContainer() = default;

    // ========================================================================
    // Core Operations
    // ========================================================================

    /// Open container (create if needed)
    /// @param create_if_not_exists Create new container if it doesn't exist
    /// @return ContainerResult indicating success or error
    virtual ContainerResult open(bool create_if_not_exists = true) = 0;

    /// Close container
    virtual void close() = 0;

    /// Check if container is open
    /// @return true if container is open and ready for I/O
    virtual bool isOpen() const = 0;

    // ========================================================================
    // I/O Operations (16KB-aligned)
    // ========================================================================

    /// Write data to container at specified offset
    /// CONSTRAINT: buffer must be 16KB-aligned, size and offset must be extent-aligned
    /// @param buffer Data to write (must be 16KB-aligned)
    /// @param size Size in bytes (must be extent-aligned)
    /// @param offset Offset in bytes (must be extent-aligned)
    /// @return ContainerResult indicating success or error
    virtual ContainerResult write(const void* buffer, uint64_t size, uint64_t offset) = 0;

    /// Read data from container at specified offset
    /// CONSTRAINT: buffer must be 16KB-aligned, size and offset must be extent-aligned
    /// @param buffer Buffer to read into (must be 16KB-aligned)
    /// @param size Size in bytes (must be extent-aligned)
    /// @param offset Offset in bytes (must be extent-aligned)
    /// @return ContainerResult indicating success or error
    virtual ContainerResult read(void* buffer, uint64_t size, uint64_t offset) = 0;

    /// Sync data to disk
    /// @return ContainerResult indicating success or error
    virtual ContainerResult sync() = 0;

    // ========================================================================
    // Container Properties
    // ========================================================================

    /// Get container type
    /// @return ContainerType (FILE_BASED or BLOCK_DEVICE)
    virtual ContainerType getType() const = 0;

    /// Get container identifier (file path or device path)
    /// @return String identifier
    virtual std::string getIdentifier() const = 0;

    /// Get container capacity in bytes
    /// @return Capacity in bytes, or 0 if dynamic/unknown
    virtual uint64_t getCapacity() const = 0;

    /// Get current container size in bytes (actual data written)
    /// @return Size in bytes, or -1 on error
    virtual int64_t getCurrentSize() const = 0;

    /// Get container metadata
    /// @return ContainerMetadata structure
    virtual const ContainerMetadata& getMetadata() const = 0;

    /// Get I/O statistics
    /// @return ContainerStats structure
    virtual const ContainerStats& getStats() const = 0;

    /// Get last error message
    /// @return Error message string
    virtual std::string getLastError() const = 0;

    // ========================================================================
    // Advanced Operations
    // ========================================================================

    /// Preallocate disk space (for file-based containers)
    /// @param size Size in bytes to preallocate
    /// @return ContainerResult indicating success or error
    virtual ContainerResult preallocate(uint64_t size) = 0;

    /// Check if container supports dynamic growth
    /// @return true if container can grow dynamically
    virtual bool supportsDynamicGrowth() const = 0;

    /// Check if container is read-only
    /// @return true if container is read-only
    virtual bool isReadOnly() const = 0;
};

// ============================================================================
// Container Factory
// ============================================================================

/// Container configuration for factory
struct ContainerConfig {
    ContainerType type;                  // Container type
    std::string path;                    // File path or device path
    ChunkLayout layout;                  // Chunk and block layout
    bool create_if_not_exists;           // Create if doesn't exist
    bool direct_io;                      // Enable O_DIRECT (bypass page cache)
    bool read_only;                      // Open in read-only mode
    bool test_mode;                      // Test mode for block device (allow regular files)
    uint64_t preallocate_size;           // Preallocate size (0 = no preallocation)
    RolloverStrategy rollover_strategy;  // Rollover strategy (file-based only)

    ContainerConfig()
        : type(ContainerType::FILE_BASED),
          path(""),
          create_if_not_exists(true),
          direct_io(false),
          read_only(false),
          test_mode(false),
          preallocate_size(0),
          rollover_strategy(RolloverStrategy::NONE) {
        // Default layout: 16KB blocks, 256MB chunks
        layout.block_size_bytes = 16384;
        layout.chunk_size_bytes = 256 * 1024 * 1024;
        layout.meta_blocks = 0;
        layout.data_blocks = 0;
    }
};

/// Container factory - creates appropriate container type
class ContainerFactory {
public:
    /// Create container instance based on configuration
    /// @param config Container configuration
    /// @return Unique pointer to IContainer, or nullptr on error
    static std::unique_ptr<IContainer> create(const ContainerConfig& config);

    /// Detect container type from path
    /// @param path File or device path
    /// @return ContainerType, or FILE_BASED if cannot detect
    static ContainerType detectType(const std::string& path);

    /// Validate container configuration
    /// @param config Container configuration
    /// @return true if configuration is valid
    static bool validateConfig(const ContainerConfig& config);
};

}  // namespace xtdb

#endif  // XTDB_CONTAINER_H_
