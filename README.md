# Heartstrings

**Sequential code:** X-18

Heartstrings is a C++23, macOS-focused HTTP server project with a non-Apple POSIX `poll` backend. It contains a self-written HTTP request parser and response serializer, optional OpenSSL TLS, a bounded macOS task scheduler, and an Aegis-backed macOS request-storage adapter.

The overall project version is defined in [`ProjectVersionInfo.h`](ProjectVersionInfo.h): **Valley River 10008 (Beta)**. `Subsystems/VersionInfo.h` describes the subsystem version separately; it is not the X-18 project version.

> **Beta notice:** Heartstrings X-18 is an experimental, macOS-focused HTTP/1.x server with optional HTTPS, explicit static-file routing, keep-alive, and request pipelining. It is suitable for evaluation and testing, but is not yet recommended for production workloads.

## Project layout

```text
Http_Handling/
  HttpParser.hpp          HTTP request parser
  HttpSerializer.h        HTTP response serializer
NetworkingCore/
  non_blocking_native.h   macOS HTTP/HTTPS server and Aegis request storage
  PortableServer.hpp     non-Apple POSIX HTTP/HTTPS server using poll
  ApplicationHandler.hpp exact file routing and request handler
  PlatformFileHandler.swift macOS file-open and response-metadata implementation
  Heartstrings.hpp        compiled, dependency-hiding server API
  Heartstrings.cpp        implementation of the compiled API
Subsystems/
  Aegis.hpp               Aegis memory manager
  StateMachine.hpp        State-machine subsystem
  ThreadPool.hpp          Queued task scheduler (three concurrent tasks)
  VersionInfo.h           Subsystem version information
tests/
  tests.cpp               GoogleTest tests, including a loopback server test
  fixtures/               Test-only self-signed TLS certificate and key
API_httpparser.md         HTTP parser API reference
API_httpserializer.md     HTTP serializer API reference
API_tls.md                TLS configuration and behavior
API_applicationhandler.md File routing and handler API
API_xcode_library.md     Xcode XCFramework consumption guide
ProjectVersionInfo.h      X-18 project version information
```

## Current HTTP server behavior

`socket_native` in `NetworkingCore/non_blocking_native.h` provides a non-blocking IPv4 TCP server using `kqueue`. Plain HTTP remains the standard/default feature. HTTPS is enabled as an optional second listener on a separate port:

1. `start()` creates, binds, and listens on the configured port. Port `0` asks the OS to choose a free port; retrieve it with `port()`.
2. `run()` blocks in the event loop, accepts clients, and accumulates request bytes across partial reads.
3. Parsed requests are handled by the file router. Register files with `File_Link(route, file)`. The routing table stores each route's file path and expected filename, not file contents. On macOS, Swift performs file inspection and content-type selection; on other POSIX systems, the C++23 `PortableServer` backend uses `poll`.
4. `GET` streams the linked file in bounded 64 KiB chunks with a `Content-Length`; the whole file is not loaded into memory. `HEAD` serves its headers without a body. Unknown routes/files return `404`; unsupported methods return `405`.
5. HTTP/1.1 connections remain alive unless a request says `Connection: close`. HTTP/1.0 connections close by default and remain open only with `Connection: keep-alive`. Pipelined requests are processed and answered in order.
6. At most 128 client connections are tracked by default across HTTP and HTTPS. Configure another positive limit before `start()` with `set_maximum_connections(limit)`; excess accepted sockets are immediately closed. Idle keep-alive connections close after 30 seconds. Incomplete requests receive `408 Request Timeout` after the configured absolute read deadline. Call `stop()` from another thread to ask the event loop to exit.
7. The default memory budget is 512 MiB. On macOS, it uses the lazy 80/20 age-weighted allocator described in the API docs. The non-Apple POSIX backend applies the configured limit to per-connection request buffering; it does not yet reproduce the macOS Aegis peak-accounting policy.

On macOS, each client connection owns its own StateMachine. It moves from `ReadingHeaders` to `KeepAlive` while a request is handled, returns to `ReadingHeaders` after a persistent response, and enters `Closing` when the connection should end.

To enable HTTPS, call `enable_tls(TLSConfiguration)` before `start()`, supplying a PEM certificate chain and matching private key. Client-certificate verification is optional and can be required with a CA file. HTTP and HTTPS use separate ports; both listeners share the event loop and request pipeline.

The macOS shared task pool allows at most three active tasks; the non-Apple POSIX backend handles clients on dedicated threads. The parser limits request bodies to 16 MiB, headers to 64 KiB and 100 fields. Routes use exact URL-path matches; query strings do not affect the match. This is a simple static-file handler, not a dynamic application framework.

This project currently has test executables rather than a standalone server executable. To embed the server, include `NetworkingCore/Heartstrings.hpp` and use:

```cpp
Heartstrings::Server server(8080);
if (auto limited = server.set_maximum_connections(64); !limited) {
    // Handle limited.error(). Configure before start().
}
if (auto budget = server.set_memory_budget(256 * 1024 * 1024); !budget) {
    // Handle budget.error(). This example sets a 256 MiB managed payload budget.
}
if (auto linked = server.File_Link("/", "www/index.html"); !linked) {
    // Handle linked.error().
}
if (auto started = server.start(); !started) {
    // Handle started.error().
}

// Blocks until server.stop() is called from another thread.
if (auto result = server.run(); !result) {
    // Handle result.error().
}
```

For use as a prebuilt Xcode library without a separate OpenSSL installation, see the [XCFramework guide](API_xcode_library.md). The current binary in the sibling `X-18 Production` folder supports Apple Silicon only and requires macOS 26.0 or later; the source-level server API above remains available for direct builds.

## Build and run tests

Requirements: macOS, Xcode with C++23 support, OpenSSL 3, and GoogleTest installed at the include/library paths currently configured in `X-18.xcodeproj/project.pbxproj`. The TLS fixtures in `tests/fixtures/` are test-only.

```sh
xcodebuild \
  -project X-18.xcodeproj \
  -scheme test \
  -configuration Debug \
  -destination 'platform=macOS' \
  -derivedDataPath /tmp/X-18-DerivedData \
  build

/tmp/X-18-DerivedData/Build/Products/Debug/test
```

The tests cover parser validation and chunk framing, file routing and multi-chunk file streaming, keep-alive and pipelined request ordering, slow-request deadlines over HTTP and HTTPS, optional and required client-certificate behavior, connection limits, 80/20 age-weighted memory allocation, the three-task queue limit, and an Aegis storage/parser round trip.

On non-Apple POSIX systems, CMake builds the C++ server and its handler/server regression tests. This backend requires OpenSSL 3 and GoogleTest:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## API references

- [HTTP parser API](API_httpparser.md)
- [HTTP serializer API](API_httpserializer.md)
- [TLS API and configuration](API_tls.md)
- [Application handler and routing API](API_applicationhandler.md)
- [Xcode library API and XCFramework](API_xcode_library.md)
