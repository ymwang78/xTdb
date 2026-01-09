#ifndef XTDB_SWINGING_DOOR_DECODER_H_
#define XTDB_SWINGING_DOOR_DECODER_H_

#include "xTdb/swinging_door_encoder.h"
#include <vector>
#include <cstdint>
#include <string>

namespace xtdb {

/// Swinging Door 压缩算法解码器
/// 通过线性插值从关键端点恢复原始时间点的值

class SwingingDoorDecoder {
public:
    /// 解码结果
    enum class DecodeResult {
        SUCCESS = 0,
        ERR_INVALID_DATA,
        ERR_TIME_OUT_OF_RANGE
    };

    /// 解码后的数据点
    struct DecodedPoint {
        int64_t timestamp_us;   // 绝对时间戳（微秒）
        double value;           // 插值后的数值
        uint8_t quality;        // 质量字节
    };

    /// 构造函数
    SwingingDoorDecoder();

    /// 从压缩点解码指定时间范围内的数据
    /// @param base_ts_us 基准时间戳（微秒）
    /// @param compressed_points 压缩的关键点
    /// @param start_ts_us 查询起始时间（微秒，包含）
    /// @param end_ts_us 查询结束时间（微秒，包含）
    /// @param decoded_points 输出：解码后的数据点
    /// @return DecodeResult
    DecodeResult decode(int64_t base_ts_us,
                       const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
                       int64_t start_ts_us,
                       int64_t end_ts_us,
                       std::vector<DecodedPoint>& decoded_points);

    /// 在指定时间点进行插值
    /// @param base_ts_us 基准时间戳（微秒）
    /// @param compressed_points 压缩的关键点
    /// @param query_ts_us 查询时间戳（微秒）
    /// @param result 输出：插值结果
    /// @return DecodeResult
    DecodeResult interpolate(int64_t base_ts_us,
                            const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
                            int64_t query_ts_us,
                            DecodedPoint& result);

    /// 获取最后错误信息
    const std::string& getLastError() const { return last_error_; }

private:
    /// 查找包含目标时间的线性段
    /// @param compressed_points 压缩点列表
    /// @param target_offset 目标时间偏移（毫秒）
    /// @param left_idx 输出：左端点索引
    /// @param right_idx 输出：右端点索引
    /// @return true if found
    bool findSegment(const std::vector<SwingingDoorEncoder::CompressedPoint>& compressed_points,
                    uint32_t target_offset,
                    size_t& left_idx,
                    size_t& right_idx) const;

    /// 线性插值
    /// @param left_point 左端点
    /// @param right_point 右端点
    /// @param target_offset 目标时间偏移（毫秒）
    /// @return 插值结果
    double linearInterpolate(const SwingingDoorEncoder::CompressedPoint& left_point,
                            const SwingingDoorEncoder::CompressedPoint& right_point,
                            uint32_t target_offset) const;

    /// 设置错误信息
    void setError(const std::string& message);

    std::string last_error_;  // 错误信息
};

}  // namespace xtdb

#endif  // XTDB_SWINGING_DOOR_DECODER_H_
