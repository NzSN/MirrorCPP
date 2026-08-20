// mirrorcpp/detail/http.cpp — minimal HTTP/1.1 GET for the registry (design 5.4).
#include "http.hpp"

#include "net.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace mirrorcpp::detail {

using std::unexpected;

namespace {

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::string to_lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Strip the trailing '\r' that read_line leaves behind (HTTP lines end in
// CRLF; the line framing returns the LF-less line still carrying the CR).
void strip_cr(std::string& s) {
  if (!s.empty() && s.back() == '\r') s.pop_back();
}

// Parse http://host[:port][/path]; http-only (design 5.4). Returns false +
// message on error. IPv6 literals (http://[::1]:8500) are a documented
// omission of the deliberately minimal parser.
bool parse_url(std::string_view url, std::string& host, std::uint16_t& port,
               std::string& path, std::string& err) {
  constexpr std::string_view kScheme = "http://";
  if (url.size() < kScheme.size() ||
      to_lower(url.substr(0, kScheme.size())) != kScheme) {
    err = "only http:// URLs are supported (got '" + std::string(url) + "')";
    return false;
  }
  std::string_view rest = url.substr(kScheme.size());
  const std::size_t end = rest.find_first_of("/?#");
  std::string_view auth = rest.substr(0, end);
  path = (end == std::string_view::npos) ? "/" : std::string(rest.substr(end));
  if (auth.empty()) {
    err = "malformed URL: empty host";
    return false;
  }
  const std::size_t colon = auth.rfind(':');
  if (colon == std::string_view::npos) {
    host = std::string(auth);
    port = 80;
    return true;
  }
  host = std::string(auth.substr(0, colon));
  const std::string port_str = trim(auth.substr(colon + 1));
  if (host.empty() || port_str.empty()) {
    err = "malformed URL: bad host:port";
    return false;
  }
  char* endp = nullptr;
  const long p = std::strtol(port_str.c_str(), &endp, 10);
  if (endp == port_str.c_str() || *endp != '\0' || p < 1 || p > 65535) {
    err = "malformed URL: bad port '" + port_str + "'";
    return false;
  }
  port = static_cast<std::uint16_t>(p);
  return true;
}

// Case-insensitive header lookup; returns the trimmed value if present.
bool header_value(const std::vector<std::string>& headers, std::string_view name,
                  std::string& out) {
  const std::string lower_name = to_lower(name);
  for (const std::string& h : headers) {
    const std::size_t colon = h.find(':');
    if (colon == std::string::npos) continue;
    if (to_lower(trim(std::string_view(h).substr(0, colon))) == lower_name) {
      out = trim(std::string_view(h).substr(colon + 1));
      return true;
    }
  }
  return false;
}

} // namespace

Result<std::string> http_get(std::string_view url, std::chrono::milliseconds timeout) {
  std::string host, path, err;
  std::uint16_t port = 0;
  if (!parse_url(url, host, port, path, err))
    return unexpected(Error(ErrorKind::io, std::move(err)));

  Socket sock;
  auto conn = sock.connect(host, port, timeout);
  if (!conn)
    return unexpected(Error(ErrorKind::io,
                            std::string("registry HTTP connect to ") + host + ":" +
                            std::to_string(port) + " failed: " + conn.error().message));

  // Bound every socket read with the timeout (SO_RCVTIMEO), so a stalled
  // server cannot hang discovery (design 5.4 'timeout').
#ifdef _WIN32
  const DWORD rcv = static_cast<DWORD>(timeout.count());
  if (::setsockopt(reinterpret_cast<SOCKET>(sock.native_handle()), SOL_SOCKET,
                   SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof rcv) != 0)
    return unexpected(Error(ErrorKind::io, "setsockopt(SO_RCVTIMEO) failed"));
#else
  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  if (::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0)
    return unexpected(Error(ErrorKind::io,
                            std::string("setsockopt(SO_RCVTIMEO) failed: ") +
                            std::strerror(errno)));
#endif

  std::string req = "GET ";
  req += path;
  req += " HTTP/1.1\r\n";
  req += "Host: " + host;
  if (port != 80) req += ":" + std::to_string(port);
  req += "\r\n";
  req += "Accept: application/json\r\n";
  req += "Connection: close\r\n";
  req += "\r\n";
  auto wr = sock.write_all(req);
  if (!wr) return unexpected(wr.error());

  // Status line, e.g. "HTTP/1.1 200 OK".
  auto status_line = sock.read_line();
  if (!status_line) return unexpected(status_line.error());
  strip_cr(*status_line);
  if (status_line->size() < 12 ||
      to_lower(std::string_view(*status_line).substr(0, 5)) != "http/")
    return unexpected(Error(ErrorKind::io,
                            "malformed HTTP status line: '" + *status_line + "'"));
  const std::size_t sp1 = status_line->find(' ');
  const std::size_t sp2 = sp1 == std::string::npos
                             ? std::string::npos
                             : status_line->find(' ', sp1 + 1);
  if (sp1 == std::string::npos || sp2 == std::string::npos)
    return unexpected(Error(ErrorKind::io,
                            "malformed HTTP status line: '" + *status_line + "'"));
  const std::string code = status_line->substr(sp1 + 1, sp2 - sp1 - 1);
  if (code != "200")
    return unexpected(Error(ErrorKind::io, "registry HTTP GET " + path +
                            " returned status " + code + " (" +
                            status_line->substr(sp2 + 1) + ")"));

  std::vector<std::string> headers;
  for (;;) {
    auto h = sock.read_line();
    if (!h) return unexpected(h.error());
    strip_cr(*h);
    if (h->empty()) break; // end of headers
    headers.push_back(std::move(*h));
  }

  std::string te;
  if (header_value(headers, "Transfer-Encoding", te) &&
      to_lower(te).find("chunked") != std::string::npos) {
    // Chunked transfer-encoding is a documented limitation (design 5.4).
    return unexpected(Error(ErrorKind::io,
                            "registry HTTP response uses chunked transfer-encoding "
                            "(documented limitation; Content-Length required)"));
  }

  std::string cl;
  if (header_value(headers, "Content-Length", cl)) {
    char* endp = nullptr;
    const long long n = std::strtoll(cl.c_str(), &endp, 10);
    if (endp == cl.c_str() || *endp != '\0' || n < 0)
      return unexpected(Error(ErrorKind::io, "malformed Content-Length: '" + cl + "'"));
    return sock.read_n(static_cast<std::size_t>(n));
  }
  return sock.read_until_eof();
}

} // namespace mirrorcpp::detail

