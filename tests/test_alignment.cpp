#include "xTdb/aligned_io.h"
#include <gtest/gtest.h>
#include <cstring>
#include <unistd.h>

using namespace xtdb;

// ============================================================================
// T1-AlignmentCheck: Verify alignment enforcement
// ============================================================================

class AlignmentTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_file_ = "/tmp/xtdb_alignment_test.dat";
        // Remove test file if exists
        ::unlink(test_file_.c_str());
    }

    void TearDown() override {
        ::unlink(test_file_.c_str());
    }

    std::string test_file_;
};

// Test 1: Valid aligned write should succeed
TEST_F(AlignmentTest, ValidAlignedWrite) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Allocate aligned buffer
    AlignedBuffer buffer(kExtentSizeBytes);
    ASSERT_TRUE(buffer.isValid());

    // Fill with test data
    buffer.zero();
    std::memset(buffer.data(), 0xAB, kExtentSizeBytes);

    // Write at aligned offset (0)
    IOResult result = io.write(buffer.data(), kExtentSizeBytes, 0);
    EXPECT_EQ(IOResult::SUCCESS, result) << "Aligned write should succeed";

    // Verify stats
    EXPECT_EQ(1, io.getStats().write_operations);
    EXPECT_EQ(kExtentSizeBytes, io.getStats().bytes_written);
}

// Test 2: Unaligned buffer should fail
TEST_F(AlignmentTest, UnalignedBufferFails) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Allocate aligned buffer, then create unaligned pointer
    AlignedBuffer buffer(kExtentSizeBytes * 2);
    void* unaligned_ptr = static_cast<char*>(buffer.data()) + 1;  // +1 byte = unaligned

    // Attempt write with unaligned buffer
    IOResult result = io.write(unaligned_ptr, kExtentSizeBytes, 0);
    EXPECT_EQ(IOResult::ERROR_ALIGNMENT, result)
        << "Write with unaligned buffer should fail";

    // Verify no write occurred
    EXPECT_EQ(0, io.getStats().write_operations);
}

// Test 3: Unaligned size should fail
TEST_F(AlignmentTest, UnalignedSizeFails) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    AlignedBuffer buffer(kExtentSizeBytes * 2);

    // Attempt write with unaligned size (not multiple of 16KB)
    uint64_t unaligned_size = kExtentSizeBytes + 1024;  // 16KB + 1KB = not aligned
    IOResult result = io.write(buffer.data(), unaligned_size, 0);
    EXPECT_EQ(IOResult::ERROR_ALIGNMENT, result)
        << "Write with unaligned size should fail";

    // Verify no write occurred
    EXPECT_EQ(0, io.getStats().write_operations);
}

// Test 4: Unaligned offset should fail
TEST_F(AlignmentTest, UnalignedOffsetFails) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    AlignedBuffer buffer(kExtentSizeBytes);

    // Attempt write with unaligned offset (not multiple of 16KB)
    uint64_t unaligned_offset = 4096;  // 4KB offset = not 16KB-aligned
    IOResult result = io.write(buffer.data(), kExtentSizeBytes, unaligned_offset);
    EXPECT_EQ(IOResult::ERROR_ALIGNMENT, result)
        << "Write with unaligned offset should fail";

    // Verify no write occurred
    EXPECT_EQ(0, io.getStats().write_operations);
}

// Test 5: Multiple aligned writes at different offsets
TEST_F(AlignmentTest, MultipleAlignedWrites) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    AlignedBuffer buffer(kExtentSizeBytes);

    // Write at offset 0
    buffer.zero();
    std::memset(buffer.data(), 0x11, kExtentSizeBytes);
    EXPECT_EQ(IOResult::SUCCESS, io.write(buffer.data(), kExtentSizeBytes, 0));

    // Write at offset 16KB
    std::memset(buffer.data(), 0x22, kExtentSizeBytes);
    EXPECT_EQ(IOResult::SUCCESS,
              io.write(buffer.data(), kExtentSizeBytes, kExtentSizeBytes));

    // Write at offset 32KB
    std::memset(buffer.data(), 0x33, kExtentSizeBytes);
    EXPECT_EQ(IOResult::SUCCESS,
              io.write(buffer.data(), kExtentSizeBytes, kExtentSizeBytes * 2));

    // Verify stats
    EXPECT_EQ(3, io.getStats().write_operations);
    EXPECT_EQ(kExtentSizeBytes * 3, io.getStats().bytes_written);
}

// Test 6: Read with alignment enforcement
TEST_F(AlignmentTest, AlignedRead) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Write test data
    AlignedBuffer write_buffer(kExtentSizeBytes);
    write_buffer.zero();
    std::memset(write_buffer.data(), 0xCD, kExtentSizeBytes);
    ASSERT_EQ(IOResult::SUCCESS, io.write(write_buffer.data(), kExtentSizeBytes, 0));

    // Read back
    AlignedBuffer read_buffer(kExtentSizeBytes);
    read_buffer.zero();
    EXPECT_EQ(IOResult::SUCCESS, io.read(read_buffer.data(), kExtentSizeBytes, 0));

    // Verify data
    EXPECT_EQ(0, std::memcmp(write_buffer.data(), read_buffer.data(), kExtentSizeBytes))
        << "Read data should match written data";

    // Verify stats
    EXPECT_EQ(1, io.getStats().read_operations);
    EXPECT_EQ(kExtentSizeBytes, io.getStats().bytes_read);
}

// Test 7: Unaligned read should fail
TEST_F(AlignmentTest, UnalignedReadFails) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Write some data first
    AlignedBuffer buffer(kExtentSizeBytes);
    buffer.zero();
    ASSERT_EQ(IOResult::SUCCESS, io.write(buffer.data(), kExtentSizeBytes, 0));

    // Attempt read with unaligned offset
    uint64_t unaligned_offset = 8192;  // 8KB = not 16KB-aligned
    IOResult result = io.read(buffer.data(), kExtentSizeBytes, unaligned_offset);
    EXPECT_EQ(IOResult::ERROR_ALIGNMENT, result)
        << "Read with unaligned offset should fail";

    // Note: read stats should still be 0 (failed before operation)
    EXPECT_EQ(0, io.getStats().read_operations);
}

// Test 8: Preallocate with alignment
TEST_F(AlignmentTest, PreallocateAligned) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Preallocate 256MB (aligned)
    uint64_t size = 256 * 1024 * 1024;  // 256MB
    EXPECT_TRUE(isExtentAligned(size));
    EXPECT_EQ(IOResult::SUCCESS, io.preallocate(size));

    // Verify file size
    EXPECT_EQ(static_cast<int64_t>(size), io.getFileSize());
}

// Test 9: Preallocate with unaligned size should fail
TEST_F(AlignmentTest, PreallocateUnalignedFails) {
    AlignedIO io;
    ASSERT_EQ(IOResult::SUCCESS, io.open(test_file_, true, false));

    // Attempt preallocate with unaligned size
    uint64_t unaligned_size = 100 * 1024 * 1024 + 1024;  // 100MB + 1KB
    EXPECT_FALSE(isExtentAligned(unaligned_size));
    EXPECT_EQ(IOResult::ERROR_ALIGNMENT, io.preallocate(unaligned_size));
}

// Test 10: AlignedBuffer automatic alignment
TEST_F(AlignmentTest, BufferAutoAlignment) {
    // Request 10KB, should round up to 16KB
    AlignedBuffer buffer(10 * 1024);
    EXPECT_EQ(kExtentSizeBytes, buffer.size())
        << "Buffer should be rounded up to 16KB";
    EXPECT_TRUE(buffer.isValid());

    // Verify alignment of buffer pointer
    uintptr_t addr = reinterpret_cast<uintptr_t>(buffer.data());
    EXPECT_EQ(0, addr % kExtentSizeBytes)
        << "Buffer address should be 16KB-aligned";
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
