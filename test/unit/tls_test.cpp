// mirrorcpp TLS unit tests (design §3.6, §5.3, §9):
//   - throwaway CA/server/client PKI generated at test time via the openssl CLI
//     (server cert carries an IP:127.0.0.1 SAN);
//   - loopback handshake against a small C++ TLS server (server requires a
//     client cert, TLS 1.3 only by default);
//   - good handshake + fingerprint matches `openssl x509 -fingerprint -sha256`;
//   - wrong-CA client cert fails; pin match/mismatch; key mode 0644 refusal;
//   - TLS 1.2-only server fails against the TLS 1.3-only client.
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>

using namespace mirrorcpp;

// Shared throwaway-PKI + loopback-TLS-server fixtures (also used by the
// registry tests): certs(), TlsTestServer, default_opts().
#include "tls_fixture.hpp"

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("tls: good handshake, fingerprint matches openssl, framing works", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), default_opts());
  REQUIRE(t.has_value());
  // Fingerprint must match `openssl x509 -fingerprint -sha256`.
  REQUIRE(t.value()->peer_fingerprint() == c.server_fingerprint);
  // Line framing works over TLS (mirror sends nothing until Register* — we
  // drive the first line ourselves).
  REQUIRE(t.value()->send_line("ping").has_value());
  const auto line = t.value()->recv_line();
  REQUIRE(line.has_value());
  REQUIRE(*line == "ping");
  const auto cl = t.value()->close();
  REQUIRE(cl.has_value());
  REQUIRE(*cl == 0L);
}

TEST_CASE("tls: pin match succeeds", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto opts = default_opts();
  opts.pin = c.server_fingerprint;
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  REQUIRE(t.has_value());
  REQUIRE(t.value()->peer_fingerprint() == c.server_fingerprint);
  const auto cl = t.value()->close();
  REQUIRE(cl.has_value());
  REQUIRE(*cl == 0L);
}

TEST_CASE("tls: CN-only server certificate is rejected (SAN required)", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.cn_server_cert, c.cn_server_key,
                    TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto opts = default_opts();
  opts.servername = "localhost";
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::tls);
}

TEST_CASE("tls: pin mismatch fails with a tls error", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto opts = default_opts();
  opts.pin = std::string(64, '0'); // definitely wrong
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::tls);
  REQUIRE(t.error().message.find("mismatch") != std::string::npos);
}

TEST_CASE("tls: invalid pin is rejected before connecting", "[tls]") {
  auto opts = default_opts();
  opts.pin = "not-a-valid-pin";
  // No server needed: the pin is validated before any I/O.
  auto t = connect_tls("127.0.0.1", 1, opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::tls);
}

TEST_CASE("tls: wrong-CA client certificate fails", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto opts = default_opts();
  opts.cert_path = c.client2_cert;
  opts.key_path = c.client2_key;
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  // In TLS 1.3 the server rejects the client cert before finishing its side of
  // the handshake, so connect_tls itself typically fails. If the handshake
  // superficially completed, the alert surfaces on the first I/O instead.
  if (t.has_value()) {
    const auto r = t.value()->recv_line();
    REQUIRE_FALSE(r);
    const bool tls_or_io = (r.error().kind == ErrorKind::tls) ||
                           (r.error().kind == ErrorKind::io);
    REQUIRE(tls_or_io);
    (void)t.value()->close();
  } else {
    REQUIRE(t.error().kind == ErrorKind::tls);
  }
}

TEST_CASE("tls: client key mode 0644 is refused, 0600 works", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  auto opts = default_opts();
  opts.key_path = c.client_0644_key;
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::tls);
  REQUIRE(t.error().message.find("0600") != std::string::npos);

  // Same key with mode 0600 succeeds.
  opts.key_path = c.client_key;
  auto ok = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
  REQUIRE(ok.has_value());
  const auto cl = ok.value()->close();
  REQUIRE(cl.has_value());
  REQUIRE(*cl == 0L);
}

TEST_CASE("tls: exact private-key mode is checked without changing credentials", "[tls][key-mode]") {
  const auto& c = certs();
  const auto read_bytes = [](const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  };
  const std::string key = c.dir + "/mode-matrix.key";
  const std::string original = read_bytes(c.client_key);
  std::filesystem::copy_file(c.client_key, key,
      std::filesystem::copy_options::overwrite_existing);
  for (const mode_t mode : {0400, 0500, 0700, 0644, 0660, 01600, 02600, 04600}) {
    INFO("requested mode " << std::oct << mode);
    REQUIRE(::chmod(key.c_str(), mode) == 0);
    struct stat before {};
    REQUIRE(::stat(key.c_str(), &before) == 0);
    // Failure to establish a fixture is an explicit platform limitation, not
    // an unexercised permission-policy pass.
    REQUIRE((before.st_mode & 07777) == mode);
    auto opts = default_opts();
    opts.key_path = key;
    auto result = connect_tls("127.0.0.1", 1, opts);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().kind == ErrorKind::tls);
    REQUIRE(result.error().message.find("must be mode 0600") != std::string::npos);
    struct stat after {};
    REQUIRE(::stat(key.c_str(), &after) == 0);
    REQUIRE((after.st_mode & 07777) == mode);
    REQUIRE(read_bytes(key) == original);
  }
  REQUIRE(::chmod(key.c_str(), 0600) == 0);
  {
    TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
    auto opts = default_opts();
    opts.key_path = key;
    auto result = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), opts);
    REQUIRE(result);
    REQUIRE(result.value()->close());
  }
  struct stat accepted {};
  REQUIRE(::stat(key.c_str(), &accepted) == 0);
  REQUIRE((accepted.st_mode & 07777) == 0600);
  REQUIRE(read_bytes(key) == original);

  const std::string invalid = c.dir + "/mode-matrix-invalid.key";
  { std::ofstream stream(invalid, std::ios::binary); stream << "invalid PEM\n"; }
  REQUIRE(::chmod(invalid.c_str(), 0600) == 0);
  auto opts = default_opts();
  opts.key_path = invalid;
  auto result = connect_tls("127.0.0.1", 1, opts);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().message.find("must be mode 0600") == std::string::npos);
  REQUIRE(result.error().message.find("SSL_CTX_use_PrivateKey_file") != std::string::npos);
  REQUIRE(read_bytes(invalid) == "invalid PEM\n");
  struct stat invalid_after {};
  REQUIRE(::stat(invalid.c_str(), &invalid_after) == 0);
  REQUIRE((invalid_after.st_mode & 07777) == 0600);
}

TEST_CASE("tls: TLS 1.2-only server fails against the TLS 1.3-only client", "[tls]") {
  const auto& c = certs();
  TlsTestServer srv12(c.ca, c.server_cert, c.server_key, TLS1_2_VERSION, TLS1_2_VERSION, true);
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv12.port()), default_opts());
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::tls);
}

TEST_CASE("tls: oversized inbound protocol payload is rejected", "[tls][framing]") {
  const auto& c = certs();
  std::string oversized(65'536, 'x');
  oversized.push_back('\n');
  TlsTestServer srv(c.ca, c.server_cert, c.server_key,
                    TLS1_3_VERSION, TLS1_3_VERSION, true,
                    std::move(oversized));
  auto t = connect_tls("127.0.0.1", static_cast<std::uint16_t>(srv.port()), default_opts());
  REQUIRE(t.has_value());

  const auto line = t.value()->recv_line();
  REQUIRE_FALSE(line.has_value());
  REQUIRE(line.error().kind == ErrorKind::io);
  REQUIRE(line.error().message.find("65535-byte") != std::string::npos);
  (void)t.value()->close();
}
