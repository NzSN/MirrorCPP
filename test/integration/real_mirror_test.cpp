// MirrorCPP real-mirror integration tests (design §8 scenarios 1, 3, 4, 5).
//
// Against a REAL ModelMirrors binary ($MIRROR_BIN) over stdio:
//   1. run_client_with_traces: replay a vendored HourClock ITF trace.
//   2. run_client: generate and replay traces from Counter.tla ($SPEC may
//      select an authoritative copy outside this repository).
//   3. run_client: register HourClock end-to-end.
//   4. run_client_gen_traces: gen_traces_done with inline itfTraces.
//   5. run_client_validate: valid verdict + bound=101 -> register_error.
//   6. explore: run_client_explore + a scripted ExploreSession.
//
// Gated on the MIRROR_BIN env var: when unset the test prints a skip notice and
// exits with 77 so ctest reports it as skipped (SKIP_RETURN_CODE 77). When set,
// every scenario must pass or the test exits 1. This is a standalone program
// (not Catch2-discovered) so the skip return code is under our control.
#include <mirrorcpp/mirrorcpp.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "hourclock_client.hpp"

using namespace mirrorcpp;

namespace {

std::filesystem::path root() {
  return std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / ".." / "..";
}

ApalacheConfig hourclock_config() {
  ApalacheConfig cfg;
  cfg.spec_path = (root() / "specs" / "HourClock.tla").string();
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 10;
  return cfg;
}

std::filesystem::path counter_spec_path() {
  const char* spec = std::getenv("SPEC");
  if (spec != nullptr && *spec != 0) return spec;
  return root() / "specs" / "Counter.tla";
}

ApalacheConfig counter_config() {
  ApalacheConfig cfg;
  cfg.spec_path = counter_spec_path().string();
  cfg.const_init = "CInit";
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 6;
  cfg.param_vars = "parameters";
  return cfg;
}

StateComputer counter_computer() {
  return [count = Value::Int(0)](std::string_view, const State& params,
                                 const State& prev) mutable -> State {
    if (prev.empty()) {
      count = 0;
    } else {
      const Value* wrapper = get_param(params, "parameters");
      const Value::Record* record = wrapper == nullptr ? nullptr : wrapper->as_record();
      if (record == nullptr) throw std::runtime_error("Counter step has no parameters record");
      const auto stride = record->fields.find("stride");
      if (stride == record->fields.end() || !stride->second.as_int())
        throw std::runtime_error("Counter step has no integer stride");
      count += *stride->second.as_int();
    }
    State state;
    state["count"] = Value(count);
    return state;
  };
}

StateComputer incorrect_counter_computer() {
  auto correct = counter_computer();
  return [correct = std::move(correct)](std::string_view action,
                                        const State& params,
                                        const State& prev) mutable -> State {
    State state = correct(action, params, prev);
    state["unexpected"] = Value(true);
    return state;
  };
}

bool is_extra_key_mismatch(const Result<void>& result) {
  if (result || result.error().kind != ErrorKind::step_mismatch ||
      !result.error().actual) {
    return false;
  }
  return result.error().actual->contains("unexpected");
}

struct TestPki {
  std::filesystem::path dir;
  std::filesystem::path ca, server_cert, server_key, client_cert, client_key;

  TestPki() {
    char pattern[] = "/tmp/mirrorcpp-server-replay-XXXXXX";
    const char* made = ::mkdtemp(pattern);
    if (made == nullptr) throw std::runtime_error("cannot create TLS fixture directory");
    dir = made;
    ca = dir / "ca.crt";
    server_cert = dir / "server.crt";
    server_key = dir / "server.key";
    client_cert = dir / "client.crt";
    client_key = dir / "client.key";

    std::ofstream(dir / "server.ext")
        << "subjectAltName=IP:127.0.0.1\n"
           "basicConstraints=CA:FALSE\n"
           "keyUsage=digitalSignature,keyEncipherment\n"
           "extendedKeyUsage=serverAuth\n";
    std::ofstream(dir / "client.ext")
        << "basicConstraints=CA:FALSE\n"
           "keyUsage=digitalSignature,keyEncipherment\n"
           "extendedKeyUsage=clientAuth\n";

    const std::string d = dir.string();
    const std::vector<std::string> commands = {
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + d +
            "/ca.key -out " + d +
            "/ca.crt -days 2 -subj '/CN=MirrorCPP Replay CA' "
            "-addext 'basicConstraints=critical,CA:TRUE' 2>/dev/null",
        "openssl req -newkey rsa:2048 -nodes -keyout " + d +
            "/server.key -out " + d +
            "/server.csr -subj '/CN=127.0.0.1' 2>/dev/null",
        "openssl x509 -req -in " + d + "/server.csr -CA " + d +
            "/ca.crt -CAkey " + d + "/ca.key -CAcreateserial -out " + d +
            "/server.crt -days 2 -sha256 -extfile " + d + "/server.ext 2>/dev/null",
        "openssl req -newkey rsa:2048 -nodes -keyout " + d +
            "/client.key -out " + d +
            "/client.csr -subj '/CN=mirrorcpp-replay-client' 2>/dev/null",
        "openssl x509 -req -in " + d + "/client.csr -CA " + d +
            "/ca.crt -CAkey " + d + "/ca.key -CAcreateserial -out " + d +
            "/client.crt -days 2 -sha256 -extfile " + d + "/client.ext 2>/dev/null",
    };
    for (const auto& command : commands) {
      if (std::system(command.c_str()) != 0)
        throw std::runtime_error("openssl failed while generating TLS fixtures");
    }
    ::chmod(server_key.c_str(), 0600);
    ::chmod(client_key.c_str(), 0600);
  }

  ~TestPki() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

int free_loopback_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    if (fd >= 0) ::close(fd);
    return -1;
  }
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

pid_t spawn_tls_server(const char* bin, int port, const TestPki& pki) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  const std::string port_s = std::to_string(port);
  const std::string cert = pki.server_cert.string();
  const std::string key = pki.server_key.string();
  const std::string ca = pki.ca.string();
  char* const argv[] = {
      const_cast<char*>(bin), const_cast<char*>("--server"),
      const_cast<char*>(port_s.c_str()), const_cast<char*>("--tls"),
      const_cast<char*>("--cert"), const_cast<char*>(cert.c_str()),
      const_cast<char*>("--key"), const_cast<char*>(key.c_str()),
      const_cast<char*>("--ca"), const_cast<char*>(ca.c_str()),
      const_cast<char*>("--jobs"), const_cast<char*>("2"), nullptr};
  ::execv(bin, argv);
  _exit(127);
}

int scenario_with_traces(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  auto result = run_client_with_traces(
      *transport, hourclock_config(),
      std::vector<std::string>{(root() / "specs" / "traces" / "HourClock_trace1.itf.json")
                                   .string()},
      test::hourclock_computer());
  if (result) {
    std::puts("PASS: run_client_with_traces -> all_steps_done");
    return 0;
  }
  std::fprintf(stderr, "FAIL: run_client_with_traces: %s\n",
               result.error().message.c_str());
  return 1;
}

int scenario_counter_replay(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  TraceGenerationConfig tc;
  tc.num_traces = 10;
  tc.view = "View";
  auto result = run_client(*transport, counter_config(), tc, counter_computer());
  if (result) {
    std::printf("PASS: Counter generate+replay -> all_steps_done (%s)\n",
                counter_spec_path().c_str());
    return 0;
  }
  std::fprintf(stderr, "FAIL: Counter generate+replay: %s\n",
               result.error().message.c_str());
  return 1;
}

int scenario_counter_mismatch(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) return 1;
  TraceGenerationConfig tc;
  tc.num_traces = 10;
  tc.view = "View";
  auto result = run_client(*transport, counter_config(), tc,
                           incorrect_counter_computer());
  if (!is_extra_key_mismatch(result)) {
    std::fprintf(stderr, "FAIL: incorrect Counter did not produce extra-key step_mismatch\n");
    return 1;
  }
  std::puts("PASS: incorrect Counter is rejected with terminal step_mismatch");
  return 0;
}

int scenario_counter_replay_mtls(const char* bin) {
  TestPki pki;
  const int port = free_loopback_port();
  if (port <= 0) {
    std::fprintf(stderr, "FAIL: cannot allocate mTLS replay port\n");
    return 1;
  }
  const pid_t server = spawn_tls_server(bin, port, pki);
  if (server <= 0) {
    std::fprintf(stderr, "FAIL: cannot spawn mirror --server\n");
    return 1;
  }
  struct ServerGuard {
    pid_t pid;
    ~ServerGuard() { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); }
  } guard{server};

  TlsOptions opts;
  opts.ca_path = pki.ca;
  opts.cert_path = pki.client_cert;
  opts.key_path = pki.client_key;
  std::unique_ptr<TlsTransport> transport;
  for (int i = 0; i < 100 && !transport; ++i) {
    auto connected = connect_tls("127.0.0.1", static_cast<std::uint16_t>(port), opts);
    if (connected) transport = std::move(*connected);
    else ::usleep(100000);
  }
  if (!transport) {
    std::fprintf(stderr, "FAIL: mTLS server never accepted a conforming client\n");
    return 1;
  }
  if (transport->peer_fingerprint().size() != 64) {
    std::fprintf(stderr, "FAIL: mTLS peer fingerprint was not captured\n");
    return 1;
  }

  auto inline_spec = spec_from_files(counter_spec_path());
  if (!inline_spec) {
    std::fprintf(stderr, "FAIL: Counter inline closure: %s\n",
                 inline_spec.error().message.c_str());
    return 1;
  }
  auto cfg = counter_config();
  cfg.spec_path = "Counter.tla";
  TraceGenerationConfig tc;
  tc.num_traces = 10;
  tc.view = "View";
  auto result = run_client(*transport, cfg, tc, counter_computer(), std::move(*inline_spec));
  if (!result) {
    std::fprintf(stderr, "FAIL: Counter mTLS generate+replay: %s\n",
                 result.error().message.c_str());
    return 1;
  }
  std::puts("PASS: Counter generate+replay over real mTLS server mode");

  auto bad_transport = connect_tls("127.0.0.1", static_cast<std::uint16_t>(port), opts);
  auto bad_spec = spec_from_files(counter_spec_path());
  if (!bad_transport || !bad_spec) {
    std::fprintf(stderr, "FAIL: could not prepare mTLS mismatch replay\n");
    return 1;
  }
  auto mismatch = run_client(**bad_transport, cfg, tc, incorrect_counter_computer(),
                             std::move(*bad_spec));
  if (!is_extra_key_mismatch(mismatch)) {
    std::fprintf(stderr, "FAIL: mTLS incorrect Counter was not rejected\n");
    return 1;
  }
  std::puts("PASS: mTLS incorrect Counter rejected with step_mismatch");
  return 0;
}

int scenario_register(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  TraceGenerationConfig tc;
  tc.num_traces = 1;
  auto result = run_client(*transport, hourclock_config(), tc, test::hourclock_computer());
  if (result) {
    std::puts("PASS: run_client register -> all_steps_done");
    return 0;
  }
  std::fprintf(stderr, "FAIL: run_client register: %s\n", result.error().message.c_str());
  return 1;
}

int scenario_gen_traces(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  TraceGenerationConfig tc;
  tc.num_traces = 1;
  ApalacheSpec spec;
  // The inline spec must be a COMPLETE parseable module: the mirror
  // materializes it to a temp file for apalache. Load the full vendored
  // body (t13 first real-mirror run: header-only source fails to parse).
  std::ifstream in((root() / "specs" / "HourClock.tla").string());
  std::ostringstream ss;
  ss << in.rdbuf();
  spec.sources = {ss.str()};
  // destPath is explicit: the Lean mirror crashes without it (its default
  // output directory is never created — server-side bug). With a destPath the
  // mirror creates the directory itself.
  const auto dest = std::filesystem::temp_directory_path() / "mirrorcpp-gen-traces-out";
  std::error_code ec;
  std::filesystem::remove_all(dest, ec);
  auto result = run_client_gen_traces(*transport, hourclock_config(), tc,
                                      dest.string(), std::move(spec));
  if (!result) {
    std::fprintf(stderr, "FAIL: run_client_gen_traces: %s\n",
                 result.error().message.c_str());
    return 1;
  }
  if (result->itf_trace_paths.empty()) {
    std::fprintf(stderr, "FAIL: run_client_gen_traces: no trace paths returned\n");
    return 1;
  }
  std::puts("PASS: run_client_gen_traces -> gen_traces_done with paths");
  return 0;
}

int scenario_validate_valid(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  auto verdict = run_client_validate(*transport, hourclock_config(), /*bound=*/10);
  if (!verdict || !verdict->valid) {
    std::fprintf(stderr, "FAIL: run_client_validate valid verdict\n");
    return 1;
  }
  std::puts("PASS: run_client_validate -> valid");
  return 0;
}

int scenario_validate_bound101(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  auto verdict = run_client_validate(*transport, hourclock_config(), /*bound=*/101);
  if (verdict) {
    std::fprintf(stderr, "FAIL: run_client_validate bound=101 should have errored\n");
    return 1;
  }
  if (verdict.error().kind != ErrorKind::registration) {
    std::fprintf(stderr, "FAIL: bound=101 expected registration error, got kind %d\n",
                 static_cast<int>(verdict.error().kind));
    return 1;
  }
  std::puts("PASS: run_client_validate bound=101 -> registration error");
  return 0;
}

// Load the full HourClock.tla body as an inline spec source.
std::vector<std::string> hourclock_sources() {
  std::ifstream in((root() / "specs" / "HourClock.tla").string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return {ss.str()};
}

int scenario_explore(const char* bin) {
  // run_client_explore: mirror-driven exploration, full expected state in
  // next_step.parameters.
  {
    auto transport = spawn_mirror(bin);
    if (!transport) {
      std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
      return 1;
    }
    ApalacheSpec spec;
    spec.sources = hourclock_sources();
    auto result = run_client_explore(*transport, spec, {"TraceComplete"}, {"Actions"},
                                     /*max_steps=*/10, test::hourclock_computer());
    if (result) {
      std::puts("PASS: run_client_explore -> all_steps_done");
    } else {
      std::fprintf(stderr, "FAIL: run_client_explore: %s\n",
                   result.error().message.c_str());
      return 1;
    }
  }

  // ExploreSession: open + a scripted sequence + done.
  {
    auto transport = spawn_mirror(bin);
    if (!transport) {
      std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
      return 1;
    }
    ApalacheSpec spec;
    spec.sources = hourclock_sources();
    auto session = ExploreSession::open(std::move(transport), spec,
                                        {"TraceComplete"}, {"Actions"});
    if (!session) {
      std::fprintf(stderr, "FAIL: ExploreSession::open: %s\n",
                   session.error().message.c_str());
      return 1;
    }
    if (session->ready().state_invariants < 0 ||
        session->ready().init_transitions <= 0) {
      std::fprintf(stderr, "FAIL: ExploreSession ready counts invalid\n");
      return 1;
    }
    // The mirror's init transition IDs are 0-based (ID 0 is the only valid one
    // for HourClock's single init transition) — t13 real-mirror finding.
    auto st = session->assume_transition(0);
    if (!st) {
      std::fprintf(stderr, "FAIL: assume_transition: %s\n", st.error().message.c_str());
      return 1;
    }
    auto done = session->done();
    if (!done) {
      std::fprintf(stderr, "FAIL: ExploreSession::done: %s\n",
                   done.error().message.c_str());
      return 1;
    }
    std::puts("PASS: ExploreSession scripted session -> done");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Verify against the real mirror that protocol_error poisons the explore
// connection as required by the client guide §9.
// ---------------------------------------------------------------------------
int scenario_explore_protocol_error(const char* bin) {
  auto transport = spawn_mirror(bin);
  if (!transport) {
    std::fprintf(stderr, "FAIL: could not spawn mirror binary '%s'\n", bin);
    return 1;
  }
  ApalacheSpec spec;
  spec.sources = hourclock_sources();
  auto session = ExploreSession::open(std::move(transport), spec,
                                      {"TraceComplete"}, {"Actions"});
  if (!session) {
    std::fprintf(stderr, "FAIL: ExploreSession::open: %s\n",
                 session.error().message.c_str());
    return 1;
  }
  // Illegal command: out-of-range transition id. The mirror answers
  // protocol_error; MirrorCPP must surface it and close the session.
  auto bad = session->assume_transition(9999);
  if (bad) {
    std::fprintf(stderr, "FAIL: assume_transition(9999) unexpectedly succeeded\n");
    return 1;
  }
  if (bad.error().kind != ErrorKind::protocol) {
    std::fprintf(stderr, "FAIL: assume_transition(9999): expected protocol error, got %s\n",
                 error_kind_name(bad.error().kind));
    return 1;
  }
  // The session must be unusable after the protocol error.
  auto good = session->assume_transition(0);
  if (good || good.error().kind != ErrorKind::io) {
    std::fprintf(stderr, "FAIL: session remained usable after protocol_error\n");
    return 1;
  }
  std::puts("PASS: explore protocol_error poisons and closes the session");
  return 0;
}

// ---------------------------------------------------------------------------
// G1.6 real-mirror async (guide §6): spawn `mirror --serve <port>`, submit a
// validate job, query it from a SECOND connection (C17), await to a terminal
// result and check the C20 payload congruence (validate outcome == sync
// verdict), then cancel a fresh job (C19).
// ---------------------------------------------------------------------------

pid_t spawn_server(const char* bin, int port) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  const int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDERR_FILENO);
  }
  const std::string port_s = std::to_string(port);
  char* const argv[] = {const_cast<char*>(bin), const_cast<char*>("--serve"),
                        const_cast<char*>(port_s.c_str()), nullptr};
  ::execv(bin, argv);
  _exit(127);
}

int scenario_counter_replay_tcp(const char* bin) {
  const int port = free_loopback_port();
  if (port <= 0) {
    std::fprintf(stderr, "FAIL: cannot allocate TCP replay port\n");
    return 1;
  }
  const pid_t server = spawn_server(bin, port);
  if (server <= 0) {
    std::fprintf(stderr, "FAIL: cannot spawn mirror --serve\n");
    return 1;
  }
  struct ServerGuard {
    pid_t pid;
    ~ServerGuard() { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); }
  } guard{server};

  std::unique_ptr<Transport> transport;
  for (int i = 0; i < 100 && !transport; ++i) {
    auto connected = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
    if (connected) transport = std::move(*connected);
    else ::usleep(100000);
  }
  if (!transport) {
    std::fprintf(stderr, "FAIL: TCP server never accepted a client\n");
    return 1;
  }
  auto inline_spec = spec_from_files(counter_spec_path());
  if (!inline_spec) {
    std::fprintf(stderr, "FAIL: Counter inline closure: %s\n",
                 inline_spec.error().message.c_str());
    return 1;
  }
  auto cfg = counter_config();
  cfg.spec_path = "Counter.tla";
  TraceGenerationConfig tc;
  tc.num_traces = 10;
  tc.view = "View";
  auto result = run_client(*transport, cfg, tc, counter_computer(), std::move(*inline_spec));
  if (!result) {
    std::fprintf(stderr, "FAIL: Counter TCP generate+replay: %s\n",
                 result.error().message.c_str());
    return 1;
  }
  std::puts("PASS: Counter generate+replay over real TCP server mode");

  auto bad_transport = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
  auto bad_spec = spec_from_files(counter_spec_path());
  if (!bad_transport || !bad_spec) {
    std::fprintf(stderr, "FAIL: could not prepare TCP mismatch replay\n");
    return 1;
  }
  auto mismatch = run_client(**bad_transport, cfg, tc, incorrect_counter_computer(),
                             std::move(*bad_spec));
  if (!is_extra_key_mismatch(mismatch)) {
    std::fprintf(stderr, "FAIL: TCP incorrect Counter was not rejected\n");
    return 1;
  }
  std::puts("PASS: TCP incorrect Counter rejected with step_mismatch");
  return 0;
}

int scenario_async_jobs(const char* bin) {
  // Ephemeral port.
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    std::fprintf(stderr, "FAIL: async: cannot allocate ephemeral port\n");
    return 1;
  }
  socklen_t len = sizeof addr;
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(fd);

  const pid_t srv = spawn_server(bin, port);
  if (srv <= 0) {
    std::fprintf(stderr, "FAIL: async: cannot spawn mirror --serve\n");
    return 1;
  }
  struct ServerGuard {
    pid_t pid;
    ~ServerGuard() { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); }
  } guard{srv};

  // Wait for the listener (poll connect up to ~10 s).
  std::unique_ptr<Transport> conn;
  for (int i = 0; i < 100 && !conn; ++i) {
    auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
    if (t) conn = std::move(*t);
    else ::usleep(100000);
  }
  if (!conn) {
    std::fprintf(stderr, "FAIL: async: mirror --serve never accepted\n");
    return 1;
  }
  if (!conn->async_capable()) {
    std::fprintf(stderr, "FAIL: async: TCP transport not async_capable\n");
    return 1;
  }

  ApalacheConfig cfg = hourclock_config();
  ApalacheSpec spec;
  spec.sources = hourclock_sources();

  auto sync_conn = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
  if (!sync_conn) {
    std::fprintf(stderr, "FAIL: async: sync comparison connection failed\n");
    return 1;
  }
  auto sync_verdict = run_client_validate(**sync_conn, cfg, /*bound=*/10, spec);
  if (!sync_verdict) {
    std::fprintf(stderr, "FAIL: async: sync comparison validate failed: %s\n",
                 sync_verdict.error().message.c_str());
    return 1;
  }

  auto accepted = submit_validate_async(*conn, cfg, /*bound=*/10, spec);
  if (!accepted) {
    std::fprintf(stderr, "FAIL: submit_validate_async: %s\n",
                 accepted.error().message.c_str());
    return 1;
  }
  if (accepted->kind != JobKind::validate) {
    std::fprintf(stderr, "FAIL: submit_validate_async: wrong kind\n");
    return 1;
  }

  // C17: query from a SECOND connection while the job runs.
  {
    auto t2 = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
    if (!t2) {
      std::fprintf(stderr, "FAIL: async: second connection failed\n");
      return 1;
    }
    auto st = query_job(**t2, accepted->job_id);
    if (!st || (st->phase != JobPhase::pending && st->phase != JobPhase::running &&
                st->phase != JobPhase::done)) {
      std::fprintf(stderr, "FAIL: async: cross-connection query_job\n");
      return 1;
    }
    (void)(*t2)->close();
  }

  // Await to terminal; the outcome.validate payload must equal the sync
  // verdict for the same config (C20): HourClock bound=10 validates.
  auto done = await_job(*conn, accepted->job_id);
  if (!done || !std::holds_alternative<JobResult>(*done)) {
    std::fprintf(stderr, "FAIL: async: await_job did not terminate with job_result\n");
    return 1;
  }
  const auto& outcome = std::get<JobResult>(*done).outcome;
  if (!std::holds_alternative<SpecValidated>(outcome.value) ||
      std::get<SpecValidated>(outcome.value).is_valid() != sync_verdict->valid) {
    std::fprintf(stderr, "FAIL: async validate payload differs from sync verdict (C20)\n");
    return 1;
  }

  // C23: submit two jobs and await in reverse order. Results must remain
  // correlated by job id rather than arrival or completion order.
  auto ordered_first = submit_validate_async(*conn, cfg, /*bound=*/10, spec);
  auto ordered_second = submit_validate_async(*conn, cfg, /*bound=*/10, spec);
  if (!ordered_first || !ordered_second) {
    std::fprintf(stderr, "FAIL: async: ordered submissions failed\n");
    return 1;
  }
  auto observer = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
  if (!observer) {
    std::fprintf(stderr, "FAIL: async: ordering observer failed\n");
    return 1;
  }
  auto second_first = await_job(**observer, ordered_second->job_id);
  auto first_second = await_job(**observer, ordered_first->job_id);
  if (!second_first || !first_second ||
      !std::holds_alternative<JobResult>(*second_first) ||
      !std::holds_alternative<JobResult>(*first_second) ||
      std::get<JobResult>(*second_first).job_id != ordered_second->job_id ||
      std::get<JobResult>(*first_second).job_id != ordered_first->job_id) {
    std::fprintf(stderr, "FAIL: async: reverse awaits lost job-id correlation (C23)\n");
    return 1;
  }
  (void)(*observer)->close();

  // C19: submit a second job and cancel it; cancel must not error.
  auto accepted2 = submit_validate_async(*conn, cfg, /*bound=*/10, spec);
  if (!accepted2) {
    std::fprintf(stderr, "FAIL: async: second submit: %s\n",
                 accepted2.error().message.c_str());
    return 1;
  }
  auto cancelled = cancel_job(*conn, accepted2->job_id);
  if (!cancelled) {
    std::fprintf(stderr, "FAIL: async: cancel_job: %s\n",
                 cancelled.error().message.c_str());
    return 1;
  }

  (void)conn->close();
  std::puts("PASS: async jobs against real mirror --serve (submit/query/await/cancel)");
  return 0;
}

}  // namespace

int main() {
  const char* bin = std::getenv("MIRROR_BIN");
  if (bin == nullptr || *bin == 0) {
    std::puts("SKIP: MIRROR_BIN not set - skipping real-mirror integration test");
    return 77;
  }

  int failures = 0;
  failures += scenario_with_traces(bin);
  failures += scenario_counter_replay(bin);
  failures += scenario_counter_mismatch(bin);
  failures += scenario_counter_replay_tcp(bin);
  failures += scenario_counter_replay_mtls(bin);
  failures += scenario_register(bin);
  failures += scenario_gen_traces(bin);
  failures += scenario_validate_valid(bin);
  failures += scenario_validate_bound101(bin);
  failures += scenario_explore(bin);
  failures += scenario_explore_protocol_error(bin);
  failures += scenario_async_jobs(bin);

  if (failures != 0) {
    std::fprintf(stderr, "FAIL: %d real-mirror scenario(s) failed\n", failures);
    return 1;
  }
  std::puts("PASS: all real-mirror scenarios");
  return 0;
}
