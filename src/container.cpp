#include "xTdb/container.h"
#include "xTdb/file_container.h"
#include "xTdb/block_device_container.h"
#include "xTdb/platform_compat.h"
#include <iostream>

namespace xtdb {

// ============================================================================
// ContainerFactory Implementation
// ============================================================================

std::unique_ptr<IContainer> ContainerFactory::create(const ContainerConfig& config) {
    // Validate configuration
    if (!validateConfig(config)) {
        std::cerr << "[ContainerFactory] Invalid configuration" << std::endl;
        return nullptr;
    }

    // Create appropriate container type
    switch (config.type) {
        case ContainerType::FILE_BASED: {
            auto container = std::make_unique<FileContainer>(
                config.path,
                config.layout,
                config.direct_io,
                config.read_only
            );

            // Open container
            ContainerResult result = container->open(config.create_if_not_exists);
            if (result != ContainerResult::SUCCESS) {
                std::cerr << "[ContainerFactory] Failed to open file container: "
                          << container->getLastError() << std::endl;
                return nullptr;
            }

            // Preallocate if requested
            if (config.preallocate_size > 0 && !config.read_only) {
                result = container->preallocate(config.preallocate_size);
                if (result != ContainerResult::SUCCESS) {
                    std::cerr << "[ContainerFactory] Warning: Preallocation failed: "
                              << container->getLastError() << std::endl;
                    // Continue anyway, preallocation is optional
                }
            }

            return container;
        }
        break;

        case ContainerType::BLOCK_DEVICE: {
            auto container = std::make_unique<BlockDeviceContainer>(
                config.path,
                config.layout,
                config.read_only,
                config.test_mode
            );

            // Open container
            ContainerResult result = container->open(config.create_if_not_exists);
            if (result != ContainerResult::SUCCESS) {
                std::cerr << "[ContainerFactory] Failed to open block device container: "
                          << container->getLastError() << std::endl;
                return nullptr;
            }

            return container;
        }
        break;

        default:
            std::cerr << "[ContainerFactory] Unknown container type" << std::endl;
            return nullptr;
    }
}

ContainerType ContainerFactory::detectType(const std::string& path) {
    // Check if path is a block device
    if (BlockDeviceContainer::isBlockDevice(path)) {
        return ContainerType::BLOCK_DEVICE;
    }

    // Default to file-based
    return ContainerType::FILE_BASED;
}

bool ContainerFactory::validateConfig(const ContainerConfig& config) {
    // Validate path
    if (config.path.empty()) {
        std::cerr << "[ContainerFactory] Empty path" << std::endl;
        return false;
    }

    // Validate layout
    if (config.layout.block_size_bytes == 0 || config.layout.chunk_size_bytes == 0) {
        std::cerr << "[ContainerFactory] Invalid layout: block_size or chunk_size is zero" << std::endl;
        return false;
    }

    // Validate alignment
    if (config.layout.block_size_bytes % kExtentSizeBytes != 0) {
        std::cerr << "[ContainerFactory] Block size must be extent-aligned (16KB)" << std::endl;
        return false;
    }

    if (config.layout.chunk_size_bytes % kExtentSizeBytes != 0) {
        std::cerr << "[ContainerFactory] Chunk size must be extent-aligned (16KB)" << std::endl;
        return false;
    }

    // Validate preallocate size
    if (config.preallocate_size > 0 && config.preallocate_size % kExtentSizeBytes != 0) {
        std::cerr << "[ContainerFactory] Preallocate size must be extent-aligned (16KB)" << std::endl;
        return false;
    }

    return true;
}

}  // namespace xtdb
