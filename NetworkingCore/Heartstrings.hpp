#pragma once

#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

namespace Heartstrings {

struct TLSConfiguration {
    int port = 0;
    std::string certificate_chain_file;
    std::string private_key_file;
    bool require_client_certificate = false;
    std::string client_ca_file;
};

// Compiled public API for the macOS Heartstrings static library.
// OpenSSL and the networking implementation remain private to the library.
class Server {
private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;

public:
    // Incomplete requests have a 30-second absolute read deadline by default.
    explicit Server(int port);
    // The request-read deadline must be positive; incoming bytes do not extend it.
    Server(int port, std::chrono::milliseconds request_read_timeout);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;

    std::expected<void, std::string> enable_tls(TLSConfiguration configuration);
    // Configure a positive HTTP+HTTPS connection cap before start(); default is 128.
    std::expected<void, std::string> set_maximum_connections(std::size_t maximum_connections);
    // Configure the positive lazy payload-memory budget before start(); default is 512 MiB.
    std::expected<void, std::string> set_memory_budget(std::size_t bytes);
    std::expected<void, std::string> File_Link(
        std::string route,
        std::filesystem::path file);
    std::expected<void, std::string> start();
    std::expected<void, std::string> run();
    void stop() noexcept;

    int port() const noexcept;
    int https_port() const noexcept;
};

} // namespace Heartstrings
