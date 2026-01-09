#include "xTdb/quantized_16_decoder.h"
#include <cmath>

namespace xtdb {

Quantized16Decoder::Quantized16Decoder(double low_extreme, double high_extreme)
    : low_extreme_(low_extreme),
      high_extreme_(high_extreme),
      range_(high_extreme - low_extreme) {
}

void Quantized16Decoder::setError(const std::string& message) {
    last_error_ = message;
}

bool Quantized16Decoder::dequantizeValue(uint16_t quantized, double& value) const {
    // Reverse mapping: [0, 65535] -> [low, high]
    // value = low + (quantized / 65535.0) * (high - low)
    double normalized = static_cast<double>(quantized) / 65535.0;
    value = low_extreme_ + normalized * range_;

    return true;
}

Quantized16Decoder::DecodeResult Quantized16Decoder::decode(
    int64_t base_ts_us,
    const std::vector<Quantized16Encoder::QuantizedPoint>& quantized_points,
    std::vector<MemRecord>& records) {

    (void)base_ts_us;  // Not used directly in decoding
    records.clear();

    if (quantized_points.empty()) {
        return DecodeResult::SUCCESS;
    }

    // Validate range
    if (range_ <= 0.0 || !std::isfinite(range_)) {
        setError("Invalid range: high must be > low");
        return DecodeResult::ERR_INVALID_RANGE;
    }

    // Decode all points
    for (const auto& qp : quantized_points) {
        MemRecord rec;
        rec.time_offset = qp.time_offset;
        rec.quality = qp.quality;

        // Dequantize value
        if (!dequantizeValue(qp.quantized_value, rec.value.f64_value)) {
            setError("Failed to dequantize value");
            return DecodeResult::ERR_INVALID_DATA;
        }

        records.push_back(rec);
    }

    return DecodeResult::SUCCESS;
}

double Quantized16Decoder::getMaxPrecisionLoss() const {
    // Maximum error is half the quantization step size
    // Step size = range / 65535
    // Max error = step_size / 2
    return range_ / 131070.0;  // 65535 * 2
}

}  // namespace xtdb
