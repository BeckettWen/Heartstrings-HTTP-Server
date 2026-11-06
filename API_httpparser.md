# HTTP Request Parser API

The parser is implemented in `Http_Handling/HttpParser.hpp`. It parses HTTP/1.0 and HTTP/1.1 requests from a byte buffer, including request framing and chunk decoding.

## Types

### `Data`

```cpp
using Data = std::vector<std::uint8_t>;
```

Request bytes and bodies use byte vectors; they are not assumed to be text.

### `HTTPRequest`

```cpp
struct HTTPRequest {
    std::string method;
    std::string target;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    Data body;
};
```

Header names are lowercase. Repeated headers are combined into one value separated by `", "`, except framing headers are separately validated before the combined representation is used. The body contains decoded chunk payload bytes for chunked requests.

### `HTTPParserError`

```cpp
enum class HTTPParserError : int {
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
```

Errors identify malformed syntax, unsupported protocol/framing features, incomplete input, or a body exceeding the parser limit. `UnexpectedEOF` is used by the server to wait for more TCP/TLS data.

## `HTTPParser`

### Constructor

```cpp
explicit HTTPParser(Data data,
                    std::size_t maximum_body_size = HTTPParser::default_maximum_body_size,
                    std::size_t maximum_header_size = HTTPParser::default_maximum_header_size,
                    std::size_t maximum_header_count = HTTPParser::default_maximum_header_count);
```

Takes the input byte vector by value. Defaults are a 16 MiB body limit, 64 KiB combined request-line/header limit, and 100 header fields.

### `parse`

```cpp
std::expected<HTTPRequest, HTTPParserError> parse();
```

Parses one request beginning at the parser's current cursor. It can be called repeatedly to process pipelined requests from one buffer; use `bytesConsumed()` to find the current offset.

The parser validates the request line, header syntax, and HTTP/1.1 `Host` requirement. For framing, it:

- parses decimal `Content-Length` values and accepts repeated values only if they agree;
- rejects requests containing both `Content-Length` and `Transfer-Encoding`;
- supports a single `chunked` transfer coding, including chunk extensions and validated trailer fields; and
- returns an empty body when neither framing header is present.

Example:

```cpp
Data bytes(raw.begin(), raw.end());
HTTPParser parser(std::move(bytes));

auto request = parser.parse();
if (!request) {
    // Inspect request.error() for an HTTPParserError.
} else {
    // Use request->method, request->target, request->headers, and request->body.
}
```

### `bytesConsumed`

```cpp
std::size_t bytesConsumed() const noexcept;
```

Returns the parser cursor, which marks the end of the most recently parsed request (or the point reached before a parse error).

### `inputData`

```cpp
std::expected<void, std::string> inputData(Data& input_data);
```

Replaces the input and resets the cursor to zero. After `UnexpectedEOF`, provide the full accumulated buffer with this method or construct a new parser before retrying.

## Current scope

- Requires CRLF line endings and supports only `HTTP/1.0` and `HTTP/1.1`.
- Validates method/header tokens, rejects control characters in header values, and rejects obsolete folded headers.
- Requires exactly one non-empty `Host` header for HTTP/1.1; duplicate `Host` is rejected for either version.
- Enforces configurable body, header-byte, and header-count limits. `HeadersTooLarge` maps to `431 Request Header Fields Too Large`.
- Supports only `Transfer-Encoding: chunked`; other transfer codings return `UnsupportedTransferEncoding`.
- Chunk extensions are validated and ignored. Trailer fields are validated and consumed, but are not added to `HTTPRequest::headers`; `Host`, `Content-Length`, `Transfer-Encoding`, `Trailer`, and `Connection` are rejected in trailers.
- Validates request-target characters and percent escapes, but does not perform URI routing or full authority-form validation.
- This is a project HTTP/1.x request parser, not a complete implementation of every RFC extension or application-level feature.
