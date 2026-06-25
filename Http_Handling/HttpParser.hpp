#pragma once

#include <iostream>
#include <vector>
#include <string>
#include <string_view>
#include <unordered_map>
#include <expected>
#include <algorithm>
#include <cstdint>
#include <ranges>
#include <charconv>

// Equivalent to Swift's Data type
using Data = std::vector<std::uint8_t>;

enum class HTTPParserError {
    InvalidStartLine,
    InvalidHeaderSyntax,
    InvalidContentLength,
    UnexpectedEOF,
    IllegalWhitespace
};

struct HTTPRequest {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    Data body;
};

// Internal utility string trimmer helpers
namespace string_utils {
    constexpr std::string_view whitespace = " \t\r\n";

    inline std::string trim(std::string_view str) {
        const auto start = str.find_first_not_of(whitespace);
        if (start == std::string_view::npos) return "";
        const auto end = str.find_last_not_of(whitespace);
        return std::string(str.substr(start, end - start + 1));
    }

    inline std::string to_lower(std::string_view str) {
        std::string result(str);
        std::ranges::transform(result, result.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return result;
    }

    inline std::vector<std::string_view> split(std::string_view str, char delim) {
        std::vector<std::string_view> out;

        auto begin = str.begin();
        for (auto it = str.begin(); it != str.end(); ++it) {
            if (*it == delim) {
                out.emplace_back(&*begin, std::distance(begin, it));
                begin = it + 1;
            }
        }

        out.emplace_back(&*begin, std::distance(begin, str.end()));
        return out;
    }
}

class HTTPParser {
protected:
    Data data_;
    std::size_t cursor_ = 0;

    friend class httpSerializer;

public:
    explicit HTTPParser(Data data) : data_(std::move(data)) {}

    std::expected<HTTPRequest, HTTPParserError> parse() {
        // PHASE 1: Start-Line
        skip_empty_lines();
        auto start_line_res = read_line();
        if (!start_line_res) return std::unexpected(start_line_res.error());

        auto start_line_parts = string_utils::split(*start_line_res, ' ');
        if (start_line_parts.size() != 3) {
            return std::unexpected(HTTPParserError::InvalidStartLine);
        }

        HTTPRequest request{
            .method = std::string(start_line_parts[0]),
            .target = std::string(start_line_parts[1]),
            .version = std::string(start_line_parts[2])
        };

        // PHASE 2: Header Section
        while (true) {
            auto line_res = read_line();
            if (!line_res) return std::unexpected(line_res.error());
            if (line_res->empty()) break; // End of headers found

            std::string_view line = *line_res;

            // Check for illegal whitespace at start (obsolete folding)
            if (line.starts_with(' ') || line.starts_with('\t')) {
                return std::unexpected(HTTPParserError::IllegalWhitespace);
            }

            auto colon_pos = line.find(':');
            if (colon_pos == std::string_view::npos) {
                return std::unexpected(HTTPParserError::InvalidHeaderSyntax);
            }

            auto name = string_utils::to_lower(line.substr(0, colon_pos));
            auto value = string_utils::trim(line.substr(colon_pos + 1));

            // RFC 9112: Combine multiple headers with same name using comma
            if (request.headers.contains(name)) {
                request.headers[name] += ", " + value;
            } else {
                request.headers[name] = std::move(value);
            }
        }

        // PHASE 3: Message Body Length Determination
        if (request.headers.contains("transfer-encoding") &&
            request.headers["transfer-encoding"].find("chunked") != std::string::npos) {
            // PHASE 4: Body Extraction (Chunked)
            if (auto chunk_res = parse_chunked_body(); chunk_res) {
                request.body = std::move(*chunk_res);
            } else {
                return std::unexpected(chunk_res.error());
            }
        }
        else if (request.headers.contains("content-length")) {
            // PHASE 4: Body Extraction (Fixed-Length)
            const auto& content_length_str = request.headers["content-length"];
            std::size_t length = 0;
            auto [ptr, ec] = std::from_chars(content_length_str.data(), content_length_str.data() + content_length_str.size(), length);

            if (ec != std::errc{}) {
                return std::unexpected(HTTPParserError::InvalidContentLength);
            }

            if (auto bytes_res = read_bytes(length); bytes_res) {
                request.body = std::move(*bytes_res);
            } else {
                return std::unexpected(bytes_res.error());
            }
        }

        return request;
    }

private:
    // MARK: - Helper Methods

    void skip_empty_lines() noexcept {
        while (cursor_ < data_.size()) {
            const auto byte = data_[cursor_];
            if (byte == 0x0D || byte == 0x0A) { // CR or LF
                ++cursor_;
            } else {
                break;
            }
        }
    }

    std::expected<std::string, HTTPParserError> read_line() {
        std::string line;
        while (cursor_ < data_.size()) {
            const auto byte = data_[cursor_];
            ++cursor_;
            if (byte == 0x0A) { // LF
                break;
            } else if (byte == 0x0D) { // CR
                if (cursor_ < data_.size() && data_[cursor_] == 0x0A) {
                    ++cursor_; // Skip following LF
                }
                break;
            }
            line.push_back(static_cast<char>(byte));
        }
        return line;
    }

    std::expected<Data, HTTPParserError> read_bytes(std::size_t count) {
        if (cursor_ + count > data_.size()) {
            return std::unexpected(HTTPParserError::UnexpectedEOF);
        }
        auto start = data_.begin() + cursor_;
        auto end = start + count;
        Data result(start, end);
        cursor_ += count;
        return result;
    }

    std::expected<Data, HTTPParserError> parse_chunked_body() {
        Data full_body;
        while (true) {
            auto size_line_res = read_line();
            if (!size_line_res) return std::unexpected(size_line_res.error());

            // Parse hex chunk size (ignoring chunk extensions separated by ';')
            auto hex_size_view = string_utils::split(*size_line_res, ';')[0];
            auto hex_size = string_utils::trim(hex_size_view);

            std::size_t chunk_size = 0;
            auto [ptr, ec] = std::from_chars(hex_size.data(), hex_size.data() + hex_size.size(), chunk_size, 16);
            if (ec != std::errc{}) {
                return std::unexpected(HTTPParserError::InvalidContentLength);
            }

            if (chunk_size == 0) {
                if (auto consume = read_line(); !consume) return std::unexpected(consume.error()); // Consume final CRLF
                break;
            }

            auto bytes_res = read_bytes(chunk_size);
            if (!bytes_res) return std::unexpected(bytes_res.error());

            full_body.insert(full_body.end(), bytes_res->begin(), bytes_res->end());

            if (auto consume = read_line(); !consume) return std::unexpected(consume.error()); // Consume CRLF after chunk data
        }
        return full_body;
    }
};
