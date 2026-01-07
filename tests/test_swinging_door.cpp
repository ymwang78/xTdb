#include <gtest/gtest.h>
#include "xTdb/swinging_door_encoder.h"
#include "xTdb/swinging_door_decoder.h"
#include <cmath>

using namespace xtdb;

class SwingingDoorTest : public ::testing::Test {
protected:
    void SetUp() override {
    }

    // 创建测试数据：线性上升
    std::vector<MemRecord> createLinearData(int64_t /*start_ts_us*/, int count, double start_value, double slope) {
        std::vector<MemRecord> records;
        for (int i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * 1000;  // 每秒一个点
            rec.value.f64_value = start_value + slope * i;
            rec.quality = 192;
            records.push_back(rec);
        }
        return records;
    }

    // 创建测试数据：噪声数据
    std::vector<MemRecord> createNoisyData(int64_t /*start_ts_us*/, int count, double base_value, double noise_amplitude) {
        std::vector<MemRecord> records;
        for (int i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * 1000;  // 每秒一个点
            // 添加正弦噪声
            double noise = noise_amplitude * std::sin(i * 0.5);
            rec.value.f64_value = base_value + noise;
            rec.quality = 192;
            records.push_back(rec);
        }
        return records;
    }
};

// 测试1：基本编码 - 平稳数据应该高度压缩
TEST_F(SwingingDoorTest, EncodeConstantData) {
    SwingingDoorEncoder encoder(1.0, 1.0);  // tolerance = 1.0, factor = 1.0

    // 创建100个恒定值点
    std::vector<MemRecord> records;
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;
        rec.value.f64_value = 50.0;  // 恒定值
        rec.quality = 192;
        records.push_back(rec);
    }

    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    auto result = encoder.encode(1000000, records, compressed);

    EXPECT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);

    // 恒定数据应该只需要2个点（起点和终点）
    EXPECT_LE(compressed.size(), 2u);

    // 压缩比应该很高
    EXPECT_GT(encoder.getCompressionRatio(), 10.0);

    std::cout << "Constant data: " << records.size() << " -> " << compressed.size()
              << " (ratio: " << encoder.getCompressionRatio() << ")" << std::endl;
}

// 测试2：线性数据压缩
TEST_F(SwingingDoorTest, EncodeLinearData) {
    SwingingDoorEncoder encoder(1.0, 1.0);

    // 创建100个线性上升点
    auto records = createLinearData(1000000, 100, 0.0, 0.1);

    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    auto result = encoder.encode(1000000, records, compressed);

    EXPECT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);

    // 线性数据应该只需要2个点（起点和终点）
    EXPECT_LE(compressed.size(), 3u);

    std::cout << "Linear data: " << records.size() << " -> " << compressed.size()
              << " (ratio: " << encoder.getCompressionRatio() << ")" << std::endl;
}

// 测试3：噪声数据压缩
TEST_F(SwingingDoorTest, EncodeNoisyData) {
    // 使用较大的容差来压缩噪声
    SwingingDoorEncoder encoder(5.0, 1.0);

    // 创建100个带噪声的点
    auto records = createNoisyData(1000000, 100, 50.0, 3.0);

    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    auto result = encoder.encode(1000000, records, compressed);

    EXPECT_EQ(SwingingDoorEncoder::EncodeResult::SUCCESS, result);

    // 噪声数据压缩比应该较低，但仍有压缩效果
    EXPECT_LT(compressed.size(), records.size());
    EXPECT_GT(encoder.getCompressionRatio(), 1.0);

    std::cout << "Noisy data: " << records.size() << " -> " << compressed.size()
              << " (ratio: " << encoder.getCompressionRatio() << ")" << std::endl;
}

// 测试4：解码 - 插值验证
TEST_F(SwingingDoorTest, DecodeInterpolation) {
    SwingingDoorEncoder encoder(1.0, 1.0);
    SwingingDoorDecoder decoder;

    // 创建简单的3点数据
    std::vector<MemRecord> records;
    MemRecord r1; r1.time_offset = 0;    r1.value.f64_value = 0.0;  r1.quality = 192;
    MemRecord r2; r2.time_offset = 5000; r2.value.f64_value = 50.0; r2.quality = 192;
    MemRecord r3; r3.time_offset = 10000; r3.value.f64_value = 100.0; r3.quality = 192;
    records.push_back(r1);
    records.push_back(r2);
    records.push_back(r3);

    // 编码
    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    encoder.encode(1000000, records, compressed);

    // 解码 - 在中间点插值
    SwingingDoorDecoder::DecodedPoint result;
    // Query at 2.5 seconds offset = 2500 ms = base + 2500*1000 microseconds
    auto decode_result = decoder.interpolate(1000000, compressed, 1000000 + 2500*1000, result);

    EXPECT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, decode_result);

    // 2.5秒处应该插值为 25.0 (在 0.0 和 50.0 之间)
    EXPECT_NEAR(result.value, 25.0, 0.1);
}

// 测试5：解码 - 边界情况
TEST_F(SwingingDoorTest, DecodeBoundary) {
    SwingingDoorEncoder encoder(1.0, 1.0);
    SwingingDoorDecoder decoder;

    // 单点数据
    std::vector<MemRecord> records;
    MemRecord r1; r1.time_offset = 0; r1.value.f64_value = 42.0; r1.quality = 192;
    records.push_back(r1);

    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    encoder.encode(1000000, records, compressed);

    // 解码单点
    SwingingDoorDecoder::DecodedPoint result;
    auto decode_result = decoder.interpolate(1000000, compressed, 1000000, result);

    EXPECT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, decode_result);
    EXPECT_DOUBLE_EQ(result.value, 42.0);
}

// 测试6：压缩因子影响
TEST_F(SwingingDoorTest, CompressionFactorEffect) {
    // Create data with sharp zigzag pattern that will stress the tolerance
    std::vector<MemRecord> records;
    for (int i = 0; i < 100; i++) {
        MemRecord rec;
        rec.time_offset = i * 1000;  // 每秒一个点
        // 锯齿波：交替上下跳动 ±5
        rec.value.f64_value = 50.0 + ((i % 2 == 0) ? 5.0 : -5.0);
        rec.quality = 192;
        records.push_back(rec);
    }

    // 小因子 = 严格压缩（tolerance = 1.0 * 0.5 = 0.5）
    // 锯齿波幅度 ±5 远大于 0.5，应该保留很多点
    SwingingDoorEncoder encoder1(1.0, 0.5);
    std::vector<SwingingDoorEncoder::CompressedPoint> compressed1;
    encoder1.encode(1000000, records, compressed1);

    // 大因子 = 宽松压缩（tolerance = 1.0 * 2.0 = 2.0）
    // 仍然小于锯齿波幅度，但能容纳更多点
    SwingingDoorEncoder encoder2(1.0, 2.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> compressed2;
    encoder2.encode(1000000, records, compressed2);

    // 大因子 = 更宽松压缩（tolerance = 1.0 * 10.0 = 10.0）
    // 应该能容纳锯齿波的大部分变化
    SwingingDoorEncoder encoder3(1.0, 10.0);
    std::vector<SwingingDoorEncoder::CompressedPoint> compressed3;
    encoder3.encode(1000000, records, compressed3);

    // 更大的因子应该产生更少的压缩点（更高的压缩比）
    EXPECT_LT(compressed3.size(), compressed2.size());
    EXPECT_LT(compressed2.size(), compressed1.size());
    EXPECT_GT(encoder3.getCompressionRatio(), encoder2.getCompressionRatio());
    EXPECT_GT(encoder2.getCompressionRatio(), encoder1.getCompressionRatio());

    std::cout << "Factor 0.5 (tol 0.5): " << compressed1.size() << " points (ratio: " << encoder1.getCompressionRatio() << ")" << std::endl;
    std::cout << "Factor 2.0 (tol 2.0): " << compressed2.size() << " points (ratio: " << encoder2.getCompressionRatio() << ")" << std::endl;
    std::cout << "Factor 10.0 (tol 10.0): " << compressed3.size() << " points (ratio: " << encoder3.getCompressionRatio() << ")" << std::endl;
}

// 测试7：解码时间范围查询
TEST_F(SwingingDoorTest, DecodeTimeRange) {
    SwingingDoorEncoder encoder(1.0, 1.0);
    SwingingDoorDecoder decoder;

    auto records = createLinearData(1000000, 10, 0.0, 10.0);

    std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
    encoder.encode(1000000, records, compressed);

    // 查询中间时间段的点 (3秒到7秒)
    // Point timestamps: 1000000, 2000000, 3000000, ..., 10000000
    // Query range: [3000000, 7000000] should include boundary points for interpolation
    std::vector<SwingingDoorDecoder::DecodedPoint> decoded;
    auto result = decoder.decode(1000000, compressed, 3000000, 7000000, decoded);

    EXPECT_EQ(SwingingDoorDecoder::DecodeResult::SUCCESS, result);
    EXPECT_GT(decoded.size(), 0u);

    // 验证返回的点：应该包含范围内的点，以及用于插值的边界点
    // 对于完全线性的数据，只会有2个边界点（用于插值）
    if (decoded.size() >= 2) {
        // 第一个点应该在范围开始之前或等于
        EXPECT_LE(decoded[0].timestamp_us, 3000000);
        // 最后一个点应该在范围结束之后或等于
        EXPECT_GE(decoded[decoded.size()-1].timestamp_us, 7000000);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
