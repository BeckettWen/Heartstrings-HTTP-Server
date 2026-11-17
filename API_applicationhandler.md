# Application Handler and Routing API

The shared routing table and response integration are implemented in `NetworkingCore/ApplicationHandler.hpp`. On macOS, `NetworkingCore/PlatformFileHandler.swift` opens and inspects the requested file and determines its content type through a small C ABI bridge. The C++ implementation remains the non-Apple file-handler fallback; the server's `kqueue` event loop is still macOS-specific, so this fallback alone does not provide a non-macOS server build.

## `Routing`

`Routing` stores exact URL-path-to-file mappings in a thread-safe `std::unordered_map`. Each entry contains the linked filesystem path and the expected filename (derived from `path.filename()`); it does not load file contents into the route table.

### `set`

```cpp
std::expected<void, std::string> set(std::string route, std::filesystem::path file);
```

Registers or replaces a route. The route must be an absolute URL path beginning with `/`; query and fragment characters are not allowed in a route key. The file path is stored as provided.

### `retrieve`

```cpp
struct Routing::FileEntry {
    std::filesystem::path path;
    std::string expected_name;
};

std::expected<FileEntry, std::string> retrieve_file(const HTTPRequest& request) const;
```

Looks up path metadata from the parsed request target. A query string is ignored for lookup, so `/guide?lang=en` matches the `/guide` route. Matching is exact; there are no wildcard routes or filesystem path joins based on client input.

## `ApplicationHandler`

### `File_Link`

```cpp
std::expected<void, std::string> File_Link(
    std::string route,
    std::filesystem::path file);
```

The public registration API. Links a URL path to a file by calling `Routing::set`.

### `handle`

```cpp
ApplicationResponse handle(const HTTPRequest& request) const;
```

Uses the route table to open and inspect the requested regular file, returning response metadata and an owned file descriptor for `GET`, rather than loading the whole file. The macOS implementation performs file opening, regular-file validation, size inspection, and content-type selection in Swift; the non-Apple fallback uses C++. The server sends the file in bounded 64 KiB chunks with a known `Content-Length`. `HEAD` returns the representation's `Content-Length` without a file body. Unknown routes or files return `404 Not Found`; other methods return `405 Method Not Allowed` with `Allow: GET, HEAD`. Common web extensions receive a basic MIME type; other extensions use `application/octet-stream`.

`ApplicationResponse` contains the normal `httpresponse` headers/status and an optional `StreamedFileBody`. The server owns and closes that descriptor after transmission or connection failure. File contents remain on disk until the response is streamed.

## Server API

`socket_native::File_Link(route, file)` forwards to its application handler. Register routes before or while the server is running. For example:

```cpp
socket_native server(8080);
auto linked = server.File_Link("/", "www/index.html");
```

The file path is interpreted by the process's filesystem. Only explicitly registered URL paths are served.
