#ifndef XTDB_COMPACT_CONTAINER_H_
#define XTDB_COMPACT_CONTAINER_H_

#include "container.h"
#include "compressor.h"
#include "compact_layout.h"
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace xtdb {

// ============================================================================
// CompactContainer - Variable-length compressed block container
// ============================================================================

/// COMPACT container implementation with variable-length compressed blocks
/// Layout: CompactChunkHeader (128) + CompactBlockIndex[] (80*n) + compressed data
class CompactContainer : public IContainer {
public:
    /// Constructor
    /// @param path File path
    /// @param layout Chunk layout configuration
    /// @param compression_type Compression algorithm to use
    /// @param direct_io Enable O_DIRECT for direct I/O
    /// @param read_only Open in read-only mode
    CompactContainer(const std::string& path,
                     const ChunkLayout& layout,
                     CompressionType compression_type = CompressionType::COMP_ZSTD,
                     bool direct_io = false,
                     bool read_only = false);

    /// Destructor
    ~CompactContainer() override;

    // Disable copy and move
    CompactContainer(const CompactContainer&) = delete;
    CompactContainer& operator=(const CompactContainer&) = delete;
    CompactContainer(CompactContainer&&) = delete;
    CompactContainer& operator=(CompactContainer&&) = delete;

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
    uint64_t getCapacity() const override;
    int64_t getCurrentSize() const override;
    const ContainerMetadata& getMetadata() const override { return metadata_; }
    const ContainerStats& getStats() const override { return stats_; }
    std::string getLastError() const override { return last_error_; }

    ContainerResult preallocate(uint64_t size) override;
    bool supportsDynamicGrowth() const override { return true; }
    bool isReadOnly() const override { return read_only_; }

    // ========================================================================
    // CompactContainer-specific Operations
    // ========================================================================

    /// Write a compressed block
    /// @param tag_id Tag identifier for the block
    /// @param block_index Original block index in RAW chunk
    /// @param data Original uncompressed data
    /// @param size Original data size
    /// @param start_ts_us Block start timestamp
    /// @param end_ts_us Block end timestamp
    /// @param record_count Number of records in block
    /// @param original_encoding Original encoding type
    /// @param value_type Value type
    /// @param time_unit Time unit
    /// @return ContainerResult
    ContainerResult writeBlock(uint32_t tag_id,
                               uint32_t block_index,
                               const void* data,
                               uint32_t size,
                               int64_t start_ts_us,
                               int64_t end_ts_us,
                               uint32_t record_count,
                               EncodingType original_encoding,
                               ValueType value_type,
                               TimeUnit time_unit);

    /// Read a compressed block
    /// @param block_index Block index to read
    /// @param buffer Output buffer for decompressed data
    /// @param buffer_size Buffer size
    /// @param actual_size Output: actual decompressed size
    /// @return ContainerResult
    ContainerResult readBlock(uint32_t block_index,
                             void* buffer,
                             uint32_t buffer_size,
                             uint32_t& actual_size);

    /// Get block index entry
    /// @param block_index Block index
    /// @return Pointer to CompactBlockIndex, or nullptr if not found
    const CompactBlockIndex* getBlockIndex(uint32_t block_index) const;

    /// Get number of blocks in container
    /// @return Number of blocks
    uint32_t getBlockCount() const;

    /// Get compression statistics
    /// @param total_original Output: total original size
    /// @param total_compressed Output: total compressed size
    /// @param compression_ratio Output: compression ratio
    void getCompressionStats(uint64_t& total_original,
                            uint64_t& total_compressed,
                            double& compression_ratio) const;

    /// Seal the container (finalize and write header)
    /// @return ContainerResult
    ContainerResult seal();

    /// Check if container is sealed
    /// @return true if sealed
    bool isSealed() const;

private:
    /// Initialize new COMPACT container
    /// @return ContainerResult
    ContainerResult initializeNewContainer();

    /// Read and validate COMPACT header
    /// @return ContainerResult
    ContainerResult readAndValidateHeader();

    /// Write COMPACT header to disk
    /// @return ContainerResult
    ContainerResult writeHeader();

    /// Read all block indices from disk
    /// @return ContainerResult
    ContainerResult readBlockIndices();

    /// Write block index to disk
    /// @param index Block index to write
    /// @return ContainerResult
    ContainerResult writeBlockIndex(const CompactBlockIndex& index);

    /// Calculate CRC32 for block data
    /// @param data Data buffer
    /// @param size Data size
    /// @return CRC32 checksum
    uint32_t calculateBlockCRC32(const void* data, uint32_t size) const;

    /// Update header metadata
    void updateHeaderMetadata();

    /// Set last error message
    /// @param message Error message
    void setError(const std::string& message);

    /// Open file descriptor
    /// @return ContainerResult
    ContainerResult openFile();

    /// Close file descriptor
    void closeFile();

    /// Read data from file
    /// @param buffer Buffer to read into
    /// @param size Size to read
    /// @param offset File offset
    /// @return ContainerResult
    ContainerResult readFile(void* buffer, size_t size, uint64_t offset);

    /// Write data to file
    /// @param buffer Data to write
    /// @param size Size to write
    /// @param offset File offset
    /// @return ContainerResult
    ContainerResult writeFile(const void* buffer, size_t size, uint64_t offset);

    /// Sync file to disk
    /// @return ContainerResult
    ContainerResult syncFile();

    /// Get file size
    /// @return File size in bytes, or -1 on error
    int64_t getFileSize() const;

    std::string path_;                              // File path
    ChunkLayout layout_;                            // Chunk layout configuration
    CompressionType compression_type_;              // Compression algorithm
    bool direct_io_;                                // O_DIRECT enabled (unused for COMPACT)
    bool read_only_;                                // Read-only mode
    bool is_open_;                                  // Open state
    bool is_sealed_;                                // Sealed state

    int fd_;                                        // File descriptor
    std::unique_ptr<ICompressor> compressor_;       // Compressor instance

    CompactChunkHeader header_;                     // COMPACT chunk header
    std::vector<CompactBlockIndex> block_indices_;  // Block index array

    uint64_t current_data_offset_;                  // Current offset in data region

    ContainerMetadata metadata_;                    // Container metadata
    ContainerStats stats_;                          // I/O statistics
    std::string last_error_;                        // Last error message

    mutable std::mutex mutex_;                      // Thread safety
};

}  // namespace xtdb

#endif  // XTDB_COMPACT_CONTAINER_H_
