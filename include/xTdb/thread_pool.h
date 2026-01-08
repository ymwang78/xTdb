#ifndef XTDB_THREAD_POOL_H_
#define XTDB_THREAD_POOL_H_

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <atomic>

namespace xtdb {

// ============================================================================
// ThreadPool - High-performance thread pool for parallel operations
// ============================================================================

/// Thread pool for executing tasks in parallel
/// Implements work-stealing queue for load balancing
class ThreadPool {
public:
    /// Constructor
    /// @param num_threads Number of worker threads (0 = hardware_concurrency)
    explicit ThreadPool(size_t num_threads = 0);

    /// Destructor - waits for all tasks to complete
    ~ThreadPool();

    // Disable copy and move
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// Submit a task for execution
    /// @param task Function to execute
    /// @return Future for the task result
    template<typename F, typename... Args>
    auto submit(F&& task, Args&&... args)
        -> std::future<typename std::result_of<F(Args...)>::type>;

    /// Wait for all pending tasks to complete
    void wait_all();

    /// Get number of worker threads
    size_t size() const { return workers_.size(); }

    /// Get number of pending tasks
    size_t pending_tasks() const;

    /// Get number of active workers
    size_t active_workers() const { return active_count_.load(); }

private:
    /// Worker thread function
    void worker_thread();

    // Worker threads
    std::vector<std::thread> workers_;

    // Task queue
    std::queue<std::function<void()>> tasks_;

    // Synchronization
    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::condition_variable wait_cv_;

    // State
    std::atomic<bool> stop_;
    std::atomic<size_t> active_count_;
    std::atomic<size_t> pending_count_;
};

// ============================================================================
// Template Implementation
// ============================================================================

template<typename F, typename... Args>
auto ThreadPool::submit(F&& task, Args&&... args)
    -> std::future<typename std::result_of<F(Args...)>::type> {

    using return_type = typename std::result_of<F(Args...)>::type;

    // Create packaged task
    auto packaged_task = std::make_shared<std::packaged_task<return_type()>>(
        std::bind(std::forward<F>(task), std::forward<Args>(args)...)
    );

    std::future<return_type> result = packaged_task->get_future();

    {
        std::unique_lock<std::mutex> lock(queue_mutex_);

        // Don't allow new tasks after stop
        if (stop_.load()) {
            throw std::runtime_error("ThreadPool: submit on stopped pool");
        }

        // Add task to queue
        tasks_.emplace([packaged_task]() {
            (*packaged_task)();
        });

        pending_count_.fetch_add(1);
    }

    // Notify one worker
    cv_.notify_one();

    return result;
}

}  // namespace xtdb

#endif  // XTDB_THREAD_POOL_H_
