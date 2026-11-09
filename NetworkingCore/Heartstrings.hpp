#pragma once

#include <chrono>
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
