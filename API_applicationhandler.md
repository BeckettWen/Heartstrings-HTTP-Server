# Application Handler and Routing API

The static-file application handler is implemented in `NetworkingCore/ApplicationHandler.hpp`.

## `Routing`

`Routing` stores exact URL-path-to-file mappings in a thread-safe `std::unordered_map`.

### `set`

```cpp
std::expected<void, std::string> set(std::string route, std::filesystem::path file);
```

Registers or replaces a route. The route must be an absolute URL path beginning with `/`; query and fragment characters are not allowed in a route key. The file path is stored as provided.

### `retrieve`

```cpp
std::expected<std::filesystem::path, std::string> retrieve(const HTTPRequest& request) const;
```

Looks up the path from the parsed request target. A query string is ignored for lookup, so `/guide?lang=en` matches the `/guide` route. Matching is exact; there are no wildcard routes or filesystem path joins based on client input.

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
httpresponse handle(const HTTPRequest& request) const;
```

Uses `Routing::retrieve` to find a file and returns its content as a response. It supports `GET` and `HEAD`; `HEAD` returns the representation's `Content-Length` but suppresses its body. Unknown routes or files return `404 Not Found`; other methods return `405 Method Not Allowed` with `Allow: GET, HEAD`. Files are read in binary mode. Common web extensions receive a basic MIME type; other extensions use `application/octet-stream`.

## Server API

`socket_native::File_Link(route, file)` forwards to its application handler. Register routes before or while the server is running. For example:

```cpp
socket_native server(8080);
auto linked = server.File_Link("/", "www/index.html");
```

The file path is interpreted by the process's filesystem. Only explicitly registered URL paths are served.
