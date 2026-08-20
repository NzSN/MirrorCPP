// mirrorcpp-validate — CI-friendly port of `ModelMirrors validate` (design §5.7).
//
// Resolves a TLA+ spec inline via spec_from_files (EXTENDS/INSTANCE closure),
// connects to a mirror over TCP / TLS / registry discovery, sends
// register_validate, and reports the verdict.
//
// Exit codes:
//   0  spec VALID   ("VALID" on stdout)
//   1  spec INVALID ("INVALID" + apalache output on stdout)
//   2  infrastructure error (message on stderr): bad flags, transport /
//      discovery / register_error / protocol_error, spec resolution failure.
#include <mirrorcpp/mirrorcpp.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace mirrorcpp;

using std::unexpected;

namespace {

struct Options {
  // Connection: either host/port (+ optional TLS) or registry (implies mTLS).
  bool has_host = false;
  std::string host;
  std::uint16_t port = 0;
  bool has_registry = false;
  std::string registry;
  bool tls = false;
  std::filesystem::path cert, key, ca;
  std::optional<std::string> pin;
  // Spec + validate inputs.
  std::filesystem::path spec;
  std::vector<std::filesystem::path> deps;  // --dep, repeatable
  long long bound = 10;
  std::string invariant;
  std::optional<std::string> init_predicate, next_predicate, const_init;
  bool show_help = false;
};

const char* kUsage =
    "usage: mirrorcpp-validate\n"
    "  --host H --port P [--tls --cert C --key K --ca CA [--pin SHA256]]\n"
    "       | --registry URL [--tls --cert C --key K --ca CA [--pin SHA256]]\n"
    "  --spec S.tla (required) [--dep D.tla]...\n"
    "  [--bound N (default 10)] [--inv I] [--init P] [--next P] [--cinit C]\n"
    "\n"
    "Resolve the spec inline via spec_from_files (EXTENDS/INSTANCE closure);\n"
    "--dep files' directories are searched for dependencies.\n"
    "--registry implies mTLS discovery and is mutually exclusive with --host/--port.\n"
    "--pin requires --tls.\n"
    "Exit codes: 0 = VALID, 1 = INVALID, 2 = infrastructure error.\n";

int usage(FILE* out) {
  std::fputs(kUsage, out);
  return 0;
}

int error2(const std::string& msg) {
  std::fprintf(stderr, "mirrorcpp-validate: %s\n", msg.c_str());
  return 2;
}

// Consume the value for the flag at argv[i]; returns false if missing.
bool take_value(int argc, char** argv, int& i, const char* flag, std::string& out) {
  if (i + 1 >= argc) {
    std::fprintf(stderr, "mirrorcpp-validate: missing value for %s\n", flag);
    return false;
  }
  out = argv[++i];
  return true;
}

bool parse_port(const std::string& s, std::uint16_t& out) {
  char* end = nullptr;
  const long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() || *end != 0 || v < 1 || v > 65535) return false;
  out = static_cast<std::uint16_t>(v);
  return true;
}

// Parse argv into Options; on error prints to stderr and returns false.
bool parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") { o.show_help = true; continue; }
    if (a == "--host") {
      if (!take_value(argc, argv, i, "--host", o.host)) return false;
      o.has_host = true;
    } else if (a == "--port") {
      std::string v;
      if (!take_value(argc, argv, i, "--port", v)) return false;
      if (!parse_port(v, o.port)) {
        std::fprintf(stderr, "mirrorcpp-validate: bad --port '%s' (1..65535)\n", v.c_str());
        return false;
      }
    } else if (a == "--registry") {
      if (!take_value(argc, argv, i, "--registry", o.registry)) return false;
      o.has_registry = true;
    } else if (a == "--spec") {
      std::string v;
      if (!take_value(argc, argv, i, "--spec", v)) return false;
      o.spec = v;
    } else if (a == "--dep") {
      std::string v;
      if (!take_value(argc, argv, i, "--dep", v)) return false;
      o.deps.emplace_back(v);
    } else if (a == "--bound") {
      std::string v;
      if (!take_value(argc, argv, i, "--bound", v)) return false;
      char* end = nullptr;
      const long long b = std::strtoll(v.c_str(), &end, 10);
      if (end == v.c_str() || *end != 0 || b < 0) {
        std::fprintf(stderr, "mirrorcpp-validate: bad --bound '%s'\n", v.c_str());
        return false;
      }
      o.bound = b;
    } else if (a == "--inv") {
      if (!take_value(argc, argv, i, "--inv", o.invariant)) return false;
    } else if (a == "--init") {
      std::string v;
      if (!take_value(argc, argv, i, "--init", v)) return false;
      o.init_predicate = v;
    } else if (a == "--next") {
      std::string v;
      if (!take_value(argc, argv, i, "--next", v)) return false;
      o.next_predicate = v;
    } else if (a == "--cinit") {
      std::string v;
      if (!take_value(argc, argv, i, "--cinit", v)) return false;
      o.const_init = v;
    } else if (a == "--tls") {
      o.tls = true;
    } else if (a == "--cert") {
      std::string v;
      if (!take_value(argc, argv, i, "--cert", v)) return false;
      o.cert = v;
    } else if (a == "--key") {
      std::string v;
      if (!take_value(argc, argv, i, "--key", v)) return false;
      o.key = v;
    } else if (a == "--ca") {
      std::string v;
      if (!take_value(argc, argv, i, "--ca", v)) return false;
      o.ca = v;
    } else if (a == "--pin") {
      std::string v;
      if (!take_value(argc, argv, i, "--pin", v)) return false;
      o.pin = v;
    } else {
      std::fprintf(stderr, "mirrorcpp-validate: unknown option '%s'\n", argv[i]);
      return false;
    }
  }

  // --spec is required.
  if (o.spec.empty()) {
    std::fprintf(stderr, "mirrorcpp-validate: --spec is required\n");
    return false;
  }
  // --registry mutually exclusive with --host/--port.
  if (o.has_registry && o.has_host) {
    std::fprintf(stderr, "mirrorcpp-validate: --registry is mutually exclusive with --host/--port\n");
    return false;
  }
  if (!o.has_registry && !o.has_host) {
    std::fprintf(stderr, "mirrorcpp-validate: need --host/--port or --registry\n");
    return false;
  }
  // --pin requires --tls.
  if (o.pin && !o.tls) {
    std::fprintf(stderr, "mirrorcpp-validate: --pin requires --tls\n");
    return false;
  }
  return true;
}

#if MIRRORCPP_WITH_TLS
TlsOptions make_tls_options(const Options& o) {
  TlsOptions t;
  t.ca_path = o.ca;
  t.cert_path = o.cert;
  t.key_path = o.key;
  t.pin = o.pin;
  return t;
}
#endif

// Connect per the flags. Returns an error Result on failure so the caller
// exits 2.
Result<std::unique_ptr<Transport>> connect(const Options& o) {
#if MIRRORCPP_WITH_TLS
  if (o.has_registry) {
    if (!o.tls) {
      // --registry implies mTLS discovery; require TLS material.
      return unexpected(Error(ErrorKind::tls,
                              "--registry requires --tls with --cert/--key/--ca"));
    }
    if (o.cert.empty() || o.key.empty() || o.ca.empty()) {
      return unexpected(Error(ErrorKind::tls,
                              "--registry --tls requires --cert, --key and --ca"));
    }
    auto t = connect_from_registry(o.registry, make_tls_options(o));
    if (!t) return unexpected(t.error());
    // Upcast TlsTransport -> Transport (see the peer_fingerprint() contract
    // note in transport.hpp: fingerprint needs a dynamic_cast post-upcast).
    return std::unique_ptr<Transport>(std::move(*t));
  }
  if (o.tls) {
    if (o.cert.empty() || o.key.empty() || o.ca.empty()) {
      return unexpected(Error(ErrorKind::tls,
                              "--tls requires --cert, --key and --ca"));
    }
    auto t = connect_tls(o.host, o.port, make_tls_options(o));
    if (!t) return unexpected(t.error());
    // Upcast TlsTransport -> Transport (see the peer_fingerprint() contract
    // note in transport.hpp: fingerprint needs a dynamic_cast post-upcast).
    return std::unique_ptr<Transport>(std::move(*t));
  }
#else
  // Without TLS neither --registry (mTLS discovery) nor --tls can work.
  if (o.has_registry) {
    return unexpected(Error(ErrorKind::tls,
                            "--registry requires a TLS build of mirrorcpp"));
  }
  if (o.tls) {
    return unexpected(Error(ErrorKind::tls,
                            "--tls requires a TLS build of mirrorcpp"));
  }
#endif
  return connect_tcp(o.host, o.port);
}

}  // namespace

int main(int argc, char** argv) {
  // --help / -h works regardless of other flag validity.
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--help" || a == "-h") return usage(stdout);
  }
  Options o;
  if (!parse_args(argc, argv, o)) return 2;   // usage already printed to stderr

  // Resolve the spec inline (EXTENDS/INSTANCE closure + --dep search dirs).
  std::vector<std::filesystem::path> search_dirs;
  for (const auto& d : o.deps) {
    const auto dir = d.parent_path();
    if (!dir.empty()) search_dirs.push_back(dir);
  }
  auto spec = spec_from_files(o.spec, search_dirs);
  if (!spec) return error2("spec resolution failed: " + spec.error().message);

  // Connect.
  auto transport = connect(o);
  if (!transport) return error2("connect failed: " + transport.error().message);

  // Build apalacheConfig; specPath is a placeholder — the inline spec travels
  // in the register_validate spec field.
  ApalacheConfig cfg;
  cfg.spec_path = o.spec.string();        // placeholder; inline spec present
  cfg.invariant = o.invariant;
  cfg.init_predicate = o.init_predicate;
  cfg.next_predicate = o.next_predicate;
  cfg.const_init = o.const_init;
  cfg.length_bound = o.bound;
  cfg.param_vars = "";

  auto verdict = run_client_validate(**transport, cfg, o.bound, std::move(*spec));
  if (!verdict) return error2("validate failed: " + verdict.error().message);

  if (verdict->valid) {
    std::puts("VALID");
    return 0;
  }
  // Invalid: "INVALID" + apalache output on stdout, exit 1.
  std::printf("INVALID\n%s\n", verdict->detail.c_str());
  return 1;
}
