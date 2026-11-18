#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <system_error>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <unistd.h>
#include <arpa/inet.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "../Http_Handling/HttpParser.hpp"
#include "../Http_Handling/HttpSerializer.h"
#include "../Subsystems/IPRateLimiter.hpp"
#include "ApplicationHandler.hpp"

struct TLSConfiguration {
    int port = 0;
    std::string certificate_chain_file;
    std::string private_key_file;
    bool require_client_certificate = false;
    std::string client_ca_file;
};

class socket_native {
private:
    static constexpr std::size_t maximum_request_size_ = 16 * 1024 * 1024;
    static constexpr std::size_t file_transfer_chunk_size_ = 64 * 1024;
    static constexpr std::size_t default_maximum_connections_ = 128;
    static constexpr std::size_t default_memory_budget_ = 512 * 1024 * 1024;
    static constexpr auto keep_alive_idle_timeout_ = std::chrono::seconds(30);

    int requested_port_ = 0;
    std::chrono::milliseconds request_read_timeout_ = std::chrono::seconds(30);
    std::size_t maximum_connections_ = default_maximum_connections_;
    std::size_t memory_budget_ = default_memory_budget_;
    int bound_port_ = 0;
    int listener_socket_ = -1;
    int bound_tls_port_ = 0;
    int tls_listener_socket_ = -1;
    SSL_CTX* tls_context_ = nullptr;
    std::optional<TLSConfiguration> tls_configuration_;
    std::atomic<bool> stop_requested_ = false;
    std::atomic<std::size_t> active_connections_ = 0;
    bool started_ = false;
    bool run_finished_ = false;
    ApplicationHandler application_handler_;
    IPRateLimiter ip_rate_limiter_;
    std::mutex clients_mutex_;
    std::unordered_set<int> active_sockets_;
    std::vector<std::jthread> client_threads_;

    static std::string socket_error(std::string_view operation) {
        return std::string(operation) + ": " +
            std::error_code(errno, std::generic_category()).message();
    }

    static std::expected<int, std::string> create_listener(int port, int& actual_port) {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor == -1) return std::unexpected<std::string>(socket_error("socket"));

        int reuse_address = 1;
        (void)setsockopt(
            descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1 ||
            listen(descriptor, SOMAXCONN) == -1) {
            const std::string error = socket_error("bind/listen");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }

        const int flags = fcntl(descriptor, F_GETFL, 0);
        if (flags == -1 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
            const std::string error = socket_error("set listener non-blocking");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }

        sockaddr_in bound_address{};
        socklen_t address_length = sizeof(bound_address);
        if (getsockname(
                descriptor, reinterpret_cast<sockaddr*>(&bound_address), &address_length) == -1) {
            const std::string error = socket_error("getsockname");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }
        actual_port = ntohs(bound_address.sin_port);
        return descriptor;
    }

    static std::expected<SSL_CTX*, std::string> create_tls_context(
        const TLSConfiguration& configuration) {
        SSL_CTX* context = SSL_CTX_new(TLS_server_method());
        if (context == nullptr) {
            return std::unexpected<std::string>("Could not create TLS context");
        }
        if (SSL_CTX_use_certificate_chain_file(
                context, configuration.certificate_chain_file.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(
                context, configuration.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(context) != 1) {
            SSL_CTX_free(context);
            return std::unexpected<std::string>("Could not load TLS certificate and private key");
        }
        if (configuration.require_client_certificate) {
            if (SSL_CTX_load_verify_locations(
                    context, configuration.client_ca_file.c_str(), nullptr) != 1) {
                SSL_CTX_free(context);
                return std::unexpected<std::string>("Could not load client CA file");
            }
            SSL_CTX_set_verify(
                context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
        }
        return context;
    }

    static bool has_token(std::string_view value, std::string_view token) {
        std::size_t begin = 0;
        while (begin <= value.size()) {
            const std::size_t end = value.find(',', begin);
            std::string part = string_utils::to_lower(
                string_utils::trim_ows(value.substr(
                    begin, end == std::string_view::npos ? value.size() - begin : end - begin)));
            if (part == token) return true;
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return false;
    }

    static bool request_allows_keep_alive(const HTTPRequest& request) {
        const auto connection = request.headers.find("connection");
        const std::string_view value = connection == request.headers.end()
            ? std::string_view{}
            : std::string_view(connection->second);
        if (request.version == "HTTP/1.1") return !has_token(value, "close");
        return request.version == "HTTP/1.0" && has_token(value, "keep-alive");
    }

    static bool wait_for_socket(int descriptor, short events, int timeout_ms) {
        pollfd event{descriptor, events, 0};
        while (true) {
            const int result = ::poll(&event, 1, timeout_ms);
            if (result > 0) return (event.revents & events) != 0;
            if (result == 0) return false;
            if (errno != EINTR) return false;
        }
    }

    static ssize_t read_client(int descriptor, SSL* session, void* buffer, std::size_t size) {
        if (session == nullptr) return recv(descriptor, buffer, size, 0);
        while (true) {
            const int result = SSL_read(
                session, buffer, static_cast<int>(std::min<std::size_t>(size, INT32_MAX)));
            if (result > 0) return result;
            const int ssl_error = SSL_get_error(session, result);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) return 0;
            if (ssl_error == SSL_ERROR_SYSCALL && errno == EINTR) return -1;
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                const short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                if (wait_for_socket(descriptor, events, 1000)) continue;
                errno = EAGAIN;
                return -1;
            }
            if (ssl_error != SSL_ERROR_SYSCALL || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                errno = EIO;
            }
            return -1;
        }
    }

    static ssize_t write_client(
        int descriptor, SSL* session, const void* buffer, std::size_t size) {
        if (session == nullptr) {
#if defined(MSG_NOSIGNAL)
            return send(descriptor, buffer, size, MSG_NOSIGNAL);
#else
            return send(descriptor, buffer, size, 0);
#endif
        }
        while (true) {
            const int result = SSL_write(
                session, buffer, static_cast<int>(std::min<std::size_t>(size, INT32_MAX)));
            if (result > 0) return result;
            const int ssl_error = SSL_get_error(session, result);
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                errno = EPIPE;
                return -1;
            }
            if (ssl_error == SSL_ERROR_SYSCALL && errno == EINTR) return -1;
            if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                const short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                if (wait_for_socket(descriptor, events, 1000)) continue;
                errno = EAGAIN;
                return -1;
            }
            if (ssl_error != SSL_ERROR_SYSCALL || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                errno = EIO;
            }
            return -1;
        }
    }

    static bool send_all(int descriptor, SSL* session, const void* data, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        std::size_t offset = 0;
        while (offset < size) {
            const ssize_t written = write_client(
                descriptor, session, bytes + offset, size - offset);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
                continue;
            }
            if (written == -1 && errno == EINTR) continue;
            if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
                wait_for_socket(descriptor, POLLOUT, 1000)) {
                continue;
            }
            return false;
        }
        return true;
    }

    static httpresponse parser_error_response(HTTPParserError error) {
        httpSerializer serializer;
        const auto [status_code, reason] = serializer.findTextWithCode(error);
        return httpresponse{
            .statusCode = status_code,
            .statusReason = reason,
            .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
            .body = reason
        };
    }

    bool send_application_response(
        int descriptor,
        SSL* session,
        const std::string& ip_address,
        const HTTPRequest& request,
        bool close_after_response) {
        if (!ip_rate_limiter_.reserve(ip_address)) {
            httpresponse response{
                .statusCode = 429,
                .statusReason = "Too Many Requests",
                .headers = {
                    {"Content-Type", "text/plain; charset=utf-8"},
                    {"Retry-After", "15"}
                },
                .body = "Too Many Requests"
            };
            response.httpVersion = request.version;
            response.headers.insert_or_assign(
                "Connection", close_after_response ? "close" : "keep-alive");
            httpSerializer serializer;
            if (!serializer.Serialize(response)) return false;
            const auto response_bytes = serializer.ReturnSerializedResult();
            return send_all(descriptor, session, response_bytes.data(), response_bytes.size());
        }

        ApplicationResponse application_response = application_handler_.handle(request);
        httpresponse response = std::move(application_response.response);
        if (!IPRateLimiter::is_success_status(response.statusCode)) {
            ip_rate_limiter_.release_reservation(ip_address);
        }
        response.httpVersion = request.version;
        response.headers.insert_or_assign(
            "Connection", close_after_response ? "close" : "keep-alive");
        httpSerializer serializer;
        if (!serializer.Serialize(response)) return false;
        const auto response_bytes = serializer.ReturnSerializedResult();
        if (!send_all(descriptor, session, response_bytes.data(), response_bytes.size())) {
            return false;
        }

        if (!application_response.file) return true;
        std::array<std::uint8_t, file_transfer_chunk_size_> chunk{};
        std::uint64_t file_offset = 0;
        const StreamedFileBody& file = *application_response.file;
        while (file_offset < file.size()) {
            const std::size_t amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(file.size() - file_offset, chunk.size()));
            ssize_t bytes_read;
            do {
                bytes_read = pread(
                    file.descriptor(), chunk.data(), amount, static_cast<off_t>(file_offset));
            } while (bytes_read == -1 && errno == EINTR);
            if (bytes_read <= 0 ||
                !send_all(descriptor, session, chunk.data(), static_cast<std::size_t>(bytes_read))) {
                return false;
            }
            file_offset += static_cast<std::uint64_t>(bytes_read);
        }
        return true;
    }

    void serve_client(int descriptor, bool use_tls, const std::string& ip_address) {
        SSL* session = nullptr;
        if (use_tls) {
            session = SSL_new(tls_context_);
            if (session == nullptr || SSL_set_fd(session, descriptor) != 1 ||
                SSL_accept(session) != 1) {
                if (session != nullptr) SSL_free(session);
                close(descriptor);
                forget_client(descriptor);
                return;
            }
        }

        timeval receive_timeout{1, 0};
        (void)setsockopt(
            descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
        timeval send_timeout{1, 0};
        (void)setsockopt(
            descriptor, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
        std::vector<std::uint8_t> request_bytes;
        std::optional<std::chrono::steady_clock::time_point> request_started;
        auto last_activity = std::chrono::steady_clock::now();
        std::array<std::uint8_t, 8192> read_buffer{};
        bool should_close = false;

        while (!should_close && !stop_requested_.load()) {
            if (!request_bytes.empty() && request_started &&
                std::chrono::steady_clock::now() - *request_started >= request_read_timeout_) {
                httpresponse timeout_response{
                    .statusCode = 408,
                    .statusReason = "Request Timeout",
                    .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                    .body = "Request Timeout"
                };
                timeout_response.headers["Connection"] = "close";
                httpSerializer serializer;
                if (serializer.Serialize(timeout_response)) {
                    const auto response_bytes = serializer.ReturnSerializedResult();
                    (void)send_all(
                        descriptor, session, response_bytes.data(), response_bytes.size());
                }
                break;
            }
            if (request_bytes.empty() &&
                std::chrono::steady_clock::now() - last_activity >= keep_alive_idle_timeout_) {
                break;
            }

            if (!request_bytes.empty()) {
                HTTPParser parser(request_bytes);
                auto parsed = parser.parse();
                if (parsed) {
                    const std::size_t consumed = parser.bytesConsumed();
                    const bool close_after_response = !request_allows_keep_alive(*parsed);
                    if (!send_application_response(
                            descriptor, session, ip_address, *parsed, close_after_response)) {
                        break;
                    }
                    request_bytes.erase(
                        request_bytes.begin(),
                        request_bytes.begin() + static_cast<std::ptrdiff_t>(consumed));
                    request_started = request_bytes.empty()
                        ? std::nullopt
                        : std::optional(std::chrono::steady_clock::now());
                    last_activity = std::chrono::steady_clock::now();
                    should_close = close_after_response;
                    continue;
                }
                if (parsed.error() != HTTPParserError::UnexpectedEOF) {
                    httpresponse response = parser_error_response(parsed.error());
                    response.headers["Connection"] = "close";
                    httpSerializer serializer;
                    if (serializer.Serialize(response)) {
                        const auto response_bytes = serializer.ReturnSerializedResult();
                        (void)send_all(
                            descriptor, session, response_bytes.data(), response_bytes.size());
                    }
                    break;
                }
            }

            const ssize_t bytes_read = read_client(
                descriptor, session, read_buffer.data(), read_buffer.size());
            if (bytes_read > 0) {
                if (request_bytes.empty()) request_started = std::chrono::steady_clock::now();
                request_bytes.insert(
                    request_bytes.end(), read_buffer.begin(),
                    read_buffer.begin() + bytes_read);
                last_activity = std::chrono::steady_clock::now();
                if (request_bytes.size() > maximum_request_size_ ||
                    request_bytes.size() > memory_budget_) {
                    httpresponse response{
                        .statusCode = request_bytes.size() > maximum_request_size_ ? 413 : 503,
                        .statusReason = request_bytes.size() > maximum_request_size_
                            ? "Payload Too Large"
                            : "Service Unavailable",
                        .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                        .body = request_bytes.size() > maximum_request_size_
                            ? "Payload Too Large"
                            : "Memory budget exhausted"
                    };
                    response.headers["Connection"] = "close";
                    httpSerializer serializer;
                    if (serializer.Serialize(response)) {
                        const auto response_bytes = serializer.ReturnSerializedResult();
                        (void)send_all(
                            descriptor, session, response_bytes.data(), response_bytes.size());
                    }
                    break;
                }
                continue;
            }
            if (bytes_read == 0) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }

        if (session != nullptr) {
            (void)SSL_shutdown(session);
            SSL_free(session);
        }
        shutdown(descriptor, SHUT_RDWR);
        close(descriptor);
        forget_client(descriptor);
    }

    void forget_client(int descriptor) {
        {
            std::lock_guard lock(clients_mutex_);
            active_sockets_.erase(descriptor);
        }
        active_connections_.fetch_sub(1);
    }

    void accept_clients(int listener, bool use_tls) {
        while (!stop_requested_.load()) {
            sockaddr_in client_address{};
            socklen_t address_length = sizeof(client_address);
            const int descriptor = accept(
                listener, reinterpret_cast<sockaddr*>(&client_address), &address_length);
            if (descriptor == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }
            const std::size_t active = active_connections_.fetch_add(1);
            if (active >= maximum_connections_) {
                active_connections_.fetch_sub(1);
                close(descriptor);
                continue;
            }
            {
                std::lock_guard lock(clients_mutex_);
                active_sockets_.insert(descriptor);
            }
            char ip_buffer[INET_ADDRSTRLEN]{};
            std::string ip_address = "unknown";
            if (inet_ntop(AF_INET, &client_address.sin_addr, ip_buffer, sizeof(ip_buffer)) != nullptr) {
                ip_address = ip_buffer;
            }
            client_threads_.emplace_back(
                [this, descriptor, use_tls, ip_address] {
                    serve_client(descriptor, use_tls, ip_address);
                });
        }
    }

public:
    explicit socket_native(int port) : requested_port_(port) {}

    socket_native(int port, std::chrono::milliseconds request_read_timeout)
        : requested_port_(port), request_read_timeout_(request_read_timeout) {}

    socket_native(const socket_native&) = delete;
    socket_native& operator=(const socket_native&) = delete;

    ~socket_native() {
        stop();
        if (listener_socket_ >= 0) close(listener_socket_);
        if (tls_listener_socket_ >= 0) close(tls_listener_socket_);
        if (tls_context_ != nullptr) SSL_CTX_free(tls_context_);
    }

    std::expected<void, std::string> enable_tls(TLSConfiguration configuration) {
        if (started_) return std::unexpected<std::string>("TLS must be configured before start()");
        if (configuration.port < 0 || configuration.port > 65535) {
            return std::unexpected<std::string>("TLS port must be between 0 and 65535");
        }
        if (configuration.certificate_chain_file.empty() ||
            configuration.private_key_file.empty()) {
            return std::unexpected<std::string>("TLS requires a certificate chain and private key file");
        }
        if (configuration.require_client_certificate &&
            configuration.client_ca_file.empty()) {
            return std::unexpected<std::string>("Client certificate verification requires a CA file");
        }
        tls_configuration_ = std::move(configuration);
        return {};
    }

    std::expected<void, std::string> set_maximum_connections(std::size_t limit) {
        if (started_) {
            return std::unexpected<std::string>(
                "Maximum connections must be configured before start()");
        }
        if (limit == 0) {
            return std::unexpected<std::string>("Maximum connections must be greater than zero");
        }
        maximum_connections_ = limit;
        return {};
    }

    std::expected<void, std::string> set_memory_budget(std::size_t bytes) {
        if (started_) {
            return std::unexpected<std::string>("Memory budget must be configured before start()");
        }
        if (bytes == 0) {
            return std::unexpected<std::string>("Memory budget must be greater than zero");
        }
        memory_budget_ = bytes;
        return {};
    }

    std::expected<void, std::string> File_Link(
        std::string route, std::filesystem::path file) {
        return application_handler_.File_Link(std::move(route), std::move(file));
    }

    std::expected<void, std::string> start() {
        if (started_) return std::unexpected<std::string>("Server has already been started");
        if (requested_port_ < 0 || requested_port_ > 65535) {
            return std::unexpected<std::string>("Port must be between 0 and 65535");
        }
        if (request_read_timeout_ <= std::chrono::milliseconds::zero()) {
            return std::unexpected<std::string>("Request read timeout must be greater than zero");
        }

        if (tls_configuration_) {
            auto context = create_tls_context(*tls_configuration_);
            if (!context) return std::unexpected<std::string>(context.error());
            tls_context_ = *context;
        }
        auto http_listener = create_listener(requested_port_, bound_port_);
        if (!http_listener) {
            if (tls_context_ != nullptr) {
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
            }
            return std::unexpected<std::string>(http_listener.error());
        }
        listener_socket_ = *http_listener;

        if (tls_configuration_) {
            auto tls_listener = create_listener(tls_configuration_->port, bound_tls_port_);
            if (!tls_listener) {
                close(listener_socket_);
                listener_socket_ = -1;
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
                return std::unexpected<std::string>(tls_listener.error());
            }
            tls_listener_socket_ = *tls_listener;
        }
        stop_requested_.store(false);
        started_ = true;
        return {};
    }

    std::expected<void, std::string> run() {
        if (!started_) return std::unexpected<std::string>("Call start() before run()");
        if (run_finished_) return std::unexpected<std::string>("Server run loop can only run once");

        std::string run_error;
        while (!stop_requested_.load()) {
            std::array<pollfd, 2> events{};
            nfds_t event_count = 0;
            events[event_count++] = {listener_socket_, POLLIN, 0};
            if (tls_listener_socket_ >= 0) {
                events[event_count++] = {tls_listener_socket_, POLLIN, 0};
            }
            const int result = ::poll(events.data(), event_count, 100);
            if (result == -1) {
                if (errno == EINTR) continue;
                run_error = socket_error("poll wait");
                break;
            }
            if (stop_requested_.load()) break;
            for (nfds_t index = 0; index < event_count; ++index) {
                if ((events[index].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                    run_error = "poll listener event failed";
                    stop_requested_.store(true);
                    break;
                }
                if ((events[index].revents & POLLIN) == 0) continue;
                accept_clients(
                    events[index].fd, events[index].fd == tls_listener_socket_);
            }
            if (!run_error.empty()) break;
        }

        {
            std::lock_guard lock(clients_mutex_);
            for (const int descriptor : active_sockets_) {
                shutdown(descriptor, SHUT_RDWR);
            }
        }
        client_threads_.clear();
        run_finished_ = true;
        if (!run_error.empty()) return std::unexpected<std::string>(std::move(run_error));
        return {};
    }

    void stop() noexcept {
        stop_requested_.store(true);
        if (listener_socket_ >= 0) shutdown(listener_socket_, SHUT_RDWR);
        if (tls_listener_socket_ >= 0) shutdown(tls_listener_socket_, SHUT_RDWR);
        std::lock_guard lock(clients_mutex_);
        for (const int descriptor : active_sockets_) shutdown(descriptor, SHUT_RDWR);
    }

    int port() const noexcept { return bound_port_; }
    int https_port() const noexcept { return bound_tls_port_; }
};
