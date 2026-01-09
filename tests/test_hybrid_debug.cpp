#include <iostream>
#include "xTdb/storage_engine.h"
#include "xTdb/raw_scanner.h"
#include "test_utils.h"

using namespace xtdb;

int main() {
    std::string temp_dir = get_temp_dir();
    std::string test_dir = join_path(temp_dir, "xtdb_hybrid_debug");
    
    remove_directory(test_dir);
    create_directory(test_dir);

    EngineConfig config;
    config.data_dir = test_dir;
    config.db_path = join_path(test_dir, "meta.db");
    config.layout.block_size_bytes = 16384;
    config.layout.chunk_size_bytes = 4 * 1024 * 1024;

    StorageEngine engine(config);

    std::cout << "=== Opening engine ===" << std::endl;
    EngineResult result = engine.open();
    if (result != EngineResult::SUCCESS) {
        std::cout << "Failed to open: " << engine.getLastError() << std::endl;
        return 1;
    }

    const uint32_t tag_id = 100;
    const int64_t base_ts = 1000000;

    std::cout << "=== Phase 1: Write 1001 points (trigger flush) ===" << std::endl;
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 100.0 + i, 192);
        if (result != EngineResult::SUCCESS) {
            std::cout << "Failed to write point " << i << ": " << engine.getLastError() << std::endl;
            return 1;
        }
    }

    auto write_stats = engine.getWriteStats();
    std::cout << "Blocks flushed: " << write_stats.blocks_flushed << std::endl;

    auto chunk_info = engine.getActiveChunk();
    std::cout << "Active chunk offset: " << chunk_info.chunk_offset << std::endl;
    std::cout << "Blocks used: " << chunk_info.blocks_used << std::endl;

    std::cout << "=== Phase 2: Write 49 more points (stay in memory) ===" << std::endl;
    for (int i = 1001; i < 1050; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 100.0 + i, 192);
        if (result != EngineResult::SUCCESS) {
            std::cout << "Failed to write point " << i << ": " << engine.getLastError() << std::endl;
            return 1;
        }
    }

    std::cout << "=== Phase 3: Query 900-1040 ===" << std::endl;
    int64_t query_start = base_ts + 900 * 1000;
    int64_t query_end = base_ts + 1040 * 1000;
    std::cout << "Query range: " << query_start << " to " << query_end << std::endl;

    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(tag_id, query_start, query_end, results);

    if (result != EngineResult::SUCCESS) {
        std::cout << "Query failed: " << engine.getLastError() << std::endl;
    } else {
        auto read_stats = engine.getReadStats();
        std::cout << "Total points: " << results.size() << std::endl;
        std::cout << "From disk: " << read_stats.points_read_disk << std::endl;
        std::cout << "From memory: " << read_stats.points_read_memory << std::endl;
        std::cout << "Blocks read: " << read_stats.blocks_read << std::endl;

        if (!results.empty()) {
            std::cout << "First point: ts=" << results.front().timestamp_us
                      << " value=" << results.front().value << std::endl;
            std::cout << "Last point: ts=" << results.back().timestamp_us
                      << " value=" << results.back().value << std::endl;
        }
    }

    engine.close();
    return 0;
}
