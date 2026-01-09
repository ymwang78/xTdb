#include "xTdb/swinging_door_decoder.h"
#include <algorithm>

namespace xtdb {

SwingingDoorDecoder::SwingingDoorDecoder() {
}

void SwingingDoorDecoder::setError(const std::string& message) {
    last_error_ = message;
}

bool SwingingDoorDecoder::findSegment(
    const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
    uint32_t target_offset,
    size_t& left_idx,
    size_t& right_idx) const {

    if (compressed_points.empty()) {
        return false;
    }

    // 特殊情况：只有一个点
    if (compressed_points.size() == 1) {
        left_idx = 0;
        right_idx = 0;
        return true;
    }

    // 二分查找第一个 time_offset >= target_offset 的点
    auto it = std::lower_bound(compressed_points.begin(),
                              compressed_points.end(),
                              target_offset,
                              [](const SwingingDoorEncoder::CompressedPoint& pt, uint32_t val) {
                                  return pt.time_offset < val;
                              });

    if (it == compressed_points.begin()) {
        // 目标在第一个点之前或刚好等于第一个点
        left_idx = 0;
        right_idx = 0;
        return true;
    }

    if (it == compressed_points.end()) {
        // 目标在最后一个点之后
        left_idx = compressed_points.size() - 1;
        right_idx = compressed_points.size() - 1;
        return true;
    }

    // 目标在两点之间
    right_idx = std::distance(compressed_points.begin(), it);
    left_idx = right_idx - 1;
    return true;
}

double SwingingDoorDecoder::linearInterpolate(
    const SwingingDoorEncoder::CompressedPoint& left_point,
    const SwingingDoorEncoder::CompressedPoint& right_point,
    uint32_t target_offset) const {

    // 如果两点相同，直接返回值
    if (left_point.time_offset == right_point.time_offset) {
        return left_point.value;
    }

    // 线性插值: v = v1 + (v2 - v1) * (t - t1) / (t2 - t1)
    double t1 = static_cast<double>(left_point.time_offset);
    double t2 = static_cast<double>(right_point.time_offset);
    double t = static_cast<double>(target_offset);

    double v1 = left_point.value;
    double v2 = right_point.value;

    double value = v1 + (v2 - v1) * (t - t1) / (t2 - t1);
    return value;
}

SwingingDoorDecoder::DecodeResult SwingingDoorDecoder::interpolate(
    int64_t base_ts_us,
    const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
    int64_t query_ts_us,
    DecodedPoint& result) {

    if (compressed_points.empty()) {
        setError("No compressed points available");
        return DecodeResult::ERR_INVALID_DATA;
    }

    // 计算查询时间相对基准时间的偏移（微秒转毫秒）
    int64_t offset_us = query_ts_us - base_ts_us;

    if (offset_us < 0) {
        setError("Query time is before base timestamp");
        return DecodeResult::ERR_TIME_OUT_OF_RANGE;
    }

    uint32_t target_offset = static_cast<uint32_t>(offset_us / 1000);

    // 查找线性段
    size_t left_idx, right_idx;
    if (!findSegment(compressed_points, target_offset, left_idx, right_idx)) {
        setError("Failed to find segment for target time");
        return DecodeResult::ERR_TIME_OUT_OF_RANGE;
    }

    const auto& left_point = compressed_points[left_idx];
    const auto& right_point = compressed_points[right_idx];

    // 线性插值
    result.timestamp_us = query_ts_us;
    result.value = linearInterpolate(left_point, right_point, target_offset);

    // 质量：使用左端点的质量（或可以使用更复杂的策略）
    result.quality = left_point.quality;

    return DecodeResult::SUCCESS;
}

SwingingDoorDecoder::DecodeResult SwingingDoorDecoder::decode(
    int64_t base_ts_us,
    const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
    int64_t start_ts_us,
    int64_t end_ts_us,
    std::vector<DecodedPoint>& decoded_points) {

    decoded_points.clear();

    if (compressed_points.empty()) {
        return DecodeResult::SUCCESS;
    }

    if (start_ts_us > end_ts_us) {
        setError("Invalid time range: start > end");
        return DecodeResult::ERR_INVALID_DATA;
    }

    // 返回在时间范围内的压缩点，以及边界点（用于插值）
    bool found_any = false;
    const SwingingDoorEncoder::CompressedPoint* left_boundary = nullptr;
    const SwingingDoorEncoder::CompressedPoint* right_boundary = nullptr;

    for (size_t i = 0; i < compressed_points.size(); i++) {
        const auto& cp = compressed_points[i];
        int64_t point_ts_us = base_ts_us + static_cast<int64_t>(cp.time_offset) * 1000;

        if (point_ts_us < start_ts_us) {
            // 保存左边界点（范围之前的最后一个点）
            left_boundary = &cp;
        } else if (point_ts_us >= start_ts_us && point_ts_us <= end_ts_us) {
            // 点在范围内
            if (!found_any && left_boundary) {
                // 第一次找到范围内的点，先添加左边界点
                DecodedPoint dp;
                dp.timestamp_us = base_ts_us + static_cast<int64_t>(left_boundary->time_offset) * 1000;
                dp.value = left_boundary->value;
                dp.quality = left_boundary->quality;
                decoded_points.push_back(dp);
            }

            DecodedPoint dp;
            dp.timestamp_us = point_ts_us;
            dp.value = cp.value;
            dp.quality = cp.quality;
            decoded_points.push_back(dp);
            found_any = true;
        } else {
            // point_ts_us > end_ts_us，找到右边界点
            right_boundary = &cp;
            break;  // 已经超出范围，不需要继续
        }
    }

    // 如果没有找到范围内的点，但有左右边界，添加它们用于插值
    if (!found_any && left_boundary && right_boundary) {
        DecodedPoint dp;
        dp.timestamp_us = base_ts_us + static_cast<int64_t>(left_boundary->time_offset) * 1000;
        dp.value = left_boundary->value;
        dp.quality = left_boundary->quality;
        decoded_points.push_back(dp);

        dp.timestamp_us = base_ts_us + static_cast<int64_t>(right_boundary->time_offset) * 1000;
        dp.value = right_boundary->value;
        dp.quality = right_boundary->quality;
        decoded_points.push_back(dp);
    } else if (found_any && right_boundary) {
        // 找到了范围内的点，添加右边界点
        DecodedPoint dp;
        dp.timestamp_us = base_ts_us + static_cast<int64_t>(right_boundary->time_offset) * 1000;
        dp.value = right_boundary->value;
        dp.quality = right_boundary->quality;
        decoded_points.push_back(dp);
    }

    return DecodeResult::SUCCESS;
}

}  // namespace xtdb
