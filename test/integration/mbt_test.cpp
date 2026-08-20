// MirrorCPP MBT fixture protocol tests (design §8 "Conformance north star").
//
// The vendored specs/traces/*.itf.json fixtures are Apalache-generated traces
// of ModelMirrors' own protocol model (MirrorProtocol.tla): each state records
// the single in-flight message as a numeric code (client_to_mirror =
// client just sent code X, mirror_to_client = mirror sends code Y). The
// trace-driven fake mirror (fake_mirror_trace + run_trace in fake_mirror.py)
// replays the mirror side from the fixture and verifies every client message
// the C++ client under test emits carries the exact code the model expects —
// TLA+-level confidence in the client implementation itself.
//
// Fixtures covered: all_steps_done, step_mismatch, register_error,
// explore_session, explore_cmd, fault_close. No MIRROR_BIN needed.
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "hourclock_client.hpp"

using namespace mirrorcpp;

namespace {

std::filesystem::path fake_mirror_path(const char* script) {
  return std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / script;
}

std::filesystem::path trace_fixture(const char* name) {
  return std::filesystem::path(MIRRORCPP_SPECS_DIR) / "traces" / (std::string(name) + ".itf.json");
}

// Point the trace-driven fake mirror at a fixture and spawn it.
std::unique_ptr<Transport> spawn_trace_mirror(const char* fixture) {
  if (::setenv("MIRRORCPP_TRACE", trace_fixture(fixture).c_str(), 1) != 0)
    return nullptr;
  return spawn_mirror(fake_mirror_path("fake_mirror_trace"));
}

ApalacheConfig hourclock_config() {
  ApalacheConfig cfg;
  cfg.spec_path = "HourClock.tla";
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 10;
  return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// all_steps_done.itf.json: register_traces(10) -> spec_validated(3) ->
// initial_state(4) -> report_state(2) -> all_steps_done(8)
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: all_steps_done drives run_client_with_traces", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("all_steps_done");
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/all_steps_done.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// step_mismatch.itf.json: ... report_state(2) -> step_mismatch(7)
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: step_mismatch surfaces ErrorKind::step_mismatch", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("step_mismatch");
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/step_mismatch.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::step_mismatch);
}

// ---------------------------------------------------------------------------
// register_error.itf.json: register(0) -> register_error(1)
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: register_error surfaces ErrorKind::registration", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("register_error");
  REQUIRE(transport != nullptr);

  ApalacheSpec spec;
  spec.sources.push_back("module HourClock");
  TraceGenerationConfig tc;
  auto result = run_client(*transport, hourclock_config(), tc,
                           test::hourclock_computer(), spec);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::registration);
}

// ---------------------------------------------------------------------------
// explore_session.itf.json: register_explore_session(14) -> explorer_ready(15)
// -> explore_done(18) -> explore_session_done(18)
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: explore_session open+done", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("explore_session");
  REQUIRE(transport != nullptr);

  ApalacheSpec spec;
  spec.sources.push_back("module HourClock");
  auto session = ExploreSession::open(std::move(transport), spec,
                                      {"TraceComplete"}, {"Actions"});
  REQUIRE(session.has_value());
  REQUIRE(session->ready().init_transitions >= 1);
  auto done = session->done();
  REQUIRE(done.has_value());
}

// ---------------------------------------------------------------------------
// explore_cmd.itf.json: register_explore_session(14) -> explorer_ready(15) ->
// explore command(16) -> explore result(17)
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: explore_cmd assume_transition + done", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("explore_cmd");
  REQUIRE(transport != nullptr);

  ApalacheSpec spec;
  spec.sources.push_back("module HourClock");
  auto session = ExploreSession::open(std::move(transport), spec,
                                      {"TraceComplete"}, {"Actions"});
  REQUIRE(session.has_value());
  auto st = session->assume_transition(0);
  REQUIRE(st.has_value());
  auto done = session->done();
  REQUIRE(done.has_value());
}

// ---------------------------------------------------------------------------
// fault_close.itf.json: register_explore(13) then the client closes the
// connection; the mirror detects the close (trace states ClientCloseConn ->
// MirrorDetectClose). Driven at the codec+transport level: the library emits
// register_explore (code 13) and closes; the fake sees the clean EOF and
// exits 0, which close() reports as the child exit code.
// ---------------------------------------------------------------------------
TEST_CASE("MBT fixture: fault_close emits register_explore then closes", "[integration][mbt]") {
  auto transport = spawn_trace_mirror("fault_close");
  REQUIRE(transport != nullptr);

  ApalacheSpec spec;
  spec.sources.push_back("module HourClock");
  RegisterExplore msg;
  msg.spec = std::move(spec);
  msg.invariants = {"TraceComplete"};
  msg.exports = {"Actions"};

  auto sent = transport->send_line(encode_client_message(msg));
  REQUIRE(sent.has_value());

  auto closed = transport->close();
  REQUIRE(closed.has_value());
  REQUIRE(closed.value() == 0);  // fake mirror detected the close cleanly
}
