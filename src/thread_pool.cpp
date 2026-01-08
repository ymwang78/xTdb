#include "xTdb/thread_pool.h"
#include <iostream>

namespace xtdb {

ThreadPool::ThreadPool(size_t num_threads)
    : stop_(false), active_count_(0), pending_count_(0) {

    // Use hardware concurrency if num_threads is 0
    if (num_threads == 0) {
        num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) {
            num_threads = 4;  // Fallback to 4 threads
        }
    }

    // Create worker threads
    workers_.reserve(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        workers_.emplace_back(&ThreadPool::worker_thread, this);
    }
}

ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        stop_.store(true);
    }

    // Wake up all workers
    cv_.notify_all();

    // Join all workers
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::worker_thread() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);

            // Wait for task or stop signal
            cv_.wait(lock, [this] {
                return stop_.load() || !tasks_.empty();
            });

            // Exit if stopped and no more tasks
            if (stop_.load() && tasks_.empty()) {
                return;
            }

            // Get task from queue
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop();
                pending_count_.fetch_sub(1);
                active_count_.fetch_add(1);
            }
        }

        // Execute task outside lock
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                std::cerr << "[ThreadPool] Task exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[ThreadPool] Unknown task exception" << std::endl;
            }

            active_count_.fetch_sub(1);

            // Notify waiters if all tasks complete
            if (pending_count_.load() == 0 && active_count_.load() == 0) {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                wait_cv_.notify_all();
            }
        }
    }
}

void ThreadPool::wait_all() {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    wait_cv_.wait(lock, [this] {
        return pending_count_.load() == 0 && active_count_.load() == 0;
    });
}

size_t ThreadPool::pending_tasks() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
}

}  // namespace xtdb
