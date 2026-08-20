// mirrorcpp/detail/line.hpp — newline framing over a raw fd.
//
// Shared by detail/net (sockets) and detail/process (pipes): buffers bytes from
// an fd until a '\n' is found and returns one line at a time (without the '\n').
// Handles partial reads (multi-read assembly) and multiple lines per read.
//
// Return semantics:
//   - a complete line (without trailing '\n') on success,
//   - an empty std::string on clean EOF with an empty buffer (caller decides the
//     error; transport layer maps this to an io/spawn error),
//   - Error{io, eof_msg} if EOF arrives mid-line (partial line is discarded),
//   - Error{io, "read failed: <errno>"} on a hard read error.
#ifndef MIRRORCPP_DETAIL_LINE_HPP
#define MIRRORCPP_DETAIL_LINE_HPP

#include <mirrorcpp/error.hpp>

#include <cerrno>
#include <cstring>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace mirrorcpp::detail {

using std::unexpected;

// max_read is the chunk size per underlying read() call.
inline constexpr std::size_t kLineChunkSize = 4096;

inline Result<std::string> read_line_from_fd(long fd, std::string& buf,
                                             const char* unexpected_eof_msg) {
  for (;;) {
    auto nl = buf.find('\n');
    if (nl != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      return line;
    }
    char chunk[kLineChunkSize];
#ifdef _WIN32
    int n = ::_read(static_cast<int>(fd), chunk, static_cast<unsigned>(sizeof chunk));
#else
    ssize_t n;
    do {
      n = ::read(fd, chunk, sizeof chunk);
    } while (n < 0 && errno == EINTR);
#endif
    if (n == 0) {
      // Clean EOF. An empty buffer is a clean EOF signal; a partial line is a
      // framing violation (the peer closed mid-line).
      if (buf.empty()) return std::string{};
      return unexpected(Error(ErrorKind::io, std::string(unexpected_eof_msg)));
    }
    if (n < 0) {
      return unexpected(Error(ErrorKind::io,
                              std::string("read failed: ") + std::strerror(errno)));
    }
    buf.append(chunk, static_cast<std::size_t>(n));
  }
}

// Framing invariant (design §5.3): Transport::send_line rejects inputs containing
// an embedded newline (defensive — the JSON encoder never produces them). Nothing
// is written on rejection. ErrorKind::io is used (a local transport-level framing
// violation). Shared by all transports (stdio/TCP/TLS).
inline Result<void> reject_embedded_newline(std::string_view line) {
  if (line.find('\n') != std::string_view::npos) {
    return unexpected(Error(ErrorKind::io,
                            "send_line: input contains an embedded newline (framing "
                            "violation — the protocol is newline-delimited JSON)"));
  }
  return {};
}

} // namespace mirrorcpp::detail

#endif
