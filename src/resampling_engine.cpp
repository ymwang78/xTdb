#include "xTdb/resampling_engine.h"
#include <cmath>
#include <algorithm>
#include <limits>

namespace xtdb {

ResamplingEngine::ResamplingEngine(uint64_t interval_us, AggregationMethod method)
    : interval_us_(interval_us),
      method_(method),
      input_count_(0),
      output_count_(0) {
}

void ResamplingEngine::setError(const std::string& message) {
    last_error_ = message;
}

void ResamplingEngine::aggregateWindow(const std::vector<MemRecord>& window_records,
                                      uint64_t window_start_us,
                                      ResampledPoint& resampled) {
    if (window_records.empty()) {
        return;
    }

    resampled.timestamp_us = window_start_us;
    resampled.count = static_cast<uint32_t>(window_records.size());

    // Initialize aggregation values
    double sum = 0.0;
    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();
    uint32_t quality_sum = 0;

    // First and last values
    resampled.first_value = window_records.front().value.f64_value;
    resampled.last_value = window_records.back().value.f64_value;

    // Compute aggregations
    for (const auto& rec : window_records) {
        double val = rec.value.f64_value;
        sum += val;
        min_val = std::min(min_val, val);
        max_val = std::max(max_val, val);
        quality_sum += rec.quality;
    }

    // Set aggregated values
    resampled.avg_value = sum / window_records.size();
    resampled.min_value = min_val;
    resampled.max_value = max_val;

    // Average quality
    resampled.quality = static_cast<uint8_t>(quality_sum / window_records.size());
}

ResampleResult ResamplingEngine::resample(int64_t base_ts_us,
                                         const std::vector<MemRecord>& records,
                                         std::vector<ResampledPoint>& resampled_points) {
    resampled_points.clear();

    // Validate input
    if (records.empty()) {
        setError("No input records to resample");
        return ResampleResult::ERR_EMPTY_INPUT;
    }

    if (interval_us_ == 0) {
        setError("Invalid resampling interval (must be > 0)");
        return ResampleResult::ERR_INVALID_INTERVAL;
    }

    input_count_ = records.size();

    // Process each time window
    std::vector<MemRecord> window_records;
    size_t record_idx = 0;

    while (record_idx < records.size()) {
        // Get current record's absolute timestamp
        int64_t current_record_ts_us = base_ts_us + static_cast<int64_t>(records[record_idx].time_offset) * 1000;

        // Align to window boundary
        uint64_t window_start_us = (static_cast<uint64_t>(current_record_ts_us) / interval_us_) * interval_us_;
        uint64_t window_end_us = window_start_us + interval_us_;

        // Collect all records in current window
        window_records.clear();
        while (record_idx < records.size()) {
            int64_t record_ts_us = base_ts_us + static_cast<int64_t>(records[record_idx].time_offset) * 1000;
            uint64_t record_ts_u64 = static_cast<uint64_t>(record_ts_us);

            // Check if record belongs to current window
            if (record_ts_u64 >= window_start_us && record_ts_u64 < window_end_us) {
                window_records.push_back(records[record_idx]);
                record_idx++;
            } else if (record_ts_u64 >= window_end_us) {
                // Record belongs to next window, exit inner loop
                break;
            } else {
                // Record before window (shouldn't happen with sorted data)
                record_idx++;
            }
        }

        // Aggregate window if not empty
        if (!window_records.empty()) {
            ResampledPoint rp;
            aggregateWindow(window_records, window_start_us, rp);
            resampled_points.push_back(rp);
        }
    }

    output_count_ = resampled_points.size();

    return ResampleResult::SUCCESS;
}

double ResamplingEngine::getCompressionRatio() const {
    if (output_count_ == 0) {
        return 1.0;
    }
    return static_cast<double>(input_count_) / static_cast<double>(output_count_);
}

}  // namespace xtdb
