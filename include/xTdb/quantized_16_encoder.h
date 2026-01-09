#ifndef XTDB_QUANTIZED_16_ENCODER_H_
#define XTDB_QUANTIZED_16_ENCODER_H_

#include "xTdb/constants.h"
#include "xTdb/mem_buffer.h"
#include <vector>
#include <cstdint>
#include <string>

namespace xtdb {

/// 16-bit Quantization Encoder
/// Maps floating-point values to 16-bit integers based on configured range
/// Achieves 50-75% storage reduction with <0.0015% precision loss
class Quantized16Encoder {
public:
    /// Encode result
    enum class EncodeResult {
        SUCCESS = 0,
        ERR_INVALID_RANGE,
        ERR_VALUE_OUT_OF_RANGE,
        ERR_INVALID_DATA
    };

    /// Quantized point (compressed format)
    struct QuantizedPoint {
        uint32_t time_offset;      // Relative to base_ts (milliseconds)
        uint16_t quantized_value;  // 16-bit quantized value
        uint8_t quality;           // Quality byte
    };

    /// Constructor
    /// @param low_extreme Lower bound of value range
    /// @param high_extreme Upper bound of value range
    Quantized16Encoder(double low_extreme, double high_extreme);

    /// Encode records to quantized format
    /// @param base_ts_us Base timestamp (microseconds)
    /// @param records Input records (must be VT_F64 or VT_F32)
    /// @param quantized_points Output: quantized points
    /// @return EncodeResult
    EncodeResult encode(int64_t base_ts_us,
                       const std::vector<MemRecord>& records,
                       std::vector<QuantizedPoint>& quantized_points);

    /// Get compression ratio (bytes saved)
    /// @return Storage reduction ratio (1.0 = no reduction, 4.0 = 75% reduction)
    double getCompressionRatio() const;

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Quantize a floating-point value to 16-bit integer
    /// @param value Input value
    /// @param quantized Output: quantized value
    /// @return true if successful, false if out of range
    bool quantizeValue(double value, uint16_t& quantized) const;

    /// Set error message
    void setError(const std::string& message);

    double low_extreme_;        // Lower bound
    double high_extreme_;       // Upper bound
    double range_;              // high - low
    uint64_t original_bytes_;   // Original size in bytes
    uint64_t compressed_bytes_; // Compressed size in bytes
    std::string last_error_;    // Error message
};

}  // namespace xtdb

#endif  // XTDB_QUANTIZED_16_ENCODER_H_
