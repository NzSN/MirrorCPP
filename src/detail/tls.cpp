// mirrorcpp/detail/tls.cpp — OpenSSL setup, handshake, pin check (design §5.3, §3.6, §9).
//
// Implements detail::SslStream (RAII TLS 1.3 client connection) and the public
// TlsTransport / connect_tls. Built only when MIRRORCPP_WITH_TLS=ON.
#include "tls.hpp"

#include "line.hpp"

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace mirrorcpp {

using std::unexpected;

namespace detail {

namespace {

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

std::string ssl_error_string() {
  const unsigned long e = ERR_get_error();
  if (e == 0) return std::string("unknown OpenSSL error");
  char buf[256];
  ERR_error_string_n(e, buf, sizeof buf);
  return std::string(buf);
}

// POSIX only: refuse to use a client key with group/other permission bits set
// (design §9 / Protocol/Transport/Tls.hs — mirrors the Haskell/TS clients).
Result<void> check_key_mode(const std::filesystem::path& key_path) {
#ifdef _WIN32
  return {};
#else
  struct stat st {};
  if (::stat(key_path.c_str(), &st) != 0) {
    return unexpected(Error(ErrorKind::tls, "cannot stat client key " + key_path.string() +
                            ": " + std::strerror(errno)));
  }
  if ((st.st_mode & 0077) != 0) {
    return unexpected(Error(ErrorKind::tls,
                            "client key file " + key_path.string() +
                            " must be mode 0600 (group/other permission bits set)"));
  }
  return {};
#endif
}

// Normalize a certificate fingerprint: trim whitespace, lowercase, and require
// exactly ^[0-9a-f]{64}$. An invalid (or absent after trimming) pin is a tls
// error — the caller rejects the TlsOptions rather than silently unpinning.
Result<std::string> normalize_pin(std::string_view pin) {
  const std::size_t b = pin.find_first_not_of(" \t\r\n");
  if (b == std::string_view::npos) {
    return unexpected(Error(ErrorKind::tls, "pin is empty"));
  }
  const std::size_t e = pin.find_last_not_of(" \t\r\n");
  std::string p(pin.substr(b, e - b + 1));
  for (char& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (p.size() != 64) {
    return unexpected(Error(ErrorKind::tls,
                            "pin must be 64 lowercase hex chars, got " + std::to_string(p.size())));
  }
  for (char c : p) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return unexpected(Error(ErrorKind::tls, "pin is not valid hex: " + p));
    }
  }
  return p;
}

// True if `host` is an IPv4/IPv6 literal (needs SSL_set1_ip_asc, not SSL_set1_host).
bool is_ip_literal(const std::string& host) {
  struct in_addr a4 {};
  struct in6_addr a6 {};
  return ::inet_pton(AF_INET, host.c_str(), &a4) == 1 ||
         ::inet_pton(AF_INET6, host.c_str(), &a6) == 1;
}

// Complete SSL_connect under a deadline using non-blocking I/O + poll. The fd
// must be in non-blocking mode for the duration of this loop.
Result<void> handshake_with_deadline(SSL* ssl, long fd, std::chrono::milliseconds timeout) {
  const auto start = std::chrono::steady_clock::now();
  long long budget_ms = timeout.count();
  if (budget_ms <= 0) budget_ms = 1;
  for (;;) {
    const int r = SSL_connect(ssl);
    if (r == 1) return {};
    const int err = SSL_get_error(ssl, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start).count();
      const long long remaining = budget_ms - elapsed;
      if (remaining <= 0) {
        return unexpected(Error(ErrorKind::tls, "TLS handshake timed out"));
      }
      struct pollfd pfd {};
      pfd.fd = fd;
      pfd.events = static_cast<short>(err == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT);
      ::poll(&pfd, 1, static_cast<int>(remaining));
      continue;
    }
    return unexpected(Error(ErrorKind::tls,
                            "TLS handshake failed: " + ssl_error_string()));
  }
}

} // namespace

// ---------------------------------------------------------------------------
// detail::SslStream
// ---------------------------------------------------------------------------

SslStream::~SslStream() { close(); }

SslStream::SslStream(SslStream&& other) noexcept
    : ctx_(std::exchange(other.ctx_, nullptr)),
      ssl_(std::exchange(other.ssl_, nullptr)),
      sock_(std::move(other.sock_)),
      buf_(std::move(other.buf_)),
      fingerprint_(std::move(other.fingerprint_)) {}

SslStream& SslStream::operator=(SslStream&& other) noexcept {
  if (this != &other) {
    close();
    ctx_ = std::exchange(other.ctx_, nullptr);
    ssl_ = std::exchange(other.ssl_, nullptr);
    sock_ = std::move(other.sock_);
    buf_ = std::move(other.buf_);
    fingerprint_ = std::move(other.fingerprint_);
  }
  return *this;
}

void SslStream::close() noexcept {
  if (ssl_) {
    SSL_shutdown(ssl_); // best-effort
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ctx_) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
  sock_.close();
  buf_.clear();
  fingerprint_.clear();
}

Result<void> SslStream::connect(std::string_view host, std::uint16_t port,
                                const TlsOptions& opts) {
  close();

  // 1. Client key permissions (POSIX) — before loading anything.
  auto keymode = check_key_mode(opts.key_path);
  if (!keymode) return unexpected(keymode.error());

  // 2. Normalize the pin (if any); an invalid pin rejects the options.
  std::optional<std::string> pin;
  if (opts.pin) {
    auto p = normalize_pin(*opts.pin);
    if (!p) return unexpected(p.error());
    pin = *p;
  }

  // 3. Build the client SSL_CTX.
  SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
  if (!ctx) return unexpected(Error(ErrorKind::tls, "SSL_CTX_new failed"));
  ctx_ = ctx;

  // TLS 1.3 only (design §3.6): min == max == TLS1_3_VERSION.
  if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
    return unexpected(Error(ErrorKind::tls, "failed to restrict to TLS 1.3: " + ssl_error_string()));
  }

  if (SSL_CTX_load_verify_locations(ctx, opts.ca_path.c_str(), nullptr) != 1) {
    return unexpected(Error(ErrorKind::tls, "SSL_CTX_load_verify_locations(" + opts.ca_path.string() +
                            "): " + ssl_error_string()));
  }
  if (SSL_CTX_use_certificate_chain_file(ctx, opts.cert_path.c_str()) != 1) {
    return unexpected(Error(ErrorKind::tls, "SSL_CTX_use_certificate_chain_file(" + opts.cert_path.string() +
                            "): " + ssl_error_string()));
  }
  if (SSL_CTX_use_PrivateKey_file(ctx, opts.key_path.c_str(), SSL_FILETYPE_PEM) != 1) {
    return unexpected(Error(ErrorKind::tls, "SSL_CTX_use_PrivateKey_file(" + opts.key_path.string() +
                            "): " + ssl_error_string()));
  }
  if (SSL_CTX_check_private_key(ctx) != 1) {
    return unexpected(Error(ErrorKind::tls, "client private key does not match certificate: " + ssl_error_string()));
  }

  // 4. TCP connect (uses the handshake timeout as the connect deadline; design
  //    §3.6: both are configurable with a 10 s default).
  auto cr = sock_.connect(host, port, opts.handshake_timeout);
  if (!cr) return unexpected(cr.error());
  const long fd = sock_.native_handle();

  // 5. Create the SSL and bind the socket.
  SSL* ssl = SSL_new(ctx);
  if (!ssl) return unexpected(Error(ErrorKind::tls, "SSL_new failed"));
  ssl_ = ssl;
  SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
  if (SSL_set_fd(ssl, static_cast<int>(fd)) != 1) {
    return unexpected(Error(ErrorKind::tls, "SSL_set_fd failed: " + ssl_error_string()));
  }

  // 6. Hostname verification + SNI. servername (or host) is the SAN name.
  const std::string verify_name = opts.servername.value_or(std::string(host));
  SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
  X509_VERIFY_PARAM* vparam = SSL_get0_param(ssl);
  if (is_ip_literal(verify_name)) {
    // IP literal: verify against an IP SAN via X509_VERIFY_PARAM_set1_ip_asc
    // (registry entries are usually IPs — design §5.3).
    if (X509_VERIFY_PARAM_set1_ip_asc(vparam, verify_name.c_str()) != 1) {
      return unexpected(Error(ErrorKind::tls, "X509_VERIFY_PARAM_set1_ip_asc failed"));
    }
  } else {
    X509_VERIFY_PARAM_set_hostflags(vparam, X509_CHECK_FLAG_NEVER_CHECK_SUBJECT);
    if (SSL_set1_host(ssl, verify_name.c_str()) != 1) {
      return unexpected(Error(ErrorKind::tls, "SSL_set1_host failed"));
    }
    if (SSL_set_tlsext_host_name(ssl, verify_name.c_str()) != 1) {
      return unexpected(Error(ErrorKind::tls, "SSL_set_tlsext_host_name failed"));
    }
  }

  // 7. Handshake under the deadline (non-blocking + poll).
  sock_.set_nonblocking(true);
  auto hs = handshake_with_deadline(ssl, fd, opts.handshake_timeout);
  sock_.set_nonblocking(false);
  if (!hs) return unexpected(hs.error());

  // 8. Fingerprint the peer leaf and enforce the pin (if any).
  const std::string fp = peer_fingerprint();
  if (pin && fp != *pin) {
    std::string msg = "certificate fingerprint mismatch: expected " + *pin +
                      " got " + (fp.empty() ? "<none>" : fp);
    close();
    return unexpected(Error(ErrorKind::tls, std::move(msg)));
  }
  return {};
}

Result<void> SslStream::write_all(std::string_view data) {
  if (!ssl_) return unexpected(Error(ErrorKind::tls, "write_all: no TLS session"));
  std::size_t off = 0;
  while (off < data.size()) {
    const int n = SSL_write(ssl_, data.data() + off,
                            static_cast<int>(data.size() - off));
    if (n > 0) { off += static_cast<std::size_t>(n); continue; }
    const int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue; // EINTR with AUTO_RETRY
    if (err == SSL_ERROR_ZERO_RETURN) {
      return unexpected(Error(ErrorKind::io, "SSL_write: connection closed by peer"));
    }
    return unexpected(Error(ErrorKind::tls, "SSL_write failed: " + ssl_error_string()));
  }
  return {};
}

Result<std::string> SslStream::read_line() {
  if (!ssl_) return unexpected(Error(ErrorKind::io, "read_line: no TLS session"));
  for (;;) {
    const auto nl = buf_.find('\n');
    if (nl != std::string::npos) {
      std::string line = buf_.substr(0, nl);
      buf_.erase(0, nl + 1);
      return line;
    }
    char chunk[4096];
    const int n = SSL_read(ssl_, chunk, sizeof chunk);
    if (n > 0) { buf_.append(chunk, static_cast<std::size_t>(n)); continue; }
    const int err = SSL_get_error(ssl_, n);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue; // blocking fd: retry on EINTR
    if (err == SSL_ERROR_ZERO_RETURN) {
      // Clean peer EOF mid-session (design §5.3).
      return unexpected(Error(ErrorKind::io, "transport closed unexpectedly"));
    }
    return unexpected(Error(ErrorKind::tls, "SSL_read failed: " + ssl_error_string()));
  }
}

std::string SslStream::peer_fingerprint() {
  if (!fingerprint_.empty()) return fingerprint_;
  if (!ssl_) return {};
  X509* cert = SSL_get0_peer_certificate(ssl_); // borrowed
  if (!cert) return {};
  unsigned char* der = nullptr;
  const int len = i2d_X509(cert, &der);
  if (len <= 0) return {};
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int mdlen = 0;
  const int ok = EVP_Digest(der, static_cast<std::size_t>(len), md, &mdlen,
                            EVP_sha256(), nullptr);
  OPENSSL_free(der); // OPENSSL_free is a function-like macro; not addressable
  if (ok != 1) return {};
  std::string out;
  out.reserve(mdlen * 2);
  static const char* hex = "0123456789abcdef";
  for (unsigned int i = 0; i < mdlen; ++i) {
    out.push_back(hex[md[i] >> 4]);
    out.push_back(hex[md[i] & 0x0f]);
  }
  fingerprint_ = out;
  return fingerprint_;
}

} // namespace detail

// ---------------------------------------------------------------------------
// TlsTransport (public API)
// ---------------------------------------------------------------------------

TlsTransport::TlsTransport(std::unique_ptr<detail::SslStream> stream)
    : stream_(std::move(stream)) {}

TlsTransport::~TlsTransport() = default; // SslStream complete here

Result<void> TlsTransport::send_line(std::string_view line) {
  if (closed_) return unexpected(Error(ErrorKind::io, "send_line on closed transport"));
  auto r = detail::reject_embedded_newline(line);
  if (!r) return r;
  std::string framed(line);
  framed.push_back('\n');
  return stream_->write_all(framed);
}

Result<std::string> TlsTransport::recv_line() {
  if (closed_) return unexpected(Error(ErrorKind::io, "recv_line on closed transport"));
  return stream_->read_line();
}

Result<long> TlsTransport::close() {
  if (closed_) return 0;
  closed_ = true;
  stream_->close();
  return 0;
}

std::string TlsTransport::peer_fingerprint() const { return stream_->peer_fingerprint(); }

Result<std::unique_ptr<TlsTransport>> connect_tls(std::string_view host, std::uint16_t port,
                                                  const TlsOptions& opts) {
  auto stream = std::make_unique<detail::SslStream>();
  auto r = stream->connect(host, port, opts);
  if (!r) return unexpected(r.error());
  return std::unique_ptr<TlsTransport>(new TlsTransport(std::move(stream)));
}

} // namespace mirrorcpp
