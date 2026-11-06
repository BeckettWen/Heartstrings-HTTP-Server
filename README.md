# Heartstrings

**Sequential code:** X-18

Heartstrings is a C++23, macOS-focused HTTP server project. It contains a self-written HTTP request parser and response serializer, a non-blocking `kqueue` server with optional OpenSSL TLS, a bounded task scheduler, and an Aegis-backed request-storage adapter.

The overall project version is defined in [`ProjectVersionInfo.h`](ProjectVersionInfo.h): **Valley River 10002 (Beta)**. `Subsystems/VersionInfo.h` describes the subsystem version separately; it is not the X-18 project version.

## Project layout

```text
Http_Handling/
  HttpParser.hpp          HTTP request parser
  HttpSerializer.h        HTTP response serializer
NetworkingCore/
  non_blocking_native.h   macOS HTTP/HTTPS server and Aegis request storage
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
ProjectVersionInfo.h      X-18 project version information
```

## Current HTTP server behavior

`socket_native` in `NetworkingCore/non_blocking_native.h` provides a non-blocking IPv4 TCP server using `kqueue`. Plain HTTP remains the standard/default feature. HTTPS is enabled as an optional second listener on a separate port:

1. `start()` creates, binds, and listens on the configured port. Port `0` asks the OS to choose a free port; retrieve it with `port()`.
2. `run()` blocks in the event loop, accepts clients, and accumulates request bytes across partial reads.
3. A complete request is staged through the normal Aegis allocator, parsed, and sent through the shared task pool.
4. The server returns `200 OK` with the request body echoed, then closes that client connection.
5. Call `stop()` from another thread to ask the event loop to exit. The server drains request tasks before `run()` returns.

To enable HTTPS, call `enable_tls(TLSConfiguration)` before `start()`, supplying a PEM certificate chain and matching private key. Client-certificate verification is optional and can be required with a CA file. HTTP and HTTPS use separate ports; both listeners share the event loop and request pipeline.

The shared task pool allows at most three active tasks; extra tasks wait in its FIFO queue. The parser limits request bodies to 16 MiB, headers to 64 KiB and 100 fields. The server currently handles one request per connection, closes connections after responding, and does not provide routing, keep-alive, or application-level pipelining.

This project currently has a test executable rather than a standalone server executable. To embed the server, include `NetworkingCore/non_blocking_native.h` and use:

```cpp
socket_native server(8080);
if (auto started = server.start(); !started) {
    // Handle started.error().
}

// Blocks until server.stop() is called from another thread.
if (auto result = server.run(); !result) {
    // Handle result.error().
}
```

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

The tests cover serializer output, loopback HTTP and HTTPS request/response, optional and required client-certificate behavior, the three-task queue limit, and an Aegis storage/parser round trip.

## API references

- [HTTP parser API](API_httpparser.md)
- [HTTP serializer API](API_httpserializer.md)
- [TLS API and configuration](API_tls.md)
