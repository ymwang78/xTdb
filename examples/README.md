# xTdb API Examples

This directory contains example programs demonstrating how to use the xTdb C API.

## Available Examples

### 1. `api_example.c` - Basic C API Usage

Comprehensive demonstration of the xTdb C API covering:
- Database configuration and initialization
- Opening and closing databases
- Writing single data points
- Batch writing multiple points
- Flushing data to disk
- Querying data with result sets
- Retrieving statistics (write, read, maintenance)
- Getting container information
- Running maintenance operations (seal, retention, reclamation)

## Building Examples

### Prerequisites

- CMake 3.14 or higher
- C compiler (GCC, Clang, or MSVC)
- SQLite3 development libraries
- Google Test (for running tests)

### Build Instructions

```bash
# From the xTdb root directory
mkdir -p build
cd build
cmake ..
make api_example
```

The executable will be created at `build/api_example`.

## Running Examples

### Basic Usage

```bash
# Run the API example
./api_example
```

The example will:
1. Create a database in `./example_data/`
2. Write sample data points
3. Query the data
4. Display statistics
5. Clean up and close

### Expected Output

```
=== xTdb C API Example ===

Library version: 1.0.0

Step 1: Configuring database...
  Data directory: ./example_data
  Retention: 30 days

Step 2: Opening database...
  Database opened successfully

Step 3: Writing data points...
  Wrote 10 points for tag 1001
  Wrote 5 points for tag 1002 (batch)

...

=== Example completed successfully ===
```

## Example Code Patterns

### Opening a Database

```c
#include "xTdb/xtdb_api.h"

xtdb_config_t config;
xtdb_config_init(&config);
config.data_dir = "./my_data";
config.retention_days = 30;

xtdb_handle_t db = NULL;
xtdb_error_t err = xtdb_open(&config, &db);
if (err != XTDB_SUCCESS) {
    fprintf(stderr, "Failed to open: %s\n", xtdb_error_string(err));
    return 1;
}

// Use database...

xtdb_close(db);
```

### Writing Data

```c
// Single point
xtdb_point_t point = {
    .tag_id = 1001,
    .timestamp_us = get_current_time_us(),
    .value = 25.5,
    .quality = 192
};
xtdb_write_point(db, &point);

// Batch write
xtdb_point_t batch[100];
// ... fill batch ...
xtdb_write_points(db, batch, 100);

// Flush to disk
xtdb_flush(db);
```

### Querying Data

```c
xtdb_result_set_t result;
xtdb_error_t err = xtdb_query_points(db, tag_id, start_time, end_time, &result);

if (err == XTDB_SUCCESS) {
    size_t count = xtdb_result_count(result);

    for (size_t i = 0; i < count; i++) {
        xtdb_point_t point;
        xtdb_result_get(result, i, &point);
        printf("Time: %lld, Value: %.2f\n", point.timestamp_us, point.value);
    }

    xtdb_result_free(result);
}
```

### Error Handling

```c
xtdb_error_t err = xtdb_write_point(db, &point);
if (err != XTDB_SUCCESS) {
    fprintf(stderr, "Write failed: %s\n", xtdb_error_string(err));
    fprintf(stderr, "Details: %s\n", xtdb_get_last_error(db));
    // Handle error...
}
```

## Integration with Your Application

### 1. Link Against xTdb

**CMake**:
```cmake
find_library(XTDB_API_LIB xtdb_api)
target_link_libraries(your_app ${XTDB_API_LIB})
```

**GCC/Clang**:
```bash
gcc your_app.c -lxtdb_api -lxtdb_core -lsqlite3 -lstdc++ -lpthread -o your_app
```

### 2. Include Header

```c
#include <xTdb/xtdb_api.h>
```

### 3. Use the API

See `api_example.c` for comprehensive usage patterns.

## Thread Safety

All API functions are thread-safe when used with different handles:

```c
// Safe: Different handles
xtdb_handle_t db1, db2;
xtdb_open(&config1, &db1);  // Thread 1
xtdb_open(&config2, &db2);  // Thread 2

// NOT safe: Same handle from multiple threads
// Use external synchronization if sharing handles
```

## Performance Tips

1. **Batch Writes**: Use `xtdb_write_points()` instead of multiple `xtdb_write_point()` calls
2. **Flush Control**: Call `xtdb_flush()` periodically instead of after every write
3. **Query Optimization**: Use specific time ranges to minimize result set size
4. **Resource Cleanup**: Always call `xtdb_result_free()` to free query results

## Troubleshooting

### Database Won't Open

- Check that the data directory exists and is writable
- Verify SQLite3 is installed
- Check file permissions

### Write Failures

- Ensure enough disk space
- Verify timestamp values are valid (microseconds since epoch)
- Check that database is open (`xtdb_is_open()`)

### Query Returns No Data

- Verify time range covers the data
- Check that data was flushed (`xtdb_flush()`)
- Confirm correct tag_id

## Further Resources

- **API Reference**: See `include/xTdb/xtdb_api.h` for detailed API documentation
- **Phase 11 Summary**: See `docs/phase11_summary.md` for design details
- **Design Document**: See `docs/design.md` for architecture overview

## Contributing Examples

If you have useful example code, please contribute:

1. Create a new `.c` file in this directory
2. Add a CMake target in `CMakeLists.txt`
3. Update this README
4. Submit a pull request

## License

Examples are provided under the same license as xTdb core library.
