#include "Heartstrings.hpp"

#include "non_blocking_native.h"

#include <utility>

namespace Heartstrings {

class Server::Implementation {
public:
    explicit Implementation(int port) : server(port) {}

    socket_native server;
};

Server::Server(int port) : implementation_(std::make_unique<Implementation>(port)) {}

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
