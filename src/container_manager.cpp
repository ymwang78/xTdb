#include "xTdb/container_manager.h"
#include <chrono>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace xtdb {

ContainerManager::ContainerManager(const ManagerConfig& config)
    : config_(config),
      active_index_(0),
      is_initialized_(false),
      last_rollover_ts_us_(0) {
}

ContainerManager::~ContainerManager() {
    close();
}

ManagerResult ContainerManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (is_initialized_) {
        setError("Manager already initialized");
        return ManagerResult::ERROR_INITIALIZATION_FAILED;
    }

    // Validate configuration
    if (config_.containers.empty()) {
        setError("No containers configured");
        return ManagerResult::ERROR_NO_CONTAINERS;
    }

    // Create and open all configured containers
    for (const auto& container_config : config_.containers) {
        auto container = ContainerFactory::create(container_config);
        if (!container) {
            setError("Failed to create container: " + container_config.path);
            // Close already opened containers
            for (auto& c : containers_) {
                c->close();
            }
            containers_.clear();
            return ManagerResult::ERROR_INITIALIZATION_FAILED;
        }

        containers_.push_back(std::move(container));
    }

    // Set active container (last one is writable)
    active_index_ = containers_.size() - 1;

    // Record initialization timestamp
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    last_rollover_ts_us_ = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    is_initialized_ = true;
    return ManagerResult::SUCCESS;
}

void ContainerManager::close() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_initialized_) {
        return;
    }

    // Close all containers
    for (auto& container : containers_) {
        if (container) {
            container->close();
        }
    }

    containers_.clear();
    is_initialized_ = false;
}

bool ContainerManager::isInitialized() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_initialized_;
}

IContainer* ContainerManager::getWritableContainer() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_initialized_ || containers_.empty()) {
        return nullptr;
    }

    // Return active container
    if (active_index_ < containers_.size()) {
        auto* container = containers_[active_index_].get();
        if (container && container->isOpen() && !container->isReadOnly()) {
            return container;
        }
    }

    return nullptr;
}

std::vector<IContainer*> ContainerManager::getAllContainers() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<IContainer*> result;
    for (auto& container : containers_) {
        if (container && container->isOpen()) {
            result.push_back(container.get());
        }
    }

    return result;
}

IContainer* ContainerManager::getContainer(size_t index) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (index >= containers_.size()) {
        return nullptr;
    }

    return containers_[index].get();
}

size_t ContainerManager::getContainerCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return containers_.size();
}

bool ContainerManager::needsRollover() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_initialized_ || config_.rollover_strategy == RolloverStrategy::NONE) {
        return false;
    }

    // Get current active container
    if (active_index_ >= containers_.size()) {
        return false;
    }

    auto* active_container = containers_[active_index_].get();
    if (!active_container) {
        return false;
    }

    // Check rollover conditions based on strategy
    switch (config_.rollover_strategy) {
        case RolloverStrategy::DAILY: {
            // Check if we've crossed midnight
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

            // Get day boundaries
            int64_t last_day = last_rollover_ts_us_ / (24LL * 3600 * 1000000);
            int64_t current_day = now_us / (24LL * 3600 * 1000000);

            return (current_day > last_day);
        }

        case RolloverStrategy::SIZE_BASED: {
            // Check if current size exceeds threshold
            int64_t current_size = active_container->getCurrentSize();
            if (current_size < 0) {
                return false;  // Error getting size
            }
            return (static_cast<uint64_t>(current_size) >= config_.rollover_size_bytes);
        }

        case RolloverStrategy::TIME_BASED: {
            // Check if time interval has passed
            auto now = std::chrono::system_clock::now();
            auto duration = now.time_since_epoch();
            int64_t now_us = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

            int64_t interval_us = config_.rollover_time_hours * 3600LL * 1000000;
            return ((now_us - last_rollover_ts_us_) >= interval_us);
        }

        default:
            return false;
    }
}

ManagerResult ContainerManager::rollover() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!is_initialized_) {
        setError("Manager not initialized");
        return ManagerResult::ERROR_ROLLOVER_FAILED;
    }

    if (config_.rollover_strategy == RolloverStrategy::NONE) {
        setError("Rollover not enabled");
        return ManagerResult::ERROR_ROLLOVER_FAILED;
    }

    // Create new container
    ManagerResult result = createRolloverContainer();
    if (result != ManagerResult::SUCCESS) {
        return result;
    }

    // Update last rollover timestamp
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    last_rollover_ts_us_ = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();

    return ManagerResult::SUCCESS;
}

std::string ContainerManager::getLastError() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

ContainerStats ContainerManager::getTotalStats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    ContainerStats total;
    for (const auto& container : containers_) {
        if (container && container->isOpen()) {
            const auto& stats = container->getStats();
            total.bytes_written += stats.bytes_written;
            total.bytes_read += stats.bytes_read;
            total.write_operations += stats.write_operations;
            total.read_operations += stats.read_operations;
            total.sync_operations += stats.sync_operations;
        }
    }

    return total;
}

size_t ContainerManager::getActiveContainerIndex() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_index_;
}

std::string ContainerManager::generateContainerName(size_t index) {
    std::string name = config_.name_pattern;

    // Replace {index} with container index
    size_t pos = name.find("{index}");
    if (pos != std::string::npos) {
        name.replace(pos, 7, std::to_string(index));
    }

    // Replace {date} with current date (YYYYMMDD format)
    pos = name.find("{date}");
    if (pos != std::string::npos) {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        struct tm tm_now;
        localtime_r(&time_t_now, &tm_now);

        std::ostringstream date_stream;
        date_stream << std::put_time(&tm_now, "%Y%m%d");
        name.replace(pos, 6, date_stream.str());
    }

    return name;
}

ManagerResult ContainerManager::createRolloverContainer() {
    // Generate new container name
    size_t new_index = containers_.size();
    std::string container_name = generateContainerName(new_index);

    // Create container config based on first container's config
    ContainerConfig new_config = config_.containers[0];  // Use first as template
    new_config.path = container_name;
    new_config.create_if_not_exists = true;
    new_config.read_only = false;

    // Create and open new container
    auto container = ContainerFactory::create(new_config);
    if (!container) {
        setError("Failed to create rollover container: " + container_name);
        return ManagerResult::ERROR_ROLLOVER_FAILED;
    }

    // Add to containers list and set as active
    containers_.push_back(std::move(container));
    active_index_ = containers_.size() - 1;

    std::cout << "[ContainerManager] Rolled over to new container: " << container_name << std::endl;

    return ManagerResult::SUCCESS;
}

void ContainerManager::setError(const std::string& message) {
    last_error_ = message;
}

}  // namespace xtdb
