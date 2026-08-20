// mirrorcpp/detail/net.hpp — blocking POSIX/Winsock socket wrapper (design §5.3).
//
// RAII blocking TCP socket with a connect timeout (non-blocking connect + poll /
// select) and buffered line reading. Reused by TcpTransport, the TLS transport
// (via native_handle() -> SSL_set_fd) and the registry HTTP client.
//
// POSIX is the fully supported/validated path (this repo's CI is Linux). The
// Winsock2 path is behind #ifdef _WIN32 and is best-effort (not validated here).
//
// Framing contract (see detail/line.hpp):
//   read_line() returns one line without its trailing '\n', handles partial
//   reads and multiple lines per read; a clean peer EOF mid-session maps to
//   ErrorKind::io "transport closed unexpectedly" (design §5.3).
#ifndef MIRRORCPP_DETAIL_NET_HPP
#define MIRRORCPP_DETAIL_NET_HPP

#include <mirrorcpp/error.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace mirrorcpp::detail {

class Socket {
public:
  Socket() = default;
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  // Connect to host:port (IPv4/IPv6 literal or hostname) within the timeout.
  // The socket is left in blocking mode on success.
  Result<void> connect(std::string_view host, std::uint16_t port,
                       std::chrono::milliseconds timeout);

  // Buffered line read. See file comment for framing semantics.
  Result<std::string> read_line();

  // Read exactly `n` bytes (blocking), consulting the line buffer first. io
  // error if the peer EOFs before n bytes arrive. Used for Content-Length
  // HTTP bodies (design 5.4).
  Result<std::string> read_n(std::size_t n);

  // Read until a clean peer EOF, returning the accumulated bytes. Unlike
  // read_line, an EOF here is the expected terminator (EOF-delimited HTTP
  // bodies), not an error. io error for genuine failures.
  Result<std::string> read_until_eof();

  // Write all bytes; returns io error on short write / connection failure.
  Result<void> write_all(std::string_view data);

  bool valid() const noexcept { return fd_ >= 0; }

  // Native descriptor for SSL_set_fd / poll; -1 when closed.
  long native_handle() const noexcept { return fd_; }

  // Toggle non-blocking mode (used by the TLS handshake deadline loop; the
  // socket is left in blocking mode by connect()). No-op on a closed socket.
  void set_nonblocking(bool nb) noexcept;

  void close() noexcept;

private:
  long fd_ = -1;   // POSIX fd; Winsock SOCKET cast to long
  std::string buf_;
};

} // namespace mirrorcpp::detail

#endif
