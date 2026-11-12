#include "Heartstrings.hpp"

#include "non_blocking_native.h"

#include <utility>

namespace Heartstrings {

class Server::Implementation {
public:
    Implementation(int port, std::chrono::milliseconds request_read_timeout)
        : server(port, request_read_timeout) {}

    socket_native server;
};

Server::Server(int port)
    : Server(port, std::chrono::seconds(30)) {}

Server::Server(int port, std::chrono::milliseconds request_read_timeout)
    : implementation_(std::make_unique<Implementation>(port, request_read_timeout)) {}

Server::~Server() = default;
Server::Server(Server&&) noexcept = default;
Server& Server::operator=(Server&&) noexcept = default;

std::expected<void, std::string> Server::enable_tls(TLSConfiguration configuration) {
    ::TLSConfiguration internal_configuration{
        .port = configuration.port,
        .certificate_chain_file = std::move(configuration.certificate_chain_file),
        .private_key_file = std::move(configuration.private_key_file),
        .require_client_certificate = configuration.require_client_certificate,
        .client_ca_file = std::move(configuration.client_ca_file)
    };
    return implementation_->server.enable_tls(std::move(internal_configuration));
}

std::expected<void, std::string> Server::set_maximum_connections(std::size_t maximum_connections) {
    return implementation_->server.set_maximum_connections(maximum_connections);
}

std::expected<void, std::string> Server::set_memory_budget(std::size_t bytes) {
    return implementation_->server.set_memory_budget(bytes);
}

std::expected<void, std::string> Server::File_Link(
    std::string route,
    std::filesystem::path file) {
    return implementation_->server.File_Link(std::move(route), std::move(file));
}

std::expected<void, std::string> Server::start() {
    return implementation_->server.start();
}

std::expected<void, std::string> Server::run() {
    return implementation_->server.run();
}

void Server::stop() noexcept {
    if (implementation_) implementation_->server.stop();
}

int Server::port() const noexcept {
    return implementation_ ? implementation_->server.port() : 0;
}

int Server::https_port() const noexcept {
    return implementation_ ? implementation_->server.https_port() : 0;
}

} // namespace Heartstrings
