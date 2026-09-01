// mirrorcpp/transport.cpp — StdioTransport + TcpTransport (design §5.3).
#include <mirrorcpp/transport.hpp>

#include "detail/line.hpp"
#include "detail/net.hpp"
#include "detail/process.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mirrorcpp {

using std::unexpected;

// ---------------------------------------------------------------------------
// StdioTransport — the mirror runs as a child process.
// ---------------------------------------------------------------------------
class StdioTransport final : public Transport {
public:
  explicit StdioTransport(std::filesystem::path mirror_bin)
      : bin_(std::move(mirror_bin)) {}

  // Spawn the child. Called once by spawn_mirror; on failure spawn_mirror
  // returns nullptr so no half-constructed transport escapes.
  Result<void> start() { return proc_.spawn(bin_.string()); }

  Result<void> send_line(std::string_view line) override {
    auto r = detail::reject_embedded_newline(line);
    if (!r) return r;
    std::string framed(line);
    framed.push_back('\n');
    return proc_.write_stdin(framed);
  }

  Result<std::string> recv_line() override {
    auto line = detail::read_line_from_fd(proc_.stdout_fd(), buf_,
                                          "transport closed unexpectedly");
    if (!line) return unexpected(line.error());
    if (line->empty()) {
      // Clean EOF. Distinguish a child that exited abnormally (spawn error)
      // from a clean EOF (io error).
      auto w = proc_.try_wait();
      if (w) {
        if (auto code = proc_.exit_code(); code && *code != 0) {
          return unexpected(Error(ErrorKind::spawn,
                                  "child process exited abnormally with code " +
                                  std::to_string(*code)));
        }
      }
      return unexpected(Error(ErrorKind::io, "transport closed unexpectedly"));
    }
    return *line;
  }

  Result<long> close() override {
    if (closed_) return exit_code_.value_or(0);
    closed_ = true;
    // Close stdin so the child sees EOF, then reap it and report the exit code.
    (void)proc_.close_stdin();
    auto code = proc_.wait();
    if (code) exit_code_ = *code;
    return code;
  }

private:
  std::filesystem::path bin_;
  detail::Subprocess proc_;
  std::string buf_;
  bool closed_ = false;
  std::optional<long> exit_code_;
};

// ---------------------------------------------------------------------------
// TcpTransport — plain TCP connection to a --serve mirror.
// ---------------------------------------------------------------------------
class TcpTransport final : public Transport {
public:
  explicit TcpTransport(detail::Socket sock) : sock_(std::move(sock)) {}

  Result<void> send_line(std::string_view line) override {
    if (closed_) return unexpected(Error(ErrorKind::io, "send_line on closed transport"));
    auto r = detail::reject_embedded_newline(line);
    if (!r) return r;
    std::string framed(line);
    framed.push_back('\n');
    return sock_.write_all(framed);
  }

  Result<std::string> recv_line() override {
    if (closed_) return unexpected(Error(ErrorKind::io, "recv_line on closed transport"));
    return sock_.read_line();
  }

  bool async_capable() const noexcept override { return true; }  // server mode (guide §6)

  Result<long> close() override {
    if (closed_) return 0;
    closed_ = true;
    sock_.close();
    return 0;
  }

private:
  detail::Socket sock_;
  bool closed_ = false;
};

// ---------------------------------------------------------------------------
// Factories
// ---------------------------------------------------------------------------

std::unique_ptr<Transport> spawn_mirror(const std::filesystem::path& mirror_bin,
                                        SpawnOptions) {
  auto t = std::make_unique<StdioTransport>(mirror_bin);
  auto r = t->start();
  if (!r) return nullptr;
  return t;
}

Result<std::unique_ptr<Transport>> connect_tcp(std::string_view host, std::uint16_t port,
                                               ConnectOptions options) {
  detail::Socket sock;
  auto r = sock.connect(host, port, options.connect_timeout);
  if (!r) return unexpected(r.error());
  return std::make_unique<TcpTransport>(std::move(sock));
}

} // namespace mirrorcpp
