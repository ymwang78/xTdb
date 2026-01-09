#include "xTdb/rotating_wal.h"
#include "xTdb/platform_compat.h"
#include <iostream>
#include <cassert>
#include <cstring>
#include <chrono>

using namespace xtdb;

// Test utilities
void cleanup_test_files() {
    unlink_file("./test_wal_container.raw");
    unlink_file("./test_wal_rotation.raw");
    unlink_file("./test_wal_clearreuse.raw");
    unlink_file("./test_wal_usage.raw");
    unlink_file("./test_wal_perf.raw");
}

void assert_success(RotatingWALResult result, const char* context) {
    if (result != RotatingWALResult::SUCCESS) {
        std::cerr << "FAILED: " << context << std::endl;
        exit(1);
    }
}

// Test 1: Basic initialization and open
void test_basic_initialization() {
    std::cout << "\n=== Test 1: Basic Initialization ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_container.raw";
    config.num_segments = 4;
    config.segment_size_bytes = 64 * 1024 * 1024;  // 64 MB
    config.auto_grow = false;

    RotatingWAL wal(config);

    // Open (create new)
    RotatingWALResult result = wal.open();
    assert_success(result, "Open WAL");

    assert(wal.isOpen());
    assert(wal.getSegments().size() == 4);
    assert(wal.getCurrentSegmentId() == 0);

    // Check file size
    struct stat st = {};
    stat(config.wal_container_path.c_str(), &st);
    uint64_t expected_size = 16 * 1024 +  // Header
                             (4 * 64 * 1024 * 1024);  // 4 segments × 64 MB
    std::cout << "Container size: " << st.st_size << " bytes (expected: " << expected_size << ")" << std::endl;

    wal.close();

    // Reopen existing container
    RotatingWAL wal2(config);
    result = wal2.open();
    assert_success(result, "Reopen WAL");

    assert(wal2.getSegments().size() == 4);

    wal2.close();

    std::cout << "✓ Basic initialization PASSED" << std::endl;
}

// Test 2: Write entries
void test_write_entries() {
    std::cout << "\n=== Test 2: Write Entries ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_container.raw";
    config.num_segments = 4;
    config.segment_size_bytes = 1024 * 1024;  // 1 MB per segment (small for testing)

    RotatingWAL wal(config);
    assert_success(wal.open(), "Open WAL");

    // Write 1000 entries
    int64_t start_ts = 1000000;
    for (int i = 0; i < 1000; i++) {
        WALEntry entry;
        entry.tag_id = 1 + (i % 10);  // 10 different tags
        entry.timestamp_us = start_ts + (i * 1000);
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 192;
        entry.value.f64_value = 100.0 + i;

        RotatingWALResult result = wal.append(entry);
        assert_success(result, "Append entry");
    }

    // Sync
    assert_success(wal.sync(), "Sync WAL");

    // Check statistics
    const RotatingWALStats& stats = wal.getStats();
    std::cout << "Entries written: " << stats.total_entries_written << std::endl;
    std::cout << "Bytes written: " << stats.total_bytes_written << std::endl;
    std::cout << "Sync operations: " << stats.sync_operations << std::endl;

    assert(stats.total_entries_written == 1000);
    assert(stats.total_bytes_written == 1000 * sizeof(WALEntry));

    // Check current segment
    const WALSegment& segment = wal.getSegment(wal.getCurrentSegmentId());
    std::cout << "Current segment: " << segment.segment_id << std::endl;
    std::cout << "Segment entries: " << segment.entry_count << std::endl;
    std::cout << "Segment tag_ids: " << segment.tag_ids.size() << std::endl;

    assert(segment.entry_count == 1000);
    assert(segment.tag_ids.size() == 10);

    wal.close();

    std::cout << "✓ Write entries PASSED" << std::endl;
}

// Test 3: Segment rotation
void test_segment_rotation() {
    std::cout << "\n=== Test 3: Segment Rotation ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_rotation.raw";  // Different file
    config.num_segments = 4;
    config.segment_size_bytes = 256 * 1024;  // 256 KB per segment (very small for testing)

    RotatingWAL wal(config);
    assert_success(wal.open(), "Open WAL");

    // Register flush callback
    int flush_count = 0;
    uint32_t last_flushed_segment = 0;
    std::set<uint32_t> last_flushed_tags;

    wal.setFlushCallback([&](uint32_t segment_id, const std::set<uint32_t>& tag_ids) {
        (void)tag_ids;  // Suppress unused parameter warning
        flush_count++;
        last_flushed_segment = segment_id;
        last_flushed_tags = tag_ids;
        std::cout << "Flush callback: segment " << segment_id
                  << ", tags: " << tag_ids.size() << std::endl;

        // In real system, this would flush buffers for these tags
        // For now, we just acknowledge
        return true;
    });

    // Write enough entries to trigger multiple rotations
    // 256 KB / 24 bytes = ~10,922 entries per segment
    // Let's write 15,000 entries to trigger at least one rotation
    int64_t start_ts = 1000000;
    int entries_written = 0;
    int expected_entries = 15000;

    for (int i = 0; i < expected_entries; i++) {
        WALEntry entry;
        entry.tag_id = 1 + (i % 100);  // 100 different tags
        entry.timestamp_us = start_ts + (i * 1000);
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 192;
        entry.value.f64_value = 100.0 + i;

        RotatingWALResult result = wal.append(entry);
        if (result == RotatingWALResult::SUCCESS) {
            entries_written++;
        } else if (result == RotatingWALResult::ERR_SEGMENT_NOT_CLEARED) {
            std::cout << "Segment not cleared (expected), stopping" << std::endl;
            break;
        } else {
            std::cerr << "Unexpected error: " << wal.getLastError() << std::endl;
            assert(false);
        }
    }

    // Check statistics
    const RotatingWALStats& stats = wal.getStats();
    std::cout << "Entries written: " << stats.total_entries_written << std::endl;
    std::cout << "Segment rotations: " << stats.segment_rotations << std::endl;
    std::cout << "Segment flushes: " << stats.segment_flushes << std::endl;
    std::cout << "Flush callback count: " << flush_count << std::endl;

    // We should have at least one rotation
    assert(stats.segment_rotations >= 1);
    assert(flush_count >= 1);

    // Check last flushed segment
    std::cout << "Last flushed segment: " << last_flushed_segment << std::endl;
    std::cout << "Last flushed tags: " << last_flushed_tags.size() << std::endl;

    wal.close();

    std::cout << "✓ Segment rotation PASSED" << std::endl;
}

// Test 4: Clear segment and reuse
void test_clear_and_reuse() {
    std::cout << "\n=== Test 4: Clear Segment and Reuse ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_clearreuse.raw";  // Different file
    config.num_segments = 4;
    config.segment_size_bytes = 256 * 1024;  // 256 KB

    RotatingWAL wal(config);
    assert_success(wal.open(), "Open WAL");

    // Print initial segment states
    std::cout << "Initial segment states:" << std::endl;
    for (size_t s = 0; s < wal.getSegments().size(); s++) {
        const WALSegment& seg = wal.getSegment(s);
        std::cout << "  Segment " << s << ": entries=" << seg.entry_count
                  << ", write_pos=" << seg.write_position
                  << ", avail=" << seg.getAvailableSpace() << std::endl;
    }

    // Flush callback that clears the segment
    wal.setFlushCallback([&](uint32_t segment_id, const std::set<uint32_t>& tag_ids) {
        (void)tag_ids;  // Suppress unused parameter warning
        std::cout << "Flush callback: clearing segment " << segment_id << std::endl;
        // Clear the segment immediately
        RotatingWALResult result = wal.clearSegment(segment_id);
        if (result != RotatingWALResult::SUCCESS) {
            std::cerr << "Failed to clear segment: " << wal.getLastError() << std::endl;
            return false;
        }
        return true;
    });

    // Write entries to fill multiple segments and trigger rotation
    int64_t start_ts = 1000000;
    int entries_per_batch = 11000;  // Enough to fill one segment

    // Only test 5 batches: 0, 1, 2, 3, 4 - this should complete a full rotation
    for (int batch = 0; batch < 5; batch++) {
        std::cout << "\nBatch " << batch << ": writing " << entries_per_batch << " entries" << std::endl;

        for (int i = 0; i < entries_per_batch; i++) {
            WALEntry entry;
            entry.tag_id = 1 + (i % 50);
            entry.timestamp_us = start_ts + (batch * entries_per_batch + i) * 1000;
            entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
            entry.quality = 192;
            entry.value.f64_value = 100.0 + i;

            RotatingWALResult result = wal.append(entry);
            if (result != RotatingWALResult::SUCCESS) {
                std::cerr << "Failed to append entry " << i << " in batch " << batch
                          << ": " << wal.getLastError() << std::endl;
                std::cerr << "Current segment: " << wal.getCurrentSegmentId() << std::endl;
                // Print all segments status
                for (size_t s = 0; s < wal.getSegments().size(); s++) {
                    const WALSegment& seg = wal.getSegment(s);
                    std::cerr << "  Segment " << s << ": entries=" << seg.entry_count
                              << ", write_pos=" << seg.write_position
                              << ", avail=" << seg.getAvailableSpace() << std::endl;
                }
                assert_success(result, "Append entry");
            }
        }

        std::cout << "Current segment: " << wal.getCurrentSegmentId() << std::endl;
    }

    const RotatingWALStats& stats = wal.getStats();
    std::cout << "\nFinal statistics:" << std::endl;
    std::cout << "Entries written: " << stats.total_entries_written << std::endl;
    std::cout << "Segment rotations: " << stats.segment_rotations << std::endl;
    std::cout << "Segment flushes: " << stats.segment_flushes << std::endl;

    // Should have completed multiple rotations
    assert(stats.segment_rotations >= 4);

    wal.close();

    std::cout << "✓ Clear and reuse PASSED" << std::endl;
}

// Test 5: Usage ratio
void test_usage_ratio() {
    std::cout << "\n=== Test 5: Usage Ratio ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_usage.raw";  // Different file
    config.num_segments = 4;
    config.segment_size_bytes = 1024 * 1024;  // 1 MB

    RotatingWAL wal(config);
    assert_success(wal.open(), "Open WAL");

    // Initially empty
    double usage = wal.getUsageRatio();
    std::cout << "Initial usage: " << (usage * 100) << "%" << std::endl;
    assert(usage == 0.0);

    // Write some entries
    int64_t start_ts = 1000000;
    for (int i = 0; i < 1000; i++) {
        WALEntry entry;
        entry.tag_id = 1;
        entry.timestamp_us = start_ts + (i * 1000);
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 192;
        entry.value.f64_value = 100.0 + i;

        wal.append(entry);
    }

    usage = wal.getUsageRatio();
    std::cout << "After 1000 entries: " << (usage * 100) << "%" << std::endl;
    assert(usage > 0.0 && usage < 0.1);  // Should be small

    wal.close();

    std::cout << "✓ Usage ratio PASSED" << std::endl;
}

// Performance test
void test_performance() {
    std::cout << "\n=== Performance Test ===" << std::endl;

    cleanup_test_files();

    RotatingWALConfig config;
    config.wal_container_path = "./test_wal_perf.raw";  // Different file
    config.num_segments = 4;
    config.segment_size_bytes = 64 * 1024 * 1024;  // 64 MB

    RotatingWAL wal(config);
    assert_success(wal.open(), "Open WAL");

    // Write 100,000 entries and measure time
    int num_entries = 100000;
    int64_t start_ts = 1000000;

    auto t_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_entries; i++) {
        WALEntry entry;
        entry.tag_id = 1 + (i % 1000);
        entry.timestamp_us = start_ts + (i * 1000);
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 192;
        entry.value.f64_value = 100.0 + i;

        wal.append(entry);

        // Sync every 1000 entries
        if ((i + 1) % 10000 == 0) {
            wal.sync();
        }
    }

    wal.sync();

    auto t_end = std::chrono::high_resolution_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    std::cout << "Wrote " << num_entries << " entries in " << elapsed_ms << " ms" << std::endl;
    std::cout << "Throughput: " << (num_entries / elapsed_ms * 1000) << " entries/sec" << std::endl;
    std::cout << "Throughput: " << (num_entries * 24 / elapsed_ms / 1024) << " MB/sec" << std::endl;

    wal.close();

    std::cout << "✓ Performance test PASSED" << std::endl;
}

int main() {
    std::cout << "Starting Rotating WAL tests..." << std::endl;

    test_basic_initialization();
    test_write_entries();
    test_segment_rotation();
    test_clear_and_reuse();
    test_usage_ratio();
    test_performance();

    cleanup_test_files();

    std::cout << "\n=== ALL TESTS PASSED ===" << std::endl;
    return 0;
}
