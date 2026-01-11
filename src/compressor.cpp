#include "xTdb/compressor.h"
#include <cstring>

namespace xtdb {

// Forward declarations for compressor implementations
std::unique_ptr<ICompressor> createZstdCompressor();

// ============================================================================
// CompressorFactory Implementation
// ============================================================================

std::unique_ptr<ICompressor> CompressorFactory::create(CompressionType type) {
    switch (type) {
        case CompressionType::COMP_ZSTD:
            return createZstdCompressor();

        case CompressionType::COMP_NONE:
            // No compression - return nullptr
            return nullptr;

        case CompressionType::COMP_LZ4:
        case CompressionType::COMP_ZLIB:
            // Not implemented yet
            return nullptr;

        default:
            return nullptr;
    }
}

bool CompressorFactory::isSupported(CompressionType type) {
    switch (type) {
        case CompressionType::COMP_NONE:
        case CompressionType::COMP_ZSTD:
            return true;

        case CompressionType::COMP_LZ4:
        case CompressionType::COMP_ZLIB:
            return false;  // Not implemented yet

        default:
            return false;
    }
}

const char* CompressorFactory::getTypeName(CompressionType type) {
    switch (type) {
        case CompressionType::COMP_NONE:  return "NONE";
        case CompressionType::COMP_ZSTD:  return "ZSTD";
        case CompressionType::COMP_LZ4:   return "LZ4";
        case CompressionType::COMP_ZLIB:  return "ZLIB";
        default: return "UNKNOWN";
    }
}

int CompressorFactory::getRecommendedLevel(CompressionType type) {
    switch (type) {
        case CompressionType::COMP_ZSTD:
            return 3;  // Zstd default level (balance speed/ratio)

        case CompressionType::COMP_LZ4:
            return 0;  // LZ4 has no levels (always fast)

        case CompressionType::COMP_ZLIB:
            return 6;  // Zlib default level

        default:
            return 0;
    }
}

// ============================================================================
// CompressionHelper Implementation
// ============================================================================

CompressionResult CompressionHelper::compressWithAlloc(
    ICompressor* compressor,
    const void* input,
    size_t input_size,
    std::vector<uint8_t>& compressed_data,
    int compression_level)
{
    if (!compressor || !input || input_size == 0) {
        return CompressionResult::ERR_INVALID_INPUT;
    }

    // Allocate buffer for compressed data
    size_t max_compressed_size = compressor->getMaxCompressedSize(input_size);
    compressed_data.resize(max_compressed_size);

    // Compress
    size_t actual_compressed_size = 0;
    CompressionResult result = compressor->compress(
        input,
        input_size,
        compressed_data.data(),
        compressed_data.size(),
        actual_compressed_size,
        compression_level
    );

    if (result == CompressionResult::SUCCESS) {
        // Resize to actual compressed size
        compressed_data.resize(actual_compressed_size);
    } else {
        compressed_data.clear();
    }

    return result;
}

CompressionResult CompressionHelper::decompressWithAlloc(
    ICompressor* compressor,
    const void* input,
    size_t input_size,
    size_t expected_size,
    std::vector<uint8_t>& decompressed_data)
{
    if (!compressor || !input || input_size == 0) {
        return CompressionResult::ERR_INVALID_INPUT;
    }

    // Allocate buffer for decompressed data
    decompressed_data.resize(expected_size);

    // Decompress
    size_t actual_decompressed_size = 0;
    CompressionResult result = compressor->decompress(
        input,
        input_size,
        decompressed_data.data(),
        decompressed_data.size(),
        actual_decompressed_size
    );

    if (result == CompressionResult::SUCCESS) {
        // Verify decompressed size matches expected
        if (actual_decompressed_size != expected_size) {
            decompressed_data.clear();
            return CompressionResult::ERR_DECOMPRESSION_FAILED;
        }
    } else {
        decompressed_data.clear();
    }

    return result;
}

}  // namespace xtdb
