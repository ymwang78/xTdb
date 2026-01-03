#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include "xTdb/constants.h"
#include <filesystem>
#include <iostream>

using namespace xtdb;
namespace fs = std::filesystem;

TEST(WriteDebugTest, MinimalWrite) {
    std::string test_dir = "/tmp/xtdb_write_debug";

    // Clean up
    if (fs::exists(test_dir)) {
        fs::remove_all(test_dir);
    }
    fs::create_directories(test_dir);

    // Setup config
    EngineConfig config;
    config.data_dir = test_dir;
    config.db_path = test_dir + "/meta.db";
    config.layout.block_size_bytes = 16384;
    config.layout.chunk_size_bytes = 4 * 1024 * 1024;

    std::cout << "Creating engine..." << std::endl;
    StorageEngine engine(config);

    std::cout << "Opening engine..." << std::endl;
    EngineResult result = engine.open();
    if (result != EngineResult::SUCCESS) {
        std::cerr << "Failed to open: " << engine.getLastError() << std::endl;
    }
    ASSERT_EQ(EngineResult::SUCCESS, result);

    std::cout << "Writing single point..." << std::endl;
    result = engine.writePoint(100, 1000000, 42.5, 192);
    if (result != EngineResult::SUCCESS) {
        std::cerr << "Failed to write: " << engine.getLastError() << std::endl;
    }
    EXPECT_EQ(EngineResult::SUCCESS, result);

    std::cout << "Flushing..." << std::endl;
    result = engine.flush();
    if (result != EngineResult::SUCCESS) {
        std::cerr << "Failed to flush: " << engine.getLastError() << std::endl;
    }
    EXPECT_EQ(EngineResult::SUCCESS, result);

    std::cout << "Closing engine..." << std::endl;
    engine.close();

    std::cout << "Done!" << std::endl;

    // Clean up
    fs::remove_all(test_dir);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
