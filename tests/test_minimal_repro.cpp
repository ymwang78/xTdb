#include <gtest/gtest.h>
#include "xTdb/storage_engine.h"
#include <filesystem>

using namespace xtdb;
namespace fs = std::filesystem;

TEST(MinimalRepro, OpenCloseOnly) {
    std::string test_dir = "/tmp/xtdb_minimal_test";

    // Clean up
    if (fs::exists(test_dir)) {
        fs::remove_all(test_dir);
    }
    fs::create_directories(test_dir);

    // Test1: Open and close
    {
        EngineConfig config;
        config.data_dir = test_dir;
        config.db_path = test_dir + "/meta.db";
        config.retention_days = 0;

        StorageEngine engine(config);
        EngineResult result = engine.open();
        ASSERT_EQ(EngineResult::SUCCESS, result);
        engine.close();
    }

    std::cout << "Test 1 passed: Open/Close" << std::endl;

    // Test2: Open, write one point, close
    {
        fprintf(stderr, "=== TEST 2 STARTING ===\n"); fflush(stderr);
        EngineConfig config;
        config.data_dir = test_dir;
        config.db_path = test_dir + "/meta.db";
        config.retention_days = 0;

        fprintf(stderr, "TEST 2: Creating engine\n"); fflush(stderr);
        StorageEngine engine(config);
        fprintf(stderr, "TEST 2: Opening\n"); fflush(stderr);
        EngineResult result = engine.open();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        fprintf(stderr, "TEST 2: Writing point\n"); fflush(stderr);
        result = engine.writePoint(100, 1000000, 42.5, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);

        fprintf(stderr, "TEST 2: Closing\n"); fflush(stderr);
        engine.close();
        fprintf(stderr, "TEST 2: Close returned\n"); fflush(stderr);
    }

    std::cout << "Test 2 passed: Write one point" << std::endl;

    // Test3: Open, write, flush, close
    {
        std::cout << "Test 3 starting - creating config..." << std::endl;
        EngineConfig config;
        config.data_dir = test_dir;
        config.db_path = test_dir + "/meta.db";
        config.retention_days = 0;

        std::cout << "Test 3 - creating engine..." << std::endl;
        StorageEngine engine(config);
        std::cout << "Test 3 - opening engine..." << std::endl;
        EngineResult result = engine.open();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        std::cout << "Test 3 - writing point..." << std::endl;
        result = engine.writePoint(100, 1000000, 42.5, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);

        std::cout << "Before flush..." << std::endl;
        result = engine.flush();
        std::cout << "After flush, result=" << static_cast<int>(result) << std::endl;
        ASSERT_EQ(EngineResult::SUCCESS, result);

        std::cout << "Before close..." << std::endl;
        engine.close();
        std::cout << "After close..." << std::endl;
    }

    std::cout << "Test 3 passed: Write and flush" << std::endl;

    // Test4: Check if destructor is the issue
    {
        std::cout << "Test 4 starting..." << std::endl;
        EngineConfig config;
        config.data_dir = test_dir;
        config.db_path = test_dir + "/meta.db";
        config.retention_days = 0;

        StorageEngine* engine = new StorageEngine(config);
        EngineResult result = engine->open();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        result = engine->writePoint(100, 1000000, 42.5, 192);
        ASSERT_EQ(EngineResult::SUCCESS, result);

        result = engine->flush();
        ASSERT_EQ(EngineResult::SUCCESS, result);

        engine->close();
        std::cout << "Before manual delete..." << std::endl;
        delete engine;
        std::cout << "After manual delete..." << std::endl;
    }

    std::cout << "Test 4 passed: Manual delete" << std::endl;

    // Clean up
    fs::remove_all(test_dir);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
