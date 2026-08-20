// MirrorCPP real-mirror integration tests (design §8 scenarios 1, 3, 4, 5).
//
// Against a REAL ModelMirrors binary ($MIRROR_BIN) over stdio:
//   1. run_client_with_traces: replay a vendored HourClock ITF trace.
//   3. run_client: register end-to-end (mirror generates traces itself).
//   4. run_client_gen_traces: gen_traces_done with inline itfTraces.
//   5. run_client_validate: valid verdict + bound=101 -> register_error.
//   6. explore: run_client_explore + a scripted ExploreSession.
//
// Gated on the MIRROR_BIN env var: when unset the test prints a skip notice and
// exits with 77 so ctest reports it as skipped (SKIP_RETURN_CODE 77). When set,
// every scenario must pass or the test exits 1. This is a standalone program
// (not Catch2-discovered) so the skip return code is under our control.
#include <mirrorcpp/mirrorcpp.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
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
  auto result = run_client_gen_traces(*transport, hourclock_config(), tc,
                                      std::nullopt, std::move(spec));
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

}  // namespace

int main() {
  const char* bin = std::getenv("MIRROR_BIN");
  if (bin == nullptr || *bin == 0) {
    std::puts("SKIP: MIRROR_BIN not set - skipping real-mirror integration test");
    return 77;
  }

  int failures = 0;
  failures += scenario_with_traces(bin);
  failures += scenario_register(bin);
  failures += scenario_gen_traces(bin);
  failures += scenario_validate_valid(bin);
  failures += scenario_validate_bound101(bin);
  failures += scenario_explore(bin);

  if (failures != 0) {
    std::fprintf(stderr, "FAIL: %d real-mirror scenario(s) failed\n", failures);
    return 1;
  }
  std::puts("PASS: all real-mirror scenarios");
  return 0;
}
