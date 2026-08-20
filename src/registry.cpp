// mirrorcpp/registry.cpp — Consul discovery + connect-from-registry (design 5.4).
#include <mirrorcpp/registry.hpp>

#include "detail/http.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <utility>

namespace mirrorcpp {

using std::unexpected;

namespace {

std::string trim(std::string_view s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

// Normalize a cert-sha256: trim + lowercase + validate ^[0-9a-f]{64}$.
// Returns std::nullopt when malformed (present pin that fails this is a
// skip-the-entry condition — no unpinned fallback, design 5.4).
std::optional<std::string> normalize_pin(std::string_view raw) {
  const std::string s = trim(raw);
  if (s.size() != 64) return std::nullopt;
  std::string out(64, ' ');
  for (std::size_t i = 0; i < 64; ++i) {
    const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return std::nullopt;
    out[i] = c;
  }
  return out;
}

// Total function (design 5.4): parse one Consul health entry. Malformed
// entries are skipped, never thrown. Returns nullopt when the entry must be
// skipped: non-object Service, missing/blank Address, Port not an integer in
// 1..65535, or a PRESENT but malformed cert-sha256. Absent pin is allowed;
// id defaults to "".
std::optional<MirrorServiceInfo> parse_service_entry(const nlohmann::json& entry) {
  const auto svc_it = entry.find("Service");
  if (svc_it == entry.end() || !svc_it->is_object()) return std::nullopt;
  const nlohmann::json& svc = *svc_it;

  MirrorServiceInfo info;
  const auto id_it = svc.find("ID");
  if (id_it != svc.end() && id_it->is_string()) info.id = id_it->get<std::string>();

  const auto addr_it = svc.find("Address");
  if (addr_it == svc.end() || !addr_it->is_string()) return std::nullopt;
  const std::string addr = trim(addr_it->get<std::string>());
  if (addr.empty()) return std::nullopt;
  info.host = addr;

  const auto port_it = svc.find("Port");
  if (port_it == svc.end() || !port_it->is_number_integer()) return std::nullopt;
  const auto port = port_it->get<long long>();
  if (port < 1 || port > 65535) return std::nullopt;
  info.port = static_cast<std::uint16_t>(port);

  const auto meta_it = svc.find("Meta");
  if (meta_it != svc.end() && meta_it->is_object()) {
    const auto pin_it = meta_it->find("cert-sha256");
    if (pin_it != meta_it->end()) {
      // Present but non-string / malformed -> skip the entry entirely
      // (no unpinned fallback for a broken advertisement).
      if (!pin_it->is_string()) return std::nullopt;
      auto norm = normalize_pin(pin_it->get<std::string>());
      if (!norm) return std::nullopt;
      info.cert_sha256 = std::move(norm);
    }
  }
  return info;
}

// Consul discovery endpoint, appended to the caller's base registry URL.
std::string discovery_url(std::string_view registry_url) {
  std::string url(registry_url);
  while (!url.empty() && url.back() == '/') url.pop_back();
  url += "/v1/health/service/modelmirrors?passing=true";
  return url;
}

} // namespace

Result<std::vector<MirrorServiceInfo>> discover_mirrors(std::string_view registry_url,
                                                        RegistryOptions options) {
  // Fail closed (design 3.6/5.4): every failure path returns an EMPTY list.
  const std::vector<MirrorServiceInfo> empty;
  const auto body = detail::http_get(discovery_url(registry_url), options.timeout);
  if (!body) return empty;
  nlohmann::json doc;
  try {
    doc = nlohmann::json::parse(*body);
  } catch (const nlohmann::json::exception&) {
    return empty;
  }
  if (!doc.is_array()) return empty; // Consul answers with an array of entries
  std::vector<MirrorServiceInfo> out;
  out.reserve(doc.size());
  for (const auto& entry : doc) {
    auto info = parse_service_entry(entry);
    if (info) out.push_back(std::move(*info));
  }
  return out;
}

Result<std::unique_ptr<TlsTransport>> connect_from_registry(
    std::string_view registry_url, const TlsOptions& tls,
    std::optional<std::string> pin_override, RegistryOptions options) {
#if MIRRORCPP_WITH_TLS
  auto candidates = discover_mirrors(registry_url, options);
  if (!candidates) return unexpected(candidates.error()); // unreachable: fail closed
  if (candidates->empty())
    return unexpected(make_error(ErrorKind::registry,
                                 std::string("no mirror candidates discovered from ") +
                                 std::string(registry_url)));

  std::vector<std::string> diagnostics;
  diagnostics.reserve(candidates->size());
  for (const auto& cand : *candidates) {
    const std::string pin = pin_override ? *pin_override
                                         : (cand.cert_sha256 ? *cand.cert_sha256 : "");
    TlsOptions opts = tls;
    opts.pin = pin.empty() ? std::optional<std::string>{}
                           : std::optional<std::string>(pin);
    auto conn = connect_tls(cand.host, cand.port, opts);
    if (conn) return conn;
    diagnostics.push_back(cand.host + ":" + std::to_string(cand.port) + ": " +
                          conn.error().message);
  }
  std::string msg = "all registry candidates failed: ";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    if (i) msg += "; ";
    msg += diagnostics[i];
  }
  return unexpected(make_error(ErrorKind::registry, std::move(msg)));
#else
  (void)registry_url;
  (void)tls;
  (void)pin_override;
  (void)options;
  return unexpected(make_error(ErrorKind::registry,
                               "connect_from_registry requires a TLS build "
                               "(MIRRORCPP_WITH_TLS=ON)"));
#endif
}

} // namespace mirrorcpp

