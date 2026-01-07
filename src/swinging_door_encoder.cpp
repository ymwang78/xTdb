#include "xTdb/swinging_door_encoder.h"
#include <cmath>
#include <limits>

namespace xtdb {

SwingingDoorEncoder::SwingingDoorEncoder(double tolerance, double compression_factor)
    : tolerance_(tolerance),
      compression_factor_(compression_factor),
      original_count_(0),
      compressed_count_(0) {
}

void SwingingDoorEncoder::setError(const std::string& message) {
    last_error_ = message;
}

SwingingDoorEncoder::EncodeResult SwingingDoorEncoder::encode(
    int64_t base_ts_us,
    const std::vector<MemRecord>& records,
    std::vector<CompressedPoint>& compressed_points) {

    (void)base_ts_us;  // Not used in encoding, only for context
    compressed_points.clear();

    if (records.empty()) {
        return EncodeResult::SUCCESS;
    }

    // 验证参数
    if (tolerance_ < 0.0) {
        setError("Tolerance must be non-negative");
        return EncodeResult::ERROR_INVALID_TOLERANCE;
    }

    if (compression_factor_ <= 0.0) {
        setError("Compression factor must be positive");
        return EncodeResult::ERROR_INVALID_TOLERANCE;
    }

    original_count_ = records.size();
    double effective_tolerance = getEffectiveTolerance();

    // 特殊情况：只有1个点
    if (records.size() == 1) {
        CompressedPoint pt;
        pt.time_offset = records[0].time_offset;
        pt.value = records[0].value.f64_value;
        pt.quality = records[0].quality;
        compressed_points.push_back(pt);
        compressed_count_ = 1;
        return EncodeResult::SUCCESS;
    }

    // Swinging Door 算法
    // 第一个点必须存储（作为锚点）
    CompressedPoint anchor;
    anchor.time_offset = records[0].time_offset;
    anchor.value = records[0].value.f64_value;
    anchor.quality = records[0].quality;
    compressed_points.push_back(anchor);

    // 初始化斜率包络
    double min_slope = -std::numeric_limits<double>::infinity();
    double max_slope = std::numeric_limits<double>::infinity();

    CompressedPoint last_candidate;
    last_candidate.time_offset = records[1].time_offset;
    last_candidate.value = records[1].value.f64_value;
    last_candidate.quality = records[1].quality;

    for (size_t i = 1; i < records.size(); ++i) {
        CompressedPoint current;
        current.time_offset = records[i].time_offset;
        current.value = records[i].value.f64_value;
        current.quality = records[i].quality;

        // 计算时间差（转换为秒以避免数值问题）
        double dt = static_cast<double>(current.time_offset - anchor.time_offset) / 1000.0;

        if (dt < 1e-9) {
            // 时间差太小，跳过
            continue;
        }

        // 计算当前点到锚点的斜率上下界
        // 上界：锚点最低可能值到当前点最高可能值
        double slope_upper = ((current.value + effective_tolerance) -
                             (anchor.value - effective_tolerance)) / dt;

        // 下界：锚点最高可能值到当前点最低可能值
        double slope_lower = ((current.value - effective_tolerance) -
                             (anchor.value + effective_tolerance)) / dt;

        // 更新斜率包络（交集）
        if (slope_lower > min_slope) {
            min_slope = slope_lower;
        }
        if (slope_upper < max_slope) {
            max_slope = slope_upper;
        }

        // 检查包络是否有效（是否有交集）
        if (min_slope <= max_slope) {
            // 包络仍然有效，当前点在包络内，更新候选点
            last_candidate = current;
        } else {
            // 包络失效，当前点超出包络
            // 存储上一个候选点作为线性段的终点
            compressed_points.push_back(last_candidate);

            // 将上一个候选点作为新的锚点
            anchor = last_candidate;

            // 重置斜率包络
            min_slope = -std::numeric_limits<double>::infinity();
            max_slope = std::numeric_limits<double>::infinity();

            // 当前点作为新的候选点
            last_candidate = current;
        }
    }

    // 存储最后一个候选点（如果与最后存储点不同）
    if (compressed_points.empty() ||
        compressed_points.back().time_offset != last_candidate.time_offset) {
        compressed_points.push_back(last_candidate);
    }

    compressed_count_ = compressed_points.size();
    return EncodeResult::SUCCESS;
}

double SwingingDoorEncoder::getCompressionRatio() const {
    if (compressed_count_ == 0) {
        return 1.0;
    }
    return static_cast<double>(original_count_) / static_cast<double>(compressed_count_);
}

}  // namespace xtdb
