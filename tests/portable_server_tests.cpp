#include <array>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../NetworkingCore/Heartstrings.hpp"

TEST(PortableServer, streams_large_file_after_receive_backpressure) {
    std::string body(2 * 1024 * 1024 + 13, '\0');
    for (std::size_t index = 0; index < body.size(); ++index) {
        body[index] = static_cast<char>('a' + index % 26);
    }
    const auto file_path = std::filesystem::temp_directory_path() /
        ("heartstrings-portable-" + std::to_string(getpid()) + ".txt");
    struct file_cleanup {
        std::filesystem::path path;
        ~file_cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } cleanup{file_path};
    {
        std::ofstream file(file_path, std::ios::binary);
        ASSERT_TRUE(file.good());
        file << body;
        ASSERT_TRUE(file.good());
    }

    Heartstrings::Server server(0);
    ASSERT_TRUE(server.File_Link("/asset", file_path));
    ASSERT_TRUE(server.start());
    std::expected<void, std::string> run_result;
    std::jthread server_thread([&] { run_result = server.run(); });

    const int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client == -1) {
        server.stop();
        server_thread.join();
        FAIL() << "Could not create portable server test socket";
        return;
    }
    int receive_buffer_size = 4096;
    if (setsockopt(
            client, SOL_SOCKET, SO_RCVBUF, &receive_buffer_size, sizeof(receive_buffer_size)) != 0) {
        close(client);
        server.stop();
        server_thread.join();
        FAIL() << "Could not configure portable server receive buffer";
        return;
    }
    timeval timeout{3, 0};
    if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(client);
        server.stop();
        server_thread.join();
        FAIL() << "Could not configure portable server test timeout";
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(server.port()));
    if (connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        close(client);
        server.stop();
        server_thread.join();
        FAIL() << "Could not connect to portable server";
        return;
    }

    const std::string request =
        "GET /asset HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    std::size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t count = send(client, request.data() + sent, request.size() - sent, 0);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::string response;
    std::array<char, 16 * 1024> buffer{};
    while (sent == request.size()) {
        const ssize_t count = recv(client, buffer.data(), buffer.size(), 0);
        if (count > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            break;
        }
    }

    close(client);
    server.stop();
    server_thread.join();

    ASSERT_TRUE(run_result.has_value());
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("Content-Length: " + std::to_string(body.size())), std::string::npos);
    EXPECT_NE(response.find(body), std::string::npos);
}
