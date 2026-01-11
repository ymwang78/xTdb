#include <iostream>
#include "xTdb/compact_container.h"
#include <filesystem>

using namespace xtdb;

int main() {
    std::string test_path = "/tmp/test_compact_minimal.dat";

    // Remove test file if exists
    if (std::filesystem::exists(test_path)) {
        std::filesystem::remove(test_path);
    }

    // Setup layout
    ChunkLayout layout;
    layout.block_size_bytes = 16384;  // 16KB
    layout.chunk_size_bytes = 256 * 1024 * 1024;  // 256MB
    layout.meta_blocks = 0;
    layout.data_blocks = 0;

    std::cout << "Creating CompactContainer..." << std::endl;
    CompactContainer container(test_path, layout, CompressionType::COMP_ZSTD);

    std::cout << "Opening container..." << std::endl;
    ContainerResult result = container.open(true);

    std::cout << "Open result: " << static_cast<int>(result) << std::endl;

    if (result == ContainerResult::SUCCESS) {
        std::cout << "SUCCESS: Container opened" << std::endl;
        std::cout << "isOpen: " << container.isOpen() << std::endl;
        container.close();
        std::cout << "Container closed" << std::endl;
        return 0;
    } else {
        std::cout << "FAILED: " << container.getLastError() << std::endl;
        return 1;
    }
}
