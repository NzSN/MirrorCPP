// mirrorcpp/detail/http.hpp — minimal HTTP/1.1 GET for the registry (design §5.4).
//
// Deliberately minimal, used ONLY for registry discovery:
//   - plain http:// URLs only (https is rejected — the design's discovery
//     endpoint is plain HTTP by contract);
//   - sends GET <path> HTTP/1.1 with Host, Accept: application/json and
//     Connection: close;
//   - requires a 200 status; reads the body via Content-Length or, when no
//     Content-Length is present, until peer EOF;
//   - chunked transfer-encoding is a DOCUMENTED LIMITATION (design §5.4):
//     a chunked response is reported as an error, not decoded — Consul answers
//     this endpoint with a Content-Length.
//
// `timeout` bounds the TCP connect and every socket read (SO_RCVTIMEO), so a
// stalled server cannot hang discovery.
#ifndef MIRRORCPP_DETAIL_HTTP_HPP
#define MIRRORCPP_DETAIL_HTTP_HPP

#include <mirrorcpp/error.hpp>

#include <chrono>
#include <string>
#include <string_view>

namespace mirrorcpp::detail {

// Perform a GET request and return the response body on a 200. Any failure
// (bad URL, connect error, non-200, timeout) is an ErrorKind::io or
// ErrorKind::json error with a human-readable message.
Result<std::string> http_get(std::string_view url, std::chrono::milliseconds timeout);

} // namespace mirrorcpp::detail

#endif

