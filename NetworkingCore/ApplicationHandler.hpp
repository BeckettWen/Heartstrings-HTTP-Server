#pragma once

#include <cctype>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "../Http_Handling/HttpParser.hpp"
#include "../Http_Handling/HttpSerializer.h"

class Routing {
private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::filesystem::path> routes_;

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

        std::unique_lock lock(mutex_);
        routes_.insert_or_assign(std::move(route), std::move(file));
        return {};
    }

    std::expected<std::filesystem::path, std::string> retrieve(const HTTPRequest& request) const {
        const std::string path = route_path(request.target);
        std::shared_lock lock(mutex_);
        const auto route = routes_.find(path);
        if (route == routes_.end()) {
            return std::unexpected<std::string>("Route not found");
        }
        return route->second;
    }
};

class ApplicationHandler {
private:
    Routing routing_;

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

    httpresponse handle(const HTTPRequest& request) const {
        if (request.method != "GET" && request.method != "HEAD") {
            auto response = error_response(405, "Method Not Allowed", "Method Not Allowed");
            response.headers["Allow"] = "GET, HEAD";
            response.suppressBody = request.method == "HEAD";
            return response;
        }

        auto route = routing_.retrieve(request);
        if (!route) {
            auto response = error_response(404, "Not Found", "Not Found");
            response.suppressBody = request.method == "HEAD";
            return response;
        }

        std::error_code filesystem_error;
        if (!std::filesystem::is_regular_file(*route, filesystem_error)) {
            auto response = error_response(404, "Not Found", "Not Found");
            response.suppressBody = request.method == "HEAD";
            return response;
        }

        std::ifstream file(*route, std::ios::binary);
        if (!file) {
            auto response = error_response(500, "Internal Server Error", "File could not be opened");
            response.suppressBody = request.method == "HEAD";
            return response;
        }

        const std::istreambuf_iterator<char> file_begin(file);
        const std::istreambuf_iterator<char> file_end;
        std::string body(file_begin, file_end);
        if (file.bad()) {
            auto response = error_response(500, "Internal Server Error", "File could not be read");
            response.suppressBody = request.method == "HEAD";
            return response;
        }

        httpresponse response{
            .statusCode = 200,
            .statusReason = "OK",
            .headers = {{"Content-Type", content_type(*route)}},
            .body = std::move(body),
            .suppressBody = request.method == "HEAD"
        };
        return response;
    }
};
