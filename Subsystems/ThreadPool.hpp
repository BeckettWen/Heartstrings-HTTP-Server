// this is the memory thread pool
#pragma once

#include <queue>
#include <thread>
#include <functional>
#include <any>

namespace Aegis_ThreadPool {

    enum thread_StatusCode: std::size_t {
        Success = 0,
        Failure = 1,
        Cleanup_Failure = 2
    };


    // the beginning of the thread pool
    class thread_pool {
    private:
        std::deque<std::jthread> threadPool;
        bool status_isShutDown = false;

    public:
        thread_pool(){}
        ~thread_pool(){}

        // create a thread into the thread pool
        template<typename type ,typename... Args>
        std::expected<std::jthread::id, thread_StatusCode> Insert_Into_thread_pool(type& task, Args&&... args) {
            try {
                threadPool.emplace_back(std::jthread( std::forward<type>(task), std::forward<Args>(args)...));
                return threadPool.back().get_id();
            } catch (const std::exception& e) {
                return std::unexpected<thread_StatusCode>(thread_StatusCode::Failure);
            }
        }

        bool isShutDown(){ return status_isShutDown; }

        std::size_t getQueueSize(){ return threadPool.size(); }

        std::expected<void, thread_StatusCode> shut_down() {
            try {
                while (!threadPool.empty()) {
                    threadPool.front().request_stop();
                    threadPool.pop_front();
                }
            }catch (std::exception& exception) {
                return std::unexpected<thread_StatusCode>(Cleanup_Failure);
            }

            status_isShutDown = true;
            return {};
        }


        std::expected<void, thread_StatusCode> execute_task(std::jthread::id& thread_id) {
            try {
                std::deque<std::jthread>::iterator find_result = threadPool.begin();
                while (find_result != threadPool.end()) {
                    if ((*find_result).get_id() == thread_id)
                        break;
                    else{ find_result++; }
                }

                if (find_result == threadPool.end() && (*find_result).get_id() != thread_id) {
                    throw std::out_of_range("Task Not Been Registered");
                }

                (*find_result).join();
            } catch (std::exception& exception) {
                return std::unexpected<thread_StatusCode>(Failure);
            }
        }

    };
}