// mirrorcpp registry unit tests (design 3.6, 5.4, 8 'Registry/TLS helpers'):
//   - small loopback HTTP stub serving canned Consul health responses
//   - fail-closed discovery matrix: unreachable, non-200, malformed JSON,
//     non-array top level, chunked (documented limitation), bad URL scheme
//   - entry-parsing matrix (total function): non-object Service, blank
//     Address, Port 0/65536/non-integer, malformed vs absent cert-sha256
//   - request shape (Consul health path, Host, Accept, Connection)
//   - connect_from_registry: no candidates, exhausted diagnostics, iteration
//     order (first-dead then live), pin_override precedence, absent pin.
// TLS parts reuse t7's throwaway PKI + loopback TLS server (tls_fixture.hpp).
#if MIRRORCPP_WITH_TLS
#include "tls_fixture.hpp"
#endif

#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace mirrorcpp;

namespace {

#if MIRRORCPP_WITH_TLS

// ---------------------------------------------------------------------------
// Loopback HTTP stub serving a canned response (one request per instance).
// Records the raw request so tests can lock the request shape.
// ---------------------------------------------------------------------------
class HttpStub {
public:
  explicit HttpStub(std::string body, int status = 200,
                    bool with_content_length = true, bool chunked = false)
      : body_(std::move(body)), status_(status),
        with_content_length_(with_content_length), chunked_(chunked) {
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

  ~HttpStub() {
    ::close(listen_fd_);
    if (th_.joinable()) th_.join();
  }

  int port() const { return port_; }
  std::string url() const { return "http://127.0.0.1:" + std::to_string(port_); }

  std::string last_request() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_req_;
  }

private:
  static const char* reason_phrase(int status) {
    switch (status) {
      case 200: return "OK";
      case 404: return "Not Found";
      case 500: return "Internal Server Error";
      default:  return "Status";
    }
  }

  void run() {
    timeval tv{};
    tv.tv_sec = 3; // per-read cap; a broken test must not hang the runner
    const int c = ::accept(listen_fd_, nullptr, nullptr);
    if (c < 0) return;
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    std::string req;
    char chunk[4096];
    while (req.find("\r\n\r\n") == std::string::npos) {
      const ssize_t n = ::read(c, chunk, sizeof chunk);
      if (n <= 0) break;
      req.append(chunk, static_cast<std::size_t>(n));
      if (req.size() > (1u << 20)) break;
    }
    {
      std::lock_guard<std::mutex> lk(mu_);
      last_req_ = req;
    }
    std::string resp = "HTTP/1.1 " + std::to_string(status_) + " " +
                       reason_phrase(status_) + "\r\n";
    resp += "Content-Type: application/json\r\n";
    if (chunked_) resp += "Transfer-Encoding: chunked\r\n";
    else if (with_content_length_)
      resp += "Content-Length: " + std::to_string(body_.size()) + "\r\n";
    resp += "Connection: close\r\n";
    resp += "\r\n";
    resp += body_;
    std::size_t off = 0;
    while (off < resp.size()) {
      const ssize_t w = ::write(c, resp.data() + off, resp.size() - off);
      if (w <= 0) break;
      off += static_cast<std::size_t>(w);
    }
    ::close(c);
  }

  std::string body_;
  int status_ = 200;
  bool with_content_length_ = true;
  bool chunked_ = false;
  int listen_fd_ = -1;
  int port_ = 0;
  mutable std::mutex mu_;
  std::string last_req_;
  std::thread th_;
};

// A port that is guaranteed to refuse connections (bind ephemeral, close).
int dead_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  const int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  socklen_t len = sizeof addr;
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  const int p = ntohs(addr.sin_port);
  ::close(fd);
  return p;
}

// Consul health response builders (we only ever emit the Service part; the
// parser is total and ignores Node/Checks).
nlohmann::json svc_obj(const std::string& addr, int port,
                       std::optional<std::string> pin = std::nullopt,
                       const std::string& id = "") {
  nlohmann::json s = {{"Service", "modelmirrors"}, {"Address", addr}, {"Port", port}};
  if (!id.empty()) s["ID"] = id;
  if (pin) s["Meta"] = nlohmann::json{{"cert-sha256", *pin}};
  return s;
}

nlohmann::json health_entry(const nlohmann::json& svc) {
  return nlohmann::json{{"Service", svc}};
}

std::string array_body(const std::vector<nlohmann::json>& entries) {
  return nlohmann::json(entries).dump(); // vector<json> ctor -> json array
}

std::string upper_hex(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

#endif // MIRRORCPP_WITH_TLS

} // namespace

// ---------------------------------------------------------------------------
// discover_mirrors — fail-closed matrix (design 3.6 / 5.4)
// ---------------------------------------------------------------------------

#if MIRRORCPP_WITH_TLS

TEST_CASE("registry: unreachable registry fails closed (empty list)", "[registry]") {
  const int p = dead_port();
  auto r = discover_mirrors("http://127.0.0.1:" + std::to_string(p));
  REQUIRE(r.has_value()); // fail closed: not an error
  REQUIRE(r->empty());
}

TEST_CASE("registry: non-200 response fails closed (empty list)", "[registry]") {
  HttpStub stub("{}", 500);
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
}

TEST_CASE("registry: malformed JSON fails closed (empty list)", "[registry]") {
  HttpStub stub("this is not json {{{)");
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
}

TEST_CASE("registry: non-array top level fails closed (empty list)", "[registry]") {
  HttpStub stub("{\"foo\": 1}");
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
}

TEST_CASE("registry: chunked response fails closed (documented limitation)", "[registry]") {
  HttpStub stub(array_body({health_entry(svc_obj("127.0.0.1", 9001))}), 200, true, true);
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
}

TEST_CASE("registry: EOF-terminated body (no Content-Length) is parsed", "[registry]") {
  HttpStub stub(array_body({health_entry(svc_obj("127.0.0.1", 9001))}), 200, false);
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 1);
  CHECK(r->at(0).host == "127.0.0.1");
  CHECK(r->at(0).port == 9001);
}

TEST_CASE("registry: unsupported URL schemes fail closed (empty list)", "[registry]") {
  // https:// is rejected by the minimal http://-only client (design 5.4);
  // anything without a scheme likewise. Both fail closed.
  auto r1 = discover_mirrors("https://127.0.0.1:8500");
  REQUIRE(r1.has_value());
  REQUIRE(r1->empty());
  auto r2 = discover_mirrors("ftp://127.0.0.1:21");
  REQUIRE(r2.has_value());
  REQUIRE(r2->empty());
}

TEST_CASE("registry: requests the Consul health endpoint (path, Host, Accept)", "[registry]") {
  HttpStub stub("[]");
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  const std::string req = stub.last_request();
  CHECK(req.find("GET /v1/health/service/modelmirrors?passing=true HTTP/1.1\r\n") == 0);
  CHECK(req.find("Host: 127.0.0.1:" + std::to_string(stub.port()) + "\r\n") != std::string::npos);
  CHECK(req.find("Accept: application/json\r\n") != std::string::npos);
  CHECK(req.find("Connection: close\r\n") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Entry-parsing matrix (total function, design 5.4)
// ---------------------------------------------------------------------------

TEST_CASE("registry: skips entries with non-object Service or bad Address", "[registry]") {
  const std::vector<nlohmann::json> bad = {
      health_entry("not-an-object"),
      nlohmann::json{{"Service", nlohmann::json::array()}},
      health_entry(svc_obj("", 9001)),
      health_entry(svc_obj("   ", 9001)),
      health_entry(svc_obj("127.0.0.1", 9001, std::nullopt, "").erase("Address")),
  };
  // Every entry must be skipped -> empty list.
  HttpStub stub_all_bad(array_body(bad));
  auto r = discover_mirrors(stub_all_bad.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
  // A malformed entry next to a good one is skipped; the good one survives.
  HttpStub stub_mixed(array_body({health_entry("not-an-object"),
                                  health_entry(svc_obj("127.0.0.1", 9001))}));
  auto m = discover_mirrors(stub_mixed.url());
  REQUIRE(m.has_value());
  REQUIRE(m->size() == 1);
  CHECK(m->at(0).host == "127.0.0.1");
  CHECK(m->at(0).port == 9001);
}

TEST_CASE("registry: skips entries with bad Port", "[registry]") {
  nlohmann::json p0 = svc_obj("127.0.0.1", 9001); p0["Port"] = 0;
  nlohmann::json p65536 = svc_obj("127.0.0.1", 9001); p65536["Port"] = 65536;
  nlohmann::json pneg = svc_obj("127.0.0.1", 9001); pneg["Port"] = -1;
  nlohmann::json pstr = svc_obj("127.0.0.1", 9001); pstr["Port"] = "8500";
  nlohmann::json pfloat = svc_obj("127.0.0.1", 9001); pfloat["Port"] = 10.5;
  nlohmann::json pmissing = svc_obj("127.0.0.1", 9001); pmissing.erase("Port");
  HttpStub stub(array_body({health_entry(p0), health_entry(p65536), health_entry(pneg),
                            health_entry(pstr), health_entry(pfloat),
                            health_entry(pmissing)}));
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
  // Boundary values are accepted: 1 and 65535.
  nlohmann::json p1 = svc_obj("127.0.0.1", 9001); p1["Port"] = 1;
  nlohmann::json pMax = svc_obj("127.0.0.1", 9001); pMax["Port"] = 65535;
  HttpStub sb(array_body({health_entry(p1), health_entry(pMax)}));
  auto ok = discover_mirrors(sb.url());
  REQUIRE(ok.has_value());
  REQUIRE(ok->size() == 2);
  CHECK(ok->at(0).port == 1);
  CHECK(ok->at(1).port == 65535);
}

TEST_CASE("registry: malformed cert-sha256 skips the entire entry", "[registry]") {
  // Present but malformed -> skip, even with valid Address/Port. No
  // unpinned fallback for a broken advertisement (design 5.4).
  const std::vector<nlohmann::json> bad = {
      health_entry(svc_obj("127.0.0.1", 9001, "abc")),
      health_entry(svc_obj("127.0.0.1", 9001, std::string(64, 'g'))), // non-hex
      health_entry(svc_obj("127.0.0.1", 9001, "")),
      health_entry([] { nlohmann::json s = svc_obj("127.0.0.1", 9001);
                      s["Meta"] = nlohmann::json{{"cert-sha256", 42}}; return s; }()),
  };
  HttpStub stub(array_body(bad));
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->empty());
}

TEST_CASE("registry: absent pin allowed; uppercase pin normalized", "[registry]") {
  const auto& c = certs();
  // Absent Meta entirely.
  nlohmann::json no_meta = svc_obj("127.0.0.1", 9001); no_meta.erase("Meta");
  // Meta present but not an object -> pin treated as absent.
  nlohmann::json meta_str = svc_obj("127.0.0.1", 9002); meta_str["Meta"] = "nope";
  // Uppercase hex is valid and normalized to lowercase.
  nlohmann::json upper = svc_obj("127.0.0.1", 9003, upper_hex(c.server_fingerprint));
  HttpStub stub(array_body({health_entry(no_meta), health_entry(meta_str),
                            health_entry(upper)}));
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 3);
  CHECK(!r->at(0).cert_sha256.has_value());
  CHECK(!r->at(1).cert_sha256.has_value());
  REQUIRE(r->at(2).cert_sha256.has_value());
  CHECK(*r->at(2).cert_sha256 == c.server_fingerprint);
}

TEST_CASE("registry: valid entries parsed in order (id/host/port/pin)", "[registry]") {
  const auto& c = certs();
  nlohmann::json a = svc_obj("127.0.0.1", 9001, c.server_fingerprint, "svc-a");
  nlohmann::json b = svc_obj("example.com", 443, std::nullopt, "svc-b");
  HttpStub stub(array_body({health_entry(a), health_entry(b)}));
  auto r = discover_mirrors(stub.url());
  REQUIRE(r.has_value());
  REQUIRE(r->size() == 2); // registry order preserved
  CHECK(r->at(0).id == "svc-a");
  CHECK(r->at(0).host == "127.0.0.1");
  CHECK(r->at(0).port == 9001);
  REQUIRE(r->at(0).cert_sha256.has_value());
  CHECK(*r->at(0).cert_sha256 == c.server_fingerprint);
  CHECK(r->at(1).id == "svc-b");
  CHECK(r->at(1).host == "example.com");
  CHECK(r->at(1).port == 443);
  CHECK(!r->at(1).cert_sha256.has_value());
}

// ---------------------------------------------------------------------------
// connect_from_registry (design 5.4)
// ---------------------------------------------------------------------------

TEST_CASE("registry: reachable registry with no candidates is a registry error", "[registry]") {
  HttpStub stub("[]");
  auto t = connect_from_registry(stub.url(), default_opts());
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::registry);
  CHECK(t.error().message.find("no mirror candidates discovered from " + stub.url()) !=
        std::string::npos);
}

TEST_CASE("registry: exhausted candidates -> registry error listing diagnostics", "[registry]") {
  const int d1 = dead_port(), d2 = dead_port();
  HttpStub stub(array_body({health_entry(svc_obj("127.0.0.1", d1)),
                            health_entry(svc_obj("127.0.0.1", d2))}));
  auto t = connect_from_registry(stub.url(), default_opts());
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::registry);
  // Per-candidate diagnostics: "host:port: message".
  CHECK(t.error().message.find("127.0.0.1:" + std::to_string(d1) + ":") != std::string::npos);
  CHECK(t.error().message.find("127.0.0.1:" + std::to_string(d2) + ":") != std::string::npos);
}

TEST_CASE("registry: iterates candidates in order; first-dead then live succeeds", "[registry]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  const int dead = dead_port();
  HttpStub stub(array_body({
      health_entry(svc_obj("127.0.0.1", dead, c.server_fingerprint)),
      health_entry(svc_obj("127.0.0.1", srv.port(), c.server_fingerprint)),
  }));
  auto t = connect_from_registry(stub.url(), default_opts());
  REQUIRE(t.has_value());
  REQUIRE(t.value()->peer_fingerprint() == c.server_fingerprint);
  const auto cl = t.value()->close();
  REQUIRE(cl.has_value());
}

TEST_CASE("registry: pin_override wins over the entry pin", "[registry]") {
  const auto& c = certs();
  // Entry advertises a WRONG pin; override supplies the right one -> success.
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  HttpStub stub_wrong(array_body({
      health_entry(svc_obj("127.0.0.1", srv.port(), std::string(64, '0')))}));
  auto ok = connect_from_registry(stub_wrong.url(), default_opts(), c.server_fingerprint);
  REQUIRE(ok.has_value());
  (void)ok.value()->close();
  // Entry advertises the RIGHT pin; override forces a wrong one -> failure.
  HttpStub stub_right(array_body({
      health_entry(svc_obj("127.0.0.1", srv.port(), c.server_fingerprint))}));
  auto bad = connect_from_registry(stub_right.url(), default_opts(), std::string(64, '0'));
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().kind == ErrorKind::registry);
  CHECK(bad.error().message.find(":") != std::string::npos);
}

TEST_CASE("registry: absent pin allows connection without pinning", "[registry]") {
  const auto& c = certs();
  TlsTestServer srv(c.ca, c.server_cert, c.server_key, TLS1_3_VERSION, TLS1_3_VERSION, true);
  nlohmann::json no_pin = svc_obj("127.0.0.1", srv.port()); no_pin.erase("Meta");
  HttpStub stub(array_body({health_entry(no_pin)}));
  auto t = connect_from_registry(stub.url(), default_opts());
  REQUIRE(t.has_value());
  REQUIRE(t.value()->peer_fingerprint() == c.server_fingerprint);
  const auto cl = t.value()->close();
  REQUIRE(cl.has_value());
}

#endif // MIRRORCPP_WITH_TLS

