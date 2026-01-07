#include "xTdb/block_writer.h"
#include <cassert>
#include <cstring>
#include <iostream>

namespace xtdb {

BlockWriter::BlockWriter(AlignedIO* io,
                        const ChunkLayout& layout,
                        uint64_t container_base)
    : io_(io),
      layout_(layout),
      container_base_(container_base) {

    assert(io_ != nullptr);
    assert(io_->isOpen());
    assert(LayoutCalculator::validateLayout(layout_));
}

void BlockWriter::setError(const std::string& message) {
    last_error_ = message;
}

uint16_t BlockWriter::calculateRecordSize(ValueType value_type) {
    // Record format: 3B time_offset + 1B quality + NB value
    uint16_t header_size = 4;  // 3B time_offset + 1B quality

    uint16_t value_size = 0;
    switch (value_type) {
        case ValueType::VT_BOOL:
            value_size = 1;
            break;
        case ValueType::VT_I32:
            value_size = 4;
            break;
        case ValueType::VT_F32:
            value_size = 4;
            break;
        case ValueType::VT_F64:
            value_size = 8;
            break;
    }

    return header_size + value_size;
}

uint64_t BlockWriter::serializeRecords(const TagBuffer& tag_buffer,
                                       void* buffer,
                                       uint64_t buffer_size) {
    char* ptr = static_cast<char*>(buffer);
    uint64_t offset = 0;

    uint16_t record_size = calculateRecordSize(tag_buffer.value_type);

    for (const auto& record : tag_buffer.records) {
        if (offset + record_size > buffer_size) {
            break;  // Buffer full
        }

        // Write time_offset (3 bytes, little-endian)
        ptr[offset + 0] = static_cast<uint8_t>(record.time_offset & 0xFF);
        ptr[offset + 1] = static_cast<uint8_t>((record.time_offset >> 8) & 0xFF);
        ptr[offset + 2] = static_cast<uint8_t>((record.time_offset >> 16) & 0xFF);

        // Write quality (1 byte)
        ptr[offset + 3] = record.quality;

        // Write value
        switch (tag_buffer.value_type) {
            case ValueType::VT_BOOL:
                ptr[offset + 4] = record.value.bool_value ? 1 : 0;
                break;
            case ValueType::VT_I32:
                std::memcpy(ptr + offset + 4, &record.value.i32_value, 4);
                break;
            case ValueType::VT_F32:
                std::memcpy(ptr + offset + 4, &record.value.f32_value, 4);
                break;
            case ValueType::VT_F64:
                std::memcpy(ptr + offset + 4, &record.value.f64_value, 8);
                break;
        }

        offset += record_size;
        stats_.records_written++;
    }

    return offset;
}

BlockWriteResult BlockWriter::writeBlock(uint64_t chunk_offset,
                                        uint32_t data_block_index,
                                        const TagBuffer& tag_buffer) {
    // Validate data_block_index
    if (data_block_index >= layout_.data_blocks) {
        setError("Data block index out of range");
        return BlockWriteResult::ERROR_INVALID_BLOCK_INDEX;
    }

    // Calculate actual block_index (meta_blocks + data_block_index)
    uint32_t physical_block_index = layout_.meta_blocks + data_block_index;

    // Calculate physical offset directly from chunk_offset
    uint64_t block_offset = chunk_offset +
                           static_cast<uint64_t>(physical_block_index) * layout_.block_size_bytes;

    // Allocate aligned buffer for block
    AlignedBuffer buffer(layout_.block_size_bytes);
    buffer.zero();

    uint64_t data_size = 0;

    // Choose encoding based on tag configuration
    if (tag_buffer.encoding_type == EncodingType::ENC_SWINGING_DOOR) {
        // Use Swinging Door compression
        SwingingDoorEncoder encoder(tag_buffer.encoding_tolerance,
                                     tag_buffer.encoding_compression_factor);

        std::vector<SwingingDoorEncoder::CompressedPoint> compressed;
        auto encode_result = encoder.encode(tag_buffer.start_ts_us,
                                            tag_buffer.records,
                                            compressed);

        if (encode_result != SwingingDoorEncoder::EncodeResult::SUCCESS) {
            setError("Swinging Door encoding failed: " + encoder.getLastError());
            return BlockWriteResult::ERROR_IO_FAILED;
        }

        // Serialize compressed points
        // Format: [count:4B] [point1] [point2] ...
        // Each point: [time_offset:4B] [value:8B] [quality:1B]
        char* ptr = static_cast<char*>(buffer.data());
        uint32_t count = static_cast<uint32_t>(compressed.size());
        std::memcpy(ptr, &count, 4);
        data_size = 4;

        for (const auto& cp : compressed) {
            std::memcpy(ptr + data_size, &cp.time_offset, 4);
            data_size += 4;
            std::memcpy(ptr + data_size, &cp.value, 8);
            data_size += 8;
            std::memcpy(ptr + data_size, &cp.quality, 1);
            data_size += 1;
        }

    } else if (tag_buffer.encoding_type == EncodingType::ENC_QUANTIZED_16) {
        // Use 16-bit Quantization compression
        // Parameters stored in encoding_tolerance (low_extreme) and encoding_compression_factor (high_extreme)
        Quantized16Encoder encoder(tag_buffer.encoding_tolerance,
                                    tag_buffer.encoding_compression_factor);

        std::vector<Quantized16Encoder::QuantizedPoint> quantized;
        auto encode_result = encoder.encode(tag_buffer.start_ts_us,
                                            tag_buffer.records,
                                            quantized);

        if (encode_result != Quantized16Encoder::EncodeResult::SUCCESS) {
            setError("16-bit Quantization encoding failed: " + encoder.getLastError());
            return BlockWriteResult::ERROR_IO_FAILED;
        }

        // Serialize quantized points
        // Format: [count:4B] [point1] [point2] ...
        // Each point: [time_offset:4B] [quantized_value:2B] [quality:1B]
        char* ptr = static_cast<char*>(buffer.data());
        uint32_t count = static_cast<uint32_t>(quantized.size());
        std::memcpy(ptr, &count, 4);
        data_size = 4;

        for (const auto& qp : quantized) {
            std::memcpy(ptr + data_size, &qp.time_offset, 4);
            data_size += 4;
            std::memcpy(ptr + data_size, &qp.quantized_value, 2);
            data_size += 2;
            std::memcpy(ptr + data_size, &qp.quality, 1);
            data_size += 1;
        }

    } else {
        // ENC_RAW: Use standard serialization
        data_size = serializeRecords(tag_buffer,
                                     buffer.data(),
                                     buffer.size());
    }

    // Check if data fits in block
    if (data_size > layout_.block_size_bytes) {
        setError("Buffer too large for block");
        return BlockWriteResult::ERROR_BUFFER_TOO_LARGE;
    }

    // Write to disk (data block only, no directory update)
    IOResult io_result = io_->write(buffer.data(),
                                    layout_.block_size_bytes,
                                    block_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to write block: " + io_->getLastError());
        return BlockWriteResult::ERROR_IO_FAILED;
    }

    // Update statistics
    stats_.blocks_written++;
    stats_.bytes_written += layout_.block_size_bytes;

    return BlockWriteResult::SUCCESS;
}

}  // namespace xtdb
