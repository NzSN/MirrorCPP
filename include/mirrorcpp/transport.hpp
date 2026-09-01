// mirrorcpp/transport.hpp — Transport interface + Stdio/Tcp/Tls transports (design §5.3).
//
// Transport moves *lines* (like MirrorECMA's Transport): it knows nothing about
// JSON or the protocol. Messages are single-line JSON objects terminated by '\n';
// this interface owns that framing.
#ifndef MIRRORCPP_TRANSPORT_HPP
#define MIRRORCPP_TRANSPORT_HPP

#include <mirrorcpp/error.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mirrorcpp {

namespace detail { class SslStream; } // PIMPL for TlsTransport (defined in src/detail/tls.cpp)

// ---------------------------------------------------------------------------
// Transport interface (design §5.3)
// ---------------------------------------------------------------------------
class Transport {
public:
  virtual ~Transport() = default;

  // Append '\n' to `line` (if not already newline-terminated), send and flush.
  // Rejects input containing an embedded '\n' with ErrorKind::io and a message
  // documenting the framing violation (defensive; the JSON encoder never emits
  // embedded newlines — design §5.3). Nothing is written on rejection.
  virtual Result<void> send_line(std::string_view line) = 0;

  // Read exactly one line, without its trailing '\n'. Has NO default timeout
  // (Apalache can legitimately take minutes — design §3.1). A clean peer EOF
  // mid-session is ErrorKind::io "transport closed unexpectedly".
  virtual Result<std::string> recv_line() = 0;

  // Close the transport. stdio: closes stdin, waits and reaps the child, and
  // returns the child's exit code. TCP/TLS: closes the socket and returns 0.
  // Idempotent: subsequent calls return the last close result.
  virtual Result<long> close() = 0;

  // Whether this transport can carry the async job interface (guide §6):
  // server-mode connections (TCP/TLS) can; stdio cannot (the mirror rejects
  // async messages on a stdio session with register_error). The async submit
  // API refuses up front on a non-capable transport. Default: false.
  virtual bool async_capable() const noexcept { return false; }
};

// ---------------------------------------------------------------------------
// Options (design §5.3)
// ---------------------------------------------------------------------------
struct SpawnOptions {
  // env, cwd — future (design §5.3)
};

struct ConnectOptions {
  std::chrono::milliseconds connect_timeout{10000}; // 10 s default
};

struct TlsOptions {
  std::filesystem::path ca_path, cert_path, key_path; // PEM; key 0600 on POSIX
  std::optional<std::string> pin;                     // 64 lowercase hex chars, normalized
  std::optional<std::string> servername;              // SNI/SAN name; defaults to host
  std::chrono::milliseconds handshake_timeout{10000}; // 10 s default
};

// ---------------------------------------------------------------------------
// Transport factories (design §5.3)
// ---------------------------------------------------------------------------

// Spawn the mirror as a child process with piped stdin/stdout and inherited
// stderr; one session per child. Returns nullptr if the process failed to start
// (ErrorKind::spawn path surfaces via the returned transport's methods / close).
std::unique_ptr<Transport> spawn_mirror(const std::filesystem::path& mirror_bin,
                                        SpawnOptions options = {});

// Connect to a running mirror over plain TCP (ModelMirrors --serve <port>).
Result<std::unique_ptr<Transport>> connect_tcp(std::string_view host, std::uint16_t port,
                                               ConnectOptions options = {});

// TLS 1.3 + client certificate + fingerprint pinning (design §5.3, §3.6, §9).
// The mirror sends nothing until a Register* message, so the same newline line
// framing as the other transports is used after the handshake.
class TlsTransport : public Transport {
public:
  ~TlsTransport() override;

  Result<void> send_line(std::string_view line) override;
  Result<std::string> recv_line() override;
  Result<long> close() override;
  bool async_capable() const noexcept override { return true; } // server mode (guide §6)

  // Lowercase hex SHA-256 of the peer LEAF certificate's DER bytes (design
  // §3.6). Empty if the handshake has not been completed.
  // NOTE: intentionally NON-virtual and TlsTransport-only. connect_tls /
  // connect_from_registry return unique_ptr<TlsTransport> specifically so
  // callers may upcast to Transport* for polymorphism; code that needs the
  // fingerprint after such an upcast must dynamic_cast<TlsTransport*> first.
  std::string peer_fingerprint() const;

private:
  friend Result<std::unique_ptr<TlsTransport>> connect_tls(std::string_view,
                                                           std::uint16_t,
                                                           const TlsOptions&);
  explicit TlsTransport(std::unique_ptr<detail::SslStream> stream);
  std::unique_ptr<detail::SslStream> stream_;
  bool closed_ = false;
};

Result<std::unique_ptr<TlsTransport>> connect_tls(std::string_view host, std::uint16_t port,
                                                  const TlsOptions& options);

} // namespace mirrorcpp

#endif

