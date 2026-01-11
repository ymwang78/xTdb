#include <gtest/gtest.h>
#include "xTdb/compressor.h"
#include <vector>
#include <cstring>
#include <random>

using namespace xtdb;

class CompressorTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize test data
        small_data_.resize(100);
        medium_data_.resize(16384);  // 16KB
        large_data_.resize(1024 * 1024);  // 1MB

        // Fill with patterns
        fillTestData(small_data_.data(), small_data_.size(), 0xAB);
        fillTestData(medium_data_.data(), medium_data_.size(), 0xCD);
        fillRandomData(large_data_.data(), large_data_.size());
    }

    void fillTestData(uint8_t* data, size_t size, uint8_t pattern) {
        for (size_t i = 0; i < size; i++) {
            data[i] = static_cast<uint8_t>((pattern + i) & 0xFF);
        }
    }

    void fillRandomData(uint8_t* data, size_t size) {
        std::mt19937 rng(42);  // Fixed seed for reproducibility
        std::uniform_int_distribution<uint32_t> dist(0, 255);
        for (size_t i = 0; i < size; i++) {
            data[i] = static_cast<uint8_t>(dist(rng));
        }
    }

    std::vector<uint8_t> small_data_;
    std::vector<uint8_t> medium_data_;
    std::vector<uint8_t> large_data_;
};

// ============================================================================
// Factory Tests
// ============================================================================

TEST_F(CompressorTest, FactoryCreateZstd) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);
    EXPECT_EQ(CompressionType::COMP_ZSTD, compressor->getType());
}

TEST_F(CompressorTest, FactoryCreateNone) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_NONE);
    EXPECT_EQ(nullptr, compressor);
}

TEST_F(CompressorTest, FactoryIsSupported) {
    EXPECT_TRUE(CompressorFactory::isSupported(CompressionType::COMP_NONE));
    EXPECT_TRUE(CompressorFactory::isSupported(CompressionType::COMP_ZSTD));
    EXPECT_FALSE(CompressorFactory::isSupported(CompressionType::COMP_LZ4));
    EXPECT_FALSE(CompressorFactory::isSupported(CompressionType::COMP_ZLIB));
}

TEST_F(CompressorTest, FactoryGetTypeName) {
    EXPECT_STREQ("NONE", CompressorFactory::getTypeName(CompressionType::COMP_NONE));
    EXPECT_STREQ("ZSTD", CompressorFactory::getTypeName(CompressionType::COMP_ZSTD));
    EXPECT_STREQ("LZ4", CompressorFactory::getTypeName(CompressionType::COMP_LZ4));
    EXPECT_STREQ("ZLIB", CompressorFactory::getTypeName(CompressionType::COMP_ZLIB));
}

TEST_F(CompressorTest, FactoryGetRecommendedLevel) {
    EXPECT_EQ(3, CompressorFactory::getRecommendedLevel(CompressionType::COMP_ZSTD));
    EXPECT_EQ(0, CompressorFactory::getRecommendedLevel(CompressionType::COMP_LZ4));
    EXPECT_EQ(6, CompressorFactory::getRecommendedLevel(CompressionType::COMP_ZLIB));
}

// ============================================================================
// Zstd Compressor Tests
// ============================================================================

TEST_F(CompressorTest, ZstdBasicCompression) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    // Compress
    size_t max_compressed = compressor->getMaxCompressedSize(small_data_.size());
    std::vector<uint8_t> compressed(max_compressed);
    size_t compressed_size = 0;

    CompressionResult result = compressor->compress(
        small_data_.data(),
        small_data_.size(),
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_GT(compressed_size, 0u);
    EXPECT_LE(compressed_size, max_compressed);
    // Note: Very small data may not compress well due to frame overhead
    // Just verify it compresses successfully

    std::cout << "  Small data: " << small_data_.size() << " bytes -> "
              << compressed_size << " bytes (ratio: "
              << (100.0 * compressed_size / small_data_.size()) << "%)" << std::endl;
}

TEST_F(CompressorTest, ZstdBasicDecompression) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    // Compress
    std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(small_data_.size()));
    size_t compressed_size = 0;
    CompressionResult result = compressor->compress(
        small_data_.data(),
        small_data_.size(),
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );
    ASSERT_EQ(CompressionResult::SUCCESS, result);

    // Decompress
    std::vector<uint8_t> decompressed(small_data_.size());
    size_t decompressed_size = 0;
    result = compressor->decompress(
        compressed.data(),
        compressed_size,
        decompressed.data(),
        decompressed.size(),
        decompressed_size
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_EQ(small_data_.size(), decompressed_size);
    EXPECT_EQ(0, std::memcmp(small_data_.data(), decompressed.data(), small_data_.size()));
}

TEST_F(CompressorTest, ZstdMediumData) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    // Compress
    std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(medium_data_.size()));
    size_t compressed_size = 0;
    CompressionResult result = compressor->compress(
        medium_data_.data(),
        medium_data_.size(),
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );
    ASSERT_EQ(CompressionResult::SUCCESS, result);

    std::cout << "  Medium data: " << medium_data_.size() << " bytes -> "
              << compressed_size << " bytes (ratio: "
              << (100.0 * compressed_size / medium_data_.size()) << "%)" << std::endl;

    // Decompress
    std::vector<uint8_t> decompressed(medium_data_.size());
    size_t decompressed_size = 0;
    result = compressor->decompress(
        compressed.data(),
        compressed_size,
        decompressed.data(),
        decompressed.size(),
        decompressed_size
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_EQ(medium_data_.size(), decompressed_size);
    EXPECT_EQ(0, std::memcmp(medium_data_.data(), decompressed.data(), medium_data_.size()));
}

TEST_F(CompressorTest, ZstdLargeData) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    // Compress
    std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(large_data_.size()));
    size_t compressed_size = 0;
    CompressionResult result = compressor->compress(
        large_data_.data(),
        large_data_.size(),
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );
    ASSERT_EQ(CompressionResult::SUCCESS, result);

    std::cout << "  Large data (random): " << large_data_.size() << " bytes -> "
              << compressed_size << " bytes (ratio: "
              << (100.0 * compressed_size / large_data_.size()) << "%)" << std::endl;

    // Decompress
    std::vector<uint8_t> decompressed(large_data_.size());
    size_t decompressed_size = 0;
    result = compressor->decompress(
        compressed.data(),
        compressed_size,
        decompressed.data(),
        decompressed.size(),
        decompressed_size
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_EQ(large_data_.size(), decompressed_size);
    EXPECT_EQ(0, std::memcmp(large_data_.data(), decompressed.data(), large_data_.size()));
}

TEST_F(CompressorTest, ZstdCompressionLevels) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    std::vector<int> levels = {1, 3, 9, 19};
    std::cout << "\n  Compression levels for " << medium_data_.size() << " bytes:" << std::endl;

    for (int level : levels) {
        std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(medium_data_.size()));
        size_t compressed_size = 0;
        CompressionResult result = compressor->compress(
            medium_data_.data(),
            medium_data_.size(),
            compressed.data(),
            compressed.size(),
            compressed_size,
            level
        );
        ASSERT_EQ(CompressionResult::SUCCESS, result);

        double ratio = 100.0 * compressed_size / medium_data_.size();
        std::cout << "    Level " << level << ": " << compressed_size
                  << " bytes (" << ratio << "%)" << std::endl;
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_F(CompressorTest, CompressInvalidInput) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    std::vector<uint8_t> output(1024);
    size_t compressed_size = 0;

    // Null input
    CompressionResult result = compressor->compress(
        nullptr,
        100,
        output.data(),
        output.size(),
        compressed_size,
        3
    );
    EXPECT_EQ(CompressionResult::ERR_INVALID_INPUT, result);

    // Zero size
    result = compressor->compress(
        small_data_.data(),
        0,
        output.data(),
        output.size(),
        compressed_size,
        3
    );
    EXPECT_EQ(CompressionResult::ERR_INVALID_INPUT, result);
}

TEST_F(CompressorTest, DecompressInvalidInput) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    std::vector<uint8_t> output(1024);
    size_t decompressed_size = 0;

    // Null input
    CompressionResult result = compressor->decompress(
        nullptr,
        100,
        output.data(),
        output.size(),
        decompressed_size
    );
    EXPECT_EQ(CompressionResult::ERR_INVALID_INPUT, result);

    // Invalid compressed data
    std::vector<uint8_t> invalid_data(100, 0xAA);
    result = compressor->decompress(
        invalid_data.data(),
        invalid_data.size(),
        output.data(),
        output.size(),
        decompressed_size
    );
    EXPECT_EQ(CompressionResult::ERR_DECOMPRESSION_FAILED, result);
}

// ============================================================================
// Helper Tests
// ============================================================================

TEST_F(CompressorTest, HelperCompressWithAlloc) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    std::vector<uint8_t> compressed;
    CompressionResult result = CompressionHelper::compressWithAlloc(
        compressor.get(),
        medium_data_.data(),
        medium_data_.size(),
        compressed,
        3
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_GT(compressed.size(), 0u);

    std::cout << "  Helper compress: " << medium_data_.size() << " bytes -> "
              << compressed.size() << " bytes" << std::endl;
}

TEST_F(CompressorTest, HelperDecompressWithAlloc) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    // Compress first
    std::vector<uint8_t> compressed;
    CompressionResult result = CompressionHelper::compressWithAlloc(
        compressor.get(),
        medium_data_.data(),
        medium_data_.size(),
        compressed,
        3
    );
    ASSERT_EQ(CompressionResult::SUCCESS, result);

    // Decompress
    std::vector<uint8_t> decompressed;
    result = CompressionHelper::decompressWithAlloc(
        compressor.get(),
        compressed.data(),
        compressed.size(),
        medium_data_.size(),
        decompressed
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_EQ(medium_data_.size(), decompressed.size());
    EXPECT_EQ(0, std::memcmp(medium_data_.data(), decompressed.data(), medium_data_.size()));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_F(CompressorTest, CompressEmptyData) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    std::vector<uint8_t> empty;
    std::vector<uint8_t> compressed(1024);
    size_t compressed_size = 0;

    CompressionResult result = compressor->compress(
        empty.data(),
        empty.size(),
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );

    EXPECT_EQ(CompressionResult::ERR_INVALID_INPUT, result);
}

TEST_F(CompressorTest, CompressSingleByte) {
    auto compressor = CompressorFactory::create(CompressionType::COMP_ZSTD);
    ASSERT_NE(nullptr, compressor);

    uint8_t single_byte = 0x42;
    std::vector<uint8_t> compressed(compressor->getMaxCompressedSize(1));
    size_t compressed_size = 0;

    CompressionResult result = compressor->compress(
        &single_byte,
        1,
        compressed.data(),
        compressed.size(),
        compressed_size,
        3
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);

    // Decompress
    uint8_t decompressed_byte = 0;
    size_t decompressed_size = 0;
    result = compressor->decompress(
        compressed.data(),
        compressed_size,
        &decompressed_byte,
        1,
        decompressed_size
    );

    ASSERT_EQ(CompressionResult::SUCCESS, result);
    EXPECT_EQ(1u, decompressed_size);
    EXPECT_EQ(single_byte, decompressed_byte);
}
