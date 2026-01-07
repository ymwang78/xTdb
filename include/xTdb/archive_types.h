#ifndef XTDB_ARCHIVE_TYPES_H_
#define XTDB_ARCHIVE_TYPES_H_

#include "xTdb/struct_defs.h"
#include <cstdint>
#include <string>

namespace xtdb {

/// Convert archive level to human-readable string
inline const char* archiveLevelToString(ArchiveLevel level) {
    switch (level) {
        case ArchiveLevel::ARCHIVE_RAW: return "RAW";
        case ArchiveLevel::ARCHIVE_RESAMPLED_1M: return "RESAMPLED_1M";
        case ArchiveLevel::ARCHIVE_RESAMPLED_1H: return "RESAMPLED_1H";
        case ArchiveLevel::ARCHIVE_AGGREGATED: return "AGGREGATED";
        default: return "UNKNOWN";
    }
}

/// Get resampling interval in microseconds
inline uint64_t getResamplingIntervalUs(ArchiveLevel level) {
    switch (level) {
        case ArchiveLevel::ARCHIVE_RAW: return 0;  // No resampling
        case ArchiveLevel::ARCHIVE_RESAMPLED_1M: return 60 * 1000000ULL;  // 1 minute
        case ArchiveLevel::ARCHIVE_RESAMPLED_1H: return 3600 * 1000000ULL;  // 1 hour
        case ArchiveLevel::ARCHIVE_AGGREGATED: return 0;  // Not time-based
        default: return 0;
    }
}

/// Archive priority for query routing
/// Higher priority archives are preferred for queries
inline uint8_t getArchivePriority(ArchiveLevel level) {
    switch (level) {
        case ArchiveLevel::ARCHIVE_RAW: return 100;  // Highest priority
        case ArchiveLevel::ARCHIVE_RESAMPLED_1M: return 75;
        case ArchiveLevel::ARCHIVE_RESAMPLED_1H: return 50;
        case ArchiveLevel::ARCHIVE_AGGREGATED: return 10;  // Lowest priority
        default: return 0;
    }
}

/// Recommended retention period in days
inline uint32_t getRecommendedRetentionDays(ArchiveLevel level) {
    switch (level) {
        case ArchiveLevel::ARCHIVE_RAW: return 180;  // 6 months
        case ArchiveLevel::ARCHIVE_RESAMPLED_1M: return 730;  // 2 years
        case ArchiveLevel::ARCHIVE_RESAMPLED_1H: return 3650;  // 10 years
        case ArchiveLevel::ARCHIVE_AGGREGATED: return 0;  // Permanent
        default: return 0;
    }
}

/// Archive metadata
struct ArchiveMetadata {
    ArchiveLevel level;
    uint64_t resampling_interval_us;
    uint32_t retention_days;
    uint64_t start_ts_us;  // Archive time range start
    uint64_t end_ts_us;    // Archive time range end
    uint64_t record_count;
    uint64_t size_bytes;

    ArchiveMetadata()
        : level(ArchiveLevel::ARCHIVE_RAW),
          resampling_interval_us(0),
          retention_days(0),
          start_ts_us(0),
          end_ts_us(0),
          record_count(0),
          size_bytes(0) {}
};

}  // namespace xtdb

#endif  // XTDB_ARCHIVE_TYPES_H_
