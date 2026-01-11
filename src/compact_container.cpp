#include "xTdb/compact_container.h"
#include "xTdb/constants.h"
#include <cstring>
#include <chrono>
#include <iostream>
#include <zlib.h>

// Platform-specific includes
#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#else
    #include <unistd.h>
    #include <fcntl.h>
    #include <sys/types.h>
    #include <sys/stat.h>
#endif

namespace xtdb {

// ============================================================================
// Constructor & Destructor
// ============================================================================

CompactContainer::CompactContainer(const std::string& path,
                                   const ChunkLayout& layout,
                                   CompressionType compression_type,
                                   bool direct_io,
                                   bool read_only)
    : path_(path),
      layout_(layout),
      compression_type_(compression_type),
      direct_io_(direct_io),
      read_only_(read_only),
      is_open_(false),
      is_sealed_(false),
      fd_(-1),
      current_data_offset_(0) {


    // Create compressor
    compressor_ = CompressorFactory::create(compression_type);
    if (!compressor_) {
        setError("Failed to create compressor for type " +
                 std::string(CompressorFactory::getTypeName(compression_type)));
    }

    // Initialize header
    std::memcpy(header_.magic, kCompactChunkMagic, 8);
    header_.version = kCompactChunkVersion;
    header_.header_size = sizeof(CompactChunkHeader);
    header_.compression_type = static_cast<uint8_t>(compression_type);

    // Initialize metadata
    metadata_.layout = ContainerLayout::LAYOUT_COMPACT_VAR;
    metadata_.capacity_type = CapacityType::CAP_DYNAMIC;
    metadata_.archive_level = ArchiveLevel::ARCHIVE_RAW;  // COMPACT is compressed RAW data
    metadata_.chunk_size_extents = layout.chunk_size_bytes / kExtentSizeBytes;
    metadata_.block_size_extents = layout.block_size_bytes / kExtentSizeBytes;
}

CompactContainer::~CompactContainer() {
    close();
}

// ============================================================================
// Core Operations
// ============================================================================

ContainerResult CompactContainer::open(bool create_if_not_exists) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_open_) {
        return ContainerResult::ERR_ALREADY_OPEN;
    }

    if (!compressor_) {
        setError("Compressor not available");
        return ContainerResult::ERR_CREATE_FAILED;
    }

    // Open file
    ContainerResult result = openFile();
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    // Try to read and validate header
    ContainerResult header_result = readAndValidateHeader();
    if (header_result == ContainerResult::SUCCESS) {
        // Successfully read header, this is an existing container
        // Read block indices
        ContainerResult read_result = readBlockIndices();
        if (read_result != ContainerResult::SUCCESS) {
            closeFile();
            return read_result;
        }

        is_open_ = true;
        return ContainerResult::SUCCESS;
    }

    // Header read failed - check if we should initialize new container
    if (!create_if_not_exists) {
        setError("Container file exists but header is invalid");
        closeFile();
        return ContainerResult::ERR_INVALID_HEADER;
    }

    // Check if this is a new file
    int64_t file_size = getFileSize();
    bool is_new_file = (file_size < 0 || file_size <= static_cast<int64_t>(sizeof(CompactChunkHeader)));

    if (is_new_file) {
        // Initialize new container
        ContainerResult init_result = initializeNewContainer();
        if (init_result != ContainerResult::SUCCESS) {
            closeFile();
            return init_result;
        }

        is_open_ = true;
        return ContainerResult::SUCCESS;
    } else {
        // File exists and is large enough but header is invalid
        setError("Container file exists but header is invalid (file size: " +
                 std::to_string(file_size) + " bytes)");
        closeFile();
        return ContainerResult::ERR_INVALID_HEADER;
    }
}

void CompactContainer::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_open_ && !read_only_ && !is_sealed_) {
        // Auto-seal on close if not read-only
        writeHeader();
        syncFile();
    }

    closeFile();

    block_indices_.clear();
    is_open_ = false;
    is_sealed_ = false;
}

bool CompactContainer::isOpen() const {
    // No mutex lock needed - atomic read of boolean
    return is_open_;
}

ContainerResult CompactContainer::write(const void* /* buffer */, uint64_t /* size */, uint64_t /* offset */) {
    // This is the generic IContainer write interface
    // For COMPACT containers, use writeBlock() instead
    setError("Direct write not supported for COMPACT containers, use writeBlock()");
    return ContainerResult::ERR_INVALID_SIZE;
}

ContainerResult CompactContainer::read(void* /* buffer */, uint64_t /* size */, uint64_t /* offset */) {
    // This is the generic IContainer read interface
    // For COMPACT containers, use readBlock() instead
    setError("Direct read not supported for COMPACT containers, use readBlock()");
    return ContainerResult::ERR_INVALID_SIZE;
}

ContainerResult CompactContainer::sync() {
    // No mutex lock needed - syncFile() is thread-safe at OS level
    // Also, this method is called from seal() which already holds the lock
    if (!is_open_) {
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        return ContainerResult::SUCCESS;
    }

    ContainerResult result = syncFile();
    if (result == ContainerResult::SUCCESS) {
        stats_.sync_operations++;
    }

    return result;
}

// ========================================================================
// Property Accessors
// ========================================================================

uint64_t CompactContainer::getCapacity() const {
    // No mutex lock needed - calculating from constants
    // COMPACT containers have dynamic capacity
    // Return theoretical maximum based on layout
    uint64_t max_chunk_size = CompactLayoutCalculator::getMaxChunkSize();

    return max_chunk_size;
}

int64_t CompactContainer::getCurrentSize() const {
    // No mutex lock needed - atomic reads and file size query is thread-safe
    if (!is_open_) {
        return -1;
    }

    return getFileSize();
}

ContainerResult CompactContainer::preallocate(uint64_t /* size */) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Cannot preallocate in read-only mode");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    // COMPACT containers use standard I/O, preallocation not needed
    // Just return success
    return ContainerResult::SUCCESS;
}

// ========================================================================
// COMPACT-specific Operations
// ========================================================================

ContainerResult CompactContainer::writeBlock(uint32_t tag_id,
                                             uint32_t block_index,
                                             const void* data,
                                             uint32_t size,
                                             int64_t start_ts_us,
                                             int64_t end_ts_us,
                                             uint32_t record_count,
                                             EncodingType original_encoding,
                                             ValueType value_type,
                                             TimeUnit time_unit) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Cannot write in read-only mode");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    if (is_sealed_) {
        setError("Container is sealed, cannot write");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    if (!data || size == 0) {
        setError("Invalid data or size");
        return ContainerResult::ERR_INVALID_SIZE;
    }

    // Check block count limit
    if (block_indices_.size() >= CompactLayoutCalculator::getMaxBlocksPerChunk()) {
        setError("Maximum block count reached");
        return ContainerResult::ERR_INVALID_SIZE;
    }

    // Compress data
    size_t max_compressed = compressor_->getMaxCompressedSize(size);
    std::vector<uint8_t> compressed_buffer(max_compressed);
    size_t compressed_size = 0;

    CompressionResult comp_result = compressor_->compress(
        data,
        size,
        compressed_buffer.data(),
        compressed_buffer.size(),
        compressed_size,
        CompressorFactory::getRecommendedLevel(compression_type_)
    );

    if (comp_result != CompressionResult::SUCCESS) {
        setError("Compression failed");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    // Calculate data offset (will be written to disk)
    // Use MAX block count to reserve space for all potential indices
    uint64_t data_region_offset = getCompactDataOffset(CompactLayoutCalculator::getMaxBlocksPerChunk());
    uint64_t block_data_offset = data_region_offset + current_data_offset_;

    // Write compressed data
    ContainerResult result = writeFile(compressed_buffer.data(), compressed_size, block_data_offset);
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    // Create block index entry
    CompactBlockIndex index;
    index.tag_id = tag_id;
    index.original_block_index = block_index;
    index.data_offset = current_data_offset_;
    index.compressed_size = static_cast<uint32_t>(compressed_size);
    index.original_size = size;
    index.start_ts_us = start_ts_us;
    index.end_ts_us = end_ts_us;
    index.record_count = record_count;
    index.original_encoding = static_cast<uint8_t>(original_encoding);
    index.value_type = static_cast<uint8_t>(value_type);
    index.time_unit = static_cast<uint8_t>(time_unit);
    index.block_crc32 = calculateBlockCRC32(data, size);

    // Add to index array
    block_indices_.push_back(index);

    // Update offsets
    current_data_offset_ += compressed_size;
    header_.block_count = static_cast<uint32_t>(block_indices_.size());
    header_.total_compressed_size += compressed_size;
    header_.total_original_size += size;

    // Update timestamp range
    if (header_.block_count == 1) {
        header_.start_ts_us = start_ts_us;
        header_.end_ts_us = end_ts_us;
    } else {
        if (start_ts_us < header_.start_ts_us) header_.start_ts_us = start_ts_us;
        if (end_ts_us > header_.end_ts_us) header_.end_ts_us = end_ts_us;
    }

    // Update statistics
    stats_.bytes_written += compressed_size;
    stats_.write_operations++;

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::readBlock(uint32_t block_index,
                                            void* buffer,
                                            uint32_t buffer_size,
                                            uint32_t& actual_size) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return ContainerResult::ERR_NOT_OPEN;
    }

    // Find block index
    if (block_index >= block_indices_.size()) {
        setError("Block index out of range");
        return ContainerResult::ERR_INVALID_OFFSET;
    }

    const CompactBlockIndex& index = block_indices_[block_index];

    // Check buffer size
    if (buffer_size < index.original_size) {
        setError("Buffer too small for decompressed data");
        return ContainerResult::ERR_INVALID_SIZE;
    }

    // Calculate file offset for compressed data
    // Use MAX block count since indices are written to reserved space during seal
    uint64_t data_region_offset = getCompactDataOffset(CompactLayoutCalculator::getMaxBlocksPerChunk());
    uint64_t block_file_offset = data_region_offset + index.data_offset;

    // Read compressed data
    std::vector<uint8_t> compressed_buffer(index.compressed_size);
    ContainerResult result = readFile(compressed_buffer.data(), index.compressed_size, block_file_offset);
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    // Decompress
    size_t decompressed_size = 0;
    CompressionResult comp_result = compressor_->decompress(
        compressed_buffer.data(),
        index.compressed_size,
        buffer,
        buffer_size,
        decompressed_size
    );

    if (comp_result != CompressionResult::SUCCESS) {
        setError("Decompression failed");
        return ContainerResult::ERR_READ_FAILED;
    }

    // Verify size
    if (decompressed_size != index.original_size) {
        setError("Decompressed size mismatch");
        return ContainerResult::ERR_READ_FAILED;
    }

    // Verify CRC32
    uint32_t crc = calculateBlockCRC32(buffer, index.original_size);
    if (crc != index.block_crc32) {
        setError("CRC32 mismatch - data corruption detected");
        return ContainerResult::ERR_READ_FAILED;
    }

    actual_size = index.original_size;
    stats_.bytes_read += index.compressed_size;
    stats_.read_operations++;

    return ContainerResult::SUCCESS;
}

const CompactBlockIndex* CompactContainer::getBlockIndex(uint32_t block_index) const {
    // No mutex lock needed - atomic read of vector size
    if (block_index >= block_indices_.size()) {
        return nullptr;
    }

    return &block_indices_[block_index];
}

uint32_t CompactContainer::getBlockCount() const {
    // No mutex lock needed - atomic read of vector size
    return static_cast<uint32_t>(block_indices_.size());
}

void CompactContainer::getCompressionStats(uint64_t& total_original,
                                          uint64_t& total_compressed,
                                          double& compression_ratio) const {
    // No mutex lock needed - atomic reads of header fields
    total_original = header_.total_original_size;
    total_compressed = header_.total_compressed_size;
    compression_ratio = calculateCompressionRatio(total_original, total_compressed);
}

ContainerResult CompactContainer::seal() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_open_) {
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("Cannot seal in read-only mode");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    if (is_sealed_) {
        return ContainerResult::SUCCESS;  // Already sealed
    }

    // Write all block indices
    uint64_t index_offset = getCompactIndexOffset();
    for (size_t i = 0; i < block_indices_.size(); i++) {
        uint64_t file_offset = index_offset + i * sizeof(CompactBlockIndex);
        ContainerResult result = writeFile(&block_indices_[i], sizeof(CompactBlockIndex), file_offset);
        if (result != ContainerResult::SUCCESS) {
            return result;
        }
    }

    // Update header offsets
    header_.index_offset = getCompactIndexOffset();
    // Data offset uses MAX block count to match where data was actually written
    header_.data_offset = getCompactDataOffset(CompactLayoutCalculator::getMaxBlocksPerChunk());

    // Calculate super CRC32 (over header + indices)
    uint32_t super_crc = 0;
    // Simplified: just use header data for now
    super_crc = crc32(0L, Z_NULL, 0);
    super_crc = crc32(super_crc, reinterpret_cast<const Bytef*>(&header_),
                      offsetof(CompactChunkHeader, super_crc32));
    header_.super_crc32 = super_crc;

    // Set sealed flag (clear the sealed bit using chunkClearBit)
    header_.flags = chunkClearBit(header_.flags, ChunkStateBit::CHB_SEALED);

    // Write final header
    ContainerResult result = writeHeader();
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    // Sync to disk
    result = sync();
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    is_sealed_ = true;
    updateHeaderMetadata();

    return ContainerResult::SUCCESS;
}

bool CompactContainer::isSealed() const {
    // No mutex lock needed - atomic read of boolean
    return is_sealed_;
}

// ========================================================================
// Private Helper Methods
// ========================================================================

ContainerResult CompactContainer::initializeNewContainer() {
    // Initialize header fields
    header_.chunk_id = 0;
    header_.flags = kChunkFlagsInit;
    header_.block_count = 0;
    header_.index_offset = getCompactIndexOffset();
    // Reserve space for maximum possible indices
    header_.data_offset = getCompactDataOffset(CompactLayoutCalculator::getMaxBlocksPerChunk());
    header_.total_compressed_size = 0;
    header_.total_original_size = 0;
    header_.start_ts_us = 0;
    header_.end_ts_us = 0;
    header_.super_crc32 = 0;

    // Set creation timestamp
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto micros = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    metadata_.created_ts_us = micros;

    // Initialize data offset tracker
    current_data_offset_ = 0;

    // Write initial header
    ContainerResult result = writeHeader();
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    updateHeaderMetadata();

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::readAndValidateHeader() {
    // Read header directly
    ContainerResult result = readFile(&header_, sizeof(CompactChunkHeader), 0);
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    // Validate header
    if (!validateCompactChunkHeader(header_)) {
        setError("Invalid COMPACT chunk header");
        return ContainerResult::ERR_INVALID_HEADER;
    }

    // Check sealed flag (using chunkIsSealed helper)
    is_sealed_ = chunkIsSealed(header_.flags);

    // Update current data offset
    if (header_.block_count > 0) {
        // Calculate based on last block
        current_data_offset_ = header_.total_compressed_size;
    } else {
        current_data_offset_ = 0;
    }

    updateHeaderMetadata();

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::writeHeader() {
    // Write header directly
    ContainerResult result = writeFile(&header_, sizeof(CompactChunkHeader), 0);
    if (result != ContainerResult::SUCCESS) {
        return result;
    }

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::readBlockIndices() {
    if (header_.block_count == 0) {
        return ContainerResult::SUCCESS;
    }

    // Allocate space for indices
    block_indices_.resize(header_.block_count);

    // Read all indices
    uint64_t index_offset = header_.index_offset;
    size_t indices_size = header_.block_count * sizeof(CompactBlockIndex);

    ContainerResult result = readFile(block_indices_.data(), indices_size, index_offset);
    if (result != ContainerResult::SUCCESS) {
        block_indices_.clear();
        return result;
    }

    return ContainerResult::SUCCESS;
}

uint32_t CompactContainer::calculateBlockCRC32(const void* data, uint32_t size) const {
    uint32_t crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, static_cast<const Bytef*>(data), size);
    return crc;
}

void CompactContainer::updateHeaderMetadata() {
    metadata_.capacity_bytes = getCapacity();
    metadata_.capacity_extents = metadata_.capacity_bytes / kExtentSizeBytes;
}

void CompactContainer::setError(const std::string& message) {
    last_error_ = message;
}

// ========================================================================
// File I/O Helper Methods
// ========================================================================

ContainerResult CompactContainer::openFile() {

    if (fd_ >= 0) {
        setError("File already open");
        return ContainerResult::ERR_ALREADY_OPEN;
    }

    int flags;
    if (read_only_) {
        flags = O_RDONLY;
    } else {
        flags = O_RDWR | O_CREAT;
    }


#ifdef _WIN32
    flags |= O_BINARY;  // Windows binary mode
    int mode = _S_IREAD | _S_IWRITE;
    fd_ = ::_open(path_.c_str(), flags, mode);
#else
    int mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    fd_ = ::open(path_.c_str(), flags, mode);
#endif


    if (fd_ < 0) {
        setError("Failed to open file: " + path_);
        return ContainerResult::ERR_OPENFD_FAILED;
    }

    return ContainerResult::SUCCESS;
}

void CompactContainer::closeFile() {
    if (fd_ >= 0) {
#ifdef _WIN32
        ::_close(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }
}

ContainerResult CompactContainer::readFile(void* buffer, size_t size, uint64_t offset) {
    if (fd_ < 0) {
        setError("File not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    // Seek to offset
#ifdef _WIN32
    if (::_lseeki64(fd_, offset, SEEK_SET) < 0) {
#else
    if (::lseek(fd_, offset, SEEK_SET) < 0) {
#endif
        setError("Seek failed");
        return ContainerResult::ERR_READ_FAILED;
    }

    // Read data
    ssize_t bytes_read = 0;
    size_t total_read = 0;
    while (total_read < size) {
#ifdef _WIN32
        bytes_read = ::_read(fd_, static_cast<char*>(buffer) + total_read, size - total_read);
#else
        bytes_read = ::read(fd_, static_cast<char*>(buffer) + total_read, size - total_read);
#endif
        if (bytes_read < 0) {
            setError("Read failed");
            return ContainerResult::ERR_READ_FAILED;
        } else if (bytes_read == 0) {
            // EOF reached before reading all data
            setError("Unexpected EOF");
            return ContainerResult::ERR_READ_FAILED;
        }
        total_read += bytes_read;
    }

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::writeFile(const void* buffer, size_t size, uint64_t offset) {
    if (fd_ < 0) {
        setError("File not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        setError("File is read-only");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    // Seek to offset
#ifdef _WIN32
    if (::_lseeki64(fd_, offset, SEEK_SET) < 0) {
#else
    if (::lseek(fd_, offset, SEEK_SET) < 0) {
#endif
        setError("Seek failed");
        return ContainerResult::ERR_WRITE_FAILED;
    }

    // Write data
    ssize_t bytes_written = 0;
    size_t total_written = 0;
    while (total_written < size) {
#ifdef _WIN32
        bytes_written = ::_write(fd_, static_cast<const char*>(buffer) + total_written, size - total_written);
#else
        bytes_written = ::write(fd_, static_cast<const char*>(buffer) + total_written, size - total_written);
#endif
        if (bytes_written < 0) {
            setError("Write failed");
            return ContainerResult::ERR_WRITE_FAILED;
        }
        total_written += bytes_written;
    }

    return ContainerResult::SUCCESS;
}

ContainerResult CompactContainer::syncFile() {
    if (fd_ < 0) {
        setError("File not open");
        return ContainerResult::ERR_NOT_OPEN;
    }

    if (read_only_) {
        return ContainerResult::SUCCESS;  // No sync needed for read-only
    }

#ifdef _WIN32
    if (::_commit(fd_) < 0) {
#else
    if (::fsync(fd_) < 0) {
#endif
        setError("Sync failed");
        return ContainerResult::ERR_SYNC_FAILED;
    }

    return ContainerResult::SUCCESS;
}

int64_t CompactContainer::getFileSize() const {
    if (fd_ < 0) {
        return -1;
    }

#ifdef _WIN32
    struct _stat64 st;
    if (::_fstat64(fd_, &st) < 0) {
        return -1;
    }
    return st.st_size;
#else
    struct stat st;
    if (::fstat(fd_, &st) < 0) {
        return -1;
    }
    return st.st_size;
#endif
}

}  // namespace xtdb
