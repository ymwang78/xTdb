#ifndef XTDB_CONSTANTS_H_
#define XTDB_CONSTANTS_H_

#include <cstdint>

namespace xtdb {

// ============================================================================
// Physical Layout Constants (V1.6)
// ============================================================================

/// Extent: Minimum disk allocation/alignment unit (16KB)
constexpr uint32_t kExtentSizeBytes = 16384u;  // 16KB

/// Helper: Convert extent count to bytes
constexpr uint64_t extentToBytes(uint32_t extent_count) {
    return static_cast<uint64_t>(extent_count) * kExtentSizeBytes;
}

/// Helper: Convert bytes to extent count (must be aligned)
constexpr uint32_t bytesToExtent(uint64_t bytes) {
    return static_cast<uint32_t>(bytes / kExtentSizeBytes);
}

/// Helper: Check if byte offset/size is extent-aligned
constexpr bool isExtentAligned(uint64_t bytes) {
    return (bytes % kExtentSizeBytes) == 0;
}

// ============================================================================
// RAW Block Classes (Fixed-size blocks)
// ============================================================================

enum class RawBlockClass : uint8_t {
    RAW_16K  = 1,  // 16KB blocks (1 extent)
    RAW_64K  = 2,  // 64KB blocks (4 extents)
    RAW_256K = 3   // 256KB blocks (16 extents)
};

/// Get block size in bytes for a given block class
constexpr uint64_t getBlockSizeBytes(RawBlockClass block_class) {
    switch (block_class) {
        case RawBlockClass::RAW_16K:  return 16384u;      // 16KB
        case RawBlockClass::RAW_64K:  return 65536u;      // 64KB
        case RawBlockClass::RAW_256K: return 262144u;     // 256KB
        default: return 0;
    }
}

/// Get block size in extents for a given block class
constexpr uint32_t getBlockSizeExtents(RawBlockClass block_class) {
    switch (block_class) {
        case RawBlockClass::RAW_16K:  return 1;   // 1 extent
        case RawBlockClass::RAW_64K:  return 4;   // 4 extents
        case RawBlockClass::RAW_256K: return 16;  // 16 extents
        default: return 0;
    }
}

// ============================================================================
// Default Chunk Sizes (Recommended: 256MB for all block classes)
// ============================================================================

/// Recommended chunk size (in extents): 256MB / 16KB = 16384 extents
constexpr uint32_t kDefaultChunkSizeExtents = 16384u;  // 256MB

/// Recommended chunk size in bytes
constexpr uint64_t kDefaultChunkSizeBytes =
    extentToBytes(kDefaultChunkSizeExtents);  // 256MB

}  // namespace xtdb

#endif  // XTDB_CONSTANTS_H_
