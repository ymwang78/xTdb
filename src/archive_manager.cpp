#include "xTdb/archive_manager.h"
#include <algorithm>
#include <cmath>

namespace xtdb {

ArchiveManager::ArchiveManager() {
}

ArchiveManager::~ArchiveManager() {
}

bool ArchiveManager::registerArchive(const ArchiveMetadata& metadata,
                                     uint64_t container_id,
                                     const std::string& container_path) {
    // Create a copy of metadata with container_id
    ArchiveMetadata meta = metadata;
    meta.container_id = container_id;

    // Add to archives map
    archives_[meta.level].push_back(meta);

    // Store container path
    container_paths_[container_id] = container_path;

    return true;
}

bool ArchiveManager::coversTimeRange(const ArchiveMetadata& archive,
                                     int64_t start_ts_us,
                                     int64_t end_ts_us) const {
    // Check if archive time range overlaps with query time range
    int64_t archive_start = static_cast<int64_t>(archive.start_ts_us);
    int64_t archive_end = static_cast<int64_t>(archive.end_ts_us);
    return !(archive_end < start_ts_us || archive_start > end_ts_us);
}

double ArchiveManager::calculateTimeSpanScore(int64_t query_start,
                                              int64_t query_end,
                                              int64_t archive_start,
                                              int64_t archive_end) const {
    // Calculate overlap ratio
    int64_t overlap_start = std::max(query_start, archive_start);
    int64_t overlap_end = std::min(query_end, archive_end);
    int64_t overlap_duration = overlap_end - overlap_start;

    int64_t query_duration = query_end - query_start;

    if (query_duration <= 0) {
        return 0.0;
    }

    // Return overlap ratio (1.0 = perfect coverage, 0.0 = no overlap)
    return static_cast<double>(overlap_duration) / static_cast<double>(query_duration);
}

double ArchiveManager::calculateResolutionScore(ArchiveLevel level,
                                               int64_t time_span_us) const {
    // Get archive priority (RAW=100, 1M=75, 1H=50, AGG=10)
    double base_priority = static_cast<double>(getArchivePriority(level));

    // For short queries, prefer high resolution
    // For long queries, lower resolution is acceptable
    if (time_span_us < 3600 * 1000000LL) {  // < 1 hour
        // Short query: prefer RAW or 1M
        return base_priority;
    } else if (time_span_us < 86400 * 1000000LL) {  // < 1 day
        // Medium query: 1M or 1H acceptable
        if (level == ArchiveLevel::ARCHIVE_RAW) {
            return base_priority * 0.8;  // Slight penalty for RAW
        }
        return base_priority;
    } else {
        // Long query: 1H preferred, RAW has penalty
        if (level == ArchiveLevel::ARCHIVE_RAW) {
            return base_priority * 0.5;  // Significant penalty for RAW
        } else if (level == ArchiveLevel::ARCHIVE_RESAMPLED_1M) {
            return base_priority * 0.9;  // Small penalty for 1M
        } else if (level == ArchiveLevel::ARCHIVE_RESAMPLED_1H) {
            return base_priority * 1.5;  // Bonus for 1H on long queries
        }
        return base_priority;
    }
}

size_t ArchiveManager::selectArchives(const ArchiveQuery& query,
                                     std::vector<ArchiveSelection>& selections) {
    selections.clear();

    // Collect all archives that cover the query time range
    std::vector<std::pair<ArchiveSelection, double>> candidates;

    for (const auto& level_archives : archives_) {
        ArchiveLevel level = level_archives.first;

        for (const auto& archive : level_archives.second) {
            // Check if archive covers query time range
            if (!coversTimeRange(archive, query.start_ts_us, query.end_ts_us)) {
                continue;
            }

            // Create selection
            ArchiveSelection sel;
            sel.level = level;
            sel.container_id = archive.container_id;
            sel.container_path = container_paths_.at(archive.container_id);
            sel.start_ts_us = archive.start_ts_us;
            sel.end_ts_us = archive.end_ts_us;
            sel.resolution_us = getResamplingIntervalUs(level);
            sel.priority = getArchivePriority(level);

            // Calculate scores
            double time_span_score = calculateTimeSpanScore(
                query.start_ts_us, query.end_ts_us,
                archive.start_ts_us, archive.end_ts_us);

            int64_t query_span = query.end_ts_us - query.start_ts_us;
            double resolution_score = calculateResolutionScore(level, query_span);

            // Combined score (weighted)
            // For short queries, time span matters more
            // For long queries, resolution efficiency matters more
            double time_weight = 0.3;
            double resolution_weight = 0.7;

            if (query_span < 3600 * 1000000LL) {  // < 1 hour
                time_weight = 0.6;
                resolution_weight = 0.4;
            }

            double combined_score = time_span_score * time_weight +
                                   (resolution_score / 100.0) * resolution_weight;

            // Prefer RAW if requested
            if (query.prefer_raw && level == ArchiveLevel::ARCHIVE_RAW) {
                combined_score *= 1.5;
            }

            candidates.push_back({sel, combined_score});
        }
    }

    // Sort by score (descending)
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) {
                  return a.second > b.second;
              });

    // Select archives (for now, just pick the best one)
    // In a full implementation, we might need to combine multiple archives
    if (!candidates.empty()) {
        selections.push_back(candidates[0].first);
    }

    return selections.size();
}

ArchiveLevel ArchiveManager::recommendArchiveLevel(int64_t start_ts_us,
                                                   int64_t end_ts_us) const {
    int64_t time_span_us = end_ts_us - start_ts_us;

    // Recommendation based on query time span
    if (time_span_us < 3600 * 1000000LL) {  // < 1 hour
        return ArchiveLevel::ARCHIVE_RAW;
    } else if (time_span_us < 86400 * 1000000LL) {  // < 1 day
        return ArchiveLevel::ARCHIVE_RESAMPLED_1M;
    } else if (time_span_us < 30 * 86400 * 1000000LL) {  // < 30 days
        return ArchiveLevel::ARCHIVE_RESAMPLED_1H;
    } else {
        return ArchiveLevel::ARCHIVE_AGGREGATED;
    }
}

size_t ArchiveManager::getArchiveCount(ArchiveLevel level) const {
    auto it = archives_.find(level);
    if (it != archives_.end()) {
        return it->second.size();
    }
    return 0;
}

void ArchiveManager::getArchives(ArchiveLevel level,
                                std::vector<ArchiveMetadata>& archives) const {
    archives.clear();

    auto it = archives_.find(level);
    if (it != archives_.end()) {
        archives = it->second;
    }
}

void ArchiveManager::clear() {
    archives_.clear();
    container_paths_.clear();
}

}  // namespace xtdb
