#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include "../NetworkingCore/ApplicationHandler.hpp"
#include "../Subsystems/IPRateLimiter.hpp"

namespace {

struct temporary_files {
    std::vector<std::filesystem::path> paths;

    ~temporary_files() {
        for (const auto& path : paths) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }

    std::filesystem::path create(std::string_view suffix, std::string_view contents) {
        const auto unique_part = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto path = std::filesystem::temp_directory_path() /
            ("heartstrings-handler-" + std::to_string(getpid()) + "-" +
             std::to_string(unique_part) + std::string(suffix));
        paths.push_back(path);
        std::ofstream file(path, std::ios::binary);
        file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!file) return {};
        return path;
    }
};

} // namespace

TEST(IPRateLimiter, limitsSuccessfulRequestsPerFixedWindow) {
    IPRateLimiter limiter;

    for (std::size_t index = 0; index < IPRateLimiter::requests_per_window; ++index) {
        EXPECT_TRUE(limiter.reserve("127.0.0.1"));
    }
    EXPECT_FALSE(limiter.reserve("127.0.0.1"));
    EXPECT_TRUE(limiter.reserve("127.0.0.2"));

    limiter.release_reservation("127.0.0.1");
    EXPECT_TRUE(limiter.reserve("127.0.0.1"));
}

TEST(ApplicationHandler, replacementAndQueryLookupUseOnlyTheLinkedFile) {
    temporary_files files;
    const auto first = files.create(".html", "first");
    const auto replacement = files.create(".txt", "replacement");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(replacement.empty());

    ApplicationHandler handler;
    ASSERT_TRUE(handler.File_Link("/asset", first));
    ASSERT_TRUE(handler.File_Link("/asset", replacement));

    HTTPRequest request{
        .method = "GET",
        .target = "/asset?version=2",
        .version = "HTTP/1.1"
    };
    auto result = handler.handle(request);
    ASSERT_EQ(result.response.statusCode, 200);
    ASSERT_TRUE(result.file.has_value());
    EXPECT_EQ(result.file->expected_name(), replacement.filename().string());
    EXPECT_EQ(result.response.headers.at("Content-Type"), "text/plain; charset=utf-8");
    EXPECT_EQ(result.response.contentLength, 11);

    std::string contents(11, '\0');
    ASSERT_EQ(pread(result.file->descriptor(), contents.data(), contents.size(), 0), 11);
    EXPECT_EQ(contents, "replacement");
}

TEST(ApplicationHandler, headAndMissingFileResponsesDoNotReturnAFileBody) {
    temporary_files files;
    const auto existing = files.create(".bin", "body");
    ASSERT_FALSE(existing.empty());
    const auto missing = std::filesystem::temp_directory_path() /
        ("heartstrings-handler-missing-" + std::to_string(getpid()) + "-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_FALSE(std::filesystem::exists(missing));

    ApplicationHandler handler;
    ASSERT_TRUE(handler.File_Link("/existing", existing));
    ASSERT_TRUE(handler.File_Link("/missing", missing));

    HTTPRequest head_request{
        .method = "HEAD",
        .target = "/existing",
        .version = "HTTP/1.1"
    };
    const auto head_result = handler.handle(head_request);
    EXPECT_EQ(head_result.response.statusCode, 200);
    EXPECT_EQ(head_result.response.contentLength, 4);
    EXPECT_FALSE(head_result.file.has_value());

    HTTPRequest missing_request{
        .method = "GET",
        .target = "/missing",
        .version = "HTTP/1.1"
    };
    const auto missing_result = handler.handle(missing_request);
    EXPECT_EQ(missing_result.response.statusCode, 404);
    EXPECT_FALSE(missing_result.file.has_value());
}
