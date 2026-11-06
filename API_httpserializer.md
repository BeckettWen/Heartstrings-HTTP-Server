# HTTP Response Serializer API

The response serializer is implemented in `Http_Handling/HttpSerializer.h`.

## Types

### `httpresponse`

```cpp
struct httpresponse {
    int statusCode = 200;
    std::string statusReason = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool suppressBody = false;
    std::string httpVersion = "HTTP/1.1";
};
```

The response body is a `std::string` and may contain binary bytes. `statusReason` can be left empty; the serializer uses its built-in reason phrase when one is known. `suppressBody` supports `HEAD` responses while retaining the representation length. `httpVersion` defaults to `HTTP/1.1` and can be set to the parsed request version.

## `httpSerializer`

### Constructor

```cpp
httpSerializer();
```

### Public `buffer` member

```cpp
std::vector<std::uint8_t> buffer;
```

This legacy public member is not read or updated by the current serializer methods. Use `ReturnSerializedResult()` to retrieve serialized bytes.

### `Serialize(const httpresponse&)`

```cpp
std::expected<void, std::string> Serialize(const httpresponse& response);
```

Serializes a response into the serializer's byte buffer. Each successful call replaces the previous serialized result. The serializer:

- writes the status line using `httpVersion` (or `HTTP/1.1` if it is empty);
- emits headers in deterministic, case-sensitive lexicographic name order;
- replaces any supplied `Content-Length` with the actual body byte count;
- adds `Connection: close` unless a connection header was supplied; and
- appends the body bytes unless `suppressBody` is true. `Content-Length` remains the unsuppressed representation length.

Example:

```cpp
httpresponse response{
    .statusCode = 200,
    .statusReason = "OK",
    .headers = {{"Connection", "close"}},
    .body = "Hello"
};

httpSerializer serializer;
auto result = serializer.Serialize(response);
if (result) {
    std::vector<std::uint8_t> bytes = serializer.ReturnSerializedResult();
}
```

### `Serialize(HTTPParser&)`

```cpp
std::expected<void, std::string> Serialize(HTTPParser& parser);
```

Compatibility convenience overload: parses one request and serializes a `200 OK` response echoing its body. On parser failure, returns an error string such as `"400 Bad Request"`; it does not serialize an error response.

### `ReturnSerializedResult`

```cpp
std::vector<std::uint8_t> ReturnSerializedResult() const;
```

Returns a copy of the serialized response bytes. The result is empty before a successful serialization.

If the parser overload fails before invoking the response overload, it leaves any previous serialized result unchanged.

### `writeToBuffer`

```cpp
std::expected<void, std::string> writeToBuffer(int& systemSocket);
```

Writes the serialized bytes to a socket descriptor, retrying interrupted writes and continuing after partial writes. It returns an error if a write fails; it does not wait for a non-blocking socket to become writable. The server writes through its `kqueue` event loop, using OpenSSL for TLS connections.

### `findTextWithCode`

```cpp
std::tuple<int, std::string> findTextWithCode(HTTPParserError error) const;
```

Maps parser errors to an HTTP status and reason phrase. Syntax/framing errors map to `400 Bad Request`; oversized bodies map to `413 Payload Too Large`, unsupported transfer codings map to `501 Not Implemented`, unsupported HTTP versions map to `505 HTTP Version Not Supported`, and oversized headers map to `431 Request Header Fields Too Large`.

## Related constants and maps

- `universalCode` is the CRLF sequence `"\r\n"`.
- `statusCodeSet` maps parser errors to status codes.
- `statusCodeToText` contains the built-in reason phrases for `200`, `400`, `413`, `431`, `500`, `501`, and `505`.

## Current scope

The serializer does not implement response streaming, chunked response encoding, header validation, or keep-alive policy management. Those connection policies are handled by `socket_native`.
