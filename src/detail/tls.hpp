// mirrorcpp/detail/tls.hpp — OpenSSL setup, handshake, pin check (design §5.3, §3.6, §9).
//
// SslStream is an internal RAII wrapper around an established TLS 1.3 client
// connection: it owns the SSL_CTX/SSL and the underlying Socket, performs the
// handshake under a deadline with hostname verification and (optionally)
// certificate-fingerprint pinning, and then exposes the same line framing as
// the other transports (the mirror sends nothing until Register*).
//
// This is an internal header (src/detail); it may include OpenSSL and net.hpp.
// The public transport.hpp keeps TlsTransport's internals behind a forward-
// declared detail::SslStream so OpenSSL never leaks into public headers.
#ifndef MIRRORCPP_DETAIL_TLS_HPP
#define MIRRORCPP_DETAIL_TLS_HPP

#include <mirrorcpp/transport.hpp>

#include "net.hpp"

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mirrorcpp::detail {

class SslStream {
public:
  SslStream() = default;
  ~SslStream();

  SslStream(const SslStream&) = delete;
  SslStream& operator=(const SslStream&) = delete;
  SslStream(SslStream&& other) noexcept;
  SslStream& operator=(SslStream&& other) noexcept;

  // Build the client SSL_CTX from `opts` (key-mode check on POSIX, CA + client
  // cert/key, TLS 1.3 only, pin normalization), connect the socket, set up
  // hostname verification, and complete the handshake within
  // opts.handshake_timeout. On success the connection is ready for line
  // framing; peer_fingerprint() holds the verified peer's leaf fingerprint.
  Result<void> connect(std::string_view host, std::uint16_t port,
                       const mirrorcpp::TlsOptions& opts);

  // Write raw bytes (no framing).
  Result<void> write_all(std::string_view data);

  // Buffered line read, no timeout (same framing semantics as detail/net).
  Result<std::string> read_line();

  void close() noexcept;

  // Lowercase hex SHA-256 of the peer LEAF certificate's DER bytes. Empty if no
  // peer certificate is available (e.g. connection not yet established).
  std::string peer_fingerprint();

  bool valid() const noexcept { return ssl_ != nullptr; }

private:
  SSL_CTX* ctx_ = nullptr;
  SSL* ssl_ = nullptr;
  Socket sock_;
  std::string buf_;
  std::string fingerprint_;
};

} // namespace mirrorcpp::detail

#endif

