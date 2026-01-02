#include "xTdb/mem_buffer.h"

namespace xtdb {

MemBuffer::MemBuffer(uint32_t max_records_per_tag)
    : max_records_per_tag_(max_records_per_tag) {
}

uint32_t MemBuffer::calculateTimeOffset(int64_t timestamp_us,
                                       int64_t base_ts_us,
                                       TimeUnit time_unit) {
    int64_t delta_us = timestamp_us - base_ts_us;

    // Convert to target time unit
    int64_t offset = 0;
    switch (time_unit) {
        case TimeUnit::TU_100MS:
            offset = delta_us / 100000;  // 100ms = 100,000us
            break;
        case TimeUnit::TU_10MS:
            offset = delta_us / 10000;   // 10ms = 10,000us
            break;
        case TimeUnit::TU_MS:
            offset = delta_us / 1000;    // 1ms = 1,000us
            break;
        case TimeUnit::TU_100US:
            offset = delta_us / 100;     // 100us
            break;
        case TimeUnit::TU_10US:
            offset = delta_us / 10;      // 10us
            break;
        case TimeUnit::TU_US:
            offset = delta_us;           // 1us
            break;
    }

    // Clamp to 24-bit range (3 bytes)
    if (offset < 0) offset = 0;
    if (offset > 0xFFFFFF) offset = 0xFFFFFF;

    return static_cast<uint32_t>(offset);
}

bool MemBuffer::addEntry(const WALEntry& entry) {
    // Get or create tag buffer
    auto it = buffers_.find(entry.tag_id);
    if (it == buffers_.end()) {
        // Create new buffer
        TagBuffer new_buffer;
        new_buffer.tag_id = entry.tag_id;
        new_buffer.value_type = static_cast<ValueType>(entry.value_type);
        new_buffer.time_unit = TimeUnit::TU_MS;  // Default: milliseconds
        new_buffer.start_ts_us = entry.timestamp_us;

        buffers_[entry.tag_id] = new_buffer;
        it = buffers_.find(entry.tag_id);
    }

    TagBuffer& buffer = it->second;

    // Create record
    MemRecord record;
    record.time_offset = calculateTimeOffset(entry.timestamp_us,
                                            buffer.start_ts_us,
                                            buffer.time_unit);
    record.quality = entry.quality;

    // Copy value based on type
    switch (buffer.value_type) {
        case ValueType::VT_BOOL:
            record.value.bool_value = entry.value.bool_value;
            break;
        case ValueType::VT_I32:
            record.value.i32_value = entry.value.i32_value;
            break;
        case ValueType::VT_F32:
            record.value.f32_value = entry.value.f32_value;
            break;
        case ValueType::VT_F64:
            record.value.f64_value = entry.value.f64_value;
            break;
    }

    // Add to buffer
    buffer.records.push_back(record);

    // Check if flush needed
    return buffer.records.size() >= max_records_per_tag_;
}

TagBuffer* MemBuffer::getTagBuffer(uint32_t tag_id) {
    auto it = buffers_.find(tag_id);
    if (it == buffers_.end()) {
        return nullptr;
    }
    return &it->second;
}

void MemBuffer::clearTag(uint32_t tag_id) {
    auto it = buffers_.find(tag_id);
    if (it != buffers_.end()) {
        it->second.records.clear();
        // Keep the buffer structure but clear records
    }
}

void MemBuffer::clearAll() {
    buffers_.clear();
}

uint32_t MemBuffer::getFlushableTag() const {
    uint32_t max_tag = 0;
    size_t max_size = 0;

    for (const auto& pair : buffers_) {
        if (pair.second.records.size() > max_size) {
            max_size = pair.second.records.size();
            max_tag = pair.first;
        }
    }

    return max_tag;
}

}  // namespace xtdb
