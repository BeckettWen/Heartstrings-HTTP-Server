# TLS API

TLS support is part of `socket_native` in `NetworkingCore/non_blocking_native.h`. It uses OpenSSL and adds a second, optional listener without changing the existing plaintext HTTP listener.

## `TLSConfiguration`

```cpp
struct TLSConfiguration {
    int port = 0;
    std::string certificate_chain_file;
    std::string private_key_file;
    bool require_client_certificate = false;
    std::string client_ca_file;
};
```

- `port` selects the HTTPS listener port. `0` asks the OS to choose a free port.
- `certificate_chain_file` is a PEM certificate chain presented by the server.
- `private_key_file` is the matching PEM private key.
- `require_client_certificate` enables mutual TLS and requires a verified client certificate.
- `client_ca_file` is a PEM CA file used to validate client certificates. It is required when `require_client_certificate` is true. When provided but client certificates are not required, a client may omit its certificate; if it supplies one, it must validate against this CA.

## `socket_native` TLS methods

### `enable_tls`

```cpp
std::expected<void, std::string> enable_tls(TLSConfiguration configuration);
```

Configures the optional HTTPS listener. Call before `start()`. The server checks the certificate/key pair during startup. TLS uses a minimum protocol version of TLS 1.2.

### `https_port`

```cpp
int https_port() const noexcept;
```

Returns the bound HTTPS port after `start()`. Returns `0` when TLS was not configured.

The existing `port()` continues to return the plaintext HTTP port. Both listeners run through the same `kqueue` event loop, and both use the same request parser, Aegis storage path, response serializer, and task pool.

## Example

```cpp
socket_native server(8080); // Plain HTTP remains enabled.

TLSConfiguration tls{
    .port = 8443,
    .certificate_chain_file = "certs/server-chain.pem",
    .private_key_file = "certs/server-key.pem",
    .require_client_certificate = false
};

if (auto configured = server.enable_tls(std::move(tls)); !configured) {
    // Handle configured.error().
}
if (auto started = server.start(); !started) {
    // Handle started.error().
}

// server.port() is the HTTP port; server.https_port() is the HTTPS port.
server.run();
```

To require client certificates, set `require_client_certificate = true` and provide `client_ca_file`. Plain HTTP remains available on the constructor's port; it is not silently upgraded or redirected.

## Current behavior and limitations

- TLS handshakes and reads are non-blocking and coordinated with `kqueue`.
- After the TLS handshake, incomplete HTTP requests use the server's absolute request-read deadline (30 seconds by default); incoming bytes do not extend it. This is not a TLS handshake or response-write timeout.
- Each accepted TLS connection owns its OpenSSL `SSL` session. Once its HTTP request is complete, the session moves with the connection into request processing; the session is not used concurrently by the event loop and worker tasks.
- TLS response writes handle partial writes and wait for the socket readiness requested by OpenSSL.
- HTTP and HTTPS use separate ports; this implementation does not sniff and multiplex both protocols on one port.
- The default client-certificate policy is off. Server-certificate presentation is enabled when TLS is configured; clients remain responsible for validating the server certificate.
- Test certificates under `tests/fixtures/` are self-signed and for tests only. Do not use them in a deployed server.

## OpenSSL references

Use the official OpenSSL documentation matching the installed OpenSSL version, particularly the manual pages for `SSL_CTX_new`, `SSL_CTX_use_certificate`, `SSL_CTX_set_verify`, `SSL_accept`, `SSL_get_error`, `SSL_read_ex`, `SSL_write_ex`, `SSL_shutdown`, and `ERR_get_error`.
