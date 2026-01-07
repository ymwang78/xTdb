#ifndef XTDB_QUANTIZED_16_DECODER_H_
#define XTDB_QUANTIZED_16_DECODER_H_

#include "xTdb/quantized_16_encoder.h"
#include "xTdb/mem_buffer.h"
#include <vector>
#include <cstdint>

namespace xtdb {

/// 16-bit Quantization Decoder
/// Converts 16-bit quantized values back to floating-point representation
class Quantized16Decoder {
public:
    /// Decode result
    enum class DecodeResult {
        SUCCESS = 0,
        ERROR_INVALID_RANGE,
        ERROR_INVALID_DATA
    };

    /// Constructor
    /// @param low_extreme Lower bound of value range (must match encoder)
    /// @param high_extreme Upper bound of value range (must match encoder)
    Quantized16Decoder(double low_extreme, double high_extreme);

    /// Decode quantized points back to records
    /// @param base_ts_us Base timestamp (microseconds)
    /// @param quantized_points Input: quantized points
    /// @param records Output: decoded records
    /// @return DecodeResult
    DecodeResult decode(int64_t base_ts_us,
                       const std::vector<Quantized16Encoder::QuantizedPoint>& quantized_points,
                       std::vector<MemRecord>& records);

    /// Get maximum precision loss
    /// @return Maximum possible error (value range / 65535)
    double getMaxPrecisionLoss() const;

    /// Get last error message
    const std::string& getLastError() const { return last_error_; }

private:
    /// Dequantize a 16-bit integer to floating-point value
    /// @param quantized Input: 16-bit quantized value
    /// @param value Output: decoded value
    /// @return true if successful
    bool dequantizeValue(uint16_t quantized, double& value) const;

    /// Set error message
    void setError(const std::string& message);

    double low_extreme_;   // Lower bound
    double high_extreme_;  // Upper bound
    double range_;         // high - low
    std::string last_error_;  // Error message
};

}  // namespace xtdb

#endif  // XTDB_QUANTIZED_16_DECODER_H_
