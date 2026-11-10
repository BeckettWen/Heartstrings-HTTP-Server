//this is the test file of the http handler, include the http parser and serializer
#include <cstdint>
#include <atomic>
#include <array>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <string_view>
#include <expected>
#include <filesystem>
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

#include <openssl/ssl.h>

#include "../Http_Handling/HttpSerializer.h"
#include "../Http_Handling/HttpParser.hpp"
#include "../Subsystems/Aegis.hpp"
#include "../Subsystems/StateMachine.hpp"
#include "../Subsystems/ThreadPool.hpp"
#include "../NetworkingCore/non_blocking_native.h"

struct TLSLoopbackOutcome {
    bool server_started = false;
    bool client_connected = false;
    bool handshake_succeeded = false;
    bool request_sent = false;
    bool response_received = false;
    bool plaintext_response_received = false;
    bool server_run_succeeded = false;
    std::string response;
    std::string plaintext_response;
    std::string error;
};

struct ParsedTestResponse {
    std::string status_line;
    std::string headers;
    std::string body;
};

static const std::string& welcome_file_body() {
    static const std::string body =
        "<!doctype html>\n"
        "<html><body>Heartstrings static route</body></html>\n";
    return body;
}

static const std::string& second_file_body() {
    static const std::string body = "Second pipelined response.\n";
    return body;
}

static bool take_http_response(const std::string& bytes, std::size_t& offset, ParsedTestResponse& response) {
    const std::size_t header_end = bytes.find("\r\n\r\n", offset);
    if (header_end == std::string::npos) return false;
    const std::size_t status_end = bytes.find("\r\n", offset);
    if (status_end == std::string::npos || status_end > header_end) return false;

    response.status_line = bytes.substr(offset, status_end - offset);
    response.headers = bytes.substr(status_end + 2, header_end - status_end - 2);

    const std::string length_header = "Content-Length: ";
    const std::size_t length_start = response.headers.find(length_header);
    if (length_start == std::string::npos) return false;
    const std::size_t value_start = length_start + length_header.size();
    const std::size_t value_end = response.headers.find("\r\n", value_start);
    const std::size_t content_length = std::stoull(response.headers.substr(
        value_start,
        value_end == std::string::npos ? std::string::npos : value_end - value_start));

    const std::size_t body_start = header_end + 4;
    if (content_length > bytes.size() - body_start) return false;
    response.body = bytes.substr(body_start, content_length);
    offset = body_start + content_length;
    return true;
}

static TLSLoopbackOutcome run_tls_loopback(
    bool require_client_certificate,
    bool send_client_certificate,
    bool configure_client_ca = false,
    std::chrono::milliseconds request_read_timeout = std::chrono::seconds(30),
    bool send_incomplete_request = false) {
    TLSLoopbackOutcome outcome;
    const std::filesystem::path fixture_directory = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    const std::string certificate_path = (fixture_directory / "localhost-cert.pem").string();
    const std::string private_key_path = (fixture_directory / "localhost-key.pem").string();

    socket_native server(0, request_read_timeout);
    auto tls_route = server.File_Link("/tls-echo", fixture_directory / "welcome.html");
    auto plaintext_route = server.File_Link("/plain-echo", fixture_directory / "second.txt");
    if (!tls_route || !plaintext_route) {
        outcome.error = "Could not configure the test file routes";
        return outcome;
    }

    TLSConfiguration tls_configuration{
        .port = 0,
        .certificate_chain_file = certificate_path,
        .private_key_file = private_key_path,
        .require_client_certificate = require_client_certificate,
        .client_ca_file = (require_client_certificate || configure_client_ca) ? certificate_path : ""
    };
    auto tls_configuration_result = server.enable_tls(std::move(tls_configuration));
    if (!tls_configuration_result) {
        outcome.error = tls_configuration_result.error();
        return outcome;
    }

    auto start_result = server.start();
    if (!start_result) {
        outcome.error = start_result.error();
        return outcome;
    }
    outcome.server_started = true;

    std::expected<void, std::string> server_result;
    std::jthread server_thread([&] { server_result = server.run(); });

    int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        outcome.error = std::strerror(errno);
        server.stop();
        server_thread.join();
        return outcome;
    }

    timeval timeout{3, 0};
    (void)setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_address.sin_port = htons(static_cast<std::uint16_t>(server.https_port()));
    if (connect(client_socket, reinterpret_cast<sockaddr*>(&server_address), sizeof(server_address)) == -1) {
        outcome.error = std::strerror(errno);
        close(client_socket);
        server.stop();
        server_thread.join();
        return outcome;
    }
    outcome.client_connected = true;

    SSL_CTX* client_context = SSL_CTX_new(TLS_client_method());
    if (client_context == nullptr) {
        outcome.error = "Could not create TLS client context";
        close(client_socket);
        server.stop();
        server_thread.join();
        return outcome;
    }
    SSL_CTX_set_verify(client_context, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_load_verify_locations(client_context, certificate_path.c_str(), nullptr) != 1) {
        outcome.error = "Could not load test certificate as a trust anchor";
        SSL_CTX_free(client_context);
        close(client_socket);
        server.stop();
        server_thread.join();
        return outcome;
    }
    if (send_client_certificate &&
        (SSL_CTX_use_certificate_chain_file(client_context, certificate_path.c_str()) != 1 ||
         SSL_CTX_use_PrivateKey_file(client_context, private_key_path.c_str(), SSL_FILETYPE_PEM) != 1)) {
        outcome.error = "Could not configure test client certificate";
        SSL_CTX_free(client_context);
        close(client_socket);
        server.stop();
        server_thread.join();
        return outcome;
    }

    SSL* client_ssl = SSL_new(client_context);
    if (client_ssl == nullptr || SSL_set_fd(client_ssl, client_socket) != 1 ||
        SSL_set_tlsext_host_name(client_ssl, "localhost") != 1 ||
        SSL_set1_host(client_ssl, "localhost") != 1) {
        outcome.error = "Could not configure TLS client session";
        if (client_ssl != nullptr) SSL_free(client_ssl);
        SSL_CTX_free(client_context);
        close(client_socket);
        server.stop();
        server_thread.join();
        return outcome;
    }

    if (SSL_connect(client_ssl) == 1) {
        outcome.handshake_succeeded = SSL_get_verify_result(client_ssl) == X509_V_OK;
        const std::string request = send_incomplete_request
            ? "GET /tls-echo HTTP/1.1\r\nHost: localhost\r\nX-Partial:"
            : "GET /tls-echo HTTP/1.1\r\n"
              "Host: localhost\r\n"
              "Connection: close\r\n"
              "\r\n";

        std::size_t bytes_sent = 0;
        while (outcome.handshake_succeeded && bytes_sent < request.size()) {
            std::size_t written = 0;
            if (SSL_write_ex(client_ssl, request.data() + bytes_sent, request.size() - bytes_sent, &written) != 1 ||
                written == 0) {
                outcome.error = "TLS client could not send the request";
                break;
            }
            bytes_sent += written;
        }
        outcome.request_sent = bytes_sent == request.size();

        std::array<char, 1024> response_buffer{};
        while (outcome.request_sent) {
            std::size_t bytes_read = 0;
            const int read_result = SSL_read_ex(client_ssl, response_buffer.data(), response_buffer.size(), &bytes_read);
            if (read_result == 1 && bytes_read > 0) {
                outcome.response.append(response_buffer.data(), bytes_read);
                continue;
            }
            if (SSL_get_error(client_ssl, read_result) == SSL_ERROR_ZERO_RETURN) {
                outcome.response_received = true;
                break;
            }
            if (!(require_client_certificate && !send_client_certificate)) {
                outcome.error = "TLS client could not read the complete response";
            }
            break;
        }
    } else if (require_client_certificate && !send_client_certificate) {
        // The handshake is expected to fail when the server requires a client certificate.
    } else {
        outcome.error = "TLS handshake failed";
    }

    SSL_free(client_ssl);
    SSL_CTX_free(client_context);
    close(client_socket);

    const int plaintext_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (plaintext_socket == -1) {
        outcome.error = std::strerror(errno);
    } else {
        (void)setsockopt(plaintext_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in plaintext_address{};
        plaintext_address.sin_family = AF_INET;
        plaintext_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        plaintext_address.sin_port = htons(static_cast<std::uint16_t>(server.port()));

        if (connect(plaintext_socket, reinterpret_cast<sockaddr*>(&plaintext_address), sizeof(plaintext_address)) == -1) {
            outcome.error = std::strerror(errno);
        } else {
            const std::string plaintext_request =
                "GET /plain-echo HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "Connection: close\r\n"
                "\r\n";
            std::size_t bytes_sent = 0;
            while (bytes_sent < plaintext_request.size()) {
                const ssize_t written = send(
                    plaintext_socket,
                    plaintext_request.data() + bytes_sent,
                    plaintext_request.size() - bytes_sent,
                    0);
                if (written > 0) {
                    bytes_sent += static_cast<std::size_t>(written);
                } else if (written == -1 && errno == EINTR) {
                    continue;
                } else {
                    outcome.error = "Plain HTTP client could not send the request";
                    break;
                }
            }

            std::array<char, 1024> plaintext_buffer{};
            while (outcome.error.empty() && bytes_sent == plaintext_request.size()) {
                const ssize_t bytes_read = recv(plaintext_socket, plaintext_buffer.data(), plaintext_buffer.size(), 0);
                if (bytes_read > 0) {
                    outcome.plaintext_response.append(plaintext_buffer.data(), static_cast<std::size_t>(bytes_read));
                    continue;
                }
                if (bytes_read == 0) {
                    outcome.plaintext_response_received = true;
                    break;
                }
                if (errno == EINTR) continue;
                outcome.error = "Plain HTTP client could not read the complete response";
            }
        }
        close(plaintext_socket);
    }

    server.stop();
    server_thread.join();
    outcome.server_run_succeeded = server_result.has_value();
    if (!server_result) outcome.error = server_result.error();
    return outcome;
}

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

TEST(http_parser_tests, decodes_chunked_body_and_exposes_unconsumed_pipeline_bytes){
    const std::string first_request =
        "POST /chunked HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4;tag=example\r\n"
        "Wiki\r\n"
        "5\r\n"
        "pedia\r\n"
        "0\r\n"
        "X-Checksum: demo\r\n"
        "\r\n";
    const std::string second_request =
        "GET /next HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";
    const std::string combined = first_request + second_request;
    Data bytes(combined.begin(), combined.end());
    HTTPParser parser(std::move(bytes));

    auto first = parser.parse();
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->body, (Data{'W', 'i', 'k', 'i', 'p', 'e', 'd', 'i', 'a'}));
    EXPECT_FALSE(first->headers.contains("x-checksum"));
    EXPECT_EQ(parser.bytesConsumed(), first_request.size());

    auto second = parser.parse();
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->target, "/next");
    EXPECT_EQ(parser.bytesConsumed(), combined.size());
}

TEST(http_parser_tests, rejects_ambiguous_message_framing_and_mismatched_lengths){
    const std::string transfer_encoding_and_length =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";
    HTTPParser conflicting_parser(Data(transfer_encoding_and_length.begin(), transfer_encoding_and_length.end()));
    auto conflicting = conflicting_parser.parse();
    ASSERT_FALSE(conflicting.has_value());
    EXPECT_EQ(conflicting.error(), HTTPParserError::ConflictingMessageFraming);

    const std::string mismatched_lengths =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 1\r\n"
        "content-length: 2\r\n"
        "\r\n"
        "ab";
    HTTPParser mismatched_parser(Data(mismatched_lengths.begin(), mismatched_lengths.end()));
    auto mismatched = mismatched_parser.parse();
    ASSERT_FALSE(mismatched.has_value());
    EXPECT_EQ(mismatched.error(), HTTPParserError::ConflictingMessageFraming);

    const std::string repeated_matching_lengths =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 1\r\n"
        "content-length: 1\r\n"
        "\r\n"
        "x";
    HTTPParser matching_parser(Data(repeated_matching_lengths.begin(), repeated_matching_lengths.end()));
    auto matching = matching_parser.parse();
    ASSERT_TRUE(matching.has_value());
    EXPECT_EQ(matching->body, (Data{'x'}));
}

TEST(http_parser_tests, rejects_unsupported_transfer_coding_and_invalid_field_name){
    const std::string unsupported_coding =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: gzip, chunked\r\n"
        "\r\n";
    HTTPParser transfer_parser(Data(unsupported_coding.begin(), unsupported_coding.end()));
    auto transfer = transfer_parser.parse();
    ASSERT_FALSE(transfer.has_value());
    EXPECT_EQ(transfer.error(), HTTPParserError::UnsupportedTransferEncoding);

    const std::string invalid_field_name =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Bad Header: value\r\n"
        "\r\n";
    HTTPParser field_parser(Data(invalid_field_name.begin(), invalid_field_name.end()));
    auto field = field_parser.parse();
    ASSERT_FALSE(field.has_value());
    EXPECT_EQ(field.error(), HTTPParserError::InvalidHeaderName);
}

TEST(http_parser_tests, reports_incomplete_input_and_accepts_replacement_data){
    const std::string incomplete_request =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "He";
    Data incomplete_bytes(incomplete_request.begin(), incomplete_request.end());
    HTTPParser parser(std::move(incomplete_bytes));
    auto incomplete = parser.parse();
    ASSERT_FALSE(incomplete.has_value());
    EXPECT_EQ(incomplete.error(), HTTPParserError::UnexpectedEOF);

    const std::string complete_request = incomplete_request + "llo";
    Data complete_bytes(complete_request.begin(), complete_request.end());
    ASSERT_TRUE(parser.inputData(complete_bytes).has_value());
    auto complete = parser.parse();
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ(complete->body, (Data{'H', 'e', 'l', 'l', 'o'}));
}

TEST(http_parser_tests, enforces_host_version_line_ending_and_body_limits){
    const std::string missing_host = "GET / HTTP/1.1\r\n\r\n";
    HTTPParser host_parser(Data(missing_host.begin(), missing_host.end()));
    auto no_host = host_parser.parse();
    ASSERT_FALSE(no_host.has_value());
    EXPECT_EQ(no_host.error(), HTTPParserError::MissingHost);

    const std::string unsupported_version = "GET / HTTP/2.0\r\nHost: localhost\r\n\r\n";
    HTTPParser version_parser(Data(unsupported_version.begin(), unsupported_version.end()));
    auto version = version_parser.parse();
    ASSERT_FALSE(version.has_value());
    EXPECT_EQ(version.error(), HTTPParserError::UnsupportedHttpVersion);

    const std::string bare_lf = "GET / HTTP/1.1\nHost: localhost\n\n";
    HTTPParser line_ending_parser(Data(bare_lf.begin(), bare_lf.end()));
    auto line_ending = line_ending_parser.parse();
    ASSERT_FALSE(line_ending.has_value());
    EXPECT_EQ(line_ending.error(), HTTPParserError::InvalidLineEnding);

    const std::string oversized_body =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: 6\r\n"
        "\r\n";
    HTTPParser size_parser(Data(oversized_body.begin(), oversized_body.end()), 5);
    auto size = size_parser.parse();
    ASSERT_FALSE(size.has_value());
    EXPECT_EQ(size.error(), HTTPParserError::RequestTooLarge);

    const std::string excessive_headers =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Extra: value\r\n"
        "\r\n";
    HTTPParser header_count_parser(
        Data(excessive_headers.begin(), excessive_headers.end()),
        HTTPParser::default_maximum_body_size,
        HTTPParser::default_maximum_header_size,
        1);
    auto header_count = header_count_parser.parse();
    ASSERT_FALSE(header_count.has_value());
    EXPECT_EQ(header_count.error(), HTTPParserError::HeadersTooLarge);
}

TEST(http_parser_tests, enforces_header_byte_and_chunked_body_limits){
    const std::string oversized_headers =
        "GET / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "X-Extra: abcdefghijklmnop\r\n"
        "\r\n";
    HTTPParser header_size_parser(
        Data(oversized_headers.begin(), oversized_headers.end()),
        HTTPParser::default_maximum_body_size,
        40);
    auto header_size = header_size_parser.parse();
    ASSERT_FALSE(header_size.has_value());
    EXPECT_EQ(header_size.error(), HTTPParserError::HeadersTooLarge);

    const std::string oversized_chunked_body =
        "POST / HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "6\r\n"
        "123456\r\n"
        "0\r\n\r\n";
    HTTPParser chunked_body_parser(
        Data(oversized_chunked_body.begin(), oversized_chunked_body.end()),
        5);
    auto chunked_body = chunked_body_parser.parse();
    ASSERT_FALSE(chunked_body.has_value());
    EXPECT_EQ(chunked_body.error(), HTTPParserError::RequestTooLarge);
}

TEST(unittests, incomplete_http_request_has_absolute_read_deadline){
    constexpr auto request_timeout = std::chrono::milliseconds(250);
    socket_native server(0, request_timeout);
    ASSERT_TRUE(server.start().has_value());

    std::expected<void, std::string> server_result;
    std::jthread server_thread([&] { server_result = server.run(); });

    const int client_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (client_socket == -1) {
        server.stop();
        server_thread.join();
        FAIL() << "Could not create slow HTTP client socket";
        return;
    }
    timeval receive_timeout{3, 0};
    (void)setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(server.port()));
    if (connect(client_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
        close(client_socket);
        server.stop();
        server_thread.join();
        FAIL() << "Could not connect slow HTTP client";
        return;
    }

    auto send_all = [client_socket](std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = send(
                client_socket, bytes.data() + offset, bytes.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    };

    const auto request_started = std::chrono::steady_clock::now();
    bool sent = send_all("GET / HTTP/1.1\r\nHost: localhost\r\nX-Partial:");
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    sent = sent && send_all("a");
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    sent = sent && send_all("b");

    std::string response;
    std::array<char, 1024> buffer{};
    bool receive_failed = false;
    while (sent) {
        const ssize_t bytes_read = recv(client_socket, buffer.data(), buffer.size(), 0);
        if (bytes_read > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(bytes_read));
        } else if (bytes_read == 0) {
            break;
        } else if (errno != EINTR) {
            receive_failed = true;
            break;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - request_started;

    close(client_socket);
    server.stop();
    server_thread.join();

    ASSERT_TRUE(sent);
    ASSERT_FALSE(receive_failed);
    ASSERT_TRUE(server_result.has_value()) << server_result.error();
    EXPECT_LT(elapsed, std::chrono::seconds(2));
    EXPECT_NE(response.find("HTTP/1.1 408 Request Timeout"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: close"), std::string::npos) << response;
}

TEST(unittests, incomplete_https_request_has_read_deadline){
    const TLSLoopbackOutcome outcome = run_tls_loopback(
        false,
        false,
        false,
        std::chrono::milliseconds(250),
        true);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;
    ASSERT_TRUE(outcome.server_started);
    ASSERT_TRUE(outcome.handshake_succeeded);
    ASSERT_TRUE(outcome.request_sent);
    ASSERT_TRUE(outcome.response_received);
    ASSERT_TRUE(outcome.plaintext_response_received);
    ASSERT_TRUE(outcome.server_run_succeeded);
    EXPECT_NE(outcome.response.find("HTTP/1.1 408 Request Timeout"), std::string::npos)
        << outcome.response;
}

TEST(unittests, maximum_connection_limit_rejects_excess_and_reuses_capacity){
    const std::filesystem::path fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "welcome.html";
    socket_native server(0);
    ASSERT_TRUE(server.set_maximum_connections(1).has_value());
    EXPECT_FALSE(server.set_maximum_connections(0).has_value());
    ASSERT_TRUE(server.File_Link("/limited", fixture).has_value());
    ASSERT_TRUE(server.start().has_value());
    EXPECT_FALSE(server.set_maximum_connections(2).has_value());

    std::expected<void, std::string> server_result;
    std::jthread server_thread([&] { server_result = server.run(); });

    auto connect_client = [&](int& descriptor) {
        descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor == -1) return false;
        timeval timeout{3, 0};
        (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(server.port()));
        if (connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            close(descriptor);
            descriptor = -1;
            return false;
        }
        return true;
    };
    auto send_all = [](int descriptor, std::string_view bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t written = send(
                descriptor, bytes.data() + offset, bytes.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    };

    int first_client = -1;
    if (!connect_client(first_client)) {
        server.stop();
        server_thread.join();
        FAIL() << "Could not connect the first limited client";
        return;
    }
    const bool first_request_started = send_all(
        first_client, "GET /limited HTTP/1.1\r\nHost: localhost\r\nX-Partial:");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    int excess_client = -1;
    const bool excess_connected = connect_client(excess_client);
    char excess_response_byte = 0;
    const ssize_t excess_read = excess_connected
        ? recv(excess_client, &excess_response_byte, 1, 0)
        : -1;
    const bool excess_was_rejected = !excess_connected || excess_read == 0 ||
        (excess_read == -1 && errno == ECONNRESET);
    if (excess_client >= 0) close(excess_client);
    close(first_client);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int replacement_client = -1;
    bool replacement_connected = connect_client(replacement_client);
    const std::string replacement_request =
        "GET /limited HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    bool replacement_sent = replacement_connected && send_all(replacement_client, replacement_request);
    std::string replacement_response;
    std::array<char, 1024> response_buffer{};
    bool replacement_receive_failed = false;
    while (replacement_sent) {
        const ssize_t bytes_read = recv(
            replacement_client, response_buffer.data(), response_buffer.size(), 0);
        if (bytes_read > 0) {
            replacement_response.append(response_buffer.data(), static_cast<std::size_t>(bytes_read));
        } else if (bytes_read == 0) {
            break;
        } else if (errno != EINTR) {
            replacement_receive_failed = true;
            break;
        }
    }
    if (replacement_client >= 0) close(replacement_client);
    server.stop();
    server_thread.join();

    EXPECT_TRUE(first_request_started);
    EXPECT_TRUE(excess_was_rejected) << "An over-limit client was not rejected";
    EXPECT_TRUE(replacement_connected);
    EXPECT_TRUE(replacement_sent);
    EXPECT_FALSE(replacement_receive_failed);
    ASSERT_TRUE(server_result.has_value()) << server_result.error();
    EXPECT_NE(replacement_response.find("HTTP/1.1 200 OK"), std::string::npos)
        << replacement_response;
}

TEST(application_handler_tests, file_link_routes_get_and_head_requests){
    const std::filesystem::path fixture =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "welcome.html";
    const std::string expected_body =
        "<!doctype html>\n"
        "<html><body>Heartstrings static route</body></html>\n";

    ApplicationHandler handler;
    auto linked = handler.File_Link("/welcome", fixture);
    ASSERT_TRUE(linked.has_value()) << linked.error();

    HTTPRequest get_request{
        .method = "GET",
        .target = "/welcome?from=test",
        .version = "HTTP/1.1"
    };
    const httpresponse get_response = handler.handle(get_request);
    EXPECT_EQ(get_response.statusCode, 200);
    EXPECT_EQ(get_response.body, expected_body);
    EXPECT_EQ(get_response.headers.at("Content-Type"), "text/html; charset=utf-8");
    EXPECT_FALSE(get_response.suppressBody);

    HTTPRequest head_request{
        .method = "HEAD",
        .target = "/welcome",
        .version = "HTTP/1.1"
    };
    const httpresponse head_response = handler.handle(head_request);
    EXPECT_EQ(head_response.statusCode, 200);
    EXPECT_EQ(head_response.body.size(), expected_body.size());
    EXPECT_TRUE(head_response.suppressBody);

    HTTPRequest missing_request{
        .method = "GET",
        .target = "/missing",
        .version = "HTTP/1.1"
    };
    EXPECT_EQ(handler.handle(missing_request).statusCode, 404);

    HTTPRequest unsupported_method{
        .method = "POST",
        .target = "/welcome",
        .version = "HTTP/1.1"
    };
    EXPECT_EQ(handler.handle(unsupported_method).statusCode, 405);
}

TEST(unittests, keep_alive_pipelining_routes_files_in_request_order){
    const std::filesystem::path fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    const std::string first_body =
        "<!doctype html>\n"
        "<html><body>Heartstrings static route</body></html>\n";
    const std::string second_body = "Second pipelined response.\n";

    socket_native server(0);
    ASSERT_TRUE(server.File_Link("/first", fixtures / "welcome.html").has_value());
    ASSERT_TRUE(server.File_Link("/second", fixtures / "second.txt").has_value());
    ASSERT_TRUE(server.start().has_value());

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
        close(client_socket);
        server.stop();
        server_thread.join();
        FAIL() << "Could not connect to the keep-alive test server";
        return;
    }

    const std::string pipelined_requests =
        "GET /first?cache=1 HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n"
        "GET /second HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
        "\r\n";
    std::size_t request_offset = 0;
    bool send_failed = false;
    while (request_offset < pipelined_requests.size()) {
        const ssize_t written = send(
            client_socket,
            pipelined_requests.data() + request_offset,
            pipelined_requests.size() - request_offset,
            0);
        if (written > 0) {
            request_offset += static_cast<std::size_t>(written);
        } else if (written == -1 && errno == EINTR) {
            continue;
        } else {
            send_failed = true;
            break;
        }
    }

    std::string raw_responses;
    std::array<char, 2048> response_buffer{};
    bool receive_failed = false;
    while (!send_failed) {
        const ssize_t bytes_read = recv(client_socket, response_buffer.data(), response_buffer.size(), 0);
        if (bytes_read > 0) {
            raw_responses.append(response_buffer.data(), static_cast<std::size_t>(bytes_read));
            continue;
        }
        if (bytes_read == 0) break;
        if (errno == EINTR) continue;
        receive_failed = true;
        break;
    }

    close(client_socket);
    server.stop();
    server_thread.join();

    ASSERT_FALSE(send_failed);
    ASSERT_FALSE(receive_failed);
    ASSERT_TRUE(server_result.has_value()) << server_result.error();

    std::size_t response_offset = 0;
    ParsedTestResponse first_response;
    ASSERT_TRUE(take_http_response(raw_responses, response_offset, first_response));
    EXPECT_EQ(first_response.status_line, "HTTP/1.1 200 OK");
    EXPECT_NE(first_response.headers.find("Connection: keep-alive"), std::string::npos);
    EXPECT_EQ(first_response.body, first_body);

    ParsedTestResponse second_response;
    ASSERT_TRUE(take_http_response(raw_responses, response_offset, second_response));
    EXPECT_EQ(second_response.status_line, "HTTP/1.1 200 OK");
    EXPECT_NE(second_response.headers.find("Connection: close"), std::string::npos);
    EXPECT_EQ(second_response.body, second_body);
    EXPECT_EQ(response_offset, raw_responses.size());
}

TEST(unittests, each_client_has_independent_connection_state){
    const std::filesystem::path fixtures = std::filesystem::path(__FILE__).parent_path() / "fixtures";
    socket_native server(0);
    ASSERT_TRUE(server.File_Link("/first", fixtures / "welcome.html").has_value());
    ASSERT_TRUE(server.File_Link("/second", fixtures / "second.txt").has_value());
    ASSERT_TRUE(server.start().has_value());

    std::expected<void, std::string> server_result;
    std::jthread server_thread([&] { server_result = server.run(); });

    auto connect_client = [&]() {
        const int descriptor = socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor == -1) return -1;
        timeval timeout{3, 0};
        (void)setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(server.port()));
        if (connect(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1) {
            close(descriptor);
            return -1;
        }
        return descriptor;
    };
    auto send_all = [](int descriptor, std::string_view data) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const ssize_t written = send(descriptor, data.data() + offset, data.size() - offset, 0);
            if (written > 0) {
                offset += static_cast<std::size_t>(written);
            } else if (written == -1 && errno == EINTR) {
                continue;
            } else {
                return false;
            }
        }
        return true;
    };
    auto receive_all = [](int descriptor, std::string& response) {
        std::array<char, 1024> buffer{};
        while (true) {
            const ssize_t bytes_read = recv(descriptor, buffer.data(), buffer.size(), 0);
            if (bytes_read > 0) {
                response.append(buffer.data(), static_cast<std::size_t>(bytes_read));
            } else if (bytes_read == 0) {
                return true;
            } else if (errno != EINTR) {
                return false;
            }
        }
    };

    const int first_client = connect_client();
    const int second_client = connect_client();
    bool first_sent = false;
    bool second_sent = false;
    bool first_received = false;
    bool second_received = false;
    std::string first_response;
    std::string second_response;

    if (first_client >= 0 && second_client >= 0) {
        const std::string first_prefix =
            "GET /first HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n";
        const std::string second_request =
            "GET /second HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Connection: close\r\n"
            "\r\n";
        first_sent = send_all(first_client, first_prefix);
        second_sent = send_all(second_client, second_request);
        if (second_sent) second_received = receive_all(second_client, second_response);
        if (first_sent) {
            first_sent = send_all(first_client, "\r\n");
            if (first_sent) first_received = receive_all(first_client, first_response);
        }
    }

    if (first_client >= 0) close(first_client);
    if (second_client >= 0) close(second_client);
    server.stop();
    server_thread.join();

    ASSERT_GE(first_client, 0);
    ASSERT_GE(second_client, 0);
    ASSERT_TRUE(first_sent);
    ASSERT_TRUE(second_sent);
    ASSERT_TRUE(first_received);
    ASSERT_TRUE(second_received);
    ASSERT_TRUE(server_result.has_value()) << server_result.error();

    std::size_t offset = 0;
    ParsedTestResponse first_parsed;
    ASSERT_TRUE(take_http_response(first_response, offset, first_parsed));
    EXPECT_EQ(first_parsed.body, welcome_file_body());
    EXPECT_EQ(offset, first_response.size());

    offset = 0;
    ParsedTestResponse second_parsed;
    ASSERT_TRUE(take_http_response(second_response, offset, second_parsed));
    EXPECT_EQ(second_parsed.body, second_file_body());
    EXPECT_EQ(offset, second_response.size());
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
    const std::filesystem::path welcome_file =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "welcome.html";
    auto file_link_result = server.File_Link("/echo", welcome_file);
    ASSERT_TRUE(file_link_result.has_value()) << file_link_result.error();
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
        "GET /echo HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Connection: close\r\n"
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
    const std::string expected_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(welcome_file_body().size()) + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n" + welcome_file_body();
    EXPECT_EQ(response, expected_response);
}

TEST(unittests, tls_networking_accepts_client_without_client_certificate){
    const TLSLoopbackOutcome outcome = run_tls_loopback(false, false);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;
    ASSERT_TRUE(outcome.server_started);
    ASSERT_TRUE(outcome.client_connected);
    ASSERT_TRUE(outcome.handshake_succeeded);
    ASSERT_TRUE(outcome.request_sent);
    ASSERT_TRUE(outcome.response_received);
    ASSERT_TRUE(outcome.plaintext_response_received);
    ASSERT_TRUE(outcome.server_run_succeeded);
    const std::string expected_tls_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(welcome_file_body().size()) + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        + welcome_file_body();
    EXPECT_EQ(outcome.response, expected_tls_response);
    const std::string expected_plaintext_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(second_file_body().size()) + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        + second_file_body();
    EXPECT_EQ(outcome.plaintext_response, expected_plaintext_response);
}

TEST(unittests, tls_networking_allows_optional_client_certificate){
    const TLSLoopbackOutcome outcome = run_tls_loopback(false, false, true);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;
    ASSERT_TRUE(outcome.server_started);
    ASSERT_TRUE(outcome.client_connected);
    ASSERT_TRUE(outcome.handshake_succeeded);
    ASSERT_TRUE(outcome.response_received);
    ASSERT_TRUE(outcome.plaintext_response_received);
    ASSERT_TRUE(outcome.server_run_succeeded);
}

TEST(unittests, tls_networking_accepts_required_client_certificate){
    const TLSLoopbackOutcome outcome = run_tls_loopback(true, true);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;
    ASSERT_TRUE(outcome.server_started);
    ASSERT_TRUE(outcome.client_connected);
    ASSERT_TRUE(outcome.handshake_succeeded);
    ASSERT_TRUE(outcome.request_sent);
    ASSERT_TRUE(outcome.response_received);
    ASSERT_TRUE(outcome.plaintext_response_received);
    ASSERT_TRUE(outcome.server_run_succeeded);
    const std::string expected_tls_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(welcome_file_body().size()) + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "\r\n"
        + welcome_file_body();
    EXPECT_EQ(outcome.response, expected_tls_response);
    const std::string expected_plaintext_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(second_file_body().size()) + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        + second_file_body();
    EXPECT_EQ(outcome.plaintext_response, expected_plaintext_response);
}

TEST(unittests, tls_networking_rejects_missing_required_client_certificate){
    const TLSLoopbackOutcome outcome = run_tls_loopback(true, false);
    ASSERT_TRUE(outcome.error.empty()) << outcome.error;
    ASSERT_TRUE(outcome.server_started);
    ASSERT_TRUE(outcome.client_connected);
    // TLS 1.3 can report the client's handshake complete before the server
    // processes its final client-certificate state; the server must still
    // reject the connection before serving HTTP.
    EXPECT_FALSE(outcome.response_received);
    ASSERT_TRUE(outcome.plaintext_response_received);
    EXPECT_TRUE(outcome.server_run_succeeded);
    const std::string expected_plaintext_response =
        "HTTP/1.1 200 OK\r\n"
        "Connection: close\r\n"
        "Content-Length: " + std::to_string(second_file_body().size()) + "\r\n"
        "Content-Type: text/plain; charset=utf-8\r\n"
        "\r\n"
        + second_file_body();
    EXPECT_EQ(outcome.plaintext_response, expected_plaintext_response);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
