// MirrorCPP example: conformance-check a C++ HourClock state machine against
// the TLA+ model with ModelMirrors (design §10).
//
//   stdio:  spawn the mirror as a child process (this example)
//   TCP:    auto transport = mirrorcpp::connect_tcp("127.0.0.1", 8823);
//           (run the mirror with: ModelMirrors --serve 8823)
//   mTLS:   mirrorcpp::TlsOptions tls{ .ca_path = "ca.crt",
//                                      .cert_path = "client.crt",
//                                      .key_path = "client.key",
//                                      .pin = "…64 lowercase sha256 hex…" };
//           auto transport = mirrorcpp::connect_tls("127.0.0.1", 8823, tls);
//   Consul: auto transport = mirrorcpp::connect_from_registry(
//               "http://127.0.0.1:8500", tls);   // service "modelmirrors"
//
// Build: cmake --build build --target hourclock; then run:
//   ./build/examples/hourclock /path/to/ModelMirrors specs/HourClock.tla
#include <mirrorcpp/mirrorcpp.hpp>

#include <cstdio>
#include <string>
#include <string_view>

using namespace mirrorcpp;

namespace {

// ---------------------------------------------------------------------------
// HourClock — the C++ implementation under test (design §10).
// Every state variable of HourClock.tla is carried in a State
// (std::map<std::string, Value>); report_state includes every variable and
// omits paramVars (§3.5).
// ---------------------------------------------------------------------------

// Adopt the oracle's initial state verbatim.
State hc_init(const State& oracle) { return oracle; }

// One tick: HCnext — hr advances modulo 12 (12 -> 1); latest_hr' = old hr;
// ticked' = TRUE; action_taken' = "tick"; step_count increments by 1.
State hc_tick(const State& prev) {
  const Value::Int hr    = prev.at("hr").as_int().value_or(Value::Int(1));
  const Value::Int latest = prev.at("latest_hr").as_int().value_or(Value::Int(1));
  const Value::Int count  = prev.at("step_count").as_int().value_or(Value::Int(0));

  Value::Record nondet;
  if (const Value::Record* r = prev.at("nondet_picks").as_record())
    nondet = *r;
  else {
    nondet.fields["start_hr"] = Value(hr);
    nondet.fields["start_latest_hr"] = Value(latest);
  }

  State s;
  s["hr"]             = Value(hr % 12 + 1);
  s["latest_hr"]      = Value(hr);
  s["ticked"]         = Value(true);
  s["action_taken"]   = Value(std::string("tick"));
  s["nondet_picks"]   = Value(std::move(nondet));
  s["step_count"]     = Value(count + 1);
  return s;
}

// StateComputer: adopt the oracle init, then hc_tick for every step.
StateComputer hourclock_computer() {
  return [](std::string_view action, const State& params_or_state,
            const State& prev_state) -> State {
    if (action == "init") return hc_init(params_or_state);
    return hc_tick(prev_state);
  };
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <ModelMirrors-binary> <spec.tla>\n"
                 "  stdio transport: spawns the mirror as a child process.\n"
                 "  spec.tla resolved via spec_from_files and sent inline\n"
                 "  (EXTENDS/INSTANCE closure; see design 3.2).\n",
                 argv[0]);
    return 2;
  }
  const std::string mirror_bin = argv[1];

  // stdio transport (design §5.3): the mirror is a child process with piped
  // stdin/stdout; stderr is inherited so Apalache logs stay visible. One
  // session per child.
  auto transport = spawn_mirror(mirror_bin);
  if (!transport) {
    std::fprintf(stderr, "spawn_mirror(%s) failed\n", mirror_bin.c_str());
    return 1;
  }

  ApalacheConfig cfg;
  cfg.spec_path = argv[2];  // specPath is always serialized, even when inline
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 13;  // init + a full 12-hour cycle

  StateComputer compute = hourclock_computer();
  TraceGenerationConfig tc;
  tc.num_traces = 1;
  Result<void> result;

  // Inline spec: EXTENDS/INSTANCE closure resolved from the file's directory
  // (and TLA_LIBRARY_PATH); sources[0] is the root module (§4.1).
  auto spec = spec_from_files(argv[2]);
  if (!spec) {
    std::fprintf(stderr, "spec_from_files: %s\n", spec.error().message.c_str());
    return 1;
  }
  result = run_client(*transport, cfg, tc, std::move(compute), *spec);

  if (!result) {
    std::fprintf(stderr, "conformance check FAILED: %s\n",
                 result.error().message.c_str());
    if (result.error().kind == ErrorKind::step_mismatch && result.error().hints) {
      std::fprintf(stderr, "  diff hints: %s\n",
                   render_diff_hints(*result.error().hints).c_str());
    }
    return 1;
  }
  std::puts("PASS: HourClock conforms to the TLA+ model (all_steps_done)");
  return 0;
}