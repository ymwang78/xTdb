#include "xTdb/compressor.h"
#include <zstd.h>
#include <cstring>

namespace xtdb {

// ============================================================================
// ZstdCompressor Implementation
// ============================================================================

class ZstdCompressor : public ICompressor {
public:
    ZstdCompressor() = default;
    ~ZstdCompressor() override = default;

    CompressionType getType() const override {
        return CompressionType::COMP_ZSTD;
    }

    size_t getMaxCompressedSize(size_t input_size) const override {
        return ZSTD_compressBound(input_size);
    }

    CompressionResult compress(const void* input,
                               size_t input_size,
                               void* output,
                               size_t output_size,
                               size_t& compressed_size,
                               int compression_level) override
    {
        if (!input || input_size == 0 || !output || output_size == 0) {
            last_error_ = "Invalid input parameters";
            return CompressionResult::ERR_INVALID_INPUT;
        }

        // Validate compression level (zstd: 1-22, negative for fast mode)
        if (compression_level < ZSTD_minCLevel() || compression_level > ZSTD_maxCLevel()) {
            compression_level = ZSTD_CLEVEL_DEFAULT;
        }

        // Compress using zstd
        size_t result = ZSTD_compress(
            output,
            output_size,
            input,
            input_size,
            compression_level
        );

        // Check for errors
        if (ZSTD_isError(result)) {
            last_error_ = std::string("Zstd compression failed: ") + ZSTD_getErrorName(result);
            return CompressionResult::ERR_COMPRESSION_FAILED;
        }

        compressed_size = result;
        last_error_.clear();
        return CompressionResult::SUCCESS;
    }

    CompressionResult decompress(const void* input,
                                 size_t input_size,
                                 void* output,
                                 size_t output_size,
                                 size_t& decompressed_size) override
    {
        if (!input || input_size == 0 || !output || output_size == 0) {
            last_error_ = "Invalid input parameters";
            return CompressionResult::ERR_INVALID_INPUT;
        }

        // Get decompressed size hint
        unsigned long long content_size = ZSTD_getFrameContentSize(input, input_size);

        if (content_size == ZSTD_CONTENTSIZE_ERROR) {
            last_error_ = "Invalid zstd frame";
            return CompressionResult::ERR_DECOMPRESSION_FAILED;
        }

        if (content_size == ZSTD_CONTENTSIZE_UNKNOWN) {
            // Content size unknown - proceed with provided buffer size
        } else if (content_size > output_size) {
            last_error_ = "Output buffer too small";
            return CompressionResult::ERR_BUFFER_TOO_SMALL;
        }

        // Decompress using zstd
        size_t result = ZSTD_decompress(
            output,
            output_size,
            input,
            input_size
        );

        // Check for errors
        if (ZSTD_isError(result)) {
            last_error_ = std::string("Zstd decompression failed: ") + ZSTD_getErrorName(result);
            return CompressionResult::ERR_DECOMPRESSION_FAILED;
        }

        decompressed_size = result;
        last_error_.clear();
        return CompressionResult::SUCCESS;
    }

    std::string getLastError() const override {
        return last_error_;
    }

private:
    std::string last_error_;
};

// ============================================================================
// Factory function
// ============================================================================

std::unique_ptr<ICompressor> createZstdCompressor() {
    return std::make_unique<ZstdCompressor>();
}

}  // namespace xtdb
