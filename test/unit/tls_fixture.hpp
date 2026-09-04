// tls_fixture.hpp — shared throwaway-PKI + loopback TLS server fixtures for the
// TLS and registry unit tests (design 3.6/5.4). Extracted from tls_test.cpp so
// registry_test.cpp can reuse the same certs ("TLS parts reuse t7's cert
// fixtures").
//
// - TestCerts: CA / server (SAN IP:127.0.0.1) / client certs + the server's
//   sha256 fingerprint as reported by `openssl x509 -fingerprint -sha256`.
// - TlsTestServer: one-connection loopback TLS server on OpenSSL; requires a
//   client cert by default; TLS 1.3 by default; echoes received lines.
#ifndef MIRRORCPP_TEST_TLS_FIXTURE_HPP
#define MIRRORCPP_TEST_TLS_FIXTURE_HPP

#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using namespace mirrorcpp;

namespace {

// OpenSSL's socket BIO (SSL_write, SSL_shutdown, post-handshake session
// tickets) can raise SIGPIPE when the peer has closed with a RST; the standard
// mitigation (OpenSSL docs, s_client, curl) is to ignore SIGPIPE. Installed
// via static init so a peer that aborts mid-session cannot kill the runner.
struct IgnoreSigpipeFixture {
  IgnoreSigpipeFixture() { ::signal(SIGPIPE, SIG_IGN); }
};
const IgnoreSigpipeFixture g_ignore_sigpipe_fixture;

bool run_cmd(const std::string& cmd) {
  return std::system(cmd.c_str()) == 0;
}

std::string read_stdout(const std::string& cmd) {
  std::string out;
  FILE* f = ::popen(cmd.c_str(), "r");
  if (!f) return out;
  char buf[256];
  while (::fgets(buf, static_cast<int>(sizeof buf), f)) out += buf;
  ::pclose(f);
  return out;
}

struct TestCerts {
  std::string dir;
  std::string ca, ca_key, server_cert, server_key;
  std::string cn_server_cert, cn_server_key;
  std::string client_cert, client_key;
  std::string client2_cert, client2_key; // signed by a different CA
  std::string client_0644_key;           // 0644 copy of the client key
  std::string server_fingerprint;        // lowercase hex, from `openssl x509 -fingerprint -sha256`

  TestCerts() {
    char tmp[] = "/tmp/mirrorcpp-tls-XXXXXX";
    char* d = ::mkdtemp(tmp);
    REQUIRE(d != nullptr);
    dir = d;
    ca = dir + "/ca.crt"; ca_key = dir + "/ca.key";
    server_cert = dir + "/server.crt"; server_key = dir + "/server.key";
    cn_server_cert = dir + "/cn-server.crt"; cn_server_key = dir + "/cn-server.key";
    client_cert = dir + "/client.crt"; client_key = dir + "/client.key";
    client2_cert = dir + "/client2.crt"; client2_key = dir + "/client2.key";
    client_0644_key = dir + "/client_0644.key";

    // CA
    REQUIRE(run_cmd("openssl req -x509 -newkey rsa:2048 -nodes -keyout " + ca_key +
                    " -out " + ca + " -days 1 -subj \"/CN=MirrorCPP Test CA\" "
                    "-addext \"basicConstraints=critical,CA:TRUE\" 2>/dev/null"));
    // server cert with IP:127.0.0.1 SAN
    REQUIRE(run_cmd("openssl req -newkey rsa:2048 -nodes -keyout " + server_key +
                    " -out " + dir + "/server.csr -subj \"/CN=localhost\" "
                    "-addext \"subjectAltName=IP:127.0.0.1\" 2>/dev/null"));
    REQUIRE(run_cmd("openssl x509 -req -in " + dir + "/server.csr -CA " + ca +
                    " -CAkey " + ca_key + " -CAcreateserial -out " + server_cert +
                    " -days 1 -sha256 -copy_extensions copy 2>/dev/null"));
    // CN-only server cert: clients must reject it because server identity must
    // be present in subjectAltName, never only in the legacy common name.
    REQUIRE(run_cmd("openssl req -newkey rsa:2048 -nodes -keyout " + cn_server_key +
                    " -out " + dir + "/cn-server.csr -subj \"/CN=localhost\" 2>/dev/null"));
    REQUIRE(run_cmd("openssl x509 -req -in " + dir + "/cn-server.csr -CA " + ca +
                    " -CAkey " + ca_key + " -CAcreateserial -out " + cn_server_cert +
                    " -days 1 -sha256 2>/dev/null"));
    // client cert (good)
    REQUIRE(run_cmd("openssl req -newkey rsa:2048 -nodes -keyout " + client_key +
                    " -out " + dir + "/client.csr -subj \"/CN=mirrorcpp-test-client\" 2>/dev/null"));
    REQUIRE(run_cmd("openssl x509 -req -in " + dir + "/client.csr -CA " + ca +
                    " -CAkey " + ca_key + " -CAcreateserial -out " + client_cert +
                    " -days 1 -sha256 2>/dev/null"));
    ::chmod(client_key.c_str(), 0600);
    // 0644 copy for the key-mode test
    REQUIRE(run_cmd("cp " + client_key + " " + client_0644_key));
    ::chmod(client_0644_key.c_str(), 0644);
    // wrong-CA client (signed by a second CA)
    const std::string ca2 = dir + "/ca2.crt", ca2k = dir + "/ca2.key";
    REQUIRE(run_cmd("openssl req -x509 -newkey rsa:2048 -nodes -keyout " + ca2k +
                    " -out " + ca2 + " -days 1 -subj \"/CN=MirrorCPP Test CA2\" "
                    "-addext \"basicConstraints=critical,CA:TRUE\" 2>/dev/null"));
    REQUIRE(run_cmd("openssl req -newkey rsa:2048 -nodes -keyout " + client2_key +
                    " -out " + dir + "/client2.csr -subj \"/CN=mirrorcpp-bad-client\" 2>/dev/null"));
    REQUIRE(run_cmd("openssl x509 -req -in " + dir + "/client2.csr -CA " + ca2 +
                    " -CAkey " + ca2k + " -CAcreateserial -out " + client2_cert +
                    " -days 1 -sha256 2>/dev/null"));
    ::chmod(client2_key.c_str(), 0600);

    // expected fingerprint from the openssl CLI (e.g. "sha256 Fingerprint=AA:BB:..")
    const std::string fp = read_stdout(
        "openssl x509 -in " + server_cert + " -noout -fingerprint -sha256 2>/dev/null");
    const auto eq = fp.find('=');
    REQUIRE(eq != std::string::npos);
    std::string clean;
    for (char c : fp.substr(eq + 1)) {
      if (c != ':') clean += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    while (!clean.empty() && (clean.back() == '\n' || clean.back() == '\r' || clean.back() == ' ')) clean.pop_back();
    server_fingerprint = clean;
    REQUIRE(server_fingerprint.size() == 64);
  }

  ~TestCerts() {
    ::system(("rm -rf '" + dir + "'").c_str());
  }
};

const TestCerts& certs() {
  static TestCerts c;
  return c;
}

// ---------------------------------------------------------------------------
// Minimal loopback TLS server (one connection). Requires a client cert unless
// disabled. Echoes received lines back (verifies the line framing over TLS).
// ---------------------------------------------------------------------------
class TlsTestServer {
public:
  TlsTestServer(const std::string& ca, const std::string& cert, const std::string& key,
                int tls_min, int tls_max, bool require_client_cert,
                std::optional<std::string> initial_payload = std::nullopt)
      : ca_(ca), cert_(cert), key_(key), tls_min_(tls_min), tls_max_(tls_max),
        require_client_(require_client_cert), initial_payload_(std::move(initial_payload)) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd_ >= 0);
    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    REQUIRE(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
    REQUIRE(::listen(listen_fd_, 4) == 0);
    socklen_t len = sizeof addr;
    REQUIRE(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
    port_ = ntohs(addr.sin_port);
    th_ = std::thread([this] { run(); });
  }

  ~TlsTestServer() {
    ::close(listen_fd_);
    if (th_.joinable()) th_.join();
  }

  int port() const { return port_; }

private:
  void run() {
    const int c = ::accept(listen_fd_, nullptr, nullptr);
    if (c < 0) return;
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) { ::close(c); return; }
    SSL_CTX_set_min_proto_version(ctx, tls_min_);
    SSL_CTX_set_max_proto_version(ctx, tls_max_);
    SSL_CTX_use_certificate_chain_file(ctx, cert_.c_str());
    SSL_CTX_use_PrivateKey_file(ctx, key_.c_str(), SSL_FILETYPE_PEM);
    if (require_client_) {
      SSL_CTX_load_verify_locations(ctx, ca_.c_str(), nullptr);
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, nullptr);
    }
    SSL* ssl = SSL_new(ctx);
    if (ssl) {
      SSL_set_fd(ssl, c);
      const int acc = SSL_accept(ssl);
      if (acc == 1) {
        if (initial_payload_) {
          std::size_t offset = 0;
          while (offset < initial_payload_->size()) {
            const int written = SSL_write(
                ssl, initial_payload_->data() + offset,
                static_cast<int>(initial_payload_->size() - offset));
            if (written <= 0) break;
            offset += static_cast<std::size_t>(written);
          }
        }
        // Echo lines back (line framing over TLS).
        char chunk[4096];
        std::string acc2;
        for (;;) {
          const int n = SSL_read(ssl, chunk, static_cast<int>(sizeof chunk));
          if (n <= 0) break;
          acc2.append(chunk, static_cast<std::size_t>(n));
          std::size_t pos;
          while ((pos = acc2.find('\n')) != std::string::npos) {
            std::string line = acc2.substr(0, pos);
            acc2.erase(0, pos + 1);
            std::string echo = line + "\n";
            SSL_write(ssl, echo.data(), static_cast<int>(echo.size()));
          }
        }
      }
      SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    ::close(c);
  }

  std::string ca_, cert_, key_;
  int tls_min_ = TLS1_3_VERSION;
  int tls_max_ = TLS1_3_VERSION;
  bool require_client_ = true;
  std::optional<std::string> initial_payload_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::thread th_;
};

// Default client options: trust the test CA, present the good client cert.
TlsOptions default_opts() {
  const auto& c = certs();
  TlsOptions o;
  o.ca_path = c.ca;
  o.cert_path = c.client_cert;
  o.key_path = c.client_key;
  return o;
}

} // namespace

#endif
