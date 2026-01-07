#include <gtest/gtest.h>
#include "xTdb/archive_manager.h"

using namespace xtdb;

class ArchiveManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = new ArchiveManager();
    }

    void TearDown() override {
        delete manager_;
        manager_ = nullptr;
    }

    ArchiveManager* manager_;
};

// Test archive registration
TEST_F(ArchiveManagerTest, RegisterArchive) {
    ArchiveMetadata metadata;
    metadata.level = ArchiveLevel::ARCHIVE_RAW;
    metadata.start_ts_us = 1000000000;
    metadata.end_ts_us = 2000000000;
    metadata.resampling_interval_us = 0;
    metadata.record_count = 1000;

    bool result = manager_->registerArchive(metadata, 1, "/path/to/container1.dat");
    EXPECT_TRUE(result);

    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RAW));
}

// Test archive selection for short query
TEST_F(ArchiveManagerTest, SelectArchiveShortQuery) {
    // Register RAW archive
    ArchiveMetadata raw_metadata;
    raw_metadata.level = ArchiveLevel::ARCHIVE_RAW;
    raw_metadata.start_ts_us = 1000000000;
    raw_metadata.end_ts_us = 2000000000;
    raw_metadata.resampling_interval_us = 0;
    raw_metadata.record_count = 1000000;

    manager_->registerArchive(raw_metadata, 1, "/path/to/raw.dat");

    // Register 1M archive
    ArchiveMetadata m1_metadata;
    m1_metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
    m1_metadata.start_ts_us = 1000000000;
    m1_metadata.end_ts_us = 2000000000;
    m1_metadata.resampling_interval_us = 60 * 1000000ULL;
    m1_metadata.record_count = 16667;

    manager_->registerArchive(m1_metadata, 2, "/path/to/1m.dat");

    // Query for 10 minutes (short query)
    ArchiveQuery query;
    query.start_ts_us = 1000000000;
    query.end_ts_us = 1000000000 + 10 * 60 * 1000000LL;  // 10 minutes

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, selections[0].level);  // Should prefer RAW for short query
}

// Test archive selection for long query
TEST_F(ArchiveManagerTest, SelectArchiveLongQuery) {
    // Register RAW archive
    ArchiveMetadata raw_metadata;
    raw_metadata.level = ArchiveLevel::ARCHIVE_RAW;
    raw_metadata.start_ts_us = 1000000000;
    raw_metadata.end_ts_us = 2000000000;

    manager_->registerArchive(raw_metadata, 1, "/path/to/raw.dat");

    // Register 1H archive
    ArchiveMetadata h1_metadata;
    h1_metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1H;
    h1_metadata.start_ts_us = 1000000000;
    h1_metadata.end_ts_us = 2000000000;
    h1_metadata.resampling_interval_us = 3600 * 1000000ULL;

    manager_->registerArchive(h1_metadata, 2, "/path/to/1h.dat");

    // Query for 7 days (long query)
    ArchiveQuery query;
    query.start_ts_us = 1000000000;
    query.end_ts_us = 1000000000 + 7 * 86400 * 1000000LL;  // 7 days

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RESAMPLED_1H, selections[0].level);  // Should prefer 1H for long query
}

// Test recommendation for different time spans
TEST_F(ArchiveManagerTest, RecommendArchiveLevel) {
    // Short query (< 1 hour) -> RAW
    int64_t start1 = 1000000000;
    int64_t end1 = start1 + 30 * 60 * 1000000LL;  // 30 minutes
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, manager_->recommendArchiveLevel(start1, end1));

    // Medium query (1-24 hours) -> 1M
    int64_t start2 = 1000000000;
    int64_t end2 = start2 + 12 * 3600 * 1000000LL;  // 12 hours
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RESAMPLED_1M, manager_->recommendArchiveLevel(start2, end2));

    // Long query (1-30 days) -> 1H
    int64_t start3 = 1000000000;
    int64_t end3 = start3 + 7 * 86400 * 1000000LL;  // 7 days
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RESAMPLED_1H, manager_->recommendArchiveLevel(start3, end3));

    // Very long query (> 30 days) -> AGG
    int64_t start4 = 1000000000;
    int64_t end4 = start4 + 90 * 86400 * 1000000LL;  // 90 days
    EXPECT_EQ(ArchiveLevel::ARCHIVE_AGGREGATED, manager_->recommendArchiveLevel(start4, end4));
}

// Test archive selection with no overlap
TEST_F(ArchiveManagerTest, SelectArchiveNoOverlap) {
    // Register archive
    ArchiveMetadata metadata;
    metadata.level = ArchiveLevel::ARCHIVE_RAW;
    metadata.start_ts_us = 1000000000;
    metadata.end_ts_us = 2000000000;

    manager_->registerArchive(metadata, 1, "/path/to/container.dat");

    // Query outside archive time range
    ArchiveQuery query;
    query.start_ts_us = 3000000000;
    query.end_ts_us = 4000000000;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    EXPECT_EQ(0u, count);  // No archive selected
}

// Test archive selection with partial overlap
TEST_F(ArchiveManagerTest, SelectArchivePartialOverlap) {
    // Register archive
    ArchiveMetadata metadata;
    metadata.level = ArchiveLevel::ARCHIVE_RAW;
    metadata.start_ts_us = 1000000000;
    metadata.end_ts_us = 2000000000;

    manager_->registerArchive(metadata, 1, "/path/to/container.dat");

    // Query partially overlaps
    ArchiveQuery query;
    query.start_ts_us = 1500000000;
    query.end_ts_us = 2500000000;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    EXPECT_EQ(1u, count);  // Should still select the archive
}

// Test prefer_raw flag
TEST_F(ArchiveManagerTest, PreferRawFlag) {
    // Register both archives
    ArchiveMetadata raw_metadata;
    raw_metadata.level = ArchiveLevel::ARCHIVE_RAW;
    raw_metadata.start_ts_us = 1000000000;
    raw_metadata.end_ts_us = 2000000000;

    manager_->registerArchive(raw_metadata, 1, "/path/to/raw.dat");

    ArchiveMetadata m1_metadata;
    m1_metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
    m1_metadata.start_ts_us = 1000000000;
    m1_metadata.end_ts_us = 2000000000;

    manager_->registerArchive(m1_metadata, 2, "/path/to/1m.dat");

    // Query with prefer_raw flag
    ArchiveQuery query;
    query.start_ts_us = 1000000000;
    query.end_ts_us = 1000000000 + 7 * 86400 * 1000000LL;  // 7 days (normally prefers 1H)
    query.prefer_raw = true;

    std::vector<ArchiveSelection> selections;
    size_t count = manager_->selectArchives(query, selections);

    ASSERT_EQ(1u, count);
    EXPECT_EQ(ArchiveLevel::ARCHIVE_RAW, selections[0].level);  // Should prefer RAW due to flag
}

// Test multiple archive levels
TEST_F(ArchiveManagerTest, MultipleArchiveLevels) {
    // Register archives at different levels
    ArchiveMetadata raw_metadata;
    raw_metadata.level = ArchiveLevel::ARCHIVE_RAW;
    raw_metadata.start_ts_us = 1000000000;
    raw_metadata.end_ts_us = 2000000000;
    manager_->registerArchive(raw_metadata, 1, "/path/to/raw.dat");

    ArchiveMetadata m1_metadata;
    m1_metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
    m1_metadata.start_ts_us = 1000000000;
    m1_metadata.end_ts_us = 2000000000;
    manager_->registerArchive(m1_metadata, 2, "/path/to/1m.dat");

    ArchiveMetadata h1_metadata;
    h1_metadata.level = ArchiveLevel::ARCHIVE_RESAMPLED_1H;
    h1_metadata.start_ts_us = 1000000000;
    h1_metadata.end_ts_us = 2000000000;
    manager_->registerArchive(h1_metadata, 3, "/path/to/1h.dat");

    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RAW));
    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1M));
    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1H));
    EXPECT_EQ(0u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_AGGREGATED));
}

// Test clear
TEST_F(ArchiveManagerTest, Clear) {
    // Register some archives
    ArchiveMetadata metadata1;
    metadata1.level = ArchiveLevel::ARCHIVE_RAW;
    metadata1.start_ts_us = 1000000000;
    metadata1.end_ts_us = 2000000000;
    manager_->registerArchive(metadata1, 1, "/path/to/container1.dat");

    ArchiveMetadata metadata2;
    metadata2.level = ArchiveLevel::ARCHIVE_RESAMPLED_1M;
    metadata2.start_ts_us = 1000000000;
    metadata2.end_ts_us = 2000000000;
    manager_->registerArchive(metadata2, 2, "/path/to/container2.dat");

    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RAW));
    EXPECT_EQ(1u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1M));

    // Clear
    manager_->clear();

    EXPECT_EQ(0u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RAW));
    EXPECT_EQ(0u, manager_->getArchiveCount(ArchiveLevel::ARCHIVE_RESAMPLED_1M));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
