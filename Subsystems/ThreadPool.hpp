// This is the task scheduler for the Aegis subsystems.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <expected>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "VersionInfo.h"

namespace Aegis_ThreadPool {

    enum thread_StatusCode: std::size_t {
        Success = 0,
        Failure = 1,
        Cleanup_Failure = 2
    };

    // A task is run on its own jthread. At most three tasks run concurrently;
    // further tasks remain in the FIFO queue until a running task completes.
    class thread_pool {
    public:
        using task_handle = std::shared_future<void>;

    private:
        struct task_record {
            std::function<void()> work;
            std::promise<void> completion;
            std::jthread worker;
            bool completed = false;
        };

        mutable std::mutex mutex_;
        std::mutex shutdown_mutex_;
        std::condition_variable condition_;
        std::deque<std::shared_ptr<task_record>> pending_tasks_;
        std::vector<std::shared_ptr<task_record>> running_tasks_;
        const std::size_t maximum_concurrent_tasks_;
        std::size_t active_task_count_ = 0;
        bool shutdown_requested_ = false;
        std::jthread dispatcher_;

        bool has_completed_task_locked() const {
            for (const auto& task : running_tasks_) {
                if (task->completed) return true;
            }
            return false;
        }

        void dispatch_tasks() {
            std::unique_lock lock(mutex_);

            while (true) {
                // Reap finished jthreads before reusing their execution slots.
                auto task_iterator = running_tasks_.begin();
                while (task_iterator != running_tasks_.end()) {
                    if (!(*task_iterator)->completed) {
                        ++task_iterator;
                        continue;
                    }

                    auto completed_task = *task_iterator;
                    lock.unlock();
                    if (completed_task->worker.joinable()) {
                        completed_task->worker.join();
                    }
                    lock.lock();
                    task_iterator = running_tasks_.erase(task_iterator);
                }

                // Start queued work in submission order, up to the shared limit.
                while (active_task_count_ < maximum_concurrent_tasks_ && !pending_tasks_.empty()) {
                    auto task = std::move(pending_tasks_.front());
                    pending_tasks_.pop_front();
                    running_tasks_.push_back(task);
                    ++active_task_count_;

                    try {
                        task->worker = std::jthread([this, task] {
                            try {
                                task->work();
                                task->completion.set_value();
                            } catch (...) {
                                task->completion.set_exception(std::current_exception());
                            }

                            {
                                std::lock_guard task_lock(mutex_);
                                task->completed = true;
                                --active_task_count_;
                            }
                            condition_.notify_all();
                        });
                    } catch (...) {
                        task->completion.set_exception(std::current_exception());
                        task->completed = true;
                        --active_task_count_;
                    }
                }

                if (shutdown_requested_ && pending_tasks_.empty() && active_task_count_ == 0) {
                    return;
                }

                condition_.wait(lock, [this] {
                    return has_completed_task_locked() ||
                        (active_task_count_ < maximum_concurrent_tasks_ && !pending_tasks_.empty()) ||
                        (shutdown_requested_ && pending_tasks_.empty() && active_task_count_ == 0);
                });
            }
        }

    public:
        explicit thread_pool(std::size_t maximum_concurrent_tasks = 3)
            : maximum_concurrent_tasks_(maximum_concurrent_tasks == 0 ? 1 : maximum_concurrent_tasks),
              dispatcher_([this] { dispatch_tasks(); }) {}

        thread_pool(const thread_pool&) = delete;
        thread_pool& operator=(const thread_pool&) = delete;

        ~thread_pool() {
            (void)shut_down();
        }

        // Enqueue work and return a handle that can be used to wait for its result.
        template<typename Task, typename... Args>
        std::expected<task_handle, thread_StatusCode> Insert_Into_thread_pool(Task&& task, Args&&... args) {
            try {
                auto bound_task = std::bind_front(std::forward<Task>(task), std::forward<Args>(args)...);
                auto record = std::make_shared<task_record>();
                record->work = [bound_task = std::move(bound_task)]() mutable {
                    std::invoke(bound_task);
                };
                task_handle handle = record->completion.get_future().share();

                {
                    std::lock_guard lock(mutex_);
                    if (shutdown_requested_) {
                        return std::unexpected<thread_StatusCode>(Failure);
                    }
                    pending_tasks_.push_back(std::move(record));
                }
                condition_.notify_all();
                return handle;
            } catch (...) {
                return std::unexpected<thread_StatusCode>(Failure);
            }
        }

        bool isShutDown() const {
            std::lock_guard lock(mutex_);
            return shutdown_requested_;
        }

        // Includes both running and queued tasks.
        std::size_t getQueueSize() const {
            std::lock_guard lock(mutex_);
            return active_task_count_ + pending_tasks_.size();
        }

        std::size_t getActiveTaskCount() const {
            std::lock_guard lock(mutex_);
            return active_task_count_;
        }

        std::size_t getPendingTaskCount() const {
            std::lock_guard lock(mutex_);
            return pending_tasks_.size();
        }

        std::size_t getMaximumConcurrentTasks() const noexcept {
            return maximum_concurrent_tasks_;
        }

        std::expected<void, thread_StatusCode> shut_down() {
            std::lock_guard shutdown_lock(shutdown_mutex_);
            bool called_from_worker = false;
            {
                std::lock_guard lock(mutex_);
                shutdown_requested_ = true;
                for (const auto& task : running_tasks_) {
                    if (task->worker.get_id() == std::this_thread::get_id()) {
                        called_from_worker = true;
                        break;
                    }
                }
            }
            condition_.notify_all();

            // The dispatcher must be allowed to join this task after it returns.
            if (!called_from_worker && dispatcher_.joinable()) {
                try {
                    dispatcher_.join();
                } catch (...) {
                    return std::unexpected<thread_StatusCode>(Cleanup_Failure);
                }
            }
            return {};
        }

        std::expected<void, thread_StatusCode> execute_task(const task_handle& task) {
            try {
                task.get();
            } catch (...) {
                return std::unexpected<thread_StatusCode>(Failure);
            }
            return {};
        }
    };

}
