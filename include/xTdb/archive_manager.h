#ifndef XTDB_ARCHIVE_MANAGER_H_
#define XTDB_ARCHIVE_MANAGER_H_

#include "xTdb/archive_types.h"
#include "xTdb/resampling_engine.h"
#include <vector>
#include <map>
#include <string>
#include <memory>

namespace xtdb {

/// Archive selection criteria
struct ArchiveQuery {
    uint32_t tag_id;
    int64_t start_ts_us;
    int64_t end_ts_us;
    bool prefer_raw;  // Prefer RAW archive if available

    ArchiveQuery()
        : tag_id(0), start_ts_us(0), end_ts_us(0), prefer_raw(false) {}
};

/// Archive selection result
struct ArchiveSelection {
    ArchiveLevel level;
    uint64_t container_id;
    std::string container_path;
    uint64_t start_ts_us;
    uint64_t end_ts_us;
    double resolution_us;  // Time resolution in microseconds
    uint8_t priority;      // Archive priority (higher = better)

    ArchiveSelection()
        : level(ArchiveLevel::ARCHIVE_RAW),
          container_id(0),
          start_ts_us(0),
          end_ts_us(0),
          resolution_us(0),
          priority(0) {}
};

/// Archive Manager - Multi-resolution archive orchestration
/// Based on PHD's tiered archive system
class ArchiveManager {
public:
    /// Constructor
    ArchiveManager();

    /// Destructor
    ~ArchiveManager();

    // Disable copy and move
    ArchiveManager(const ArchiveManager&) = delete;
    ArchiveManager& operator=(const ArchiveManager&) = delete;
    ArchiveManager(ArchiveManager&&) = delete;
    ArchiveManager& operator=(ArchiveManager&&) = delete;

    /// Register an archive with metadata
    /// @param metadata Archive metadata
    /// @param container_id Container ID
    /// @param container_path Path to container file
    /// @return true if successful
    bool registerArchive(const ArchiveMetadata& metadata,
                        uint64_t container_id,
                        const std::string& container_path);

    /// Select best archive for query
    /// @param query Query criteria
    /// @param selections Output: list of archives that cover the time range
    /// @return Number of archives selected
    size_t selectArchives(const ArchiveQuery& query,
                         std::vector<ArchiveSelection>& selections);

    /// Get recommended archive level for query
    /// @param start_ts_us Query start time
    /// @param end_ts_us Query end time
    /// @return Recommended archive level
    ArchiveLevel recommendArchiveLevel(int64_t start_ts_us, int64_t end_ts_us) const;

    /// Get archive statistics
    /// @param level Archive level
    /// @return Number of archives at this level
    size_t getArchiveCount(ArchiveLevel level) const;

    /// Get all archives for a specific level
    /// @param level Archive level
    /// @param archives Output: list of archives
    void getArchives(ArchiveLevel level, std::vector<ArchiveMetadata>& archives) const;

    /// Clear all registered archives
    void clear();

private:
    /// Calculate time span score (lower is better)
    /// Prefers archives that closely match query time range
    double calculateTimeSpanScore(int64_t query_start,
                                  int64_t query_end,
                                  int64_t archive_start,
                                  int64_t archive_end) const;

    /// Calculate resolution score (higher is better)
    /// Prefers higher resolution archives
    double calculateResolutionScore(ArchiveLevel level, int64_t time_span_us) const;

    /// Check if archive covers query time range
    bool coversTimeRange(const ArchiveMetadata& archive,
                        int64_t start_ts_us,
                        int64_t end_ts_us) const;

    /// Map of archives by level
    std::map<ArchiveLevel, std::vector<ArchiveMetadata>> archives_;

    /// Map of container IDs to paths
    std::map<uint64_t, std::string> container_paths_;
};

}  // namespace xtdb

#endif  // XTDB_ARCHIVE_MANAGER_H_
