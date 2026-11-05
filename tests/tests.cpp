//this is the test file of the http handler, include the http parser and serializer
#include <cstdint>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <expected>
#include <gtest/gtest.h>
#include <iostream>
#include <latch>
#include <mutex>
#include <ostream>
#include <string>
#include <thread>
#include <vector>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../Http_Handling/HttpSerializer.h"
#include "../Http_Handling/HttpParser.hpp"
#include "../Subsystems/Aegis.hpp"
#include "../Subsystems/StateMachine.hpp"
#include "../Subsystems/ThreadPool.hpp"
#include "../NetworkingCore/non_blocking_native.h"

TEST(unittests, serializertest){
    //preparation for the test
    std::vector<std::uint8_t> testHttpRequest = {}, finalizedRequest = {}, answer{};
    httpSerializer testserializer;
    std::string rawRequest =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";
    testHttpRequest.insert(testHttpRequest.end(), rawRequest.begin(), rawRequest.end());
    HTTPParser testparser(testHttpRequest);

    //the test function and the result
    std::string expectedSerialized =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";
    std::expected<void, std::string> testResult = testserializer.Serialize(testparser);


    // 1. Ensure serialization didn't return an error
    ASSERT_TRUE(testResult.has_value()) << "Serialization failed with error: " << testResult.error();

    auto result = testserializer.ReturnSerializedResult();
    finalizedRequest.insert(finalizedRequest.end(), result.begin(), result.end());
    answer.insert(answer.end(), expectedSerialized.begin(), expectedSerialized.end());

    // 2. Now check the actual output
    EXPECT_EQ(finalizedRequest, answer);
}

TEST(thread_pool_tests, queues_tasks_after_three_are_running){
    Aegis_ThreadPool::thread_pool pool;
    std::latch release_tasks(1);
    std::mutex started_mutex;
    std::condition_variable started_condition;
    std::atomic<int> started_tasks = 0;
    std::atomic<int> active_tasks = 0;
    std::atomic<int> maximum_active_tasks = 0;
    std::vector<Aegis_ThreadPool::thread_pool::task_handle> task_handles;

    for (int task_number = 0; task_number < 6; ++task_number) {
        auto submission = pool.Insert_Into_thread_pool([&] {
            const int active_count = active_tasks.fetch_add(1) + 1;
            int observed_maximum = maximum_active_tasks.load();
            while (active_count > observed_maximum &&
                   !maximum_active_tasks.compare_exchange_weak(observed_maximum, active_count)) {}

            started_tasks.fetch_add(1);
            started_condition.notify_all();
            release_tasks.wait();
            active_tasks.fetch_sub(1);
        });

        if (!submission) {
            release_tasks.count_down();
            FAIL() << "Could not submit task to the thread pool";
            return;
        }
        task_handles.push_back(*submission);
    }

    bool three_tasks_started = false;
    {
        std::unique_lock lock(started_mutex);
        three_tasks_started = started_condition.wait_for(
            lock, std::chrono::seconds(2), [&] { return started_tasks.load() >= 3; });
    }

    EXPECT_TRUE(three_tasks_started);
    EXPECT_EQ(pool.getActiveTaskCount(), 3);
    EXPECT_EQ(pool.getPendingTaskCount(), 3);
    EXPECT_EQ(pool.getQueueSize(), 6);

    release_tasks.count_down();
    for (const auto& task_handle : task_handles) {
        EXPECT_TRUE(pool.execute_task(task_handle).has_value());
    }

    EXPECT_EQ(started_tasks.load(), 6);
    EXPECT_EQ(maximum_active_tasks.load(), 3);
    EXPECT_EQ(active_tasks.load(), 0);
}

TEST(aegis_http_integration, request_survives_aegis_storage_and_parses){
    const std::string raw_request =
        "POST /memory-backed HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello";
    std::vector<std::uint8_t> request_bytes(raw_request.begin(), raw_request.end());
    AegisHttpRequestStorage request_storage;

    auto store_result = request_storage.store(request_bytes);
    ASSERT_TRUE(store_result.has_value()) << store_result.error();

    auto restored_request = request_storage.retrieve();
    ASSERT_TRUE(restored_request.has_value()) << restored_request.error();
    EXPECT_EQ(*restored_request, request_bytes);

    HTTPParser parser(std::move(*restored_request));
    auto parsed_request = parser.parse();
    ASSERT_TRUE(parsed_request.has_value());
    EXPECT_EQ(parsed_request->method, "POST");
    EXPECT_EQ(parsed_request->target, "/memory-backed");
    EXPECT_EQ(parsed_request->body, (Data{'H', 'e', 'l', 'l', 'o'}));
}

TEST(unittests, networking_test){
    socket_native server(0);
    auto start_result = server.start();
    ASSERT_TRUE(start_result.has_value()) << start_result.error();

    std::expected<void, std::string> server_result;
    std::jthread server_thread([&] { server_result = server.run(); });

    const int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        server.stop();
        server_thread.join();
        FAIL() << "Could not create client socket";
        return;
    }

    timeval receive_timeout{3, 0};
    (void)setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(static_cast<std::uint16_t>(server.port()));

    if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        const std::string error = std::strerror(errno);
        close(client_socket);
        server.stop();
        server_thread.join();
        FAIL() << "Could not connect to the test server: " << error;
        return;
    }

    const std::string request_prefix =
        "POST /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "He";
    const std::string request_suffix = "llo";
    const ssize_t prefix_written = send(client_socket, request_prefix.data(), request_prefix.size(), 0);
    if (prefix_written == static_cast<ssize_t>(request_prefix.size())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const ssize_t suffix_written = send(client_socket, request_suffix.data(), request_suffix.size(), 0);

    std::string response;
    std::array<char, 1024> response_buffer{};
    bool receive_failed = prefix_written != static_cast<ssize_t>(request_prefix.size()) ||
        suffix_written != static_cast<ssize_t>(request_suffix.size());
    while (!receive_failed) {
        const ssize_t bytes_read = recv(client_socket, response_buffer.data(), response_buffer.size(), 0);
        if (bytes_read > 0) {
            response.append(response_buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) break;
        if (errno == EINTR) continue;
        receive_failed = true;
    }

    close(client_socket);
    server.stop();
    server_thread.join();

    ASSERT_FALSE(receive_failed) << "Client failed while sending or receiving HTTP data";
    ASSERT_TRUE(server_result.has_value()) << server_result.error();
    EXPECT_EQ(response,
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "Hello");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
