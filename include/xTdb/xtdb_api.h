#ifndef XTDB_API_H_
#define XTDB_API_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Version Information
// ============================================================================

#define XTDB_VERSION_MAJOR 1
#define XTDB_VERSION_MINOR 0
#define XTDB_VERSION_PATCH 0

/// Get xTdb version string
/// @return Version string in format "major.minor.patch"
const char* xtdb_version(void);

// ============================================================================
// Error Codes
// ============================================================================

typedef enum {
    XTDB_SUCCESS = 0,
    XTDB_ERR_INVALID_PATH = 1,
    XTDB_ERR_CONTAINER_OPEN_FAILED = 2,
    XTDB_ERR_CONTAINER_HEADER_INVALID = 3,
    XTDB_ERR_METADATA_OPEN_FAILED = 4,
    XTDB_ERR_WAL_OPEN_FAILED = 5,
    XTDB_ERR_CHUNK_ALLOCATION_FAILED = 6,
    XTDB_ERR_STATE_RESTORATION_FAILED = 7,
    XTDB_ERR_WAL_REPLAY_FAILED = 8,
    XTDB_ERR_ENGINE_NOT_OPEN = 9,
    XTDB_ERR_INVALID_DATA = 10,
    XTDB_ERR_INVALID_HANDLE = 11,
    XTDB_ERR_OUT_OF_MEMORY = 12,
    XTDB_ERR_INVALID_PARAMETER = 13,
    XTDB_ERR_QUERY_FAILED = 14,
    XTDB_ERR_WRITE_FAILED = 15,
    XTDB_ERR_FLUSH_FAILED = 16
} xtdb_error_t;

/// Get error message for error code
/// @param error Error code
/// @return Human-readable error message
const char* xtdb_error_string(xtdb_error_t error);

// ============================================================================
// Opaque Handle Types
// ============================================================================

/// Opaque handle to xTdb storage engine instance
typedef struct xtdb_handle_impl* xtdb_handle_t;

// ============================================================================
// Configuration
// ============================================================================

/// Storage engine configuration
typedef struct {
    const char* data_dir;           ///< Data directory path
    const char* db_path;            ///< SQLite database path
    uint32_t block_size_bytes;      ///< Block size (default: 16384)
    uint64_t chunk_size_bytes;      ///< Chunk size (default: 256MB)
    int32_t retention_days;         ///< Data retention in days (0 = no limit)
} xtdb_config_t;

/// Initialize configuration with default values
/// @param config Configuration structure to initialize
void xtdb_config_init(xtdb_config_t* config);

// ============================================================================
// Lifecycle Management
// ============================================================================

/// Open xTdb storage engine
/// @param config Configuration (NULL for defaults)
/// @param handle Output: handle to opened storage engine
/// @return Error code
xtdb_error_t xtdb_open(const xtdb_config_t* config, xtdb_handle_t* handle);

/// Close xTdb storage engine and free resources
/// @param handle Handle to storage engine (can be NULL)
void xtdb_close(xtdb_handle_t handle);

/// Check if storage engine is open
/// @param handle Handle to storage engine
/// @return 1 if open, 0 otherwise
int xtdb_is_open(xtdb_handle_t handle);

/// Get last error message for this handle
/// @param handle Handle to storage engine
/// @return Error message string (valid until next API call on this handle)
const char* xtdb_get_last_error(xtdb_handle_t handle);

// ============================================================================
// Tag Configuration
// ============================================================================

/// Value type enumeration
typedef enum {
    XTDB_VT_BOOL = 0,   ///< Boolean
    XTDB_VT_I32  = 1,   ///< 32-bit integer
    XTDB_VT_F32  = 2,   ///< 32-bit float
    XTDB_VT_F64  = 3    ///< 64-bit float (default)
} xtdb_value_type_t;

/// Time unit enumeration
typedef enum {
    XTDB_TU_100MS = 0,  ///< 100 milliseconds
    XTDB_TU_10MS  = 1,  ///< 10 milliseconds
    XTDB_TU_MS    = 2,  ///< 1 millisecond (default)
    XTDB_TU_100US = 3,  ///< 100 microseconds
    XTDB_TU_10US  = 4,  ///< 10 microseconds
    XTDB_TU_US    = 5   ///< 1 microsecond
} xtdb_time_unit_t;

/// Encoding type enumeration
typedef enum {
    XTDB_ENC_RAW            = 0,  ///< No compression (default)
    XTDB_ENC_SWINGING_DOOR  = 1,  ///< Swinging Door compression
    XTDB_ENC_QUANTIZED_16   = 2,  ///< 16-bit quantization
    XTDB_ENC_GORILLA        = 3,  ///< Gorilla/XOR compression
    XTDB_ENC_DELTA_OF_DELTA = 4   ///< Delta-of-Delta compression
} xtdb_encoding_type_t;

/// Tag configuration (provided by upper application layer)
/// This configuration is used temporarily in xTdb Core for compression decisions
/// and debug logging. It is NOT persisted by xTdb Core.
typedef struct {
    uint32_t tag_id;                ///< Tag identifier
    const char* tag_name;           ///< Tag name (optional, for debug logging, can be NULL)
    xtdb_value_type_t value_type;   ///< Value type
    xtdb_time_unit_t time_unit;     ///< Time unit for compression
    xtdb_encoding_type_t encoding_type;  ///< Encoding/compression method
    double encoding_param1;         ///< Encoding parameter 1 (meaning depends on encoding_type)
    double encoding_param2;         ///< Encoding parameter 2 (meaning depends on encoding_type)
} xtdb_tag_config_t;

/// Initialize tag configuration with default values
/// @param config Configuration structure to initialize
/// @param tag_id Tag identifier
void xtdb_tag_config_init(xtdb_tag_config_t* config, uint32_t tag_id);

// ============================================================================
// Data Point Structure
// ============================================================================

/// Data point for write/read operations
typedef struct {
    const xtdb_tag_config_t* tag_config;  ///< Tag configuration (provided by application)
    int64_t timestamp_us;                 ///< Timestamp in microseconds (PostgreSQL epoch)
    double value;                         ///< Value (float64)
    uint8_t quality;                      ///< Quality byte (default: 192 = GOOD)
} xtdb_point_t;

// ============================================================================
// Write Operations
// ============================================================================

/// Write a single data point
/// @param handle Handle to storage engine
/// @param point Data point to write
/// @return Error code
xtdb_error_t xtdb_write_point(xtdb_handle_t handle, const xtdb_point_t* point);

/// Write multiple data points (batch write)
/// @param handle Handle to storage engine
/// @param points Array of data points
/// @param count Number of points
/// @return Error code
xtdb_error_t xtdb_write_points(xtdb_handle_t handle,
                               const xtdb_point_t* points,
                               size_t count);

/// Flush buffers to disk
/// @param handle Handle to storage engine
/// @return Error code
xtdb_error_t xtdb_flush(xtdb_handle_t handle);

// ============================================================================
// Read Operations
// ============================================================================

/// Query result set (opaque)
typedef struct xtdb_result_set_impl* xtdb_result_set_t;

/// Query data points by tag and time range
/// @param handle Handle to storage engine
/// @param tag_id Tag identifier
/// @param start_ts_us Start timestamp (microseconds, inclusive)
/// @param end_ts_us End timestamp (microseconds, inclusive)
/// @param result_set Output: result set handle
/// @return Error code
xtdb_error_t xtdb_query_points(xtdb_handle_t handle,
                               uint32_t tag_id,
                               int64_t start_ts_us,
                               int64_t end_ts_us,
                               xtdb_result_set_t* result_set);

/// Get number of points in result set
/// @param result_set Result set handle
/// @return Number of points (0 if invalid handle)
size_t xtdb_result_count(xtdb_result_set_t result_set);

/// Get data point from result set by index
/// @param result_set Result set handle
/// @param index Point index (0-based)
/// @param point Output: data point
/// @return Error code
xtdb_error_t xtdb_result_get(xtdb_result_set_t result_set,
                             size_t index,
                             xtdb_point_t* point);

/// Free result set
/// @param result_set Result set handle (can be NULL)
void xtdb_result_free(xtdb_result_set_t result_set);

// ============================================================================
// Maintenance Operations
// ============================================================================

/// Run retention service to clean up old data
/// @param handle Handle to storage engine
/// @param current_time_us Current time in microseconds (0 = use system time)
/// @return Error code
xtdb_error_t xtdb_run_retention(xtdb_handle_t handle, int64_t current_time_us);

/// Reclaim space from deprecated chunks
/// @param handle Handle to storage engine
/// @return Error code
xtdb_error_t xtdb_reclaim_space(xtdb_handle_t handle);

/// Seal current active chunk
/// @param handle Handle to storage engine
/// @return Error code
xtdb_error_t xtdb_seal_chunk(xtdb_handle_t handle);

// ============================================================================
// Statistics
// ============================================================================

/// Write statistics
typedef struct {
    uint64_t points_written;    ///< Total points written
    uint64_t blocks_flushed;    ///< Total blocks flushed
    uint64_t chunks_sealed;     ///< Total chunks sealed
    uint64_t chunks_allocated;  ///< Total chunks allocated
} xtdb_write_stats_t;

/// Get write statistics
/// @param handle Handle to storage engine
/// @param stats Output: write statistics
/// @return Error code
xtdb_error_t xtdb_get_write_stats(xtdb_handle_t handle, xtdb_write_stats_t* stats);

/// Read statistics
typedef struct {
    uint64_t queries_executed;   ///< Total queries executed
    uint64_t blocks_read;        ///< Total blocks read from disk
    uint64_t points_read_disk;   ///< Total points read from disk
    uint64_t points_read_memory; ///< Total points read from memory
} xtdb_read_stats_t;

/// Get read statistics
/// @param handle Handle to storage engine
/// @param stats Output: read statistics
/// @return Error code
xtdb_error_t xtdb_get_read_stats(xtdb_handle_t handle, xtdb_read_stats_t* stats);

/// Maintenance statistics
typedef struct {
    uint64_t chunks_deprecated;      ///< Chunks marked as deprecated
    uint64_t chunks_freed;           ///< Chunks marked as free
    uint64_t last_retention_run_ts;  ///< Last retention run timestamp
} xtdb_maintenance_stats_t;

/// Get maintenance statistics
/// @param handle Handle to storage engine
/// @param stats Output: maintenance statistics
/// @return Error code
xtdb_error_t xtdb_get_maintenance_stats(xtdb_handle_t handle,
                                        xtdb_maintenance_stats_t* stats);

// ============================================================================
// Container Information
// ============================================================================

/// Container information
typedef struct {
    uint32_t container_id;      ///< Container ID
    const char* file_path;      ///< Container file path
    uint64_t capacity_bytes;    ///< Container capacity in bytes
} xtdb_container_info_t;

/// Get number of mounted containers
/// @param handle Handle to storage engine
/// @return Number of containers (0 if invalid handle)
size_t xtdb_get_container_count(xtdb_handle_t handle);

/// Get container information by index
/// @param handle Handle to storage engine
/// @param index Container index (0-based)
/// @param info Output: container information
/// @return Error code
xtdb_error_t xtdb_get_container_info(xtdb_handle_t handle,
                                     size_t index,
                                     xtdb_container_info_t* info);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XTDB_API_H_
