// mirrorcpp/registry.hpp — Consul discovery + connect-from-registry (design 5.4).
//
// Discovery is FAIL CLOSED: any network / HTTP / JSON error means "no servers"
// and surfaces as an empty list, never an error (design 3.6). Entry parsing is
// a total function: malformed entries are silently skipped. The registry is
// plain HTTP by contract and provides location only — authentication always
// happens in the mTLS handshake against the pinned CA (design 3.6).
#ifndef MIRRORCPP_REGISTRY_HPP
#define MIRRORCPP_REGISTRY_HPP

#include <mirrorcpp/error.hpp>
#include <mirrorcpp/transport.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mirrorcpp {

// One discovered mirror service (design 5.4).
struct MirrorServiceInfo {
  std::string id;                          // service instance ID; "" when absent
  std::string host;                        // trimmed, non-empty Address
  std::uint16_t port;                      // integer in 1..65535
  std::optional<std::string> cert_sha256;  // normalized lowercase; absent if not advertised
};

struct RegistryOptions {
  std::chrono::milliseconds timeout{5000}; // 5 s default (design 5.4)
};

// GET <registry>/v1/health/service/modelmirrors?passing=true and parse the
// Consul health response. Fail closed: any network/HTTP/JSON error yields an
// EMPTY list (not an error). Entries are validated by a total function
// (design 5.4): non-object Service, blank Address, non-integer/out-of-range
// Port and a PRESENT-but-malformed cert-sha256 all skip the entry; an absent
// pin is allowed; id defaults to "".
Result<std::vector<MirrorServiceInfo>> discover_mirrors(std::string_view registry_url,
                                                        RegistryOptions options = {});

// Discover candidates then connect in registry order with
//   pin = pin_override ?? entry.cert_sha256   (absent pin means no pinning).
// Failures are collected per candidate as "host:port: message" diagnostics.
// Empty discovery -> registry error "no mirror candidates discovered from
// <registry_url>"; all candidates failed -> one registry error listing every
// diagnostic. Requires a TLS build (MIRRORCPP_WITH_TLS); otherwise the
// function returns a registry error at runtime.
Result<std::unique_ptr<TlsTransport>> connect_from_registry(
    std::string_view registry_url, const TlsOptions& tls,
    std::optional<std::string> pin_override = std::nullopt,
    RegistryOptions options = {});

} // namespace mirrorcpp

#endif

