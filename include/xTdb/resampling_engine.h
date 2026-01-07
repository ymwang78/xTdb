#ifndef XTDB_RESAMPLING_ENGINE_H_
#define XTDB_RESAMPLING_ENGINE_H_

#include "xTdb/archive_types.h"
#include "xTdb/mem_buffer.h"
#include <vector>
#include <cstdint>
#include <string>

namespace xtdb {

/// Resampling result
enum class ResampleResult {
    SUCCESS = 0,
    ERROR_INVALID_INTERVAL,
    ERROR_EMPTY_INPUT,
    ERROR_INVALID_DATA
};

/// Aggregation method for resampling
enum class AggregationMethod {
    /// Average value in time window
    AVG = 0,
    /// Minimum value in time window
    MIN = 1,
    /// Maximum value in time window
    MAX = 2,
    /// First value in time window
    FIRST = 3,
    /// Last value in time window
    LAST = 4,
    /// Sum of values in time window
    SUM = 5,
    /// Count of values in time window
    COUNT = 6
};

/// Resampled data point with aggregated values
struct ResampledPoint {
    uint64_t timestamp_us;  // Window start time
    double avg_value;       // Average value
    double min_value;       // Minimum value
    double max_value;       // Maximum value
    double first_value;     // First value in window
    double last_value;      // Last value in window
    uint32_t count;         // Number of points in window
    uint8_t quality;        // Average quality
};

/// Resampling Engine - Downsample high-frequency data to lower resolution
/// Based on PHD's resampling mechanism
class ResamplingEngine {
public:
    /// Constructor
    /// @param interval_us Resampling interval in microseconds
    /// @param method Primary aggregation method (default: AVG)
    ResamplingEngine(uint64_t interval_us,
                     AggregationMethod method = AggregationMethod::AVG);

    /// Resample records to lower resolution
    /// @param base_ts_us Base timestamp for records
    /// @param records Input records (high-frequency)
    /// @param resampled_points Output: resampled points (low-frequency)
    /// @return ResampleResult
    ResampleResult resample(int64_t base_ts_us,
                           const std::vector<MemRecord>& records,
                           std::vector<ResampledPoint>& resampled_points);

    /// Get compression ratio (input count / output count)
    /// @return Compression ratio (e.g., 60 for 60:1 compression)
    double getCompressionRatio() const;

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Aggregate values within a time window
    /// @param window_records Records in current window
    /// @param window_start_us Window start timestamp
    /// @param resampled Output: resampled point
    void aggregateWindow(const std::vector<MemRecord>& window_records,
                        uint64_t window_start_us,
                        ResampledPoint& resampled);

    /// Set error message
    void setError(const std::string& message);

    uint64_t interval_us_;          // Resampling interval (microseconds)
    AggregationMethod method_;      // Primary aggregation method
    uint64_t input_count_;          // Number of input records
    uint64_t output_count_;         // Number of resampled points
    std::string last_error_;        // Last error message
};

}  // namespace xtdb

#endif  // XTDB_RESAMPLING_ENGINE_H_
