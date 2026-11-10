# Xcode Library API

The binary distribution is `Heartstrings.xcframework` in the sibling `X-18 Production` folder. It exposes the compiled C++ API in `NetworkingCore/Heartstrings.hpp`; its implementation is in `NetworkingCore/Heartstrings.cpp`.

## Requirements

- macOS 26.0 or later, Apple Silicon (`arm64`).
- C++23.
- No separate OpenSSL installation: the XCFramework statically includes OpenSSL 3.6.3.

The current OpenSSL archives were available only for `arm64` and were built with a macOS 26.0 deployment target. The XCFramework therefore has no Intel slice. The OpenSSL license is included beside the framework as `OPENSSL-LICENSE.txt`.

## Add to an Xcode app or tool

Add `Heartstrings.xcframework` to **Frameworks, Libraries, and Embedded Content**, selecting **Do Not Embed**. Set the target's C++ dialect to C++23 and deployment target to macOS 26.0 or later. Include the public header:

```cpp
#include <Heartstrings/Heartstrings.hpp>
```

## Server interface

```cpp
namespace Heartstrings {
    struct TLSConfiguration;
    class Server;
}
```

`Server` exposes `File_Link(route, file)`, `enable_tls(configuration)`, `set_maximum_connections(limit)`, `start()`, blocking `run()`, `stop()`, `port()`, and `https_port()`. `TLSConfiguration` accepts the certificate-chain path, private-key path, and optional client-certificate verification settings. Call `enable_tls()` and set connection limits before `start()`.

The default is **128 tracked client connections total across HTTP and HTTPS**. Use `set_maximum_connections(positive_limit)` before `start()` to choose another limit. It returns an error for zero or if called after startup. When the limit is reached, newly accepted sockets are closed immediately without allocating per-client or TLS session state; clients may see a connection close rather than an HTTP error response.

The `Server(int port)` constructor uses a **30-second absolute request-read deadline**. To choose another positive duration, use `Server(int port, std::chrono::milliseconds request_read_timeout)`. The deadline starts when the first bytes of a request arrive and is not extended by trickled bytes. If an incomplete request exceeds it, the server returns `408 Request Timeout` and closes that connection. The separate 30-second keep-alive idle timeout is unchanged. The request deadline covers HTTP request reads after TLS handshake; it is not a TLS handshake or response-write timeout.

```cpp
Heartstrings::Server server(8080);
if (auto linked = server.File_Link("/", "www/index.html"); !linked) {
    // Handle linked.error().
}
if (auto started = server.start(); !started) {
    // Handle started.error().
}
// Run server.run() on a thread; call server.stop() to stop it.
```

The façade hides OpenSSL and the internal networking implementation from consumers. The lower-level `socket_native` header remains available to source builds, but directly including it still requires OpenSSL development headers and libraries.
