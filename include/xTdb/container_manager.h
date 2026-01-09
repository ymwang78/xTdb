#ifndef XTDB_CONTAINER_MANAGER_H_
#define XTDB_CONTAINER_MANAGER_H_

#include "container.h"
#include <vector>
#include <memory>
#include <string>
#include <mutex>

namespace xtdb {

// ============================================================================
// ContainerManager - Multi-container orchestration
// ============================================================================

/// Container manager result codes
enum class ManagerResult {
    SUCCESS = 0,
    ERR_NO_CONTAINERS,
    ERR_INITIALIZATION_FAILED,
    ERR_NO_WRITABLE_CONTAINER,
    ERR_ROLLOVER_FAILED,
    ERR_INVALID_INDEX
};

/// Container manager configuration
struct ManagerConfig {
    std::vector<ContainerConfig> containers;  // Container configurations
    RolloverStrategy rollover_strategy;        // Rollover strategy
    uint64_t rollover_size_bytes;              // Size threshold for SIZE_BASED rollover
    int rollover_time_hours;                   // Time interval for TIME_BASED rollover
    std::string name_pattern;                  // Naming pattern for new containers (e.g., "container_{date}.raw")

    ManagerConfig()
        : rollover_strategy(RolloverStrategy::NONE),
          rollover_size_bytes(0),
          rollover_time_hours(24),
          name_pattern("container_{index}.raw") {
    }
};

/// Container manager - manages multiple containers
class ContainerManager {
public:
    /// Constructor
    explicit ContainerManager(const ManagerConfig& config);

    /// Destructor
    ~ContainerManager();

    // Disable copy and move
    ContainerManager(const ContainerManager&) = delete;
    ContainerManager& operator=(const ContainerManager&) = delete;
    ContainerManager(ContainerManager&&) = delete;
    ContainerManager& operator=(ContainerManager&&) = delete;

    // ========================================================================
    // Initialization and Lifecycle
    // ========================================================================

    /// Initialize container manager (open all containers)
    /// @return ManagerResult
    ManagerResult initialize();

    /// Close all containers
    void close();

    /// Check if manager is initialized
    /// @return true if initialized
    bool isInitialized() const;

    // ========================================================================
    // Container Access
    // ========================================================================

    /// Get writable container (active container)
    /// @return Pointer to writable container, or nullptr if none available
    IContainer* getWritableContainer();

    /// Get all containers (for queries)
    /// @return Vector of container pointers
    std::vector<IContainer*> getAllContainers();

    /// Get container by index
    /// @param index Container index
    /// @return Pointer to container, or nullptr if invalid index
    IContainer* getContainer(size_t index);

    /// Get number of containers
    /// @return Number of containers
    size_t getContainerCount() const;

    // ========================================================================
    // Rollover Operations
    // ========================================================================

    /// Check if rollover is needed
    /// @return true if rollover should be performed
    bool needsRollover();

    /// Perform container rollover (create new active container)
    /// @return ManagerResult
    ManagerResult rollover();

    /// Get last error message
    /// @return Error message string
    std::string getLastError() const;

    // ========================================================================
    // Statistics
    // ========================================================================

    /// Get total I/O statistics across all containers
    /// @return Aggregated ContainerStats
    ContainerStats getTotalStats() const;

    /// Get active container index
    /// @return Active container index
    size_t getActiveContainerIndex() const;

private:
    /// Generate container name using pattern
    /// @param index Container index
    /// @return Generated container name
    std::string generateContainerName(size_t index);

    /// Create new container for rollover
    /// @return ManagerResult
    ManagerResult createRolloverContainer();

    /// Set last error message
    /// @param message Error message
    void setError(const std::string& message);

    ManagerConfig config_;                              // Manager configuration
    std::vector<std::unique_ptr<IContainer>> containers_;  // Container instances
    size_t active_index_;                               // Active container index
    bool is_initialized_;                               // Initialization state
    int64_t last_rollover_ts_us_;                       // Last rollover timestamp
    std::string last_error_;                            // Last error message

    mutable std::mutex mutex_;                          // Thread-safety mutex
};

}  // namespace xtdb

#endif  // XTDB_CONTAINER_MANAGER_H_
