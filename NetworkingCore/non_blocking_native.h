#pragma once

// macOS networking implementation using non-blocking sockets and kqueue.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <fcntl.h>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <system_error>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unistd.h>

#include "../Http_Handling/HttpParser.hpp"
#include "../Http_Handling/HttpSerializer.h"
#include "../Subsystems/Aegis.hpp"
#include "../Subsystems/ThreadPool.hpp"

// Owns one request's Aegis allocation. The normal allocator reads back whole
// 1 MiB chunks, so the original byte count is retained to return only request data.
class AegisHttpRequestStorage {
private:
    Aegis_MemoryManager::Aegis_allocator memory_manager_;
    mutable std::mutex mutex_;
    std::size_t allocation_index_ = 0;
    std::size_t byte_count_ = 0;
    bool storage_attempted_ = false;
    bool storage_succeeded_ = false;

public:
    AegisHttpRequestStorage() = default;
    AegisHttpRequestStorage(const AegisHttpRequestStorage&) = delete;
    AegisHttpRequestStorage& operator=(const AegisHttpRequestStorage&) = delete;

    std::expected<void, std::string> store(const std::vector<std::uint8_t>& request_data) {
        std::lock_guard lock(mutex_);
        if (storage_attempted_) {
            return std::unexpected<std::string>("Request data has already been stored");
        }
        storage_attempted_ = true;

        try {
            allocation_index_ = memory_manager_.allocateMemory_Optimized(request_data.size());
            auto write_result = memory_manager_.writeDataToMemory_Optimized(allocation_index_, request_data);
            if (!write_result) {
                return std::unexpected<std::string>(write_result.error());
            }
            byte_count_ = request_data.size();
            storage_succeeded_ = true;
            return {};
        } catch (const std::exception& error) {
            return std::unexpected<std::string>(error.what());
        }
    }

    std::expected<std::vector<std::uint8_t>, std::string> retrieve() {
        std::lock_guard lock(mutex_);
        if (!storage_succeeded_) {
            return std::unexpected<std::string>("No request data has been stored");
        }

        try {
            auto stored_bytes = memory_manager_.readData_Optimized(allocation_index_);
            if (!stored_bytes) {
                return std::unexpected<std::string>(stored_bytes.error());
            }
            if (stored_bytes->size() < byte_count_) {
                return std::unexpected<std::string>("Aegis returned fewer bytes than the request size");
            }

            std::vector<std::uint8_t> request_data;
            request_data.reserve(byte_count_);
            for (std::size_t index = 0; index < byte_count_; ++index) {
                request_data.push_back(std::to_integer<std::uint8_t>((*stored_bytes)[index]));
            }
            return request_data;
        } catch (const std::exception& error) {
            return std::unexpected<std::string>(error.what());
        }
    }
};

class socket_native {
private:
    struct client_connection {
        std::vector<std::uint8_t> request_bytes;
    };

    struct client_socket_guard {
        int descriptor = -1;

        ~client_socket_guard() {
            if (descriptor >= 0) close(descriptor);
        }

        int release() noexcept {
            const int released_descriptor = descriptor;
            descriptor = -1;
            return released_descriptor;
        }
    };

    static constexpr std::size_t maximum_request_size_ = 16 * 1024 * 1024;
    static constexpr std::size_t maximum_events_ = 64;

    int requested_port_ = 0;
    int bound_port_ = 0;
    int listener_socket_ = -1;
    int kqueue_descriptor_ = -1;
    bool started_ = false;
    bool run_finished_ = false;
    std::atomic<bool> stop_requested_ = false;
    std::array<struct kevent, maximum_events_> events_{};
    std::unordered_map<int, client_connection> clients_;

    // Both client-request and parsed-request work shares the same three slots.
    Aegis_ThreadPool::thread_pool request_task_pool_{3};
    std::vector<Aegis_ThreadPool::thread_pool::task_handle> client_request_tasks_;

    static std::string last_socket_error(const std::string& operation) {
        return operation + ": " + std::error_code(errno, std::generic_category()).message();
    }

    static bool set_nonblocking_descriptor(int descriptor) {
        const int flags = fcntl(descriptor, F_GETFL, 0);
        return flags != -1 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != -1;
    }

    std::expected<void, std::string> register_read_event(int descriptor) {
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(descriptor), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr) == -1) {
            return std::unexpected<std::string>(last_socket_error("kevent register read"));
        }
        return {};
    }

    void remove_read_event(int descriptor) noexcept {
        if (kqueue_descriptor_ < 0) return;
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(descriptor), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        (void)kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr);
    }

    static std::expected<void, std::string> write_all(int descriptor, const std::vector<std::uint8_t>& bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = send(descriptor, bytes.data() + offset, bytes.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written == -1 && errno == EINTR) continue;
            if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                struct pollfd writable{descriptor, POLLOUT, 0};
                const int poll_result = poll(&writable, 1, -1);
                if (poll_result > 0) continue;
                if (poll_result == -1 && errno == EINTR) continue;
            }
            return std::unexpected<std::string>(last_socket_error("send response"));
        }
        return {};
    }

    static std::expected<void, std::string> send_http_response(
        int descriptor,
        int status_code,
        std::string reason,
        std::string body) {
        httpSerializer serializer;
        httpresponse response{
            .statusCode = status_code,
            .statusReason = std::move(reason),
            .headers = {{"Connection", "close"}},
            .body = std::move(body)
        };
        if (auto serialized = serializer.Serialize(response); !serialized) {
            return std::unexpected<std::string>(serialized.error());
        }
        return write_all(descriptor, serializer.ReturnSerializedResult());
    }

    static std::string request_body_as_string(const Data& body) {
        std::string result;
        result.reserve(body.size());
        for (const std::uint8_t byte : body) result.push_back(static_cast<char>(byte));
        return result;
    }

    std::expected<std::shared_ptr<AegisHttpRequestStorage>, std::string>
    create_aegis_request_storage(const std::vector<std::uint8_t>& request_data) const {
        try {
            auto request_storage = std::make_shared<AegisHttpRequestStorage>();
            auto store_result = request_storage->store(request_data);
            if (!store_result) return std::unexpected<std::string>(store_result.error());
            return request_storage;
        } catch (const std::exception& error) {
            return std::unexpected<std::string>(error.what());
        }
    }

    void accept_ready_clients() {
        while (true) {
            sockaddr_in client_address{};
            socklen_t address_length = sizeof(client_address);
            const int client_socket = accept(
                listener_socket_, reinterpret_cast<sockaddr*>(&client_address), &address_length);

            if (client_socket == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }

            int no_sigpipe = 1;
            (void)setsockopt(client_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
            if (!set_nonblocking_descriptor(client_socket)) {
                close(client_socket);
                continue;
            }
            if (auto registration = register_read_event(client_socket); !registration) {
                close(client_socket);
                continue;
            }
            clients_.try_emplace(client_socket);
        }
    }

    void dispatch_complete_request(int descriptor, std::vector<std::uint8_t> raw_request) {
        remove_read_event(descriptor);
        clients_.erase(descriptor);

        auto submission = request_task_pool_.Insert_Into_thread_pool(
            [this, descriptor, raw_request = std::move(raw_request)]() mutable {
                client_socket_guard client{descriptor};
                auto request_storage = create_aegis_request_storage(raw_request);
                if (!request_storage) {
                    (void)send_http_response(descriptor, 500, "Internal Server Error", "Request storage failed");
                    return;
                }

                auto restored_request = (*request_storage)->retrieve();
                if (!restored_request) {
                    (void)send_http_response(descriptor, 500, "Internal Server Error", "Request storage failed");
                    return;
                }

                HTTPParser parser(std::move(*restored_request));
                auto parsed_request = parser.parse();
                if (!parsed_request) {
                    (void)send_http_response(descriptor, 400, "Bad Request", "Bad Request");
                    return;
                }

                auto parsed_submission = request_task_pool_.Insert_Into_thread_pool(
                    [descriptor, request = std::move(*parsed_request)]() mutable {
                        client_socket_guard response_client{descriptor};
                        AegisHttpRequestStorage body_storage;
                        if (auto stored = body_storage.store(request.body); !stored) {
                            (void)send_http_response(descriptor, 500, "Internal Server Error", "Request storage failed");
                            return;
                        }

                        auto echoed_body = body_storage.retrieve();
                        if (!echoed_body) {
                            (void)send_http_response(descriptor, 500, "Internal Server Error", "Request storage failed");
                            return;
                        }

                        (void)send_http_response(
                            descriptor, 200, "OK", request_body_as_string(*echoed_body));
                    });

                if (!parsed_submission) {
                    (void)send_http_response(descriptor, 503, "Service Unavailable", "Server is shutting down");
                    return;
                }
                (void)client.release();
            });

        if (!submission) {
            close(descriptor);
            return;
        }
        client_request_tasks_.push_back(*submission);
    }

    void close_client(int descriptor) noexcept {
        remove_read_event(descriptor);
        clients_.erase(descriptor);
        close(descriptor);
    }

    void handle_client_read(int descriptor, bool peer_closed) {
        auto client = clients_.find(descriptor);
        if (client == clients_.end()) return;

        std::array<std::uint8_t, 8192> read_buffer{};
        while (true) {
            const ssize_t bytes_read = recv(descriptor, read_buffer.data(), read_buffer.size(), 0);
            if (bytes_read > 0) {
                const std::size_t incoming_size = static_cast<std::size_t>(bytes_read);
                if (incoming_size > maximum_request_size_ - client->second.request_bytes.size()) {
                    close_client(descriptor);
                    return;
                }
                client->second.request_bytes.insert(
                    client->second.request_bytes.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);
                continue;
            }
            if (bytes_read == 0) {
                peer_closed = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(descriptor);
            return;
        }

        client = clients_.find(descriptor);
        if (client == clients_.end()) return;

        HTTPParser parser(client->second.request_bytes);
        auto parse_result = parser.parse();
        if (parse_result || parse_result.error() != HTTPParserError::UnexpectedEOF || peer_closed) {
            dispatch_complete_request(descriptor, std::move(client->second.request_bytes));
        }
    }

    void reap_finished_client_tasks(bool wait_for_all) {
        auto task = client_request_tasks_.begin();
        while (task != client_request_tasks_.end()) {
            if (wait_for_all) {
                (void)request_task_pool_.execute_task(*task);
                task = client_request_tasks_.erase(task);
                continue;
            }

            if (task->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                (void)request_task_pool_.execute_task(*task);
                task = client_request_tasks_.erase(task);
            } else {
                ++task;
            }
        }
    }

    void close_unclaimed_clients() noexcept {
        for (const auto& [descriptor, connection] : clients_) {
            (void)connection;
            remove_read_event(descriptor);
            close(descriptor);
        }
        clients_.clear();
    }

public:
    explicit socket_native(int port) : requested_port_(port) {}

    socket_native(const socket_native&) = delete;
    socket_native& operator=(const socket_native&) = delete;

    ~socket_native() {
        stop();
        (void)request_task_pool_.shut_down();
        close_unclaimed_clients();
        if (listener_socket_ >= 0) close(listener_socket_);
        if (kqueue_descriptor_ >= 0) close(kqueue_descriptor_);
    }

    using request_task_handle = Aegis_ThreadPool::thread_pool::task_handle;

    std::expected<void, std::string> start() {
        if (started_) return std::unexpected<std::string>("Server has already been started");
        if (requested_port_ < 0 || requested_port_ > 65535) {
            return std::unexpected<std::string>("Port must be between 0 and 65535");
        }

        listener_socket_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listener_socket_ == -1) {
            return std::unexpected<std::string>(last_socket_error("socket"));
        }

        int reuse_address = 1;
        if (setsockopt(listener_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) == -1) {
            const std::string error = last_socket_error("setsockopt SO_REUSEADDR");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }

        if (!set_nonblocking_descriptor(listener_socket_)) {
            const std::string error = last_socket_error("fcntl listener non-blocking");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }

        sockaddr_in listen_address{};
        listen_address.sin_family = AF_INET;
        listen_address.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_address.sin_port = htons(static_cast<std::uint16_t>(requested_port_));
        if (bind(listener_socket_, reinterpret_cast<sockaddr*>(&listen_address), sizeof(listen_address)) == -1) {
            const std::string error = last_socket_error("bind");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }

        if (listen(listener_socket_, SOMAXCONN) == -1) {
            const std::string error = last_socket_error("listen");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }

        sockaddr_in bound_address{};
        socklen_t bound_address_length = sizeof(bound_address);
        if (getsockname(listener_socket_, reinterpret_cast<sockaddr*>(&bound_address), &bound_address_length) == -1) {
            const std::string error = last_socket_error("getsockname");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }
        bound_port_ = ntohs(bound_address.sin_port);

        kqueue_descriptor_ = kqueue();
        if (kqueue_descriptor_ == -1) {
            const std::string error = last_socket_error("kqueue");
            close(listener_socket_);
            listener_socket_ = -1;
            return std::unexpected<std::string>(error);
        }

        if (auto registration = register_read_event(listener_socket_); !registration) {
            close(kqueue_descriptor_);
            close(listener_socket_);
            kqueue_descriptor_ = -1;
            listener_socket_ = -1;
            return std::unexpected<std::string>(registration.error());
        }

        started_ = true;
        stop_requested_.store(false);
        return {};
    }

    std::expected<void, std::string> run() {
        if (!started_) return std::unexpected<std::string>("Call start() before run()");
        if (run_finished_) return std::unexpected<std::string>("Server run loop can only run once");

        std::string run_error;
        while (!stop_requested_.load()) {
            timespec timeout{0, 100'000'000};
            const int event_count = kevent(
                kqueue_descriptor_, nullptr, 0, events_.data(), static_cast<int>(events_.size()), &timeout);
            if (event_count == -1) {
                if (errno == EINTR) continue;
                run_error = last_socket_error("kevent wait");
                break;
            }

            for (int event_index = 0; event_index < event_count; ++event_index) {
                const struct kevent& event = events_[static_cast<std::size_t>(event_index)];
                const int descriptor = static_cast<int>(event.ident);

                if ((event.flags & EV_ERROR) && event.data != 0) {
                    if (descriptor != listener_socket_) close_client(descriptor);
                    continue;
                }
                if (descriptor == listener_socket_ && event.filter == EVFILT_READ) {
                    accept_ready_clients();
                } else if (event.filter == EVFILT_READ) {
                    handle_client_read(descriptor, (event.flags & EV_EOF) != 0);
                }
            }
            reap_finished_client_tasks(false);
        }

        close_unclaimed_clients();
        reap_finished_client_tasks(true);
        auto shutdown_result = request_task_pool_.shut_down();
        run_finished_ = true;

        if (!run_error.empty()) return std::unexpected<std::string>(std::move(run_error));
        if (!shutdown_result) return std::unexpected<std::string>("Failed to drain request tasks");
        return {};
    }

    void stop() noexcept {
        stop_requested_.store(true);
    }

    int port() const noexcept {
        return bound_port_;
    }

    template<typename Task, typename... Args>
    std::expected<request_task_handle, Aegis_ThreadPool::thread_StatusCode>
    submit_client_request_task(Task&& task, Args&&... args) {
        return request_task_pool_.Insert_Into_thread_pool(
            std::forward<Task>(task), std::forward<Args>(args)...);
    }

    template<typename Task, typename... Args>
    std::expected<request_task_handle, Aegis_ThreadPool::thread_StatusCode>
    submit_parsed_request_task(Task&& task, Args&&... args) {
        return request_task_pool_.Insert_Into_thread_pool(
            std::forward<Task>(task), std::forward<Args>(args)...);
    }
};
