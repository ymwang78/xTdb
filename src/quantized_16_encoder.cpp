#include "xTdb/quantized_16_encoder.h"
#include <cmath>
#include <limits>

namespace xtdb {

Quantized16Encoder::Quantized16Encoder(double low_extreme, double high_extreme)
    : low_extreme_(low_extreme),
      high_extreme_(high_extreme),
      range_(high_extreme - low_extreme),
      original_bytes_(0),
      compressed_bytes_(0) {
}

void Quantized16Encoder::setError(const std::string& message) {
    last_error_ = message;
}

bool Quantized16Encoder::quantizeValue(double value, uint16_t& quantized) const {
    // Check if value is within range
    if (value < low_extreme_ || value > high_extreme_) {
        return false;
    }

    // Linear mapping: [low, high] -> [0, 65535]
    // quantized = (value - low) / (high - low) * 65535
    double normalized = (value - low_extreme_) / range_;
    double scaled = normalized * 65535.0;

    // Round to nearest integer
    quantized = static_cast<uint16_t>(std::round(scaled));

    return true;
}

Quantized16Encoder::EncodeResult Quantized16Encoder::encode(
    int64_t base_ts_us,
    const std::vector<MemRecord>& records,
    std::vector<QuantizedPoint>& quantized_points) {

    (void)base_ts_us;  // Not used directly in encoding
    quantized_points.clear();

    if (records.empty()) {
        return EncodeResult::SUCCESS;
    }

    // Validate range
    if (range_ <= 0.0 || !std::isfinite(range_)) {
        setError("Invalid range: high must be > low");
        return EncodeResult::ERROR_INVALID_RANGE;
    }

    // Calculate original size (assuming F64 storage: 4B time + 8B value + 1B quality = 13B)
    original_bytes_ = records.size() * 13;

    // Encode all points
    for (const auto& rec : records) {
        QuantizedPoint qp;
        qp.time_offset = rec.time_offset;
        qp.quality = rec.quality;

        // Quantize value
        if (!quantizeValue(rec.value.f64_value, qp.quantized_value)) {
            setError("Value out of range: " + std::to_string(rec.value.f64_value) +
                    " (range: [" + std::to_string(low_extreme_) + ", " +
                    std::to_string(high_extreme_) + "])");
            return EncodeResult::ERROR_VALUE_OUT_OF_RANGE;
        }

        quantized_points.push_back(qp);
    }

    // Calculate compressed size (4B time + 2B quantized + 1B quality = 7B per point)
    compressed_bytes_ = quantized_points.size() * 7;

    return EncodeResult::SUCCESS;
}

double Quantized16Encoder::getCompressionRatio() const {
    if (compressed_bytes_ == 0) {
        return 1.0;
    }
    return static_cast<double>(original_bytes_) / static_cast<double>(compressed_bytes_);
}

}  // namespace xtdb
