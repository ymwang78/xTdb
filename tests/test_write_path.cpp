#include "xTdb/wal_writer.h"
#include "xTdb/mem_buffer.h"
#include "xTdb/block_writer.h"
#include "xTdb/state_mutator.h"
#include "xTdb/platform_compat.h"
#include "test_utils.h"
#include <gtest/gtest.h>

using namespace xtdb;

// ============================================================================
// T5-BlindWrite: Verify data writes to disk (without directory)
// ============================================================================

class WritePathTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::string temp_dir = get_temp_dir();
        test_file_ = join_path(temp_dir, "xtdb_write_test.dat");
        unlink_file(test_file_);

        // Open file
        io_ = std::make_unique<AlignedIO>();
        ASSERT_EQ(IOResult::SUCCESS, io_->open(test_file_, true, false));

        // Preallocate 256MB
        ASSERT_EQ(IOResult::SUCCESS,
                  io_->preallocate(256 * 1024 * 1024));

        // Calculate layout for RAW16K
        layout_ = LayoutCalculator::calculateLayout(
            RawBlockClass::RAW_16K,
            kDefaultChunkSizeExtents);
    }

    void TearDown() override {
        io_.reset();
        unlink_file(test_file_);
    }

    std::string test_file_;
    std::unique_ptr<AlignedIO> io_;
    ChunkLayout layout_;
};

// Test 1: WAL basic write
TEST_F(WritePathTest, WALBasicWrite) {
    // Create WAL at offset 256MB (end of first chunk)
    uint64_t wal_offset = 256 * 1024 * 1024;
    uint64_t wal_size = 16 * 1024 * 1024;  // 16MB WAL
    WALWriter wal(io_.get(), wal_offset, wal_size);

    // Create entry
    WALEntry entry;
    entry.tag_id = 100;
    entry.timestamp_us = 1000000;
    entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
    entry.quality = 0xC0;  // Good quality
    entry.value.f64_value = 123.456;

    // Write entry
    ASSERT_EQ(WALResult::SUCCESS, wal.append(entry));

    // Write more entries
    for (int i = 0; i < 100; i++) {
        entry.timestamp_us = 1000000 + i * 1000;
        entry.value.f64_value = 100.0 + i;
        ASSERT_EQ(WALResult::SUCCESS, wal.append(entry));
    }

    // Sync to disk
    ASSERT_EQ(WALResult::SUCCESS, wal.sync());

    // Verify statistics
    EXPECT_EQ(101, wal.getStats().entries_written);
    EXPECT_GT(wal.getStats().bytes_written, 0);
    EXPECT_EQ(1, wal.getStats().sync_operations);
}

// Test 2: MemBuffer aggregation
TEST_F(WritePathTest, MemBufferAggregation) {
    MemBuffer buffer(1000);  // Max 1000 records per tag

    // Add entries for multiple tags
    for (uint32_t tag_id = 100; tag_id < 105; tag_id++) {
        for (int i = 0; i < 50; i++) {
            WALEntry entry;
            entry.tag_id = tag_id;
            entry.timestamp_us = 1000000 + i * 1000;
            entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
            entry.quality = 0xC0;
            entry.value.f64_value = tag_id * 100.0 + i;

            bool need_flush = buffer.addEntry(entry);
            EXPECT_FALSE(need_flush);  // Should not need flush yet
        }
    }

    // Verify buffers
    EXPECT_EQ(5, buffer.getAllBuffers().size());
    EXPECT_EQ(250, buffer.getTotalRecords());

    // Get specific tag buffer
    TagBuffer* tag_buffer = buffer.getTagBuffer(100);
    ASSERT_NE(nullptr, tag_buffer);
    EXPECT_EQ(100u, tag_buffer->tag_id);
    EXPECT_EQ(50, tag_buffer->records.size());
}

// Test 3: BlockWriter blind write (THE KEY TEST)
TEST_F(WritePathTest, BlockWriterBlindWrite) {
    // Initialize chunk 0 header
    StateMutator mutator(io_.get());

    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator.initChunkHeader(0, header));
    ASSERT_EQ(MutateResult::SUCCESS, mutator.allocateChunk(0));

    // Create tag buffer with test data
    TagBuffer tag_buffer;
    tag_buffer.tag_id = 100;
    tag_buffer.value_type = ValueType::VT_F64;
    tag_buffer.time_unit = TimeUnit::TU_MS;
    tag_buffer.start_ts_us = 1000000;

    // Add 100 records
    for (int i = 0; i < 100; i++) {
        MemRecord record;
        record.time_offset = i * 10;  // 10ms intervals
        record.quality = 0xC0;
        record.value.f64_value = 100.0 + i;
        tag_buffer.records.push_back(record);
    }

    // Write to data block 0 (first data block)
    BlockWriter writer(io_.get(), layout_);
    ASSERT_EQ(BlockWriteResult::SUCCESS,
              writer.writeBlock(0, 0, tag_buffer));

    // Sync to ensure data is written to disk
    ASSERT_EQ(IOResult::SUCCESS, io_->sync());

    // Verify statistics
    EXPECT_EQ(1, writer.getStats().blocks_written);
    EXPECT_EQ(layout_.block_size_bytes, writer.getStats().bytes_written);
    EXPECT_EQ(100, writer.getStats().records_written);

    // CRITICAL: Verify data is on disk by reading raw bytes
    // Calculate offset of data block 0
    uint32_t data_block_0_index = layout_.meta_blocks;  // First block after meta
    uint64_t data_offset = LayoutCalculator::calculateBlockOffset(
        0, data_block_0_index, layout_, 0);  // container_base=0 (no container header in test)

    // Check file size and offset
    int64_t file_size = io_->getFileSize();
    EXPECT_GT(file_size, 0) << "File should have been preallocated";
    EXPECT_GE(file_size, static_cast<int64_t>(data_offset + layout_.block_size_bytes))
        << "File size (" << file_size << ") should be >= offset + block_size ("
        << (data_offset + layout_.block_size_bytes) << ")";

    // Read back the block
    AlignedBuffer read_buffer(layout_.block_size_bytes);
    IOResult read_result = io_->read(read_buffer.data(), layout_.block_size_bytes, data_offset);
    if (read_result != IOResult::SUCCESS) {
        FAIL() << "Read failed: " << io_->getLastError()
               << " (file_size=" << file_size
               << ", data_offset=" << data_offset
               << ", block_size=" << layout_.block_size_bytes << ")";
    }
    ASSERT_EQ(IOResult::SUCCESS, read_result);

    // Verify first record
    const char* data = static_cast<const char*>(read_buffer.data());

    // Record 0: time_offset=0, quality=0xC0, value=100.0
    EXPECT_EQ(0x00, data[0]);  // time_offset byte 0
    EXPECT_EQ(0x00, data[1]);  // time_offset byte 1
    EXPECT_EQ(0x00, data[2]);  // time_offset byte 2
    EXPECT_EQ(0xC0, static_cast<uint8_t>(data[3]));  // quality

    // Value (8 bytes, little-endian double)
    double value;
    std::memcpy(&value, data + 4, 8);
    EXPECT_DOUBLE_EQ(100.0, value);

    // Verify record 1: time_offset=10, value=101.0
    EXPECT_EQ(0x0A, data[12]);  // time_offset = 10
    EXPECT_EQ(0x00, data[13]);
    EXPECT_EQ(0x00, data[14]);
    EXPECT_EQ(0xC0, static_cast<uint8_t>(data[15]));

    std::memcpy(&value, data + 16, 8);
    EXPECT_DOUBLE_EQ(101.0, value);
}

// Test 4: Verify data block does not overwrite chunk header
TEST_F(WritePathTest, DataBlockPreservesHeader) {
    // Initialize chunk header
    StateMutator mutator(io_.get());

    RawChunkHeaderV16 header;
    header.chunk_id = 5;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator.initChunkHeader(0, header));

    // Create tag buffer
    TagBuffer tag_buffer;
    tag_buffer.tag_id = 200;
    tag_buffer.value_type = ValueType::VT_F64;
    tag_buffer.start_ts_us = 2000000;

    for (int i = 0; i < 50; i++) {
        MemRecord record;
        record.time_offset = i * 5;
        record.quality = 0xC0;
        record.value.f64_value = 200.0 + i;
        tag_buffer.records.push_back(record);
    }

    // Write to data block 0
    BlockWriter writer(io_.get(), layout_);
    ASSERT_EQ(BlockWriteResult::SUCCESS,
              writer.writeBlock(0, 0, tag_buffer));

    // Read back chunk header and verify it's intact
    RawChunkHeaderV16 read_header;
    ASSERT_EQ(MutateResult::SUCCESS,
              mutator.readChunkHeader(0, read_header));

    EXPECT_EQ(5u, read_header.chunk_id);
    EXPECT_EQ(0, std::memcmp(read_header.magic, kRawChunkMagic, 8));
    EXPECT_EQ(0x0106, read_header.version);
}

// Test 5: Write to multiple data blocks
TEST_F(WritePathTest, MultipleDataBlocks) {
    // Initialize chunk
    StateMutator mutator(io_.get());

    RawChunkHeaderV16 header;
    header.chunk_id = 0;
    header.chunk_size_extents = layout_.chunk_size_extents;
    header.block_size_extents = layout_.block_size_extents;
    header.meta_blocks = layout_.meta_blocks;
    header.data_blocks = layout_.data_blocks;

    ASSERT_EQ(MutateResult::SUCCESS, mutator.initChunkHeader(0, header));

    BlockWriter writer(io_.get(), layout_);

    // Write to 10 different data blocks
    for (uint32_t block_idx = 0; block_idx < 10; block_idx++) {
        TagBuffer tag_buffer;
        tag_buffer.tag_id = 100 + block_idx;
        tag_buffer.value_type = ValueType::VT_F64;
        tag_buffer.start_ts_us = 1000000 + block_idx * 100000;

        for (int i = 0; i < 20; i++) {
            MemRecord record;
            record.time_offset = i;
            record.quality = 0xC0;
            record.value.f64_value = static_cast<double>(block_idx * 100 + i);
            tag_buffer.records.push_back(record);
        }

        ASSERT_EQ(BlockWriteResult::SUCCESS,
                  writer.writeBlock(0, block_idx, tag_buffer));
    }

    // Verify statistics
    EXPECT_EQ(10, writer.getStats().blocks_written);
    EXPECT_EQ(200, writer.getStats().records_written);
}

// Test 6: WAL full detection
TEST_F(WritePathTest, WALFullDetection) {
    // Create small WAL (32KB = 2 extents)
    uint64_t wal_offset = 256 * 1024 * 1024;
    uint64_t wal_size = 32 * 1024;
    WALWriter wal(io_.get(), wal_offset, wal_size);

    // Fill WAL
    int entries_written = 0;
    for (int i = 0; i < 10000; i++) {
        WALEntry entry;
        entry.tag_id = 100;
        entry.timestamp_us = 1000000 + i;
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 0xC0;
        entry.value.f64_value = static_cast<double>(i);

        WALResult result = wal.append(entry);
        if (result == WALResult::ERR_FULL) {
            break;
        }
        ASSERT_EQ(WALResult::SUCCESS, result);
        entries_written++;
    }

    EXPECT_GT(entries_written, 0);
    EXPECT_LT(entries_written, 10000);  // Should fill before 10000
    EXPECT_TRUE(wal.isFull());
}

// Test 7: MemBuffer flush threshold
TEST_F(WritePathTest, MemBufferFlushThreshold) {
    MemBuffer buffer(100);  // Max 100 records per tag

    bool need_flush = false;

    // Add 100 entries (should trigger flush)
    for (int i = 0; i < 100; i++) {
        WALEntry entry;
        entry.tag_id = 100;
        entry.timestamp_us = 1000000 + i;
        entry.value_type = static_cast<uint8_t>(ValueType::VT_F64);
        entry.quality = 0xC0;
        entry.value.f64_value = static_cast<double>(i);

        need_flush = buffer.addEntry(entry);
    }

    EXPECT_TRUE(need_flush);
    EXPECT_TRUE(buffer.hasFlushableTag());
    EXPECT_EQ(100u, buffer.getFlushableTag());
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
