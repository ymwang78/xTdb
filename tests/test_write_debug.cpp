#include <iostream>
#include "xTdb/storage_engine.h"
#include "xTdb/raw_scanner.h"
#include "test_utils.h"

using namespace xtdb;

int main() {
    // Clean and setup
    std::string temp_dir = get_temp_dir();
    std::string test_dir = join_path(temp_dir, "xtdb_debug");
    
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

    std::cout << "=== Writing 1001 points ===" << std::endl;
    for (int i = 0; i < 1001; i++) {
        result = engine.writePoint(tag_id, base_ts + i * 1000, 42.0 + i, 192);
        if (result != EngineResult::SUCCESS) {
            std::cout << "Failed to write point " << i << ": " << engine.getLastError() << std::endl;
            return 1;
        }
    }

    std::cout << "=== Checking write stats ===" << std::endl;
    auto write_stats = engine.getWriteStats();
    std::cout << "Points written: " << write_stats.points_written << std::endl;
    std::cout << "Blocks flushed: " << write_stats.blocks_flushed << std::endl;

    if (write_stats.blocks_flushed == 0) {
        std::cout << "ERROR: No blocks flushed!" << std::endl;
        return 1;
    }

    std::cout << "=== Checking active chunk ===" << std::endl;
    auto chunk_info = engine.getActiveChunk();
    std::cout << "Chunk ID: " << chunk_info.chunk_id << std::endl;
    std::cout << "Chunk offset: " << chunk_info.chunk_offset << std::endl;
    std::cout << "Blocks used: " << chunk_info.blocks_used << std::endl;

    std::cout << "=== Scanning chunk with RawScanner ===" << std::endl;
    AlignedIO* io = engine.getMetadataSync()->getIO();
    RawScanner scanner(io);
    ScannedChunk scanned_chunk;
    ScanResult scan_result = scanner.scanChunk(chunk_info.chunk_offset,
                                               config.layout,
                                               scanned_chunk);

    if (scan_result != ScanResult::SUCCESS) {
        std::cout << "Scan failed: " << scanner.getLastError() << std::endl;
    } else {
        std::cout << "Scan success!" << std::endl;
        std::cout << "Sealed blocks found: " << scanned_chunk.blocks.size() << std::endl;
        for (size_t i = 0; i < scanned_chunk.blocks.size(); i++) {
            const auto& block = scanned_chunk.blocks[i];
            std::cout << "  Block " << i << ": tag=" << block.tag_id
                      << " records=" << block.record_count
                      << " sealed=" << block.is_sealed << std::endl;
        }
    }

    std::cout << "=== Querying data ===" << std::endl;
    std::vector<StorageEngine::QueryPoint> results;
    result = engine.queryPoints(tag_id, base_ts, base_ts + 500 * 1000, results);

    std::cout << "Query result: " << (result == EngineResult::SUCCESS ? "SUCCESS" : "FAILED") << std::endl;
    std::cout << "Points returned: " << results.size() << std::endl;

    auto read_stats = engine.getReadStats();
    std::cout << "Points from memory: " << read_stats.points_read_memory << std::endl;
    std::cout << "Points from disk: " << read_stats.points_read_disk << std::endl;
    std::cout << "Blocks read: " << read_stats.blocks_read << std::endl;

    engine.close();

    return 0;
}
