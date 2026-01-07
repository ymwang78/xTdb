#include <gtest/gtest.h>
#include "xTdb/archive_manager.h"
#include "xTdb/resampling_engine.h"
#include <vector>
#include <cmath>
#include <iostream>

using namespace xtdb;

class MultiResolutionQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = new ArchiveManager();
    }

    void TearDown() override {
        delete manager_;
        manager_ = nullptr;
    }

    // Helper: Generate sample data with known pattern
    std::vector<MemRecord> generateTestData(size_t count, double base_value = 100.0, double increment = 0.1) {
        std::vector<MemRecord> records;

        for (size_t i = 0; i < count; i++) {
            MemRecord rec;
            rec.time_offset = i * 1000;  // 1 second intervals
            rec.value.f64_value = base_value + i * increment;
            rec.quality = 192;
            records.push_back(rec);
        }

        return records;
    }

    // Helper: Create and register RAW archive
    void createRawArchive(int64_t start_ts_us, int64_t end_ts_us, uint64_t container_id) {
        ArchiveMetadata metadata;
        metadata.level = ArchiveLevel::ARCHIVE_RAW;
        metadata.start_ts_us = start_ts_us;
        metadata.end_ts_us = end_ts_us;
        metadata.resampling_interval_us = 0;
        metadata.record_count = (end_ts_us - start_ts_us) / 1000000;  // Assuming 1 Hz

        bool result = manager_->registerArchive(metadata, container_id,
                                               "/data/raw_" + std::to_string(container_id) + ".dat");
        ASSERT_TRUE(result);
    }

    // Helper: Create and register 1M resampled archive
    void create1MArchive(int64_t start_ts_us, int64_t end_ts_us, uint64_t container_id) {
        ArchiveMetadata metadata;
        metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
        metadata.start_ts_us = start_ts_us;
        metadata.end_ts_us = end_ts_us;
        metadata.resampling_interval_us = 60 * 1000000ULL;  // 1 minute
        metadata.record_count = (end_ts_us - start_ts_us) / (60 * 1000000ULL);

        bool result = manager_->registerArchive(metadata, container_id,
                                               "/data/1m_" + std::to_string(container_id) + ".dat");
        ASSERT_TRUE(result);
    }

    // Helper: Create and register 1H resampled archive
    void create1HArchive(int64_t start_ts_us, int64_t end_ts_us, uint64_t container_id) {
        ArchiveMetadata metadata;
        metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1H;
        metadata.start_ts_us = start_ts_us;
        metadata.end_ts_us = end_ts_us;
        metadata.resampling_interval_us = 3600 * 1000000ULL;  // 1 hour
        metadata.record_count = (end_ts_us - start_ts_us) / (3600 * 1000000ULL);

        bool result = manager_->registerArchive(metadata, container_id,
                                               "/data/1h_" + std::to_string(container_id) + ".dat");
        ASSERT_TRUE(result);
    }

    ArchiveManager* manager_;
};

// Test 1: Short query (< 1 hour) should select RAW archive
TEST_F(MultiResolutionQueryTest, ShortQuerySelectsRaw) {
    std::cout << "\n=== Test 1: Short Query Selection ===" << std::endl;

    // Time base: 2024-01-01 00:00:00
    int64_t base_ts = 1704067200000000LL;  // microseconds

    // Create overlapping archives
    // RAW: Recent 6 hours
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);

    // 1M: Recent 7 days
    create1MArchive(base_ts - 7 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 2);

    // 1H: Recent 30 days
    create1HArchive(base_ts - 30 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 3);

    // Query: Recent 30 minutes (short query)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts;
    query.end_ts_us = base_ts + 30 * 60 * 1000000LL;  // 30 minutes
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, selections[0].level);

    std::cout << "Query span: 30 minutes" << std::endl;
    std::cout << "Selected archive: " << archiveLevelToString(selections[0].level) << std::endl;
    std::cout << "Priority: " << static_cast<int>(selections[0].priority) << std::endl;
}

// Test 2: Medium query (1 hour - 1 day) should select 1M archive
TEST_F(MultiResolutionQueryTest, MediumQuerySelects1M) {
    std::cout << "\n=== Test 2: Medium Query Selection ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Create overlapping archives
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);
    create1MArchive(base_ts - 7 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 2);
    create1HArchive(base_ts - 30 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 3);

    // Query: Recent 12 hours (medium query)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts - 12 * 3600 * 1000000LL;
    query.end_ts_us = base_ts;
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RESAMPLED_1M, selections[0].level);

    std::cout << "Query span: 12 hours" << std::endl;
    std::cout << "Selected archive: " << archiveLevelToString(selections[0].level) << std::endl;
    std::cout << "Priority: " << static_cast<int>(selections[0].priority) << std::endl;
}

// Test 3: Long query (> 1 day) should select 1H archive
TEST_F(MultiResolutionQueryTest, LongQuerySelects1H) {
    std::cout << "\n=== Test 3: Long Query Selection ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Create overlapping archives
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);
    create1MArchive(base_ts - 7 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 2);
    create1HArchive(base_ts - 30 * 86400 * 1000000LL, base_ts + 6 * 3600 * 1000000LL, 3);

    // Query: Recent 7 days (long query)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts - 7 * 86400 * 1000000LL;
    query.end_ts_us = base_ts;
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RESAMPLED_1H, selections[0].level);

    std::cout << "Query span: 7 days" << std::endl;
    std::cout << "Selected archive: " << archiveLevelToString(selections[0].level) << std::endl;
    std::cout << "Priority: " << static_cast<int>(selections[0].priority) << std::endl;
}

// Test 4: Prefer RAW flag overrides automatic selection
TEST_F(MultiResolutionQueryTest, PreferRawOverride) {
    std::cout << "\n=== Test 4: Prefer RAW Override ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Create overlapping archives covering the same range
    // RAW: Recent 7 days (extended to cover the query)
    createRawArchive(base_ts - 7 * 86400 * 1000000LL, base_ts, 1);
    create1MArchive(base_ts - 7 * 86400 * 1000000LL, base_ts, 2);
    create1HArchive(base_ts - 30 * 86400 * 1000000LL, base_ts, 3);

    // Query: 7 days (normally would select 1H, but prefer_raw forces RAW)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts - 7 * 86400 * 1000000LL;
    query.end_ts_us = base_ts;
    query.prefer_raw = true;  // Force RAW

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, selections[0].level);

    std::cout << "Query span: 7 days (with prefer_raw=true)" << std::endl;
    std::cout << "Selected archive: " << archiveLevelToString(selections[0].level) << std::endl;
    std::cout << "Override successful!" << std::endl;
}

// Test 5: Archive recommendation based on time span
TEST_F(MultiResolutionQueryTest, ArchiveRecommendation) {
    std::cout << "\n=== Test 5: Archive Recommendation ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Test various time spans
    struct TestCase {
        int64_t span_us;
        ArchiveLevel expected;
        std::string description;
    };

    std::vector<TestCase> test_cases = {
        {30 * 60 * 1000000LL, ArchiveLevel::ARCHIVE_RAW, "30 minutes"},
        {2 * 3600 * 1000000LL, ArchiveLevel::ARCHIVE_RESAMPLED_1M, "2 hours"},
        {12 * 3600 * 1000000LL, ArchiveLevel::ARCHIVE_RESAMPLED_1M, "12 hours"},
        {3 * 86400 * 1000000LL, ArchiveLevel::ARCHIVE_RESAMPLED_1H, "3 days"},
        {14 * 86400 * 1000000LL, ArchiveLevel::ARCHIVE_RESAMPLED_1H, "14 days"},
        {60 * 86400 * 1000000LL, ArchiveLevel::ARCHIVE_AGGREGATED, "60 days"}
    };

    for (const auto& test_case : test_cases) {
        ArchiveLevel recommended = manager_->recommendArchiveLevel(
            base_ts, base_ts + test_case.span_us);

        std::cout << test_case.description << " -> "
                  << archiveLevelToString(recommended) << std::endl;

        EXPECT_EQ(test_case.expected, recommended);
    }
}

// Test 6: Query with no matching archive
TEST_F(MultiResolutionQueryTest, NoMatchingArchive) {
    std::cout << "\n=== Test 6: No Matching Archive ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Only register RAW archive for recent 6 hours
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);

    // Query: 30 days ago (no archive covers this range)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts - 30 * 86400 * 1000000LL;
    query.end_ts_us = base_ts - 29 * 86400 * 1000000LL;
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    EXPECT_EQ(0u, count);
    std::cout << "Query span: 1 day (30 days ago)" << std::endl;
    std::cout << "Selected archives: " << count << " (expected 0)" << std::endl;
}

// Test 7: Query at archive boundary
TEST_F(MultiResolutionQueryTest, ArchiveBoundaryQuery) {
    std::cout << "\n=== Test 7: Archive Boundary Query ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // RAW archive: 0-6 hours
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);

    // 1M archive: 6-48 hours
    create1MArchive(base_ts + 6 * 3600 * 1000000LL,
                    base_ts + 48 * 3600 * 1000000LL, 2);

    // Query exactly at the boundary
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts + 5 * 3600 * 1000000LL;   // 5 hours
    query.end_ts_us = base_ts + 7 * 3600 * 1000000LL;     // 7 hours
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    // Should select one archive (currently only returns best match)
    ASSERT_GE(count, 1u);
    std::cout << "Query: 5h-7h (spans RAW/1M boundary)" << std::endl;
    std::cout << "Selected: " << archiveLevelToString(selections[0].level) << std::endl;
}

// Test 8: Multiple archives at same level
TEST_F(MultiResolutionQueryTest, MultipleArchivesSameLevel) {
    std::cout << "\n=== Test 8: Multiple Archives Same Level ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Multiple RAW archives covering different time ranges
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);  // 0-6h
    createRawArchive(base_ts + 6 * 3600 * 1000000LL,
                    base_ts + 12 * 3600 * 1000000LL, 2);            // 6-12h
    createRawArchive(base_ts + 12 * 3600 * 1000000LL,
                    base_ts + 24 * 3600 * 1000000LL, 3);            // 12-24h

    // Query: Recent 3 hours (covered by first archive)
    ArchiveQuery query;
    query.tag_id = 1;
    query.start_ts_us = base_ts;
    query.end_ts_us = base_ts + 3 * 3600 * 1000000LL;
    query.prefer_raw = false;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, selections[0].level);

    std::cout << "3 RAW archives registered" << std::endl;
    std::cout << "Query: 0-3h" << std::endl;
    std::cout << "Selected: container_" << selections[0].container_id << std::endl;
    std::cout << "Time range: " << selections[0].start_ts_us << " - "
              << selections[0].end_ts_us << std::endl;
}

// Test 9: Archive statistics
TEST_F(MultiResolutionQueryTest, ArchiveStatistics) {
    std::cout << "\n=== Test 9: Archive Statistics ===" << std::endl;

    int64_t base_ts = 1704067200000000LL;

    // Register various archives
    createRawArchive(base_ts, base_ts + 6 * 3600 * 1000000LL, 1);
    createRawArchive(base_ts + 6 * 3600 * 1000000LL,
                    base_ts + 12 * 3600 * 1000000LL, 2);

    create1MArchive(base_ts - 7 * 86400 * 1000000LL,
                   base_ts + 6 * 3600 * 1000000LL, 3);

    create1HArchive(base_ts - 30 * 86400 * 1000000LL,
                   base_ts + 6 * 3600 * 1000000LL, 4);

    // Get statistics
    size_t raw_count = manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RAW);
    size_t m1_count = manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1M);
    size_t h1_count = manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1H);
    size_t agg_count = manager_->getArchiveCount(ArchiveLevel::ARCHIVE_AGGREGATED);

    std::cout << "Archive Statistics:" << std::endl;
    std::cout << "  RAW: " << raw_count << std::endl;
    std::cout << "  1M:  " << m1_count << std::endl;
    std::cout << "  1H:  " << h1_count << std::endl;
    std::cout << "  AGG: " << agg_count << std::endl;

    EXPECT_EQ(2u, raw_count);
    EXPECT_EQ(1u, m1_count);
    EXPECT_EQ(1u, h1_count);
    EXPECT_EQ(0u, agg_count);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
