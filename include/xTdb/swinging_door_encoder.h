#ifndef XTDB_SWINGING_DOOR_ENCODER_H_
#define XTDB_SWINGING_DOOR_ENCODER_H_

#include "xTdb/constants.h"
#include "xTdb/struct_defs.h"
#include "xTdb/mem_buffer.h"
#include <vector>
#include <cstdint>
#include <string>

namespace xtdb {

/// Swinging Door 压缩算法编码器
/// 基于工程容差的斜率包络判定，存储线性段关键端点
class SwingingDoorEncoder {
public:
    /// 编码结果
    enum class EncodeResult {
        SUCCESS = 0,
        ERROR_INVALID_TOLERANCE,
        ERROR_INVALID_DATA,
        ERROR_BUFFER_TOO_SMALL
    };

    /// 压缩点（存储的关键端点）
    struct CompressedPoint {
        uint32_t time_offset;   // 相对 base_ts 的时间偏移（毫秒）
        double value;           // 数值
        uint8_t quality;        // 质量字节
    };

    /// 构造函数
    /// @param tolerance 工程容差（绝对值）
    /// @param compression_factor 压缩因子（通常 0.1 ~ 2.0）
    SwingingDoorEncoder(double tolerance, double compression_factor);

    /// 编码一组数据点
    /// @param base_ts_us 基准时间戳（微秒）
    /// @param records 原始记录
    /// @param compressed_points 输出：压缩后的关键点
    /// @return EncodeResult
    EncodeResult encode(int64_t base_ts_us,
                       const std::vector<MemRecord>& records,
                       std::vector<CompressedPoint>& compressed_points);

    /// 获取压缩比
    /// @return 压缩比（原始点数 / 压缩点数）
    double getCompressionRatio() const;

    /// 获取最后错误信息
    const std::string& getLastError() const { return last_error_; }

private:
    /// 计算有效压缩容差
    double getEffectiveTolerance() const {
        return tolerance_ * compression_factor_;
    }

    /// 检查新点是否在斜率包络内
    /// @param anchor_point 锚点（上次存储点）
    /// @param last_candidate 最后候选点
    /// @param new_point 新到达点
    /// @return true if new_point 在包络内
    bool isWithinEnvelope(const CompressedPoint& anchor_point,
                         const CompressedPoint& last_candidate,
                         const CompressedPoint& new_point) const;

    /// 设置错误信息
    void setError(const std::string& message);

    double tolerance_;           // 工程容差
    double compression_factor_;  // 压缩因子
    uint64_t original_count_;    // 原始点数
    uint64_t compressed_count_;  // 压缩后点数
    std::string last_error_;     // 错误信息
};

}  // namespace xtdb

#endif  // XTDB_SWINGING_DOOR_ENCODER_H_
