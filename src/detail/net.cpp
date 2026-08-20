// mirrorcpp/detail/net.cpp — blocking POSIX/Winsock socket wrapper (design §5.3).
// POSIX is the validated path (Linux CI). Winsock2 path is best-effort/unvalidated.
#include "net.hpp"

#include "line.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mstcpip.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mirrorcpp::detail {

using std::unexpected;

namespace {

#ifdef _WIN32
// Winsock must be initialised exactly once per process; keep a refcount so
// WSACleanup balances WSAStartup.
struct WsaGuard {
  WsaGuard() {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) ok_ = true;
  }
  ~WsaGuard() {
    if (ok_) WSACleanup();
  }
  bool ok_ = false;
};

bool wsa_ready() {
  static WsaGuard g;
  return g.ok_;
}

// Cast a POSIX-style 'long' handle to the Winsock SOCKET type.
SOCKET to_sock(long fd) { return static_cast<SOCKET>(fd); }
long from_sock(SOCKET s) { return static_cast<long>(s); }

void set_nonblocking(long fd, bool nb) {
  u_long mode = nb ? 1 : 0;
  ioctlsocket(to_sock(fd), FIONBIO, &mode);
}

const char* wsa_last_error_str() {
  static thread_local char buf[256];
  snprintf(buf, sizeof buf, "Winsock error %lu", (unsigned long)WSAGetLastError());
  return buf;
}
#else
void set_nonblocking(long fd, bool nb) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) return;
  if (nb) flags |= O_NONBLOCK;
  else flags &= ~O_NONBLOCK;
  ::fcntl(fd, F_SETFL, flags);
}
#endif

} // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept
    : fd_(other.fd_), buf_(std::move(other.buf_)) {
  other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    buf_ = std::move(other.buf_);
    other.fd_ = -1;
  }
  return *this;
}

void Socket::close() noexcept {
#ifdef _WIN32
  if (fd_ >= 0) ::closesocket(to_sock(fd_));
#else
  if (fd_ >= 0) ::close(fd_);
#endif
  fd_ = -1;
  buf_.clear();
}

void Socket::set_nonblocking(bool nb) noexcept {
  if (fd_ < 0) return;
  mirrorcpp::detail::set_nonblocking(fd_, nb);
}

Result<void> Socket::connect(std::string_view host, std::uint16_t port,
                             std::chrono::milliseconds timeout) {
  close();
#ifdef _WIN32
  if (!wsa_ready())
    return unexpected(Error(ErrorKind::io, "Winsock failed to initialise"));
#endif

  const std::string host_s(host);
  const std::string port_s = std::to_string(port);

  struct addrinfo hints {};
  hints.ai_family = AF_UNSPEC; // IPv4 + IPv6
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = nullptr;
#ifdef _WIN32
  const int rc = ::getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &res);
#else
  const int rc = ::getaddrinfo(host_s.c_str(), port_s.c_str(), &hints, &res);
#endif
  if (rc != 0) {
    return unexpected(Error(ErrorKind::io, "getaddrinfo(" + host_s + "): " +
#if defined(_WIN32)
                                             gai_strerror(rc)));
#else
                                             (rc == EAI_SYSTEM ? std::strerror(errno) : gai_strerror(rc))));
#endif
  }
  std::unique_ptr<struct addrinfo, decltype(&freeaddrinfo)> guard(res, &freeaddrinfo);

  std::string last_error = "no addresses";
  int poll_timeout_ms = 0;
  {
    // Cap the poll timeout at INT_MAX milliseconds so the int cast never wraps.
    long long t = timeout.count();
    if (t < 0) t = 0;
    if (t > INT_MAX) t = INT_MAX;
    poll_timeout_ms = static_cast<int>(t);
  }

  for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
#ifdef _WIN32
    SOCKET s = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == INVALID_SOCKET) {
      last_error = wsa_last_error_str();
      continue;
    }
    long fd = from_sock(s);
    mirrorcpp::detail::set_nonblocking(fd, true);
    const int r = ::connect(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
    if (r == 0) { mirrorcpp::detail::set_nonblocking(fd, false); fd_ = fd; return {}; }
    if (WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
      last_error = wsa_last_error_str();
      ::closesocket(s);
      continue;
    }
    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_ZERO(&efds);
    FD_SET(s, &wfds); FD_SET(s, &efds);
    timeval tv{};
    tv.tv_sec = poll_timeout_ms / 1000;
    tv.tv_usec = (poll_timeout_ms % 1000) * 1000;
    const int sr = ::select(0, nullptr, &wfds, &efds, &tv);
    if (sr <= 0) {
      last_error = sr == 0 ? "connect timed out" : wsa_last_error_str();
      ::closesocket(s);
      continue;
    }
    int soerr = 0;
    int len = static_cast<int>(sizeof soerr);
    if (::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len) < 0 ||
        soerr != 0) {
      last_error = soerr ? "connect failed" : wsa_last_error_str();
      ::closesocket(s);
      continue;
    }
    mirrorcpp::detail::set_nonblocking(fd, false);
    fd_ = fd;
    return {};
#else
    long fd = ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    mirrorcpp::detail::set_nonblocking(fd, true);

    const int r = ::connect(fd, ai->ai_addr, static_cast<socklen_t>(ai->ai_addrlen));
    if (r == 0) {
      mirrorcpp::detail::set_nonblocking(fd, false);
      fd_ = fd;
      return {};
    }
    if (errno != EINPROGRESS) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }

    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int pr = ::poll(&pfd, 1, poll_timeout_ms);
    if (pr <= 0) {
      last_error = pr == 0 ? "connect timed out" : std::strerror(errno);
      ::close(fd);
      continue;
    }
    int soerr = 0;
    socklen_t len = sizeof soerr;
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) < 0 || soerr != 0) {
      last_error = soerr != 0 ? std::strerror(soerr)
                              : std::string("getsockopt failed: ") + std::strerror(errno);
      ::close(fd);
      continue;
    }
    mirrorcpp::detail::set_nonblocking(fd, false);
    fd_ = fd;
    return {};
#endif
  }
  return unexpected(Error(ErrorKind::io,
                          "connect(" + host_s + ":" + port_s + ") failed: " + last_error));
}

Result<std::string> Socket::read_line() {
  if (fd_ < 0) return unexpected(Error(ErrorKind::io, "read_line on closed socket"));
  auto line = read_line_from_fd(fd_, buf_, "transport closed unexpectedly");
  if (!line) return unexpected(line.error());
  if (line->empty())
    return unexpected(Error(ErrorKind::io, "transport closed unexpectedly"));
  return *line;
}

Result<std::string> Socket::read_n(std::size_t n) {
  if (fd_ < 0) return unexpected(Error(ErrorKind::io, "read_n on closed socket"));
  std::string out;
  out.reserve(n);
  while (out.size() < n) {
    // Bytes already buffered by read_line() (headers+body in one segment).
    if (!buf_.empty()) {
      const std::size_t take = std::min(n - out.size(), buf_.size());
      out.append(buf_, 0, take);
      buf_.erase(0, take);
      continue;
    }
    char chunk[4096];
#ifdef _WIN32
    const int r = ::recv(to_sock(fd_), chunk, sizeof chunk, 0);
    if (r < 0) {
      if (WSAGetLastError() == WSAEINTR) continue;
      return unexpected(Error(ErrorKind::io, wsa_last_error_str()));
    }
#else
    const ssize_t r = ::read(fd_, chunk, sizeof chunk);
    if (r < 0) {
      if (errno == EINTR) continue;
      return unexpected(Error(ErrorKind::io,
                              std::string("read failed: ") + std::strerror(errno)));
    }
#endif
    if (r == 0)
      return unexpected(Error(ErrorKind::io,
                              "transport closed unexpectedly (EOF before " +
                              std::to_string(n) + " bytes)"));
    out.append(chunk, static_cast<std::size_t>(r));
  }
  return out;
}

Result<std::string> Socket::read_until_eof() {
  if (fd_ < 0) return unexpected(Error(ErrorKind::io, "read_until_eof on closed socket"));
  std::string out = std::move(buf_);
  buf_.clear();
  char chunk[4096];
  for (;;) {
#ifdef _WIN32
    const int r = ::recv(to_sock(fd_), chunk, sizeof chunk, 0);
    if (r < 0) {
      if (WSAGetLastError() == WSAEINTR) continue;
      return unexpected(Error(ErrorKind::io, wsa_last_error_str()));
    }
#else
    const ssize_t r = ::read(fd_, chunk, sizeof chunk);
    if (r < 0) {
      if (errno == EINTR) continue;
      return unexpected(Error(ErrorKind::io,
                              std::string("read failed: ") + std::strerror(errno)));
    }
#endif
    if (r == 0) return out; // clean EOF terminates an EOF-delimited body
    out.append(chunk, static_cast<std::size_t>(r));
  }
}

Result<void> Socket::write_all(std::string_view data) {
  if (fd_ < 0) return unexpected(Error(ErrorKind::io, "write_all on closed socket"));
  std::size_t off = 0;
  while (off < data.size()) {
#ifdef _WIN32
    const int n = ::send(to_sock(fd_), data.data() + off,
                         static_cast<int>(data.size() - off), 0);
    if (n < 0) {
      if (WSAGetLastError() == WSAEINTR) continue;
      return unexpected(Error(ErrorKind::io, wsa_last_error_str()));
    }
    off += static_cast<std::size_t>(n);
#else
    const ssize_t n = ::send(fd_, data.data() + off, data.size() - off, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) continue;
      return unexpected(Error(ErrorKind::io,
                              std::string("send failed: ") + std::strerror(errno)));
    }
    if (n == 0)
      return unexpected(Error(ErrorKind::io, "send: connection closed by peer"));
    off += static_cast<std::size_t>(n);
#endif
  }
  return {};
}

} // namespace mirrorcpp::detail
