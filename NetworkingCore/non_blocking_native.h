#pragma once

// macOS networking implementation using non-blocking sockets and kqueue.

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <optional>
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

#include <openssl/err.h>
#include <openssl/ssl.h>

#include "../Http_Handling/HttpParser.hpp"
#include "../Http_Handling/HttpSerializer.h"
#include "../Subsystems/StateMachine.hpp"
#include "../Subsystems/Aegis.hpp"
#include "../Subsystems/ThreadPool.hpp"
#include "ApplicationHandler.hpp"

struct TLSConfiguration {
    int port = 0;
    std::string certificate_chain_file;
    std::string private_key_file;
    bool require_client_certificate = false;
    std::string client_ca_file;
};

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
    enum class connection_state {
        ReadingHeaders,
        KeepAlive,
        Closing
    };

    enum class connection_event {
        RequestParsed,
        ResponseCompleted,
        CloseRequested
    };

    struct client_connection {
        using state_machine_type =
            Aegis_stateMachine::StateMachine<connection_state, connection_event>;

        int descriptor = -1;
        SSL* ssl_session = nullptr;
        bool tls_handshake_complete = false;
        bool write_interest = false;
        bool tls_write_wants_read = false;
        bool tls_read_wants_write = false;
        bool request_in_flight = false;
        bool close_after_response = false;
        bool peer_closed = false;
        std::string response_version = "HTTP/1.1";
        std::vector<std::uint8_t> request_bytes;
        std::optional<std::chrono::steady_clock::time_point> request_read_started;
        std::vector<std::uint8_t> response_bytes;
        std::size_t response_offset = 0;
        std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
        state_machine_type state_machine;

        client_connection() : state_machine(connection_state::ReadingHeaders) {
            auto reading_headers = connection_state::ReadingHeaders;
            auto keep_alive = connection_state::KeepAlive;
            auto closing = connection_state::Closing;
            state_machine.Register_Rules_With_Events(
                reading_headers, [] {}, connection_event::RequestParsed, keep_alive);
            state_machine.Register_Rules_With_Events(
                keep_alive, [] {}, connection_event::ResponseCompleted, reading_headers);
            state_machine.Register_Rules_With_Events(
                closing, [] {}, connection_event::CloseRequested, closing);
        }

        ~client_connection() {
            if (ssl_session != nullptr) {
                if (tls_handshake_complete) (void)SSL_shutdown(ssl_session);
                SSL_free(ssl_session);
            }
            if (descriptor >= 0) close(descriptor);
        }
    };

    enum class handshake_state { complete, waiting, failed };

    static constexpr std::size_t maximum_request_size_ = 16 * 1024 * 1024;
    static constexpr std::size_t default_maximum_connections_ = 128;
    static constexpr std::size_t maximum_events_ = 64;
    static constexpr uintptr_t response_ready_event_id_ = 1;
    static constexpr auto keep_alive_idle_timeout_ = std::chrono::seconds(30);

    int requested_port_ = 0;
    std::chrono::milliseconds request_read_timeout_ = std::chrono::seconds(30);
    std::size_t maximum_connections_ = default_maximum_connections_;
    int bound_port_ = 0;
    int listener_socket_ = -1;
    int bound_tls_port_ = 0;
    int tls_listener_socket_ = -1;
    int kqueue_descriptor_ = -1;
    SSL_CTX* tls_context_ = nullptr;
    std::optional<TLSConfiguration> tls_configuration_;
    bool started_ = false;
    bool run_finished_ = false;
    std::atomic<bool> stop_requested_ = false;
    std::array<struct kevent, maximum_events_> events_{};
    std::unordered_map<int, std::shared_ptr<client_connection>> clients_;
    ApplicationHandler application_handler_;
    struct response_completion {
        std::shared_ptr<client_connection> connection;
        httpresponse response;
    };
    std::mutex completion_mutex_;
    std::deque<response_completion> response_completions_;

    // Both client-request and parsed-request work shares the same three slots.
    Aegis_ThreadPool::thread_pool request_task_pool_{3};
    std::vector<Aegis_ThreadPool::thread_pool::task_handle> client_request_tasks_;

    static std::string last_socket_error(const std::string& operation) {
        return operation + ": " + std::error_code(errno, std::generic_category()).message();
    }

    static std::string last_openssl_error(const std::string& operation) {
        const unsigned long error_code = ERR_get_error();
        if (error_code == 0) return operation + ": OpenSSL operation failed";

        std::array<char, 256> error_message{};
        ERR_error_string_n(error_code, error_message.data(), error_message.size());
        return operation + ": " + error_message.data();
    }

    static bool set_nonblocking_descriptor(int descriptor) {
        const int flags = fcntl(descriptor, F_GETFL, 0);
        return flags != -1 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != -1;
    }

    static std::expected<SSL_CTX*, std::string> create_tls_context(const TLSConfiguration& configuration) {
        if (configuration.certificate_chain_file.empty() || configuration.private_key_file.empty()) {
            return std::unexpected<std::string>("TLS requires a certificate chain and private key file");
        }
        if (configuration.require_client_certificate && configuration.client_ca_file.empty()) {
            return std::unexpected<std::string>("Client certificate verification requires a CA file");
        }

        ERR_clear_error();
        SSL_CTX* context = SSL_CTX_new(TLS_server_method());
        if (context == nullptr) {
            return std::unexpected<std::string>(last_openssl_error("create TLS context"));
        }

        const auto fail = [context](const std::string& message) -> std::expected<SSL_CTX*, std::string> {
            SSL_CTX_free(context);
            return std::unexpected<std::string>(message);
        };

        if (SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION) != 1) {
            return fail(last_openssl_error("set minimum TLS version"));
        }
        if (SSL_CTX_use_certificate_chain_file(context, configuration.certificate_chain_file.c_str()) != 1) {
            return fail(last_openssl_error("load TLS certificate chain"));
        }
        if (SSL_CTX_use_PrivateKey_file(
                context, configuration.private_key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            return fail(last_openssl_error("load TLS private key"));
        }
        if (SSL_CTX_check_private_key(context) != 1) {
            return fail(last_openssl_error("verify TLS certificate and private key"));
        }

        if (!configuration.client_ca_file.empty()) {
            if (SSL_CTX_load_verify_locations(context, configuration.client_ca_file.c_str(), nullptr) != 1) {
                return fail(last_openssl_error("load client certificate CA file"));
            }
            STACK_OF(X509_NAME)* acceptable_client_cas =
                SSL_load_client_CA_file(configuration.client_ca_file.c_str());
            if (acceptable_client_cas == nullptr) {
                return fail(last_openssl_error("load acceptable client certificate names"));
            }
            SSL_CTX_set_client_CA_list(context, acceptable_client_cas);

            int verification_mode = SSL_VERIFY_PEER;
            if (configuration.require_client_certificate) {
                verification_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            }
            SSL_CTX_set_verify(context, verification_mode, nullptr);
        } else {
            SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        }

        SSL_CTX_set_mode(context, SSL_MODE_ENABLE_PARTIAL_WRITE);
        return context;
    }

    static std::expected<int, std::string> create_listener(int port, int& actual_port) {
        const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor == -1) {
            return std::unexpected<std::string>(last_socket_error("socket"));
        }

        int reuse_address = 1;
        if (setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)) == -1) {
            const std::string error = last_socket_error("setsockopt SO_REUSEADDR");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }
        if (!set_nonblocking_descriptor(descriptor)) {
            const std::string error = last_socket_error("fcntl listener non-blocking");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }

        sockaddr_in listen_address{};
        listen_address.sin_family = AF_INET;
        listen_address.sin_addr.s_addr = htonl(INADDR_ANY);
        listen_address.sin_port = htons(static_cast<std::uint16_t>(port));
        if (bind(descriptor, reinterpret_cast<sockaddr*>(&listen_address), sizeof(listen_address)) == -1) {
            const std::string error = last_socket_error("bind");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }
        if (listen(descriptor, SOMAXCONN) == -1) {
            const std::string error = last_socket_error("listen");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }

        sockaddr_in bound_address{};
        socklen_t address_length = sizeof(bound_address);
        if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&bound_address), &address_length) == -1) {
            const std::string error = last_socket_error("getsockname");
            close(descriptor);
            return std::unexpected<std::string>(error);
        }
        actual_port = ntohs(bound_address.sin_port);
        return descriptor;
    }

    std::expected<void, std::string> register_read_event(int descriptor) {
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(descriptor), EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        if (kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr) == -1) {
            return std::unexpected<std::string>(last_socket_error("kevent register read"));
        }
        return {};
    }

    std::expected<void, std::string> set_client_write_interest(
        const std::shared_ptr<client_connection>& connection,
        bool enabled) {
        if (connection->write_interest == enabled) return {};

        struct kevent change{};
        if (enabled) {
            EV_SET(&change, static_cast<uintptr_t>(connection->descriptor),
                EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
        } else {
            EV_SET(&change, static_cast<uintptr_t>(connection->descriptor), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        }
        if (kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr) == -1) {
            if (!enabled && errno == ENOENT) {
                connection->write_interest = false;
                return {};
            }
            return std::unexpected<std::string>(last_socket_error("kevent update write interest"));
        }
        connection->write_interest = enabled;
        return {};
    }

    void remove_read_event(int descriptor) noexcept {
        if (kqueue_descriptor_ < 0) return;
        struct kevent change{};
        EV_SET(&change, static_cast<uintptr_t>(descriptor), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
        (void)kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr);
        EV_SET(&change, static_cast<uintptr_t>(descriptor), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
        (void)kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr);
    }

    std::expected<void, std::string> register_response_ready_event() {
        struct kevent change{};
        EV_SET(&change, response_ready_event_id_, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        if (kevent(kqueue_descriptor_, &change, 1, nullptr, 0, nullptr) == -1) {
            return std::unexpected<std::string>(last_socket_error("kevent register response event"));
        }
        return {};
    }

    void trigger_response_ready_event() noexcept {
        if (kqueue_descriptor_ < 0) return;
        struct kevent trigger{};
        EV_SET(&trigger, response_ready_event_id_, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        (void)kevent(kqueue_descriptor_, &trigger, 1, nullptr, 0, nullptr);
    }

    void enqueue_response_completion(
        const std::shared_ptr<client_connection>& connection,
        httpresponse response) {
        {
            std::lock_guard lock(completion_mutex_);
            response_completions_.push_back({connection, std::move(response)});
        }
        trigger_response_ready_event();
    }

    static bool has_connection_token(const HTTPRequest& request, std::string_view token) {
        const auto connection_header = request.headers.find("connection");
        if (connection_header == request.headers.end()) return false;

        for (const std::string_view value : string_utils::split(connection_header->second, ',')) {
            if (string_utils::to_lower(string_utils::trim_ows(value)) == token) return true;
        }
        return false;
    }

    static bool request_allows_keep_alive(const HTTPRequest& request) {
        if (has_connection_token(request, "close")) return false;
        if (request.version == "HTTP/1.1") return true;
        return has_connection_token(request, "keep-alive");
    }

    bool transition_to_keep_alive(const std::shared_ptr<client_connection>& connection) {
        auto state = connection_state::ReadingHeaders;
        return connection->state_machine.Transition(state).has_value();
    }

    bool transition_to_reading_headers(const std::shared_ptr<client_connection>& connection) {
        auto state = connection_state::KeepAlive;
        return connection->state_machine.Transition(state).has_value();
    }

    void mark_connection_closing(const std::shared_ptr<client_connection>& connection) {
        auto state = connection_state::Closing;
        (void)connection->state_machine.Store_State(state);
    }

    void accept_ready_clients(int listener_descriptor, bool use_tls) {
        while (true) {
            sockaddr_in client_address{};
            socklen_t address_length = sizeof(client_address);
            const int client_socket = accept(
                listener_descriptor, reinterpret_cast<sockaddr*>(&client_address), &address_length);

            if (client_socket == -1) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                return;
            }

            // Count every registered client, including connections still in a TLS handshake.
            // Reject before allocating connection state or an OpenSSL session.
            if (clients_.size() >= maximum_connections_) {
                close(client_socket);
                continue;
            }

            int no_sigpipe = 1;
            (void)setsockopt(client_socket, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
            if (!set_nonblocking_descriptor(client_socket)) {
                close(client_socket);
                continue;
            }

            std::shared_ptr<client_connection> connection;
            try {
                connection = std::make_shared<client_connection>();
            } catch (...) {
                close(client_socket);
                continue;
            }
            connection->descriptor = client_socket;

            if (use_tls) {
                ERR_clear_error();
                connection->ssl_session = SSL_new(tls_context_);
                if (connection->ssl_session == nullptr ||
                    SSL_set_fd(connection->ssl_session, client_socket) != 1) {
                    continue;
                }
                int verification_mode = SSL_VERIFY_NONE;
                if (!tls_configuration_->client_ca_file.empty()) {
                    verification_mode = SSL_VERIFY_PEER;
                    if (tls_configuration_->require_client_certificate) {
                        verification_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
                    }
                }
                SSL_set_verify(connection->ssl_session, verification_mode, nullptr);
                SSL_set_accept_state(connection->ssl_session);
            }

            if (auto registration = register_read_event(client_socket); !registration) {
                continue;
            }
            clients_.insert_or_assign(client_socket, std::move(connection));
        }
    }

    handshake_state advance_tls_handshake(const std::shared_ptr<client_connection>& connection) {
        ERR_clear_error();
        const int result = SSL_accept(connection->ssl_session);
        if (result == 1) {
            if (tls_configuration_ && tls_configuration_->require_client_certificate) {
                X509* client_certificate = SSL_get1_peer_certificate(connection->ssl_session);
                if (client_certificate == nullptr) return handshake_state::failed;
                X509_free(client_certificate);
            }
            if (SSL_get_verify_result(connection->ssl_session) != X509_V_OK) {
                return handshake_state::failed;
            }
            connection->tls_handshake_complete = true;
            if (!set_client_write_interest(connection, false)) return handshake_state::failed;
            return handshake_state::complete;
        }

        const int ssl_error = SSL_get_error(connection->ssl_session, result);
        if (ssl_error == SSL_ERROR_WANT_READ) {
            return set_client_write_interest(connection, false)
                ? handshake_state::waiting
                : handshake_state::failed;
        }
        if (ssl_error == SSL_ERROR_WANT_WRITE) {
            return set_client_write_interest(connection, true)
                ? handshake_state::waiting
                : handshake_state::failed;
        }
        return handshake_state::failed;
    }

    void close_client(int descriptor) noexcept {
        remove_read_event(descriptor);
        const auto client = clients_.find(descriptor);
        if (client != clients_.end()) {
            mark_connection_closing(client->second);
            clients_.erase(client);
        }
    }

    void enqueue_immediate_response(
        const std::shared_ptr<client_connection>& connection,
        httpresponse response,
        bool close_after_response) {
        if (!transition_to_keep_alive(connection)) {
            close_client(connection->descriptor);
            return;
        }
        connection->request_in_flight = true;
        connection->close_after_response = close_after_response;
        enqueue_response_completion(connection, std::move(response));
    }

    void dispatch_request(
        const std::shared_ptr<client_connection>& connection,
        HTTPRequest request) {
        if (!transition_to_keep_alive(connection)) {
            close_client(connection->descriptor);
            return;
        }

        connection->request_in_flight = true;
        connection->close_after_response =
            !request_allows_keep_alive(request) || (connection->peer_closed && connection->request_bytes.empty());
        connection->response_version = request.version;

        auto submission = request_task_pool_.Insert_Into_thread_pool(
            [this, connection, request = std::move(request)]() mutable {
                httpresponse response;
                try {
                    AegisHttpRequestStorage request_body_storage;
                    auto stored_body = request_body_storage.store(request.body);
                    if (!stored_body) {
                        response = httpresponse{
                            .statusCode = 500,
                            .statusReason = "Internal Server Error",
                            .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                            .body = "Request storage failed"
                        };
                    } else {
                        auto restored_body = request_body_storage.retrieve();
                        if (!restored_body) {
                            response = httpresponse{
                                .statusCode = 500,
                                .statusReason = "Internal Server Error",
                                .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                                .body = "Request storage failed"
                            };
                        } else {
                            request.body = std::move(*restored_body);
                            response = application_handler_.handle(request);
                        }
                    }
                } catch (...) {
                    response = httpresponse{
                        .statusCode = 500,
                        .statusReason = "Internal Server Error",
                        .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                        .body = "Application handler failed"
                    };
                }

                enqueue_response_completion(connection, std::move(response));
            });

        if (!submission) {
            connection->close_after_response = true;
            enqueue_response_completion(connection, httpresponse{
                .statusCode = 503,
                .statusReason = "Service Unavailable",
                .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                .body = "Server is shutting down"
            });
            return;
        }
        client_request_tasks_.push_back(*submission);
    }

    void process_buffered_request(
        const std::shared_ptr<client_connection>& connection,
        bool peer_closed) {
        if (connection->request_in_flight || !connection->response_bytes.empty()) return;
        if (connection->request_bytes.empty()) {
            connection->request_read_started.reset();
            if (peer_closed) close_client(connection->descriptor);
            return;
        }

        HTTPParser parser(connection->request_bytes);
        auto parsed_request = parser.parse();
        if (!parsed_request) {
            if (parsed_request.error() == HTTPParserError::UnexpectedEOF && !peer_closed) return;
            connection->request_read_started.reset();

            httpSerializer serializer;
            const auto [status_code, reason] = serializer.findTextWithCode(parsed_request.error());
            enqueue_immediate_response(connection,
                httpresponse{
                    .statusCode = status_code,
                    .statusReason = reason,
                    .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                    .body = reason
                }, true);
            return;
        }

        const std::size_t consumed_bytes = parser.bytesConsumed();
        connection->request_read_started.reset();
        connection->request_bytes.erase(
            connection->request_bytes.begin(),
            connection->request_bytes.begin() + static_cast<std::ptrdiff_t>(consumed_bytes));
        dispatch_request(connection, std::move(*parsed_request));
    }

    void handle_plain_client_read(const std::shared_ptr<client_connection>& connection, bool peer_closed) {
        std::array<std::uint8_t, 8192> read_buffer{};
        while (true) {
            const ssize_t bytes_read = recv(
                connection->descriptor, read_buffer.data(), read_buffer.size(), 0);
            if (bytes_read > 0) {
                const std::size_t incoming_size = static_cast<std::size_t>(bytes_read);
                if (incoming_size > maximum_request_size_ - connection->request_bytes.size()) {
                    enqueue_immediate_response(connection,
                        httpresponse{
                            .statusCode = 413,
                            .statusReason = "Payload Too Large",
                            .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                            .body = "Payload Too Large"
                    }, true);
                    return;
                }
                if (!connection->request_in_flight && connection->request_bytes.empty() &&
                    !connection->request_read_started) {
                    connection->request_read_started = std::chrono::steady_clock::now();
                }
                connection->request_bytes.insert(
                    connection->request_bytes.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);
                connection->last_activity = std::chrono::steady_clock::now();
                if (!connection->request_in_flight) process_buffered_request(connection, false);
                continue;
            }
            if (bytes_read == 0) {
                peer_closed = true;
                break;
            }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_client(connection->descriptor);
            return;
        }

        if (peer_closed) {
            connection->peer_closed = true;
            if (connection->request_in_flight) {
                if (connection->request_bytes.empty()) connection->close_after_response = true;
            } else {
                process_buffered_request(connection, true);
            }
        }
    }

    void handle_tls_client_read(const std::shared_ptr<client_connection>& connection, bool peer_closed) {
        std::array<std::uint8_t, 8192> read_buffer{};
        while (true) {
            std::size_t bytes_read = 0;
            ERR_clear_error();
            const int result = SSL_read_ex(
                connection->ssl_session, read_buffer.data(), read_buffer.size(), &bytes_read);
            if (result == 1 && bytes_read > 0) {
                if (bytes_read > maximum_request_size_ - connection->request_bytes.size()) {
                    enqueue_immediate_response(connection,
                        httpresponse{
                            .statusCode = 413,
                            .statusReason = "Payload Too Large",
                            .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                            .body = "Payload Too Large"
                    }, true);
                    return;
                }
                if (!connection->request_in_flight && connection->request_bytes.empty() &&
                    !connection->request_read_started) {
                    connection->request_read_started = std::chrono::steady_clock::now();
                }
                connection->request_bytes.insert(
                    connection->request_bytes.end(), read_buffer.begin(), read_buffer.begin() + bytes_read);
                connection->last_activity = std::chrono::steady_clock::now();
                if (!connection->request_in_flight) process_buffered_request(connection, false);
                if (SSL_pending(connection->ssl_session) > 0) continue;
                continue;
            }

            const int ssl_error = SSL_get_error(connection->ssl_session, result);
            if (ssl_error == SSL_ERROR_WANT_READ) {
                connection->tls_read_wants_write = false;
                connection->tls_write_wants_read = false;
                (void)set_client_write_interest(connection, false);
                break;
            }
            if (ssl_error == SSL_ERROR_WANT_WRITE) {
                connection->tls_read_wants_write = true;
                if (!set_client_write_interest(connection, true)) close_client(connection->descriptor);
                return;
            }
            if (ssl_error == SSL_ERROR_ZERO_RETURN) {
                peer_closed = true;
                break;
            }
            if (ssl_error == SSL_ERROR_SYSCALL && errno == EINTR) continue;
            close_client(connection->descriptor);
            return;
        }

        if (peer_closed) {
            connection->peer_closed = true;
            if (connection->request_in_flight) {
                if (connection->request_bytes.empty()) connection->close_after_response = true;
            } else {
                process_buffered_request(connection, true);
            }
        }
    }

    void finish_response_write(const std::shared_ptr<client_connection>& connection) {
        connection->response_bytes.clear();
        connection->response_offset = 0;
        connection->tls_write_wants_read = false;
        (void)set_client_write_interest(connection, false);
        connection->last_activity = std::chrono::steady_clock::now();

        if (connection->close_after_response ||
            (connection->peer_closed && connection->request_bytes.empty())) {
            mark_connection_closing(connection);
            close_client(connection->descriptor);
            return;
        }

        if (!transition_to_reading_headers(connection)) {
            close_client(connection->descriptor);
            return;
        }
        connection->request_in_flight = false;
        connection->close_after_response = false;
        connection->request_read_started = connection->request_bytes.empty()
            ? std::nullopt
            : std::optional(std::chrono::steady_clock::now());
        process_buffered_request(connection, connection->peer_closed);
    }

    void handle_client_write(const std::shared_ptr<client_connection>& connection) {
        while (connection->response_offset < connection->response_bytes.size()) {
            if (connection->ssl_session != nullptr) {
                std::size_t bytes_written = 0;
                ERR_clear_error();
                const int result = SSL_write_ex(
                    connection->ssl_session,
                    connection->response_bytes.data() + connection->response_offset,
                    connection->response_bytes.size() - connection->response_offset,
                    &bytes_written);
                if (result == 1 && bytes_written > 0) {
                    connection->response_offset += bytes_written;
                    connection->tls_write_wants_read = false;
                    connection->last_activity = std::chrono::steady_clock::now();
                    continue;
                }

                const int ssl_error = SSL_get_error(connection->ssl_session, result);
                if (ssl_error == SSL_ERROR_WANT_READ) {
                    connection->tls_write_wants_read = true;
                    (void)set_client_write_interest(connection, false);
                    return;
                }
                if (ssl_error == SSL_ERROR_WANT_WRITE) {
                    connection->tls_write_wants_read = false;
                    if (!set_client_write_interest(connection, true)) close_client(connection->descriptor);
                    return;
                }
                if (ssl_error == SSL_ERROR_SYSCALL && errno == EINTR) continue;
                close_client(connection->descriptor);
                return;
            }

            const ssize_t written = send(
                connection->descriptor,
                connection->response_bytes.data() + connection->response_offset,
                connection->response_bytes.size() - connection->response_offset,
                0);
            if (written > 0) {
                connection->response_offset += static_cast<std::size_t>(written);
                connection->last_activity = std::chrono::steady_clock::now();
                continue;
            }
            if (written == -1 && errno == EINTR) continue;
            if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!set_client_write_interest(connection, true)) close_client(connection->descriptor);
                return;
            }
            close_client(connection->descriptor);
            return;
        }

        finish_response_write(connection);
    }

    void handle_client_event(int descriptor, short filter, bool peer_closed) {
        const auto client = clients_.find(descriptor);
        if (client == clients_.end()) return;
        const std::shared_ptr<client_connection> connection = client->second;

        if (connection->ssl_session != nullptr && !connection->tls_handshake_complete) {
            if ((connection->write_interest && filter != EVFILT_WRITE) ||
                (!connection->write_interest && filter != EVFILT_READ)) {
                return;
            }
            const handshake_state state = advance_tls_handshake(connection);
            if (state == handshake_state::failed) {
                close_client(descriptor);
                return;
            }
            if (state == handshake_state::waiting) return;
        }

        if (!connection->response_bytes.empty()) {
            if (connection->ssl_session == nullptr) {
                if (filter == EVFILT_WRITE) handle_client_write(connection);
            } else if ((connection->tls_write_wants_read && filter == EVFILT_READ) ||
                       (connection->write_interest && filter == EVFILT_WRITE)) {
                handle_client_write(connection);
            }
            return;
        }

        if (connection->ssl_session == nullptr) {
            if (filter == EVFILT_READ) handle_plain_client_read(connection, peer_closed);
        } else if ((filter == EVFILT_READ && !connection->tls_read_wants_write) ||
                   (filter == EVFILT_WRITE && connection->tls_read_wants_write) ||
                   SSL_pending(connection->ssl_session) > 0) {
            handle_tls_client_read(connection, peer_closed);
        }
    }

    void drain_response_completions() {
        std::deque<response_completion> completions;
        {
            std::lock_guard lock(completion_mutex_);
            completions.swap(response_completions_);
        }

        for (auto& completion : completions) {
            const int descriptor = completion.connection->descriptor;
            const auto client = clients_.find(descriptor);
            if (client == clients_.end() || client->second != completion.connection) continue;

            httpresponse response = std::move(completion.response);
            response.httpVersion = completion.connection->response_version;
            response.headers.insert_or_assign(
                "Connection", completion.connection->close_after_response ? "close" : "keep-alive");
            httpSerializer serializer;
            if (auto serialized = serializer.Serialize(response); !serialized) {
                close_client(descriptor);
                continue;
            }
            completion.connection->response_bytes = serializer.ReturnSerializedResult();
            completion.connection->response_offset = 0;
            if (!set_client_write_interest(completion.connection, true)) {
                close_client(descriptor);
            }
        }
    }

    void expire_idle_clients() {
        const auto now = std::chrono::steady_clock::now();
        std::vector<int> expired_clients;
        std::vector<std::shared_ptr<client_connection>> timed_out_requests;
        for (const auto& [descriptor, connection] : clients_) {
            if (!connection->request_in_flight && connection->response_bytes.empty()) {
                if (connection->request_read_started &&
                    now - *connection->request_read_started >= request_read_timeout_) {
                    timed_out_requests.push_back(connection);
                } else if (connection->request_bytes.empty() &&
                           now - connection->last_activity >= keep_alive_idle_timeout_) {
                    expired_clients.push_back(descriptor);
                }
            }
        }
        for (const auto& connection : timed_out_requests) {
            enqueue_immediate_response(connection,
                httpresponse{
                    .statusCode = 408,
                    .statusReason = "Request Timeout",
                    .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
                    .body = "Request Timeout"
                }, true);
        }
        for (const int descriptor : expired_clients) close_client(descriptor);
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
        std::vector<int> descriptors;
        descriptors.reserve(clients_.size());
        for (const auto& [descriptor, connection] : clients_) {
            (void)connection;
            descriptors.push_back(descriptor);
        }
        for (const int descriptor : descriptors) close_client(descriptor);
    }

public:
    explicit socket_native(int port) : socket_native(port, std::chrono::seconds(30)) {}

    socket_native(int port, std::chrono::milliseconds request_read_timeout)
        : requested_port_(port), request_read_timeout_(request_read_timeout) {}

    socket_native(const socket_native&) = delete;
    socket_native& operator=(const socket_native&) = delete;

    ~socket_native() {
        stop();
        (void)request_task_pool_.shut_down();
        close_unclaimed_clients();
        if (listener_socket_ >= 0) close(listener_socket_);
        if (tls_listener_socket_ >= 0) close(tls_listener_socket_);
        if (kqueue_descriptor_ >= 0) close(kqueue_descriptor_);
        if (tls_context_ != nullptr) SSL_CTX_free(tls_context_);
    }

    using request_task_handle = Aegis_ThreadPool::thread_pool::task_handle;

    std::expected<void, std::string> enable_tls(TLSConfiguration configuration) {
        if (started_) return std::unexpected<std::string>("TLS must be configured before start()");
        if (configuration.port < 0 || configuration.port > 65535) {
            return std::unexpected<std::string>("TLS port must be between 0 and 65535");
        }
        if (configuration.certificate_chain_file.empty() || configuration.private_key_file.empty()) {
            return std::unexpected<std::string>("TLS requires a certificate chain and private key file");
        }
        if (configuration.require_client_certificate && configuration.client_ca_file.empty()) {
            return std::unexpected<std::string>("Client certificate verification requires a CA file");
        }
        tls_configuration_ = std::move(configuration);
        return {};
    }

    std::expected<void, std::string> start() {
        if (started_) return std::unexpected<std::string>("Server has already been started");
        if (request_read_timeout_ <= std::chrono::milliseconds::zero()) {
            return std::unexpected<std::string>("Request read timeout must be greater than zero");
        }
        if (requested_port_ < 0 || requested_port_ > 65535) {
            return std::unexpected<std::string>("Port must be between 0 and 65535");
        }

        if (tls_configuration_) {
            auto context_result = create_tls_context(*tls_configuration_);
            if (!context_result) return std::unexpected<std::string>(context_result.error());
            tls_context_ = *context_result;
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
            auto https_listener = create_listener(tls_configuration_->port, bound_tls_port_);
            if (!https_listener) {
                close(listener_socket_);
                listener_socket_ = -1;
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
                return std::unexpected<std::string>(https_listener.error());
            }
            tls_listener_socket_ = *https_listener;
        }

        kqueue_descriptor_ = kqueue();
        if (kqueue_descriptor_ == -1) {
            const std::string error = last_socket_error("kqueue");
            close(listener_socket_);
            listener_socket_ = -1;
            if (tls_listener_socket_ >= 0) {
                close(tls_listener_socket_);
                tls_listener_socket_ = -1;
            }
            if (tls_context_ != nullptr) {
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
            }
            return std::unexpected<std::string>(error);
        }

        if (auto registration = register_read_event(listener_socket_); !registration) {
            close(kqueue_descriptor_);
            close(listener_socket_);
            kqueue_descriptor_ = -1;
            listener_socket_ = -1;
            if (tls_listener_socket_ >= 0) {
                close(tls_listener_socket_);
                tls_listener_socket_ = -1;
            }
            if (tls_context_ != nullptr) {
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
            }
            return std::unexpected<std::string>(registration.error());
        }

        if (tls_listener_socket_ >= 0) {
            if (auto registration = register_read_event(tls_listener_socket_); !registration) {
                close(kqueue_descriptor_);
                close(listener_socket_);
                close(tls_listener_socket_);
                kqueue_descriptor_ = -1;
                listener_socket_ = -1;
                tls_listener_socket_ = -1;
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
                return std::unexpected<std::string>(registration.error());
            }
        }

        if (auto registration = register_response_ready_event(); !registration) {
            close(kqueue_descriptor_);
            close(listener_socket_);
            kqueue_descriptor_ = -1;
            listener_socket_ = -1;
            if (tls_listener_socket_ >= 0) {
                close(tls_listener_socket_);
                tls_listener_socket_ = -1;
            }
            if (tls_context_ != nullptr) {
                SSL_CTX_free(tls_context_);
                tls_context_ = nullptr;
            }
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

                if (event.filter == EVFILT_USER && event.ident == response_ready_event_id_) {
                    drain_response_completions();
                    continue;
                }

                if ((event.flags & EV_ERROR) && event.data != 0) {
                    if (descriptor == listener_socket_ || descriptor == tls_listener_socket_) {
                        run_error = "kqueue listener event failed: " + std::to_string(event.data);
                        stop_requested_.store(true);
                    } else {
                        close_client(descriptor);
                    }
                    continue;
                }
                if (descriptor == listener_socket_ && event.filter == EVFILT_READ) {
                    accept_ready_clients(listener_socket_, false);
                } else if (descriptor == tls_listener_socket_ && event.filter == EVFILT_READ) {
                    accept_ready_clients(tls_listener_socket_, true);
                } else if (event.filter == EVFILT_READ || event.filter == EVFILT_WRITE) {
                    handle_client_event(descriptor, event.filter, (event.flags & EV_EOF) != 0);
                }
            }
            drain_response_completions();
            expire_idle_clients();
            reap_finished_client_tasks(false);
        }

        close_unclaimed_clients();
        reap_finished_client_tasks(true);
        auto shutdown_result = request_task_pool_.shut_down();
        {
            std::lock_guard lock(completion_mutex_);
            response_completions_.clear();
        }
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

    int https_port() const noexcept {
        return bound_tls_port_;
    }

    std::expected<void, std::string> set_maximum_connections(std::size_t maximum_connections) {
        if (started_) {
            return std::unexpected<std::string>(
                "Maximum connections must be configured before start()");
        }
        if (maximum_connections == 0) {
            return std::unexpected<std::string>(
                "Maximum connections must be greater than zero");
        }
        maximum_connections_ = maximum_connections;
        return {};
    }

    std::expected<void, std::string> File_Link(
        std::string route,
        std::filesystem::path file) {
        return application_handler_.File_Link(std::move(route), std::move(file));
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
