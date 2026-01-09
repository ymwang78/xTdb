# Block Device Container Testing Guide

## Overview

Block device container testing supports two modes:

1. **Test Mode (Default)** - Uses regular files to simulate block devices
2. **Real Device Mode** - Uses actual block devices (requires root/sudo)

## Test Mode (Simulated File)

This is the **default mode** when no block device is provided. The tests automatically create a 512MB file to simulate a block device.

### Running Tests

```bash
# Build the tests
cmake --build build --target test_block_device_advanced

# Run tests (uses simulated file automatically)
./build/test_block_device_advanced
```

### Output Example

```
[Setup] No XTDB_TEST_DEVICE provided. Using simulated file.
[Setup] Creating 512MB test file: /tmp/test_block_device_advanced/simulated_block_device.img
[Setup] Test file created successfully (512 MB)
[Setup] Running in TEST MODE (no real block device required)
```

### Advantages

- **No root required** - Can run as regular user
- **Portable** - Works on any system with sufficient disk space
- **Safe** - No risk of damaging actual hardware
- **Fast** - File operations are typically faster than device I/O

## Real Device Mode (Optional)

To test with actual block devices, use the `XTDB_TEST_DEVICE` environment variable.

### Using Loop Devices (Recommended for Testing)

```bash
# 1. Create a loop device (requires sudo)
sudo ./tests/setup_loop_device.sh [size_mb]

# Example output:
# Loop device created: /dev/loop0
# Run tests with:
#   XTDB_TEST_DEVICE=/dev/loop0 ./test_block_device_advanced

# 2. Run tests with the loop device
XTDB_TEST_DEVICE=/dev/loop0 ./build/test_block_device_advanced

# 3. Cleanup after testing
sudo losetup -d /dev/loop0
rm /tmp/xTdb_test_device.img
```

### Using Real Block Devices (Advanced)

⚠️ **WARNING**: Using real block devices will **DESTROY ALL DATA** on the device!

```bash
# Identify available devices
lsblk

# Run tests (DESTRUCTIVE - will erase all data!)
sudo XTDB_TEST_DEVICE=/dev/sdX ./build/test_block_device_advanced
```

## Implementation Details

### Test Mode Features

When `test_mode=true` is enabled in `BlockDeviceContainer`:

1. **Block Device Check Bypass** - Skips the `S_ISBLK()` check that verifies if a path is a block device
2. **Regular File Support** - Uses `fstat()` to get file size instead of `ioctl(BLKGETSIZE64)`
3. **No O_DIRECT** - Disables Direct I/O for compatibility with regular files
4. **Same API** - All container operations work identically to real block devices

### Test Coverage

The `test_block_device_advanced` test suite includes:

1. **BlockDeviceContainer Basic Operations**
   - Container open/close
   - Read/write operations
   - Data verification
   - AlignedIO interface

2. **StorageEngine Integration**
   - Engine initialization with block device
   - Point writing and flushing
   - Query operations
   - Statistics tracking

3. **Data Persistence**
   - Write data and close
   - Reopen and verify data integrity
   - Tests crash recovery scenarios

4. **Performance Comparison**
   - Compare simulated file vs regular file-based storage
   - Throughput measurements
   - Performance ratio analysis

## Configuration in Your Application

To use test mode in your own code:

```cpp
// Option 1: Direct BlockDeviceContainer usage
ChunkLayout layout;
layout.block_size_bytes = 16384;
layout.chunk_size_bytes = 16 * 1024 * 1024;

auto container = std::make_unique<BlockDeviceContainer>(
    "/path/to/file",    // Regular file path
    layout,
    false,              // read_only
    true                // test_mode = true
);

// Option 2: Via StorageEngine
EngineConfig config;
config.container_type = ContainerType::BLOCK_DEVICE;
config.block_device_path = "/path/to/file";
config.block_device_test_mode = true;  // Enable test mode

StorageEngine engine(config);
```

## Best Practices

1. **CI/CD Pipelines** - Use test mode for automated testing (no sudo required)
2. **Development** - Use test mode for rapid iteration
3. **Pre-Production Validation** - Use loop devices for realistic testing
4. **Production** - Use real block devices (test_mode=false)

## Troubleshooting

### "Not a block device" Error

If you see this error, ensure test mode is enabled:
- For direct usage: Set `test_mode=true` in constructor
- For StorageEngine: Set `config.block_device_test_mode = true`

### Permission Denied

- **Test Mode**: Check file system permissions on the directory
- **Real Device**: Requires root/sudo access

### File Too Small

Ensure the simulated file is at least as large as one chunk (default: 16MB).

## Summary

- **Default behavior**: Automatically uses simulated files (no configuration needed)
- **Test mode**: Fully functional, safe, and portable
- **Real devices**: Optional, for production-like testing
- **All features**: Work identically in both modes
