#ifndef XTDB_COMPRESSOR_H_
#define XTDB_COMPRESSOR_H_

#include "xTdb/struct_defs.h"
#include <cstdint>
#include <cstddef>
#include <string>
#include <memory>
#include <vector>

namespace xtdb {

// ============================================================================
// Compression Result
// ============================================================================

enum class CompressionResult {
    SUCCESS = 0,
    ERR_INVALID_INPUT = 1,
    ERR_BUFFER_TOO_SMALL = 2,
    ERR_COMPRESSION_FAILED = 3,
    ERR_DECOMPRESSION_FAILED = 4,
    ERR_UNSUPPORTED_TYPE = 5,
    ERR_OUT_OF_MEMORY = 6
};

// ============================================================================
// Compressor Interface
// ============================================================================

/// Abstract interface for compression algorithms
class ICompressor {
public:
    virtual ~ICompressor() = default;

    /// Get compression type
    virtual CompressionType getType() const = 0;

    /// Get maximum compressed size for input data
    /// @param input_size Size of uncompressed data
    /// @return Maximum possible compressed size
    virtual size_t getMaxCompressedSize(size_t input_size) const = 0;

    /// Compress data
    /// @param input Input data buffer
    /// @param input_size Size of input data
    /// @param output Output buffer (must be at least getMaxCompressedSize bytes)
    /// @param output_size Size of output buffer
    /// @param compressed_size Output: actual compressed size
    /// @param compression_level Compression level (algorithm-specific)
    /// @return CompressionResult
    virtual CompressionResult compress(const void* input,
                                       size_t input_size,
                                       void* output,
                                       size_t output_size,
                                       size_t& compressed_size,
                                       int compression_level = 3) = 0;

    /// Decompress data
    /// @param input Compressed data buffer
    /// @param input_size Size of compressed data
    /// @param output Output buffer for decompressed data
    /// @param output_size Size of output buffer
    /// @param decompressed_size Output: actual decompressed size
    /// @return CompressionResult
    virtual CompressionResult decompress(const void* input,
                                         size_t input_size,
                                         void* output,
                                         size_t output_size,
                                         size_t& decompressed_size) = 0;

    /// Get last error message
    virtual std::string getLastError() const = 0;
};

// ============================================================================
// Compressor Factory
// ============================================================================

/// Factory for creating compressor instances
class CompressorFactory {
public:
    /// Create compressor instance
    /// @param type Compression type
    /// @return Compressor instance, or nullptr if unsupported
    static std::unique_ptr<ICompressor> create(CompressionType type);

    /// Check if compression type is supported
    /// @param type Compression type
    /// @return true if supported
    static bool isSupported(CompressionType type);

    /// Get human-readable name for compression type
    /// @param type Compression type
    /// @return Name string
    static const char* getTypeName(CompressionType type);

    /// Get recommended compression level
    /// @param type Compression type
    /// @return Recommended level (algorithm-specific)
    static int getRecommendedLevel(CompressionType type);
};

// ============================================================================
// Compression Helper - RAII buffer management
// ============================================================================

/// Helper class for compression with automatic buffer management
class CompressionHelper {
public:
    /// Compress data with automatic buffer allocation
    /// @param compressor Compressor instance
    /// @param input Input data
    /// @param input_size Size of input
    /// @param compressed_data Output: compressed data (dynamically allocated)
    /// @param compressed_size Output: compressed size
    /// @param compression_level Compression level
    /// @return CompressionResult
    static CompressionResult compressWithAlloc(ICompressor* compressor,
                                               const void* input,
                                               size_t input_size,
                                               std::vector<uint8_t>& compressed_data,
                                               int compression_level = 3);

    /// Decompress data with known output size
    /// @param compressor Compressor instance
    /// @param input Compressed data
    /// @param input_size Size of compressed data
    /// @param expected_size Expected decompressed size
    /// @param decompressed_data Output: decompressed data
    /// @return CompressionResult
    static CompressionResult decompressWithAlloc(ICompressor* compressor,
                                                 const void* input,
                                                 size_t input_size,
                                                 size_t expected_size,
                                                 std::vector<uint8_t>& decompressed_data);
};

}  // namespace xtdb

#endif  // XTDB_COMPRESSOR_H_
