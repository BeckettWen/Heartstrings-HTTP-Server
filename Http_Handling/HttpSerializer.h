#pragma once

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <expected>
#include <map>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

#include "HttpParser.hpp"

inline constexpr char universalCode[] = "\r\n";

struct httpresponse {
    int statusCode = 200;
    std::string statusReason = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool suppressBody = false;
    std::string httpVersion = "HTTP/1.1";
};

inline std::unordered_map<HTTPParserError, int> statusCodeSet{
    {HTTPParserError::InvalidStartLine, 400},
    {HTTPParserError::InvalidHeaderSyntax, 400},
    {HTTPParserError::InvalidContentLength, 400},
    {HTTPParserError::UnexpectedEOF, 400},
    {HTTPParserError::IllegalWhitespace, 400},
    {HTTPParserError::InvalidLineEnding, 400},
    {HTTPParserError::InvalidMethod, 400},
    {HTTPParserError::InvalidTarget, 400},
    {HTTPParserError::UnsupportedHttpVersion, 505},
    {HTTPParserError::InvalidHeaderName, 400},
    {HTTPParserError::InvalidHeaderValue, 400},
    {HTTPParserError::MissingHost, 400},
    {HTTPParserError::ConflictingMessageFraming, 400},
    {HTTPParserError::UnsupportedTransferEncoding, 501},
    {HTTPParserError::InvalidChunkSyntax, 400},
    {HTTPParserError::RequestTooLarge, 413},
    {HTTPParserError::HeadersTooLarge, 431}
};

inline std::unordered_map<int, std::string> statusCodeToText{
    {200, "OK"},
    {400, "Bad Request"},
    {413, "Payload Too Large"},
    {500, "Internal Server Error"},
    {501, "Not Implemented"},
    {505, "HTTP Version Not Supported"},
    {431, "Request Header Fields Too Large"}
};

class httpSerializer {
protected:
    std::vector<std::uint8_t> serializedHTTPRequest;

    static std::string lowercase(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return value;
    }

public:
    std::vector<std::uint8_t> buffer;

    httpSerializer() = default;

    std::vector<std::uint8_t> ReturnSerializedResult() const {
        return serializedHTTPRequest;
    }

    std::expected<void, std::string> Serialize(const httpresponse& response) {
        serializedHTTPRequest.clear();

        std::string reason = response.statusReason;
        if (reason.empty()) {
            const auto known_status = statusCodeToText.find(response.statusCode);
            reason = known_status == statusCodeToText.end() ? "Unknown" : known_status->second;
        }

        const std::string version = response.httpVersion.empty() ? "HTTP/1.1" : response.httpVersion;
        const std::string status_line = version + " " + std::to_string(response.statusCode) +
            " " + reason + universalCode;
        serializedHTTPRequest.insert(serializedHTTPRequest.end(), status_line.begin(), status_line.end());

        std::map<std::string, std::string> ordered_headers;
        bool has_connection_header = false;
        for (const auto& [name, value] : response.headers) {
            const std::string normalized_name = lowercase(name);
            if (normalized_name == "content-length") continue;
            if (normalized_name == "connection") has_connection_header = true;
            ordered_headers[name] = value;
        }
        ordered_headers["Content-Length"] = std::to_string(response.body.size());
        if (!has_connection_header) ordered_headers["Connection"] = "close";

        for (const auto& [name, value] : ordered_headers) {
            const std::string header_line = name + ": " + value + universalCode;
            serializedHTTPRequest.insert(serializedHTTPRequest.end(), header_line.begin(), header_line.end());
        }
        serializedHTTPRequest.insert(serializedHTTPRequest.end(), universalCode, universalCode + 2);
        if (!response.suppressBody) {
            serializedHTTPRequest.insert(serializedHTTPRequest.end(), response.body.begin(), response.body.end());
        }
        return {};
    }

    // Compatibility convenience: parse a request and return a 200 response
    // that echoes its body, matching the initial server behavior.
    std::expected<void, std::string> Serialize(HTTPParser& parser) {
        auto parsed_request = parser.parse();
        if (!parsed_request) {
            const auto [status_code, reason] = findTextWithCode(parsed_request.error());
            return std::unexpected<std::string>(std::to_string(status_code) + " " + reason);
        }

        std::string body;
        body.reserve(parsed_request->body.size());
        for (const std::uint8_t byte : parsed_request->body) {
            body.push_back(static_cast<char>(byte));
        }

        httpresponse response{
            .statusCode = 200,
            .statusReason = "OK",
            .headers = {{"Connection", "close"}},
            .body = std::move(body),
            .httpVersion = parsed_request->version
        };
        return Serialize(response);
    }

    std::expected<void, std::string> writeToBuffer(int& systemSocket) {
        std::size_t bytes_written = 0;
        while (bytes_written < serializedHTTPRequest.size()) {
            const ssize_t result = write(
                systemSocket,
                serializedHTTPRequest.data() + bytes_written,
                serializedHTTPRequest.size() - bytes_written);
            if (result > 0) {
                bytes_written += static_cast<std::size_t>(result);
                continue;
            }
            if (result == -1 && errno == EINTR) continue;
            return std::unexpected<std::string>("write to the socket failed");
        }
        return {};
    }

    std::tuple<int, std::string> findTextWithCode(HTTPParserError error) const {
        const auto status = statusCodeSet.find(error);
        if (status == statusCodeSet.end()) return {400, "Bad Request"};

        const auto reason = statusCodeToText.find(status->second);
        if (reason == statusCodeToText.end()) return {status->second, "Unknown"};
        return {reason->first, reason->second};
    }
};
