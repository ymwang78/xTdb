#include "xTdb/xtdb_api.h"
#include "xTdb/storage_engine.h"
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <ctime>

// ============================================================================
// Internal Structures
// ============================================================================

/// Handle implementation (opaque to C API users)
struct xtdb_handle_impl {
    xtdb::StorageEngine* engine;
    std::mutex mutex;  // Thread-safe access
    std::string last_error;

    xtdb_handle_impl() : engine(nullptr) {}
    ~xtdb_handle_impl() {
        delete engine;
    }
};

/// Result set implementation
struct xtdb_result_set_impl {
    std::vector<xtdb::StorageEngine::QueryPoint> points;
};

// ============================================================================
// Version Information
// ============================================================================

const char* xtdb_version(void) {
    static char version_str[32];
    snprintf(version_str, sizeof(version_str), "%d.%d.%d",
             XTDB_VERSION_MAJOR, XTDB_VERSION_MINOR, XTDB_VERSION_PATCH);
    return version_str;
}

// ============================================================================
// Error Handling
// ============================================================================

const char* xtdb_error_string(xtdb_error_t error) {
    switch (error) {
        case XTDB_SUCCESS: return "Success";
        case XTDB_ERR_INVALID_PATH: return "Invalid path";
        case XTDB_ERR_CONTAINER_OPEN_FAILED: return "Container open failed";
        case XTDB_ERR_CONTAINER_HEADER_INVALID: return "Container header invalid";
        case XTDB_ERR_METADATA_OPEN_FAILED: return "Metadata open failed";
        case XTDB_ERR_WAL_OPEN_FAILED: return "WAL open failed";
        case XTDB_ERR_CHUNK_ALLOCATION_FAILED: return "Chunk allocation failed";
        case XTDB_ERR_STATE_RESTORATION_FAILED: return "State restoration failed";
        case XTDB_ERR_WAL_REPLAY_FAILED: return "WAL replay failed";
        case XTDB_ERR_ENGINE_NOT_OPEN: return "Engine not open";
        case XTDB_ERR_INVALID_DATA: return "Invalid data";
        case XTDB_ERR_INVALID_HANDLE: return "Invalid handle";
        case XTDB_ERR_OUT_OF_MEMORY: return "Out of memory";
        case XTDB_ERR_INVALID_PARAMETER: return "Invalid parameter";
        case XTDB_ERR_QUERY_FAILED: return "Query failed";
        case XTDB_ERR_WRITE_FAILED: return "Write failed";
        case XTDB_ERR_FLUSH_FAILED: return "Flush failed";
        default: return "Unknown error";
    }
}

/// Convert EngineResult to xtdb_error_t
static xtdb_error_t convertEngineResult(xtdb::EngineResult result) {
    switch (result) {
        case xtdb::EngineResult::SUCCESS:
            return XTDB_SUCCESS;
        case xtdb::EngineResult::ERR_INVALID_PATH:
            return XTDB_ERR_INVALID_PATH;
        case xtdb::EngineResult::ERR_CONTAINER_OPEN_FAILED:
            return XTDB_ERR_CONTAINER_OPEN_FAILED;
        case xtdb::EngineResult::ERR_CONTAINER_HEADER_INVALID:
            return XTDB_ERR_CONTAINER_HEADER_INVALID;
        case xtdb::EngineResult::ERR_METADATA_OPEN_FAILED:
            return XTDB_ERR_METADATA_OPEN_FAILED;
        case xtdb::EngineResult::ERR_WAL_OPEN_FAILED:
            return XTDB_ERR_WAL_OPEN_FAILED;
        case xtdb::EngineResult::ERR_CHUNK_ALLOCATION_FAILED:
            return XTDB_ERR_CHUNK_ALLOCATION_FAILED;
        case xtdb::EngineResult::ERR_STATE_RESTORATION_FAILED:
            return XTDB_ERR_STATE_RESTORATION_FAILED;
        case xtdb::EngineResult::ERR_WAL_REPLAY_FAILED:
            return XTDB_ERR_WAL_REPLAY_FAILED;
        case xtdb::EngineResult::ERR_ENGINE_NOT_OPEN:
            return XTDB_ERR_ENGINE_NOT_OPEN;
        case xtdb::EngineResult::ERR_INVALID_DATA:
            return XTDB_ERR_INVALID_DATA;
        default:
            return XTDB_ERR_INVALID_DATA;
    }
}

// ============================================================================
// Type Conversion Helpers
// ============================================================================

/// Convert C API value type to C++ Core ValueType
static xtdb::ValueType convertValueType(xtdb_value_type_t type) {
    switch (type) {
        case XTDB_VT_BOOL: return xtdb::ValueType::VT_BOOL;
        case XTDB_VT_I32:  return xtdb::ValueType::VT_I32;
        case XTDB_VT_F32:  return xtdb::ValueType::VT_F32;
        case XTDB_VT_F64:  return xtdb::ValueType::VT_F64;
        default:           return xtdb::ValueType::VT_F64;
    }
}

/// Convert C API time unit to C++ Core TimeUnit
static xtdb::TimeUnit convertTimeUnit(xtdb_time_unit_t unit) {
    switch (unit) {
        case XTDB_TU_100MS: return xtdb::TimeUnit::TU_100MS;
        case XTDB_TU_10MS:  return xtdb::TimeUnit::TU_10MS;
        case XTDB_TU_MS:    return xtdb::TimeUnit::TU_MS;
        case XTDB_TU_100US: return xtdb::TimeUnit::TU_100US;
        case XTDB_TU_10US:  return xtdb::TimeUnit::TU_10US;
        case XTDB_TU_US:    return xtdb::TimeUnit::TU_US;
        default:            return xtdb::TimeUnit::TU_MS;
    }
}

/// Convert C API encoding type to C++ Core EncodingType
static xtdb::EncodingType convertEncodingType(xtdb_encoding_type_t type) {
    switch (type) {
        case XTDB_ENC_RAW:            return xtdb::EncodingType::ENC_RAW;
        case XTDB_ENC_SWINGING_DOOR:  return xtdb::EncodingType::ENC_SWINGING_DOOR;
        case XTDB_ENC_QUANTIZED_16:   return xtdb::EncodingType::ENC_QUANTIZED_16;
        case XTDB_ENC_GORILLA:        return xtdb::EncodingType::ENC_GORILLA;
        case XTDB_ENC_DELTA_OF_DELTA: return xtdb::EncodingType::ENC_DELTA_OF_DELTA;
        default:                      return xtdb::EncodingType::ENC_RAW;
    }
}

/// Convert C API tag config to C++ Core TagConfig
static xtdb::StorageEngine::TagConfig convertTagConfig(const xtdb_tag_config_t* c_config) {
    xtdb::StorageEngine::TagConfig cpp_config;
    cpp_config.tag_id = c_config->tag_id;
    cpp_config.tag_name = c_config->tag_name;
    cpp_config.value_type = convertValueType(c_config->value_type);
    cpp_config.time_unit = convertTimeUnit(c_config->time_unit);
    cpp_config.encoding_type = convertEncodingType(c_config->encoding_type);
    cpp_config.encoding_param1 = c_config->encoding_param1;
    cpp_config.encoding_param2 = c_config->encoding_param2;
    return cpp_config;
}

// ============================================================================
// Configuration
// ============================================================================

void xtdb_config_init(xtdb_config_t* config) {
    if (!config) return;

    config->data_dir = "./data";
    config->db_path = "./data/meta.db";
    config->block_size_bytes = 16384;        // 16KB
    config->chunk_size_bytes = 256 * 1024 * 1024;  // 256MB
    config->retention_days = 0;  // No retention limit
}

void xtdb_tag_config_init(xtdb_tag_config_t* config, uint32_t tag_id) {
    if (!config) return;

    config->tag_id = tag_id;
    config->tag_name = nullptr;  // Optional, for debug logging
    config->value_type = XTDB_VT_F64;  // Default: 64-bit float
    config->time_unit = XTDB_TU_MS;    // Default: millisecond
    config->encoding_type = XTDB_ENC_RAW;  // Default: no compression
    config->encoding_param1 = 0.0;     // Default: no parameters
    config->encoding_param2 = 0.0;
}

// ============================================================================
// Lifecycle Management
// ============================================================================

xtdb_error_t xtdb_open(const xtdb_config_t* config, xtdb_handle_t* handle) {
    if (!handle) {
        return XTDB_ERR_INVALID_PARAMETER;
    }

    try {
        // Create handle
        xtdb_handle_impl* impl = new xtdb_handle_impl();

        // Create engine config
        xtdb::EngineConfig engine_config;
        if (config) {
            if (config->data_dir) {
                engine_config.data_dir = config->data_dir;
            }
            if (config->db_path) {
                engine_config.db_path = config->db_path;
            }
            engine_config.layout.block_size_bytes = config->block_size_bytes;
            engine_config.layout.chunk_size_bytes = config->chunk_size_bytes;
            engine_config.retention_days = config->retention_days;
        }

        // Create storage engine
        impl->engine = new xtdb::StorageEngine(engine_config);

        // Open engine
        xtdb::EngineResult result = impl->engine->open();
        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            xtdb_error_t error = convertEngineResult(result);
            delete impl;
            return error;
        }

        *handle = impl;
        return XTDB_SUCCESS;

    } catch (const std::bad_alloc&) {
        return XTDB_ERR_OUT_OF_MEMORY;
    } catch (...) {
        return XTDB_ERR_INVALID_DATA;
    }
}

void xtdb_close(xtdb_handle_t handle) {
    if (!handle) return;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (impl->engine) {
        impl->engine->close();
    }

    delete impl;
}

int xtdb_is_open(xtdb_handle_t handle) {
    if (!handle) return 0;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    return impl->engine && impl->engine->isOpen() ? 1 : 0;
}

const char* xtdb_get_last_error(xtdb_handle_t handle) {
    if (!handle) return "Invalid handle";

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (impl->last_error.empty() && impl->engine) {
        impl->last_error = impl->engine->getLastError();
    }

    return impl->last_error.c_str();
}

// ============================================================================
// Write Operations
// ============================================================================

xtdb_error_t xtdb_write_point(xtdb_handle_t handle, const xtdb_point_t* point) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!point) return XTDB_ERR_INVALID_PARAMETER;
    if (!point->tag_config) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        // Convert C API tag config to C++ Core TagConfig
        xtdb::StorageEngine::TagConfig cpp_config = convertTagConfig(point->tag_config);

        // Call new writePoint with TagConfig
        xtdb::EngineResult result = impl->engine->writePoint(
            &cpp_config,
            point->timestamp_us,
            point->value,
            point->quality
        );

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            return convertEngineResult(result);
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during write";
        return XTDB_ERR_WRITE_FAILED;
    }
}

xtdb_error_t xtdb_write_points(xtdb_handle_t handle,
                               const xtdb_point_t* points,
                               size_t count) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!points || count == 0) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        // Write points one by one
        // Future optimization: batch write in StorageEngine
        for (size_t i = 0; i < count; ++i) {
            if (!points[i].tag_config) {
                impl->last_error = "Tag configuration is null for point " + std::to_string(i);
                return XTDB_ERR_INVALID_PARAMETER;
            }

            // Convert C API tag config to C++ Core TagConfig
            xtdb::StorageEngine::TagConfig cpp_config = convertTagConfig(points[i].tag_config);

            // Call new writePoint with TagConfig
            xtdb::EngineResult result = impl->engine->writePoint(
                &cpp_config,
                points[i].timestamp_us,
                points[i].value,
                points[i].quality
            );

            if (result != xtdb::EngineResult::SUCCESS) {
                impl->last_error = impl->engine->getLastError();
                return convertEngineResult(result);
            }
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during batch write";
        return XTDB_ERR_WRITE_FAILED;
    }
}

xtdb_error_t xtdb_flush(xtdb_handle_t handle) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        xtdb::EngineResult result = impl->engine->flush();

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            return convertEngineResult(result);
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during flush";
        return XTDB_ERR_FLUSH_FAILED;
    }
}

// ============================================================================
// Read Operations
// ============================================================================

xtdb_error_t xtdb_query_points(xtdb_handle_t handle,
                               uint32_t tag_id,
                               int64_t start_ts_us,
                               int64_t end_ts_us,
                               xtdb_result_set_t* result_set) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!result_set) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        // Create result set
        xtdb_result_set_impl* rs = new xtdb_result_set_impl();

        // Query points
        xtdb::EngineResult result = impl->engine->queryPoints(
            tag_id,
            start_ts_us,
            end_ts_us,
            rs->points
        );

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            delete rs;
            return convertEngineResult(result);
        }

        *result_set = rs;
        return XTDB_SUCCESS;

    } catch (const std::bad_alloc&) {
        return XTDB_ERR_OUT_OF_MEMORY;
    } catch (...) {
        impl->last_error = "Exception during query";
        return XTDB_ERR_QUERY_FAILED;
    }
}

size_t xtdb_result_count(xtdb_result_set_t result_set) {
    if (!result_set) return 0;

    xtdb_result_set_impl* rs = static_cast<xtdb_result_set_impl*>(result_set);
    return rs->points.size();
}

xtdb_error_t xtdb_result_get(xtdb_result_set_t result_set,
                             size_t index,
                             xtdb_point_t* point) {
    if (!result_set) return XTDB_ERR_INVALID_HANDLE;
    if (!point) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_result_set_impl* rs = static_cast<xtdb_result_set_impl*>(result_set);

    if (index >= rs->points.size()) {
        return XTDB_ERR_INVALID_PARAMETER;
    }

    const auto& qp = rs->points[index];
    point->tag_config = nullptr;  // Note: QueryPoint doesn't store tag config (read-only)
    point->timestamp_us = qp.timestamp_us;
    point->value = qp.value;
    point->quality = qp.quality;

    return XTDB_SUCCESS;
}

void xtdb_result_free(xtdb_result_set_t result_set) {
    if (!result_set) return;

    xtdb_result_set_impl* rs = static_cast<xtdb_result_set_impl*>(result_set);
    delete rs;
}

// ============================================================================
// Maintenance Operations
// ============================================================================

xtdb_error_t xtdb_run_retention(xtdb_handle_t handle, int64_t current_time_us) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        xtdb::EngineResult result = impl->engine->runRetentionService(current_time_us);

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            return convertEngineResult(result);
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during retention";
        return XTDB_ERR_INVALID_DATA;
    }
}

xtdb_error_t xtdb_reclaim_space(xtdb_handle_t handle) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        xtdb::EngineResult result = impl->engine->reclaimDeprecatedChunks();

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            return convertEngineResult(result);
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during space reclamation";
        return XTDB_ERR_INVALID_DATA;
    }
}

xtdb_error_t xtdb_seal_chunk(xtdb_handle_t handle) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    try {
        xtdb::EngineResult result = impl->engine->sealCurrentChunk();

        if (result != xtdb::EngineResult::SUCCESS) {
            impl->last_error = impl->engine->getLastError();
            return convertEngineResult(result);
        }

        return XTDB_SUCCESS;

    } catch (...) {
        impl->last_error = "Exception during chunk seal";
        return XTDB_ERR_INVALID_DATA;
    }
}

// ============================================================================
// Statistics
// ============================================================================

xtdb_error_t xtdb_get_write_stats(xtdb_handle_t handle, xtdb_write_stats_t* stats) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!stats) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    const auto& ws = impl->engine->getWriteStats();
    stats->points_written = ws.points_written;
    stats->blocks_flushed = ws.blocks_flushed;
    stats->chunks_sealed = ws.chunks_sealed;
    stats->chunks_allocated = ws.chunks_allocated;

    return XTDB_SUCCESS;
}

xtdb_error_t xtdb_get_read_stats(xtdb_handle_t handle, xtdb_read_stats_t* stats) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!stats) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    const auto& rs = impl->engine->getReadStats();
    stats->queries_executed = rs.queries_executed;
    stats->blocks_read = rs.blocks_read;
    stats->points_read_disk = rs.points_read_disk;
    stats->points_read_memory = rs.points_read_memory;

    return XTDB_SUCCESS;
}

xtdb_error_t xtdb_get_maintenance_stats(xtdb_handle_t handle,
                                        xtdb_maintenance_stats_t* stats) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!stats) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    const auto& ms = impl->engine->getMaintenanceStats();
    stats->chunks_deprecated = ms.chunks_deprecated;
    stats->chunks_freed = ms.chunks_freed;
    stats->last_retention_run_ts = ms.last_retention_run_ts;

    return XTDB_SUCCESS;
}

// ============================================================================
// Container Information
// ============================================================================

size_t xtdb_get_container_count(xtdb_handle_t handle) {
    if (!handle) return 0;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return 0;

    return impl->engine->getContainers().size();
}

xtdb_error_t xtdb_get_container_info(xtdb_handle_t handle,
                                     size_t index,
                                     xtdb_container_info_t* info) {
    if (!handle) return XTDB_ERR_INVALID_HANDLE;
    if (!info) return XTDB_ERR_INVALID_PARAMETER;

    xtdb_handle_impl* impl = static_cast<xtdb_handle_impl*>(handle);
    std::lock_guard<std::mutex> lock(impl->mutex);

    if (!impl->engine) return XTDB_ERR_INVALID_HANDLE;

    const auto& containers = impl->engine->getContainers();
    if (index >= containers.size()) {
        return XTDB_ERR_INVALID_PARAMETER;
    }

    const auto& c = containers[index];
    info->container_id = c.container_id;
    info->file_path = c.file_path.c_str();
    info->capacity_bytes = c.capacity_bytes;

    return XTDB_SUCCESS;
}
