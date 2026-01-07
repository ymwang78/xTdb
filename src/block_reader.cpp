#include "xTdb/block_reader.h"
#include "xTdb/constants.h"
#include <cstring>
#include <iostream>

namespace xtdb {

// ============================================================================
// CRC32 Implementation (reuse)
// ============================================================================

static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

static void init_crc32_table() {
    if (crc32_table_initialized) return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320u;
            } else {
                crc >>= 1;
            }
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

uint32_t BlockReader::calculateCRC32(const void* data, uint64_t size) {
    init_crc32_table();

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;

    for (uint64_t i = 0; i < size; i++) {
        uint8_t index = (crc ^ ptr[i]) & 0xFF;
        crc = (crc >> 8) ^ crc32_table[index];
    }

    return crc ^ 0xFFFFFFFFu;
}

// ============================================================================
// BlockReader Implementation
// ============================================================================

BlockReader::BlockReader(AlignedIO* io, const ChunkLayout& layout)
    : io_(io), layout_(layout) {
}

BlockReader::~BlockReader() {
}

uint32_t BlockReader::getRecordSize(ValueType value_type) const {
    // Record format: 3B time_offset + 1B quality + value
    switch (value_type) {
        case ValueType::VT_BOOL:
            return 3 + 1 + 1;  // 5 bytes
        case ValueType::VT_I32:
            return 3 + 1 + 4;  // 8 bytes
        case ValueType::VT_F32:
            return 3 + 1 + 4;  // 8 bytes
        case ValueType::VT_F64:
            return 3 + 1 + 8;  // 12 bytes
        default:
            return 0;
    }
}

ReadResult BlockReader::parseRecords(const void* data,
                                    uint64_t size,
                                    ValueType value_type,
                                    uint32_t record_count,
                                    std::vector<MemRecord>& records) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    uint32_t record_size = getRecordSize(value_type);

    if (record_size == 0) {
        setError("Invalid value type");
        return ReadResult::ERROR_INVALID_BLOCK;
    }

    // Check if buffer has enough space
    if (size < record_count * record_size) {
        setError("Buffer too small for record count");
        return ReadResult::ERROR_PARSE_FAILED;
    }

    records.clear();
    records.reserve(record_count);

    for (uint32_t i = 0; i < record_count; i++) {
        uint64_t offset = i * record_size;

        MemRecord record;

        // Parse time_offset (3 bytes, little-endian)
        record.time_offset = static_cast<uint32_t>(ptr[offset + 0]) |
                            (static_cast<uint32_t>(ptr[offset + 1]) << 8) |
                            (static_cast<uint32_t>(ptr[offset + 2]) << 16);

        // Parse quality (1 byte)
        record.quality = ptr[offset + 3];

        // Parse value based on type
        switch (value_type) {
            case ValueType::VT_BOOL:
                record.value.bool_value = ptr[offset + 4] != 0;
                break;

            case ValueType::VT_I32:
                std::memcpy(&record.value.i32_value, ptr + offset + 4, 4);
                break;

            case ValueType::VT_F32:
                std::memcpy(&record.value.f32_value, ptr + offset + 4, 4);
                break;

            case ValueType::VT_F64:
                std::memcpy(&record.value.f64_value, ptr + offset + 4, 8);
                break;
        }

        records.push_back(record);
    }

    return ReadResult::SUCCESS;
}

ReadResult BlockReader::readBlock(uint64_t chunk_offset,
                                 uint32_t block_index,
                                 uint32_t tag_id,
                                 int64_t start_ts_us,
                                 TimeUnit time_unit,
                                 ValueType value_type,
                                 uint32_t record_count,
                                 std::vector<MemRecord>& records) {
    // Calculate data block physical offset
    // block_offset = chunk_offset + (meta_blocks + data_block_index) * block_size
    uint32_t physical_block_index = layout_.meta_blocks + block_index;
    uint64_t block_offset = chunk_offset +
                           static_cast<uint64_t>(physical_block_index) * layout_.block_size_bytes;

    std::cerr << "[BlockReader] Reading from block_offset=" << block_offset
              << " (chunk_offset=" << chunk_offset
              << ", physical_block_index=" << physical_block_index
              << ", block_size=" << layout_.block_size_bytes << ")" << std::endl;

    // Read block
    AlignedBuffer buffer(layout_.block_size_bytes);
    IOResult io_result = io_->read(buffer.data(), layout_.block_size_bytes, block_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read data block: " + io_->getLastError());
        return ReadResult::ERROR_IO_FAILED;
    }

    // Parse records
    ReadResult parse_result = parseRecords(buffer.data(),
                                          layout_.block_size_bytes,
                                          value_type,
                                          record_count,
                                          records);
    if (parse_result != ReadResult::SUCCESS) {
        return parse_result;
    }

    // Update statistics
    stats_.blocks_read++;
    stats_.bytes_read += layout_.block_size_bytes;
    stats_.records_read += record_count;

    // Suppress unused parameter warnings
    (void)tag_id;
    (void)start_ts_us;
    (void)time_unit;

    return ReadResult::SUCCESS;
}

ReadResult BlockReader::verifyBlockIntegrity(uint64_t chunk_offset,
                                            uint32_t block_index,
                                            uint32_t expected_crc32) {
    // Calculate data block physical offset
    // block_offset = chunk_offset + (meta_blocks + data_block_index) * block_size
    uint32_t physical_block_index = layout_.meta_blocks + block_index;
    uint64_t block_offset = chunk_offset +
                           static_cast<uint64_t>(physical_block_index) * layout_.block_size_bytes;

    // Read block
    AlignedBuffer buffer(layout_.block_size_bytes);
    IOResult io_result = io_->read(buffer.data(), layout_.block_size_bytes, block_offset);
    if (io_result != IOResult::SUCCESS) {
        setError("Failed to read block for CRC: " + io_->getLastError());
        return ReadResult::ERROR_IO_FAILED;
    }

    // Calculate CRC32
    uint32_t calculated_crc = calculateCRC32(buffer.data(), layout_.block_size_bytes);

    // Verify CRC
    if (calculated_crc != expected_crc32) {
        setError("Block CRC mismatch");
        return ReadResult::ERROR_CRC_MISMATCH;
    }

    return ReadResult::SUCCESS;
}

void BlockReader::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
