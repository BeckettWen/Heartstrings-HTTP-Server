#pragma once

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

// Equivalent to Swift's Data type.
using Data = std::vector<std::uint8_t>;

enum class HTTPParserError: int {
    InvalidStartLine,
    InvalidHeaderSyntax,
    InvalidContentLength,
    UnexpectedEOF,
    IllegalWhitespace,
    InvalidLineEnding,
    InvalidMethod,
    InvalidTarget,
    UnsupportedHttpVersion,
    InvalidHeaderName,
    InvalidHeaderValue,
    MissingHost,
    ConflictingMessageFraming,
    UnsupportedTransferEncoding,
    InvalidChunkSyntax,
    RequestTooLarge,
    HeadersTooLarge
};

struct HTTPRequest {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    Data body;
};

namespace string_utils {
    constexpr std::string_view whitespace = " \t\r\n";

    inline std::string trim(std::string_view str) {
        const auto start = str.find_first_not_of(whitespace);
        if (start == std::string_view::npos) return "";
        const auto end = str.find_last_not_of(whitespace);
        return std::string(str.substr(start, end - start + 1));
    }

    inline std::string trim_ows(std::string_view str) {
        const auto start = str.find_first_not_of(" \t");
        if (start == std::string_view::npos) return "";
        const auto end = str.find_last_not_of(" \t");
        return std::string(str.substr(start, end - start + 1));
    }

    inline std::string to_lower(std::string_view str) {
        std::string result(str);
        std::ranges::transform(result, result.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        return result;
    }

    inline std::vector<std::string_view> split(std::string_view str, char delimiter) {
        std::vector<std::string_view> result;
        std::size_t begin = 0;
        while (true) {
            const std::size_t end = str.find(delimiter, begin);
            if (end == std::string_view::npos) {
                result.emplace_back(str.substr(begin));
                break;
            }
            result.emplace_back(str.substr(begin, end - begin));
            begin = end + 1;
        }
        return result;
    }
}

class HTTPParser {
private:
    Data data_;
    std::size_t cursor_ = 0;
    std::size_t maximum_body_size_;
    std::size_t maximum_header_size_;
    std::size_t maximum_header_count_;

    static constexpr std::string_view token_characters =
        "!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    static bool is_token_character(unsigned char character) noexcept {
        return token_characters.find(static_cast<char>(character)) != std::string_view::npos;
    }

    static bool is_hex_digit(unsigned char character) noexcept {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
    }

    static bool valid_request_target(std::string_view target) noexcept {
        if (target.empty()) return false;

        for (std::size_t index = 0; index < target.size(); ++index) {
            const auto character = static_cast<unsigned char>(target[index]);
            if (character <= 0x20 || character >= 0x7f || character == '#') return false;
            if (character == '%') {
                if (index + 2 >= target.size() ||
                    !is_hex_digit(static_cast<unsigned char>(target[index + 1])) ||
                    !is_hex_digit(static_cast<unsigned char>(target[index + 2]))) {
                    return false;
                }
                index += 2;
            }
        }
        return true;
    }

    static bool valid_chunk_extensions(std::string_view extensions) noexcept {
        std::size_t cursor = 0;
        while (cursor < extensions.size()) {
            if (extensions[cursor++] != ';') return false;

            const std::size_t name_begin = cursor;
            while (cursor < extensions.size() &&
                   is_token_character(static_cast<unsigned char>(extensions[cursor]))) {
                ++cursor;
            }
            if (cursor == name_begin) return false;

            if (cursor < extensions.size() && extensions[cursor] == '=') {
                ++cursor;
                if (cursor == extensions.size()) return false;

                if (extensions[cursor] == '"') {
                    ++cursor;
                    bool closed_quote = false;
                    while (cursor < extensions.size()) {
                        const auto character = static_cast<unsigned char>(extensions[cursor++]);
                        if (character == '"') {
                            closed_quote = true;
                            break;
                        }
                        if (character == '\\') {
                            if (cursor == extensions.size()) return false;
                            const auto escaped = static_cast<unsigned char>(extensions[cursor++]);
                            if ((escaped < 0x20 && escaped != '\t') || escaped == 0x7f) return false;
                        } else if ((character < 0x20 && character != '\t') || character == 0x7f) {
                            return false;
                        }
                    }
                    if (!closed_quote) return false;
                } else {
                    const std::size_t value_begin = cursor;
                    while (cursor < extensions.size() &&
                           is_token_character(static_cast<unsigned char>(extensions[cursor]))) {
                        ++cursor;
                    }
                    if (cursor == value_begin) return false;
                }
            }

            if (cursor < extensions.size() && extensions[cursor] != ';') return false;
        }
        return !extensions.empty();
    }

    std::expected<void, HTTPParserError> skip_leading_empty_lines() {
        while (cursor_ < data_.size()) {
            if (data_[cursor_] == '\n') {
                return std::unexpected(HTTPParserError::InvalidLineEnding);
            }
            if (data_[cursor_] != '\r') break;
            if (cursor_ + 1 >= data_.size()) {
                return std::unexpected(HTTPParserError::UnexpectedEOF);
            }
            if (data_[cursor_ + 1] != '\n') {
                return std::unexpected(HTTPParserError::InvalidLineEnding);
            }
            cursor_ += 2;
        }
        return {};
    }

    std::expected<std::string, HTTPParserError> read_line() {
        std::string line;
        while (cursor_ < data_.size()) {
            const std::uint8_t byte = data_[cursor_];
            if (byte == '\r') {
                if (cursor_ + 1 >= data_.size()) {
                    return std::unexpected(HTTPParserError::UnexpectedEOF);
                }
                if (data_[cursor_ + 1] != '\n') {
                    return std::unexpected(HTTPParserError::InvalidLineEnding);
                }
                cursor_ += 2;
                return line;
            }
            if (byte == '\n') {
                return std::unexpected(HTTPParserError::InvalidLineEnding);
            }
            if (line.size() == maximum_header_size_) {
                return std::unexpected(HTTPParserError::HeadersTooLarge);
            }
            line.push_back(static_cast<char>(byte));
            ++cursor_;
        }
        return std::unexpected(HTTPParserError::UnexpectedEOF);
    }

    std::expected<std::pair<std::string, std::string>, HTTPParserError>
    parse_header_line(std::string_view line) const {
        if (line.starts_with(' ') || line.starts_with('\t')) {
            return std::unexpected(HTTPParserError::IllegalWhitespace);
        }

        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            return std::unexpected(HTTPParserError::InvalidHeaderSyntax);
        }
        if (colon == 0) return std::unexpected(HTTPParserError::InvalidHeaderName);

        for (const unsigned char character : line.substr(0, colon)) {
            if (!is_token_character(character)) {
                return std::unexpected(HTTPParserError::InvalidHeaderName);
            }
        }

        const std::string_view raw_value = line.substr(colon + 1);
        for (const unsigned char character : raw_value) {
            if ((character < 0x20 && character != '\t') || character == 0x7f) {
                return std::unexpected(HTTPParserError::InvalidHeaderValue);
            }
        }

        return std::pair{
            string_utils::to_lower(line.substr(0, colon)),
            string_utils::trim_ows(raw_value)
        };
    }

    std::expected<std::size_t, HTTPParserError> parse_content_length(std::string_view value) const {
        const std::string normalized = string_utils::trim_ows(value);
        if (normalized.empty()) return std::unexpected(HTTPParserError::InvalidContentLength);

        for (const unsigned char character : normalized) {
            if (character < '0' || character > '9') {
                return std::unexpected(HTTPParserError::InvalidContentLength);
            }
        }

        std::size_t length = 0;
        const auto [end, error] = std::from_chars(
            normalized.data(), normalized.data() + normalized.size(), length);
        if (error == std::errc::result_out_of_range) {
            return std::unexpected(HTTPParserError::RequestTooLarge);
        }
        if (error != std::errc{} || end != normalized.data() + normalized.size()) {
            return std::unexpected(HTTPParserError::InvalidContentLength);
        }
        if (length > maximum_body_size_) {
            return std::unexpected(HTTPParserError::RequestTooLarge);
        }
        return length;
    }

    std::expected<Data, HTTPParserError> read_bytes(std::size_t count) {
        if (count > data_.size() - cursor_) {
            return std::unexpected(HTTPParserError::UnexpectedEOF);
        }
        const auto begin = data_.begin() + static_cast<std::ptrdiff_t>(cursor_);
        const auto end = begin + static_cast<std::ptrdiff_t>(count);
        Data result(begin, end);
        cursor_ += count;
        return result;
    }

    std::expected<Data, HTTPParserError> parse_chunked_body() {
        Data body;

        while (true) {
            auto size_line = read_line();
            if (!size_line) return std::unexpected(size_line.error());

            const std::size_t extension_begin = size_line->find(';');
            const std::string_view size_text = extension_begin == std::string::npos
                ? std::string_view(*size_line)
                : std::string_view(*size_line).substr(0, extension_begin);
            if (size_text.empty()) return std::unexpected(HTTPParserError::InvalidChunkSyntax);

            for (const unsigned char character : size_text) {
                if (!is_hex_digit(character)) {
                    return std::unexpected(HTTPParserError::InvalidChunkSyntax);
                }
            }
            if (extension_begin != std::string::npos &&
                !valid_chunk_extensions(std::string_view(*size_line).substr(extension_begin))) {
                return std::unexpected(HTTPParserError::InvalidChunkSyntax);
            }

            std::size_t chunk_size = 0;
            const auto [end, error] = std::from_chars(
                size_text.data(), size_text.data() + size_text.size(), chunk_size, 16);
            if (error == std::errc::result_out_of_range) {
                return std::unexpected(HTTPParserError::RequestTooLarge);
            }
            if (error != std::errc{} || end != size_text.data() + size_text.size()) {
                return std::unexpected(HTTPParserError::InvalidChunkSyntax);
            }

            if (chunk_size == 0) {
                std::size_t trailer_bytes = 0;
                std::size_t trailer_count = 0;
                while (true) {
                    auto trailer_line = read_line();
                    if (!trailer_line) return std::unexpected(trailer_line.error());
                    const std::size_t line_size = trailer_line->size() + 2;
                    if (line_size > maximum_header_size_ - trailer_bytes) {
                        return std::unexpected(HTTPParserError::HeadersTooLarge);
                    }
                    trailer_bytes += line_size;
                    if (trailer_line->empty()) return body;
                    if (++trailer_count > maximum_header_count_) {
                        return std::unexpected(HTTPParserError::HeadersTooLarge);
                    }

                    auto trailer = parse_header_line(*trailer_line);
                    if (!trailer) return std::unexpected(trailer.error());
                    const std::string& name = trailer->first;
                    if (name == "content-length" || name == "transfer-encoding" ||
                        name == "host" || name == "trailer" || name == "connection") {
                        return std::unexpected(HTTPParserError::InvalidHeaderSyntax);
                    }
                }
            }

            if (chunk_size > maximum_body_size_ - body.size()) {
                return std::unexpected(HTTPParserError::RequestTooLarge);
            }
            if (chunk_size > data_.size() - cursor_) {
                return std::unexpected(HTTPParserError::UnexpectedEOF);
            }

            auto chunk = read_bytes(chunk_size);
            if (!chunk) return std::unexpected(chunk.error());
            body.insert(body.end(), chunk->begin(), chunk->end());

            if (data_.size() - cursor_ < 2) {
                return std::unexpected(HTTPParserError::UnexpectedEOF);
            }
            if (data_[cursor_] != '\r' || data_[cursor_ + 1] != '\n') {
                return std::unexpected(HTTPParserError::InvalidLineEnding);
            }
            cursor_ += 2;
        }
    }

public:
    static constexpr std::size_t default_maximum_body_size = 16 * 1024 * 1024;
    static constexpr std::size_t default_maximum_header_size = 64 * 1024;
    static constexpr std::size_t default_maximum_header_count = 100;

    explicit HTTPParser(
        Data data,
        std::size_t maximum_body_size = default_maximum_body_size,
        std::size_t maximum_header_size = default_maximum_header_size,
        std::size_t maximum_header_count = default_maximum_header_count)
        : data_(std::move(data)),
          maximum_body_size_(maximum_body_size),
          maximum_header_size_(maximum_header_size),
          maximum_header_count_(maximum_header_count) {}

    std::expected<HTTPRequest, HTTPParserError> parse() {
        if (auto leading_lines = skip_leading_empty_lines(); !leading_lines) {
            return std::unexpected(leading_lines.error());
        }

        auto start_line = read_line();
        if (!start_line) return std::unexpected(start_line.error());
        std::size_t header_section_size = start_line->size() + 2;
        if (header_section_size > maximum_header_size_) {
            return std::unexpected(HTTPParserError::HeadersTooLarge);
        }

        const std::size_t first_space = start_line->find(' ');
        const std::size_t second_space = first_space == std::string::npos
            ? std::string::npos
            : start_line->find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos ||
            first_space == 0 || second_space == first_space + 1 ||
            second_space + 1 == start_line->size() ||
            start_line->find(' ', second_space + 1) != std::string::npos) {
            return std::unexpected(HTTPParserError::InvalidStartLine);
        }

        HTTPRequest request;
        request.method = start_line->substr(0, first_space);
        request.target = start_line->substr(first_space + 1, second_space - first_space - 1);
        request.version = start_line->substr(second_space + 1);

        for (const unsigned char character : request.method) {
            if (!is_token_character(character)) {
                return std::unexpected(HTTPParserError::InvalidMethod);
            }
        }
        if (!valid_request_target(request.target) ||
            (request.target == "*" && request.method != "OPTIONS")) {
            return std::unexpected(HTTPParserError::InvalidTarget);
        }
        if (request.version != "HTTP/1.0" && request.version != "HTTP/1.1") {
            return std::unexpected(HTTPParserError::UnsupportedHttpVersion);
        }

        std::unordered_map<std::string, std::size_t> header_counts;
        std::vector<std::string> content_length_values;
        std::vector<std::string> transfer_encoding_values;
        std::size_t header_count = 0;

        while (true) {
            auto header_line = read_line();
            if (!header_line) return std::unexpected(header_line.error());
            const std::size_t line_size = header_line->size() + 2;
            if (line_size > maximum_header_size_ - header_section_size) {
                return std::unexpected(HTTPParserError::HeadersTooLarge);
            }
            header_section_size += line_size;
            if (header_line->empty()) break;

            auto header = parse_header_line(*header_line);
            if (!header) return std::unexpected(header.error());

            const std::string& name = header->first;
            const std::string& value = header->second;
            ++header_counts[name];
            if (++header_count > maximum_header_count_) {
                return std::unexpected(HTTPParserError::HeadersTooLarge);
            }

            if (name == "content-length") content_length_values.push_back(value);
            if (name == "transfer-encoding") transfer_encoding_values.push_back(value);

            const auto existing = request.headers.find(name);
            if (existing == request.headers.end()) {
                request.headers.emplace(name, value);
            } else {
                existing->second += ", " + value;
            }
        }

        if (header_counts["host"] > 1) {
            return std::unexpected(HTTPParserError::InvalidHeaderSyntax);
        }
        if (request.version == "HTTP/1.1") {
            const auto host = request.headers.find("host");
            if (host == request.headers.end() || host->second.empty()) {
                return std::unexpected(HTTPParserError::MissingHost);
            }
        }

        if (!content_length_values.empty() && !transfer_encoding_values.empty()) {
            return std::unexpected(HTTPParserError::ConflictingMessageFraming);
        }

        if (!transfer_encoding_values.empty()) {
            std::vector<std::string> codings;
            for (const std::string& value : transfer_encoding_values) {
                for (const std::string_view coding : string_utils::split(value, ',')) {
                    const std::string normalized = string_utils::to_lower(string_utils::trim_ows(coding));
                    if (normalized.empty()) {
                        return std::unexpected(HTTPParserError::UnsupportedTransferEncoding);
                    }
                    codings.push_back(normalized);
                }
            }
            if (codings.size() != 1 || codings.front() != "chunked") {
                return std::unexpected(HTTPParserError::UnsupportedTransferEncoding);
            }

            auto body = parse_chunked_body();
            if (!body) return std::unexpected(body.error());
            request.body = std::move(*body);
        } else if (!content_length_values.empty()) {
            std::optional<std::size_t> content_length;
            for (const std::string& value : content_length_values) {
                auto parsed_length = parse_content_length(value);
                if (!parsed_length) return std::unexpected(parsed_length.error());
                if (content_length && *content_length != *parsed_length) {
                    return std::unexpected(HTTPParserError::ConflictingMessageFraming);
                }
                content_length = *parsed_length;
            }

            auto body = read_bytes(*content_length);
            if (!body) return std::unexpected(body.error());
            request.body = std::move(*body);
        }

        return request;
    }

    // Number of input bytes consumed by the most recent parse attempt.
    std::size_t bytesConsumed() const noexcept {
        return cursor_;
    }

    std::expected<void, std::string> inputData(Data& input_data) {
        data_ = input_data;
        cursor_ = 0;
        return {};
    }
};
