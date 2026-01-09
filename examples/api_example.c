/**
 * xTdb C API Usage Example
 *
 * This example demonstrates how to use the xTdb C API for:
 * - Opening and closing a database
 * - Configuring tags with different encodings (RAW, SWINGING_DOOR, QUANTIZED_16)
 * - Writing data points with tag configurations
 * - Querying data
 * - Running maintenance operations
 * - Getting statistics
 */

#include "xTdb/xtdb_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/// Get current time in microseconds
static int64_t get_current_time_us(void) {
#ifdef _WIN32
    // Windows implementation using GetSystemTimeAsFileTime
    FILETIME ft;
    ULARGE_INTEGER uli;
    GetSystemTimeAsFileTime(&ft);
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // FileTime is in 100-nanosecond intervals since January 1, 1601
    // Convert to microseconds since Unix epoch (January 1, 1970)
    int64_t us = (int64_t)(uli.QuadPart / 10LL) - 11644473600000000LL;
    return us;
#else
    // POSIX implementation
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)ts.tv_nsec / 1000LL;
#endif
}

int main(void) {
    printf("=== xTdb C API Example ===\n\n");
    printf("Library version: %s\n\n", xtdb_version());

    // Step 1: Configure database
    printf("Step 1: Configuring database...\n");
    xtdb_config_t config;
    xtdb_config_init(&config);
    config.data_dir = "./example_data";
    config.db_path = "./example_data/meta.db";
    config.retention_days = 30;  // 30 days retention
    printf("  Data directory: %s\n", config.data_dir);
    printf("  Retention: %d days\n", config.retention_days);
    printf("\n");

    // Step 2: Open database
    printf("Step 2: Opening database...\n");
    xtdb_handle_t db = NULL;
    xtdb_error_t err = xtdb_open(&config, &db);
    if (err != XTDB_SUCCESS) {
        fprintf(stderr, "Failed to open database: %s\n", xtdb_error_string(err));
        return 1;
    }
    printf("  Database opened successfully\n");
    printf("\n");

    // Step 3: Write some data points
    printf("Step 3: Writing data points...\n");
    int64_t base_time = get_current_time_us();

    // Configure tag 1001: Temperature sensor with RAW encoding
    xtdb_tag_config_t tag1001_config;
    xtdb_tag_config_init(&tag1001_config, 1001);
    tag1001_config.tag_name = "Temperature_Sensor_01";
    tag1001_config.value_type = XTDB_VT_F64;
    tag1001_config.time_unit = XTDB_TU_MS;
    tag1001_config.encoding_type = XTDB_ENC_RAW;
    printf("  Tag 1001 (%s): RAW encoding, F64, 1ms resolution\n", tag1001_config.tag_name);

    // Write single points with tag configuration
    for (int i = 0; i < 10; i++) {
        xtdb_point_t point;
        point.tag_config = &tag1001_config;
        point.timestamp_us = base_time + i * 1000000LL;  // 1 second intervals
        point.value = 20.0 + i * 0.5;  // Temperature: 20.0 to 24.5
        point.quality = 192;  // GOOD quality

        err = xtdb_write_point(db, &point);
        if (err != XTDB_SUCCESS) {
            fprintf(stderr, "Failed to write point: %s\n", xtdb_error_string(err));
        }
    }
    printf("  Wrote 10 points for tag 1001\n");

    // Configure tag 1002: Pressure sensor with SWINGING_DOOR compression
    xtdb_tag_config_t tag1002_config;
    xtdb_tag_config_init(&tag1002_config, 1002);
    tag1002_config.tag_name = "Pressure_Sensor_01";
    tag1002_config.value_type = XTDB_VT_F64;
    tag1002_config.time_unit = XTDB_TU_MS;
    tag1002_config.encoding_type = XTDB_ENC_SWINGING_DOOR;
    tag1002_config.encoding_param1 = 0.5;  // tolerance = 0.5
    tag1002_config.encoding_param2 = 1.0;  // compression_factor = 1.0 (typical range: 0.1-2.0)
    printf("  Tag 1002 (%s): SWINGING_DOOR (tolerance=%.1f, factor=%.1f), F64, 1ms\n",
           tag1002_config.tag_name, tag1002_config.encoding_param1, tag1002_config.encoding_param2);

    // Write batch of points with compression
    xtdb_point_t batch[5];
    for (int i = 0; i < 5; i++) {
        batch[i].tag_config = &tag1002_config;
        batch[i].timestamp_us = base_time + i * 1000000LL;
        batch[i].value = 50.0 + i * 2.0;  // Pressure: 50.0 to 58.0
        batch[i].quality = 192;
    }
    err = xtdb_write_points(db, batch, 5);
    if (err != XTDB_SUCCESS) {
        fprintf(stderr, "Failed to write batch: %s\n", xtdb_error_string(err));
    }
    printf("  Wrote 5 points for tag 1002 (batch)\n");

    // Configure tag 1003: Flow meter with QUANTIZED_16 compression
    xtdb_tag_config_t tag1003_config;
    xtdb_tag_config_init(&tag1003_config, 1003);
    tag1003_config.tag_name = "Flow_Meter_01";
    tag1003_config.value_type = XTDB_VT_F32;
    tag1003_config.time_unit = XTDB_TU_100MS;
    tag1003_config.encoding_type = XTDB_ENC_QUANTIZED_16;
    tag1003_config.encoding_param1 = 0.0;    // low_extreme
    tag1003_config.encoding_param2 = 100.0;  // high_extreme
    printf("  Tag 1003 (%s): QUANTIZED_16 [%.1f, %.1f], F32, 100ms\n",
           tag1003_config.tag_name, tag1003_config.encoding_param1, tag1003_config.encoding_param2);

    // Write some points for tag 1003
    for (int i = 0; i < 8; i++) {
        xtdb_point_t point;
        point.tag_config = &tag1003_config;
        point.timestamp_us = base_time + i * 1000000LL;
        point.value = 25.0 + i * 5.0;  // Flow: 25.0 to 60.0
        point.quality = 192;

        err = xtdb_write_point(db, &point);
        if (err != XTDB_SUCCESS) {
            fprintf(stderr, "Failed to write point: %s\n", xtdb_error_string(err));
        }
    }
    printf("  Wrote 8 points for tag 1003\n");
    printf("\n");

    // Step 4: Flush to disk
    printf("Step 4: Flushing data to disk...\n");
    err = xtdb_flush(db);
    if (err != XTDB_SUCCESS) {
        fprintf(stderr, "Failed to flush: %s\n", xtdb_error_string(err));
    } else {
        printf("  Flush successful\n");
    }
    printf("\n");

    // Step 5: Query data
    printf("Step 5: Querying data...\n");
    xtdb_result_set_t result = NULL;
    err = xtdb_query_points(db, 1001, base_time, base_time + 10000000LL, &result);
    if (err != XTDB_SUCCESS) {
        fprintf(stderr, "Failed to query: %s\n", xtdb_error_string(err));
    } else {
        size_t count = xtdb_result_count(result);
        printf("  Query returned %zu points:\n", count);

        for (size_t i = 0; i < count && i < 5; i++) {
            xtdb_point_t point;
            if (xtdb_result_get(result, i, &point) == XTDB_SUCCESS) {
                printf("    [%zu] timestamp=%lld, value=%.2f, quality=%u\n",
                       i, (long long)point.timestamp_us, point.value, point.quality);
            }
        }
        if (count > 5) {
            printf("    ... (%zu more points)\n", count - 5);
        }

        xtdb_result_free(result);
    }
    printf("\n");

    // Step 6: Get write statistics
    printf("Step 6: Getting statistics...\n");
    xtdb_write_stats_t write_stats;
    err = xtdb_get_write_stats(db, &write_stats);
    if (err == XTDB_SUCCESS) {
        printf("  Write Statistics:\n");
        printf("    Points written: %llu\n", (unsigned long long)write_stats.points_written);
        printf("    Blocks flushed: %llu\n", (unsigned long long)write_stats.blocks_flushed);
        printf("    Chunks sealed: %llu\n", (unsigned long long)write_stats.chunks_sealed);
        printf("    Chunks allocated: %llu\n", (unsigned long long)write_stats.chunks_allocated);
    }

    xtdb_read_stats_t read_stats;
    err = xtdb_get_read_stats(db, &read_stats);
    if (err == XTDB_SUCCESS) {
        printf("  Read Statistics:\n");
        printf("    Queries executed: %llu\n", (unsigned long long)read_stats.queries_executed);
        printf("    Blocks read: %llu\n", (unsigned long long)read_stats.blocks_read);
        printf("    Points read (disk): %llu\n", (unsigned long long)read_stats.points_read_disk);
        printf("    Points read (memory): %llu\n", (unsigned long long)read_stats.points_read_memory);
    }
    printf("\n");

    // Step 7: Container information
    printf("Step 7: Container information...\n");
    size_t container_count = xtdb_get_container_count(db);
    printf("  Total containers: %zu\n", container_count);
    for (size_t i = 0; i < container_count; i++) {
        xtdb_container_info_t info;
        if (xtdb_get_container_info(db, i, &info) == XTDB_SUCCESS) {
            printf("    Container %zu: ID=%u, path=%s, capacity=%llu bytes\n",
                   i, info.container_id, info.file_path,
                   (unsigned long long)info.capacity_bytes);
        }
    }
    printf("\n");

    // Step 8: Maintenance operations
    printf("Step 8: Running maintenance...\n");

    // Seal current chunk
    err = xtdb_seal_chunk(db);
    if (err == XTDB_SUCCESS) {
        printf("  Sealed current chunk\n");
    }

    // Run retention (with custom time for testing)
    err = xtdb_run_retention(db, get_current_time_us());
    if (err == XTDB_SUCCESS) {
        printf("  Retention service completed\n");
    }

    // Reclaim space
    err = xtdb_reclaim_space(db);
    if (err == XTDB_SUCCESS) {
        printf("  Space reclamation completed\n");
    }

    xtdb_maintenance_stats_t maint_stats;
    err = xtdb_get_maintenance_stats(db, &maint_stats);
    if (err == XTDB_SUCCESS) {
        printf("  Maintenance Statistics:\n");
        printf("    Chunks deprecated: %llu\n", (unsigned long long)maint_stats.chunks_deprecated);
        printf("    Chunks freed: %llu\n", (unsigned long long)maint_stats.chunks_freed);
    }
    printf("\n");

    // Step 9: Close database
    printf("Step 9: Closing database...\n");
    xtdb_close(db);
    printf("  Database closed\n");
    printf("\n");

    printf("=== Example completed successfully ===\n");
    return 0;
}
