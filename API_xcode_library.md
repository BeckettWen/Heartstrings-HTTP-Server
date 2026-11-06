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

`Server` exposes `File_Link(route, file)`, `enable_tls(configuration)`, `start()`, blocking `run()`, `stop()`, `port()`, and `https_port()`. `TLSConfiguration` accepts the certificate-chain path, private-key path, and optional client-certificate verification settings. Call `enable_tls()` before `start()`.

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
