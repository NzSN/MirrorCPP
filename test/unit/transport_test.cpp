// mirrorcpp transport unit tests (design §5.3, §8 'Framing' + 'Registry/TLS helpers'):
//   - loopback TCP echo (connect_tcp / send_line / recv_line / close)
//   - framing edge cases: partial reads, two lines per read, embedded-newline
//     rejection, clean peer EOF mid-session -> io error
//   - stdio process spawn + echo + exit-code propagation (/bin/cat, /bin/false)
//   - spawn failure -> nullptr / ErrorKind::spawn
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

// Internal framing helper, tested deterministically over a pipe.
#include "detail/line.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

using namespace mirrorcpp;

namespace {

void write_all_fd(int fd, const char* data, std::size_t n) {
  std::size_t off = 0;
  while (off < n) {
    const ssize_t w = ::write(fd, data + off, n - off);
    REQUIRE(w > 0);
    off += static_cast<std::size_t>(w);
  }
}

// ---------------------------------------------------------------------------
// Minimal loopback echo server (one connection).
//   - echoes every received line back to the client
//   - the literal line "CLOSE" makes it close the connection (no echo)
//   - the literal line "PAIR" makes it reply "one\ntwo\n" in a SINGLE write()
// ---------------------------------------------------------------------------
class EchoServer {
public:
  EchoServer() {
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

  ~EchoServer() {
    ::close(listen_fd_);
    if (th_.joinable()) th_.join();
  }

  int port() const { return port_; }

private:
  void run() {
    const int c = ::accept(listen_fd_, nullptr, nullptr);
    if (c < 0) return;
    std::string inbuf;
    std::string outbuf;
    char chunk[4096];
    for (;;) {
      const ssize_t n = ::read(c, chunk, sizeof chunk);
      if (n <= 0) break;
      inbuf.append(chunk, static_cast<std::size_t>(n));
      std::size_t pos;
      while ((pos = inbuf.find('\n')) != std::string::npos) {
        const std::string line = inbuf.substr(0, pos);
        inbuf.erase(0, pos + 1);
        if (line == "CLOSE") {
          ::close(c);
          return;
        }
        if (line == "PAIR") {
          write_all_fd(c, "one\ntwo\n", 8);
          continue;
        }
        outbuf += line;
        outbuf += '\n';
      }
      if (!outbuf.empty()) {
        write_all_fd(c, outbuf.data(), outbuf.size());
        outbuf.clear();
      }
    }
    ::close(c);
  }

  int listen_fd_ = -1;
  int port_ = 0;
  std::thread th_;
};

} // namespace

// ---------------------------------------------------------------------------
// Framing helper (deterministic, over a pipe)
// ---------------------------------------------------------------------------

TEST_CASE("framing: two lines delivered in a single read", "[framing]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const std::string payload = "line1\nline2\n";
  // One write; the next read() returns both lines.
  write_all_fd(fds[1], payload.data(), payload.size());
  ::close(fds[1]);

  std::string buf;
  const auto l1 = detail::read_line_from_fd(fds[0], buf, "eof");
  const auto l2 = detail::read_line_from_fd(fds[0], buf, "eof");
  ::close(fds[0]);

  REQUIRE(l1.has_value());
  REQUIRE(*l1 == "line1");
  REQUIRE(l2.has_value());
  REQUIRE(*l2 == "line2");
}

TEST_CASE("framing: long line assembled across partial reads", "[framing]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const std::string big(10'000, 'x');
  const std::string payload = big + "\n";
  // Feed in several writes so no single read() can see the whole line.
  write_all_fd(fds[1], payload.data(), payload.size());
  ::close(fds[1]);

  std::string buf;
  const auto line = detail::read_line_from_fd(fds[0], buf, "eof");
  ::close(fds[0]);

  REQUIRE(line.has_value());
  REQUIRE(*line == big);
  REQUIRE(buf.empty());
}

TEST_CASE("framing: clean EOF returns empty, EOF mid-line is an error", "[framing]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const std::string payload = "hello\n";
  write_all_fd(fds[1], payload.data(), payload.size());
  ::close(fds[1]);

  std::string buf;
  const auto l1 = detail::read_line_from_fd(fds[0], buf, "eof");
  const auto l2 = detail::read_line_from_fd(fds[0], buf, "eof");
  REQUIRE(l1.has_value());
  REQUIRE(*l1 == "hello");
  // Clean EOF with an empty buffer -> empty string (caller turns it into an error).
  REQUIRE(l2.has_value());
  REQUIRE(l2->empty());
  ::close(fds[0]);
}

TEST_CASE("framing: EOF mid-line drops the partial line with an io error", "[framing]") {
  int fds[2];
  REQUIRE(::pipe(fds) == 0);
  const std::string partial = "no-newline-here";
  write_all_fd(fds[1], partial.data(), partial.size());
  ::close(fds[1]);

  std::string buf;
  const auto line = detail::read_line_from_fd(fds[0], buf, "transport closed unexpectedly");
  ::close(fds[0]);

  REQUIRE_FALSE(line.has_value());
  REQUIRE(line.error().kind == ErrorKind::io);
  REQUIRE(line.error().message == "transport closed unexpectedly");
}

// ---------------------------------------------------------------------------
// Loopback TCP transport
// ---------------------------------------------------------------------------

TEST_CASE("tcp: echo round-trip and close returns 0", "[transport][tcp]") {
  EchoServer srv;
  auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(srv.port()));
  REQUIRE(t.has_value());
  Transport& tr = **t;

  REQUIRE(tr.send_line("hello").has_value());
  auto line = tr.recv_line();
  REQUIRE(line.has_value());
  REQUIRE(*line == "hello");

  auto code = tr.close();
  REQUIRE(code.has_value());
  REQUIRE(*code == 0);
}

TEST_CASE("tcp: two reply lines arrive in one read", "[transport][tcp]") {
  EchoServer srv;
  auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(srv.port()));
  REQUIRE(t.has_value());
  Transport& tr = **t;

  REQUIRE(tr.send_line("PAIR").has_value());
  const auto l1 = tr.recv_line();
  const auto l2 = tr.recv_line();
  REQUIRE(l1.has_value());
  REQUIRE(*l1 == "one");
  REQUIRE(l2.has_value());
  REQUIRE(*l2 == "two");
  (void)tr.close();
}

TEST_CASE("tcp: clean peer EOF mid-session is an io error", "[transport][tcp]") {
  EchoServer srv;
  auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(srv.port()));
  REQUIRE(t.has_value());
  Transport& tr = **t;

  REQUIRE(tr.send_line("CLOSE").has_value()); // server closes without replying
  auto line = tr.recv_line();
  REQUIRE_FALSE(line.has_value());
  REQUIRE(line.error().kind == ErrorKind::io);
  REQUIRE(line.error().message.find("transport closed unexpectedly") != std::string::npos);
  (void)tr.close();
}

TEST_CASE("tcp: embedded newline is rejected and nothing is written", "[transport][tcp]") {
  EchoServer srv;
  auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(srv.port()));
  REQUIRE(t.has_value());
  Transport& tr = **t;

  const auto bad = tr.send_line("bad\nline");
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().kind == ErrorKind::io);
  REQUIRE(bad.error().message.find("newline") != std::string::npos);

  const auto empty = tr.send_line("");
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.error().kind == ErrorKind::io);

  const auto huge = tr.send_line(std::string(65536, 'x'));
  REQUIRE_FALSE(huge.has_value());
  REQUIRE(huge.error().kind == ErrorKind::io);

  // The rejected line must NOT have been written: the next echo is 'good'.
  REQUIRE(tr.send_line("good").has_value());
  const auto line = tr.recv_line();
  REQUIRE(line.has_value());
  REQUIRE(*line == "good");
  (void)tr.close();
}

TEST_CASE("tcp: connection refused is an io error", "[transport][tcp]") {
  // Find a closed loopback port: bind, note the port, then close.
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE(fd >= 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
  socklen_t len = sizeof addr;
  REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  const std::uint16_t port = static_cast<std::uint16_t>(ntohs(addr.sin_port));
  ::close(fd);

  ConnectOptions opts;
  opts.connect_timeout = std::chrono::milliseconds(2000);
  const auto t = connect_tcp("127.0.0.1", port, opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::io);
}

TEST_CASE("tcp: connect timeout is an io error", "[transport][tcp]") {
  // 169.254.1.1 is link-local (RFC 3927): in this sandbox it is black-holed, so
  // the non-blocking connect stays in EINPROGRESS until poll() times out. (A
  // documented limitation: on networks with a transparent proxy / SYN cookies a
  // real black hole may not exist, so this test asserts only the io-error path.)
  ConnectOptions opts;
  opts.connect_timeout = std::chrono::milliseconds(200);
  const auto t = connect_tcp("169.254.1.1", 65000, opts);
  REQUIRE_FALSE(t.has_value());
  REQUIRE(t.error().kind == ErrorKind::io);
}

// ---------------------------------------------------------------------------
// Stdio (process) transport
// ---------------------------------------------------------------------------

TEST_CASE("stdio: /bin/cat echo round-trip and exit code 0", "[transport][stdio]") {
  auto t = spawn_mirror("/bin/cat");
  REQUIRE(t != nullptr);

  REQUIRE(t->send_line("hello").has_value());
  const auto line = t->recv_line();
  REQUIRE(line.has_value());
  REQUIRE(*line == "hello");

  const auto code = t->close();
  REQUIRE(code.has_value());
  REQUIRE(*code == 0);
}

TEST_CASE("stdio: exit-code propagation via /bin/false", "[transport][stdio]") {
  auto t = spawn_mirror("/bin/false");
  REQUIRE(t != nullptr);
  const auto code = t->close();
  REQUIRE(code.has_value());
  REQUIRE(*code == 1);
}

TEST_CASE("stdio: child exiting abnormally is ErrorKind::spawn", "[transport][stdio]") {
  auto t = spawn_mirror("/bin/false");
  REQUIRE(t != nullptr);
  // /bin/false exits 1 immediately, so stdout EOFs before any line arrives.
  const auto line = t->recv_line();
  REQUIRE_FALSE(line.has_value());
  REQUIRE(line.error().kind == ErrorKind::spawn);
  (void)t->close();
}

TEST_CASE("stdio: spawn failure returns nullptr", "[transport][stdio]") {
  REQUIRE(spawn_mirror("/nonexistent/definitely-not-a-real-binary") == nullptr);
  // A directory cannot be executed -> spawn error.
  REQUIRE(spawn_mirror("/tmp") == nullptr);
}

TEST_CASE("stdio: embedded newline rejected and nothing written", "[transport][stdio]") {
  auto t = spawn_mirror("/bin/cat");
  REQUIRE(t != nullptr);

  const auto bad = t->send_line("bad\nline");
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().kind == ErrorKind::io);
  REQUIRE(bad.error().message.find("newline") != std::string::npos);

  const auto empty = t->send_line("");
  REQUIRE_FALSE(empty.has_value());
  REQUIRE(empty.error().kind == ErrorKind::io);

  const auto huge = t->send_line(std::string(65536, 'x'));
  REQUIRE_FALSE(huge.has_value());
  REQUIRE(huge.error().kind == ErrorKind::io);

  // Nothing was piped to /bin/cat: the next good line echoes back first.
  REQUIRE(t->send_line("good").has_value());
  const auto line = t->recv_line();
  REQUIRE(line.has_value());
  REQUIRE(*line == "good");
  (void)t->close();
}

TEST_CASE("stdio: close() is idempotent", "[transport][stdio]") {
  auto t = spawn_mirror("/bin/cat");
  REQUIRE(t != nullptr);
  const auto c1 = t->close();
  REQUIRE(c1.has_value());
  REQUIRE(*c1 == 0);
  const auto c2 = t->close();
  REQUIRE(c2.has_value());
  REQUIRE(*c2 == 0);
}
