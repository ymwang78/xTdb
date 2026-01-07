#ifndef XTDB_MEM_BUFFER_H_
#define XTDB_MEM_BUFFER_H_

#include "struct_defs.h"
#include "wal_writer.h"
#include <vector>
#include <map>
#include <cstdint>

namespace xtdb {

// ============================================================================
// MemBuffer: In-memory buffer for tag-based aggregation
// ============================================================================

/// Record in memory (time_offset format)
struct MemRecord {
    uint32_t time_offset;    // 3 bytes used (relative to start_ts_us)
    uint8_t  quality;
    union {
        bool     bool_value;
        int32_t  i32_value;
        float    f32_value;
        double   f64_value;
    } value;

    MemRecord() : time_offset(0), quality(0) {
        value.f64_value = 0.0;
    }
};

/// Tag buffer: records for a single tag
struct TagBuffer {
    uint32_t tag_id;
    ValueType value_type;
    TimeUnit time_unit;
    EncodingType encoding_type;  // Compression/encoding method
    int64_t start_ts_us;         // Base timestamp
    std::vector<MemRecord> records;

    // Encoding parameters (interpretation depends on encoding_type)
    double encoding_tolerance;           // For ENC_SWINGING_DOOR: tolerance value
    double encoding_compression_factor;  // For ENC_SWINGING_DOOR: compression factor

    TagBuffer() : tag_id(0), value_type(ValueType::VT_F64),
                  time_unit(TimeUnit::TU_MS), encoding_type(EncodingType::ENC_RAW),
                  start_ts_us(0), encoding_tolerance(0.0), encoding_compression_factor(1.0) {}
};

class MemBuffer {
public:
    /// Constructor
    /// @param max_records_per_tag Maximum records per tag before flush
    explicit MemBuffer(uint32_t max_records_per_tag = 10000);

    /// Add entry from WAL
    /// @param entry WAL entry
    /// @return true if buffer needs flush
    bool addEntry(const WALEntry& entry);

    /// Get buffer for a specific tag
    /// @param tag_id Tag ID
    /// @return Pointer to TagBuffer, or nullptr if not found
    TagBuffer* getTagBuffer(uint32_t tag_id);

    /// Get all tag buffers
    const std::map<uint32_t, TagBuffer>& getAllBuffers() const {
        return buffers_;
    }

    /// Clear buffer for a specific tag
    void clearTag(uint32_t tag_id);

    /// Clear all buffers
    void clearAll();

    /// Get total number of records across all tags
    uint64_t getTotalRecords() const {
        uint64_t total = 0;
        for (const auto& pair : buffers_) {
            total += pair.second.records.size();
        }
        return total;
    }

    /// Check if any tag buffer is ready for flush
    bool hasFlushableTag() const {
        for (const auto& pair : buffers_) {
            if (pair.second.records.size() >= max_records_per_tag_) {
                return true;
            }
        }
        return false;
    }

    /// Get tag ID that needs flush (largest buffer)
    uint32_t getFlushableTag() const;

private:
    /// Calculate time offset from base timestamp
    uint32_t calculateTimeOffset(int64_t timestamp_us,
                                 int64_t base_ts_us,
                                 TimeUnit time_unit);

    std::map<uint32_t, TagBuffer> buffers_;  // Tag ID -> TagBuffer
    uint32_t max_records_per_tag_;           // Flush threshold
};

}  // namespace xtdb

#endif  // XTDB_MEM_BUFFER_H_
