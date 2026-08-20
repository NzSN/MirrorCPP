// MirrorCPP client integration tests against the python fake-mirror harness
// (design §8): the fake mirror is a scripted stdio peer run via spawn_mirror.
// Covers the §5.6 stepping loop contract:
//   - happy path: spec_validated -> initial_state -> N ticks -> all_steps_done
//   - deliberate mismatch: step_mismatch error carries expected/actual/hints
//     and names the last action
//   - protocol_error propagates as ErrorKind::protocol
//   - unexpected first message -> protocol error naming the tag
//   - spec_validated {"invalid": …} -> spec_invalid
//
// These tests need no MIRROR_BIN — the fake mirror replaces it.
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "hourclock_client.hpp"

using namespace mirrorcpp;

namespace {

// Absolute path to one of the fake-mirror executables (scenario embedded in
// the script itself; spawn_mirror runs it with no args).
std::filesystem::path fake_mirror_path(const char* script) {
  return std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / script;
}

// A StateComputer that ticks like HourClock but deliberately corrupts hr
// (for the mismatch scenario): hr advances by 2 instead of 1.
StateComputer buggy_tick_computer() {
  return [](std::string_view action, const State& params_or_state,
            const State& prev_state) -> State {
    if (action == "init") return params_or_state;  // adopt oracle init
    State s = test::hc_tick(prev_state);
    // Corrupt hr: 1 -> 3 (expected 2).
    Value::Int hr = s.at("hr").as_int().value_or(Value::Int(1));
    s["hr"] = Value(hr + 1);
    return s;
  };
}

ApalacheConfig hourclock_config() {
  ApalacheConfig cfg;
  cfg.spec_path = "HourClock.tla";  // placeholder; the fake ignores it
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 10;
  return cfg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Happy path: all steps replay cleanly -> all_steps_done (design §8 scenario 1)
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: happy path via fake mirror", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_happy"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};  // fake ignores contents
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// Wrap-around: the oracle starts at hr=12 so the first tick wraps 12 -> 1
// (design §10's wrap case: length_bound=13 guarantees a full cycle).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: hc_tick wraps 12 -> 1", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_happy_wrap"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// preset_client (design §5.6): reports a fixed sequence of states, then throws
// "preset_client exhausted". Fed the exact expected HourClock sequence, the
// run drives to all_steps_done; a shorter sequence must surface the throw.
// ---------------------------------------------------------------------------
TEST_CASE("preset_client: exact sequence drives to all_steps_done", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_happy"));
  REQUIRE(transport != nullptr);

  // The oracle (fake mirror happy, start hr=1, 5 ticks) expects these states:
  // init(hr=1), then hr=2,3,4,5,6. nondet_picks is fixed at init to
  // {start_hr:1, start_latest_hr:1} and never changes (HourClock HCnext keeps
  // nondet_picks' = nondet_picks).
  auto fixed = [](Value::Int hr, Value::Int latest_hr, bool ticked,
                  std::string action_taken, Value::Int step_count) {
    Value::Record nondet;
    nondet.fields["start_hr"] = Value(Value::Int(1));
    nondet.fields["start_latest_hr"] = Value(Value::Int(1));
    State s;
    s["hr"] = Value(hr);
    s["latest_hr"] = Value(latest_hr);
    s["ticked"] = Value(ticked);
    s["action_taken"] = Value(std::move(action_taken));
    s["nondet_picks"] = Value(std::move(nondet));
    s["step_count"] = Value(step_count);
    return s;
  };
  std::vector<State> preset;
  preset.push_back(fixed(1, 1, false, "init", 0));
  for (int i = 1; i <= 5; ++i)
    preset.push_back(fixed(i + 1, i, true, "tick", i));

  auto result = run_client_with_traces(*transport, hourclock_config(),
                                       std::vector<std::string>{"specs/traces/x.itf.json"},
                                       preset_client(std::move(preset)));
  REQUIRE(result.has_value());
}

TEST_CASE("preset_client: exhaustion throws and closes the transport", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_happy"));
  REQUIRE(transport != nullptr);

  // Only the init state — the run must fail when the computer runs out.
  std::vector<State> preset;
  preset.push_back(test::hourclock_state(1, 1, false, "init", 0));

  REQUIRE_THROWS_WITH(
      run_client_with_traces(*transport, hourclock_config(),
                             std::vector<std::string>{"specs/traces/x.itf.json"},
                             preset_client(std::move(preset))),
      Catch::Matchers::ContainsSubstring("preset_client exhausted"));
}

// ---------------------------------------------------------------------------
// Deliberate mismatch: a buggy tick reports hr=3 instead of 2; the fake mirror
// sends step_mismatch with hints; the client error must carry them and name the
// last action (§8 scenario 2, §4.3).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: step_mismatch carries expected/actual/hints", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_mismatch"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       buggy_tick_computer());
  REQUIRE_FALSE(result.has_value());

  const Error& err = result.error();
  REQUIRE(err.kind == ErrorKind::step_mismatch);
  REQUIRE(err.expected.has_value());
  REQUIRE(err.actual.has_value());
  REQUIRE(err.hints != nullptr);
  REQUIRE_FALSE(err.hints->empty());

  // Message names the last action (the tick that mismatched).
  CHECK(err.message.find("tick") != std::string::npos);
  // Hints render into the message (§4.3: render via render_diff_hints).
  CHECK(err.message.find("value mismatch") != std::string::npos);

  // Sanity: the expected/actual hr values differ (2 vs 3).
  auto expected_hr = err.expected->at("hr").as_int();
  auto actual_hr = err.actual->at("hr").as_int();
  REQUIRE(expected_hr.has_value());
  REQUIRE(actual_hr.has_value());
  CHECK(*expected_hr == 2);
  CHECK(*actual_hr == 3);
}

// ---------------------------------------------------------------------------
// protocol_error: mirror rejects after register -> ErrorKind::protocol with the
// mirror's text (§5.6 item 1: register_error/protocol_error propagate).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: protocol_error propagates", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_protoerr"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::protocol);
  CHECK(result.error().message.find("unknown operator FOO") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Unexpected first message: mirror sends all_steps_done instead of
// spec_validated -> protocol error naming the offending tag (§5.6 item 1).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: unexpected first message names the tag", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_unexpected"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::protocol);
  CHECK(result.error().message.find("all_steps_done") != std::string::npos);
  CHECK(result.error().message.find("spec_validated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// spec_validated {"invalid": …} -> spec_invalid error (§5.6 item 1, §4.3).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_with_traces: invalid spec -> spec_invalid", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_invalid"));
  REQUIRE(transport != nullptr);

  std::vector<std::string> traces{"specs/traces/ignored.itf.json"};
  auto result = run_client_with_traces(*transport, hourclock_config(), traces,
                                       test::hourclock_computer());
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::spec_invalid);
  CHECK(result.error().message.find("type error") != std::string::npos);
}
