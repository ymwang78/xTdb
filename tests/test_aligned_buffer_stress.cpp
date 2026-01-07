#include <iostream>
#include "xTdb/aligned_io.h"

using namespace xtdb;

int main() {
    std::cout << "Testing AlignedBuffer allocation/deallocation..." << std::endl;

    // Test 1: Simple creation and destruction
    {
        std::cout << "Test 1: Single buffer" << std::endl;
        AlignedBuffer buf(16384);
        std::cout << "  Created buffer of size " << buf.size() << std::endl;
    }
    std::cout << "  Buffer destroyed" << std::endl;

    // Test 2: Multiple buffers in sequence
    {
        std::cout << "Test 2: Sequential buffers" << std::endl;
        for (int i = 0; i < 10; i++) {
            AlignedBuffer buf(16384);
            std::cout << "  Buffer " << i << " created and destroyed" << std::endl;
        }
    }
    std::cout << "Test 2 complete" << std::endl;

    // Test 3: Nested buffers (similar to flush() pattern)
    {
        std::cout << "Test 3: Nested buffers (flush pattern)" << std::endl;
        for (int i = 0; i < 5; i++) {
            std::cout << "  Iteration " << i << std::endl;

            // Simulate BlockWriter buffer
            {
                AlignedBuffer block_buf(16384);
                std::cout << "    BlockWriter buffer created" << std::endl;
            }
            std::cout << "    BlockWriter buffer destroyed" << std::endl;

            // Simulate DirectoryBuilder buffer
            {
                AlignedBuffer dir_buf(49152);  // 3 * 16KB
                std::cout << "    DirectoryBuilder buffer created" << std::endl;
            }
            std::cout << "    DirectoryBuilder buffer destroyed" << std::endl;
        }
    }
    std::cout << "Test 3 complete" << std::endl;

    std::cout << "All tests passed!" << std::endl;
    return 0;
}
