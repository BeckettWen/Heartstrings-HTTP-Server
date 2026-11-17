#pragma once

#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unordered_map>
#include <utility>
#include <unistd.h>

#include "../Http_Handling/HttpParser.hpp"
#include "../Http_Handling/HttpSerializer.h"

#if defined(__APPLE__)
extern "C" std::int32_t heartstrings_swift_prepare_file_response(
    const char* path,
    std::int32_t is_get_request,
    std::int32_t* descriptor_out,
    std::uint64_t* size_out,
    char* content_type_out,
    std::uint32_t content_type_capacity);
#endif

class StreamedFileBody {
private:
    int descriptor_ = -1;
    std::uint64_t size_ = 0;
    std::string expected_name_;

    void close_descriptor() noexcept {
        if (descriptor_ >= 0) ::close(descriptor_);
        descriptor_ = -1;
    }

public:
    StreamedFileBody(int descriptor, std::uint64_t size, std::string expected_name)
        : descriptor_(descriptor), size_(size), expected_name_(std::move(expected_name)) {}

    StreamedFileBody(const StreamedFileBody&) = delete;
    StreamedFileBody& operator=(const StreamedFileBody&) = delete;

    StreamedFileBody(StreamedFileBody&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)),
          size_(other.size_),
          expected_name_(std::move(other.expected_name_)) {}

    StreamedFileBody& operator=(StreamedFileBody&& other) noexcept {
        if (this == &other) return *this;
        close_descriptor();
        descriptor_ = std::exchange(other.descriptor_, -1);
        size_ = other.size_;
        expected_name_ = std::move(other.expected_name_);
        return *this;
    }

    ~StreamedFileBody() { close_descriptor(); }

    int descriptor() const noexcept { return descriptor_; }
    std::uint64_t size() const noexcept { return size_; }
    const std::string& expected_name() const noexcept { return expected_name_; }
};

struct ApplicationResponse {
    httpresponse response;
    std::optional<StreamedFileBody> file;
};

class Routing {
public:
    struct FileEntry {
        std::filesystem::path path;
        std::string expected_name;
    };

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, FileEntry> routes_;

    static std::string route_path(std::string_view request_target) {
        const std::size_t query_position = request_target.find('?');
        return std::string(request_target.substr(0, query_position));
    }

public:
    std::expected<void, std::string> set(std::string route, std::filesystem::path file) {
        if (route.empty() || route.front() != '/' ||
            route.find('?') != std::string::npos || route.find('#') != std::string::npos) {
            return std::unexpected<std::string>("Route must be an absolute URL path without a query or fragment");
        }
        const std::string expected_name = file.filename().string();
        if (expected_name.empty()) {
            return std::unexpected<std::string>("File path must include a filename");
        }

        std::unique_lock lock(mutex_);
        routes_.insert_or_assign(
            std::move(route), FileEntry{std::move(file), expected_name});
        return {};
    }

    std::expected<FileEntry, std::string> retrieve_file(const HTTPRequest& request) const {
        const std::string path = route_path(request.target);
        std::shared_lock lock(mutex_);
        const auto route = routes_.find(path);
        if (route == routes_.end()) {
            return std::unexpected<std::string>("Route not found");
        }
        return route->second;
    }

    std::expected<std::filesystem::path, std::string> retrieve(const HTTPRequest& request) const {
        auto file = retrieve_file(request);
        if (!file) return std::unexpected<std::string>(file.error());
        return file->path;
    }
};

class ApplicationHandler {
private:
    Routing routing_;

#if !defined(__APPLE__)
    static std::string content_type(const std::filesystem::path& file) {
        std::string extension = file.extension().string();
        for (char& character : extension) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }

        static const std::unordered_map<std::string, std::string> types{
            {".css", "text/css; charset=utf-8"},
            {".htm", "text/html; charset=utf-8"},
            {".html", "text/html; charset=utf-8"},
            {".js", "text/javascript; charset=utf-8"},
            {".json", "application/json"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".png", "image/png"},
            {".svg", "image/svg+xml"},
            {".txt", "text/plain; charset=utf-8"}
        };
        const auto found = types.find(extension);
        return found == types.end() ? "application/octet-stream" : found->second;
    }
#endif

    static httpresponse error_response(int status_code, std::string reason, std::string body) {
        return httpresponse{
            .statusCode = status_code,
            .statusReason = std::move(reason),
            .headers = {{"Content-Type", "text/plain; charset=utf-8"}},
            .body = std::move(body)
        };
    }

public:
    std::expected<void, std::string> File_Link(std::string route, std::filesystem::path file) {
        return routing_.set(std::move(route), std::move(file));
    }

    ApplicationResponse handle(const HTTPRequest& request) const {
        if (request.method != "GET" && request.method != "HEAD") {
            auto response = error_response(405, "Method Not Allowed", "Method Not Allowed");
            response.headers["Allow"] = "GET, HEAD";
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }

        auto file_entry = routing_.retrieve_file(request);
        if (!file_entry) {
            auto response = error_response(404, "Not Found", "Not Found");
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }

        if (file_entry->expected_name != file_entry->path.filename().string()) {
            auto response = error_response(404, "Not Found", "Linked file name is no longer available");
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }

        std::string response_content_type;
#if defined(__APPLE__)
        std::int32_t opened_descriptor = -1;
        std::uint64_t file_size = 0;
        std::array<char, 64> content_type_buffer{};
        const std::int32_t open_result = heartstrings_swift_prepare_file_response(
            file_entry->path.c_str(),
            request.method == "GET" ? 1 : 0,
            &opened_descriptor,
            &file_size,
            content_type_buffer.data(),
            static_cast<std::uint32_t>(content_type_buffer.size()));
        const int descriptor = opened_descriptor;
        if (open_result != 0) {
            if (open_result == 1 || open_result == 2) {
                auto response = error_response(404, "Not Found", "Not Found");
                response.suppressBody = request.method == "HEAD";
                return ApplicationResponse{.response = std::move(response)};
            }
            const char* message = open_result == 4
                ? "File could not be inspected"
                : open_result == 3
                    ? "File could not be opened"
                    : "File response metadata could not be prepared";
            auto response = error_response(500, "Internal Server Error", message);
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }
        response_content_type = content_type_buffer.data();
#else
        const int descriptor = ::open(file_entry->path.c_str(), O_RDONLY | O_CLOEXEC);
        if (descriptor == -1) {
            if (errno == ENOENT || errno == ENOTDIR) {
                auto response = error_response(404, "Not Found", "Not Found");
                response.suppressBody = request.method == "HEAD";
                return ApplicationResponse{.response = std::move(response)};
            }
            auto response = error_response(500, "Internal Server Error", "File could not be opened");
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }

        struct stat file_status{};
        if (fstat(descriptor, &file_status) == -1) {
            ::close(descriptor);
            auto response = error_response(500, "Internal Server Error", "File could not be inspected");
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }
        if (!S_ISREG(file_status.st_mode) || file_status.st_size < 0) {
            ::close(descriptor);
            auto response = error_response(404, "Not Found", "Not Found");
            response.suppressBody = request.method == "HEAD";
            return ApplicationResponse{.response = std::move(response)};
        }

        const auto file_size = static_cast<std::uint64_t>(file_status.st_size);
        response_content_type = content_type(file_entry->path);
#endif
        StreamedFileBody opened_file(
            descriptor, file_size, std::move(file_entry->expected_name));
        httpresponse response{
            .statusCode = 200,
            .statusReason = "OK",
            .headers = {{"Content-Type", std::move(response_content_type)}},
            .suppressBody = true,
            .contentLength = file_size
        };

        std::optional<StreamedFileBody> file;
        if (request.method == "GET" && file_size > 0) {
            file.emplace(std::move(opened_file));
        }
        return ApplicationResponse{
            .response = std::move(response),
            .file = std::move(file)
        };
    }
};
