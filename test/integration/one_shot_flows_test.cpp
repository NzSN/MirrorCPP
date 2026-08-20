// MirrorCPP one-shot flow integration tests against the python fake-mirror
// harness (design §8, t6): register, register_validate, register_trace_gen.
// Covers the §5.6 one-shot contract:
//   - run_client: register flow drives the same stepping loop as register_traces
//   - run_client with inline spec: spec sent AND apalacheConfig.specPath present
//   - run_client_validate: valid / invalid verdicts (one reply, session ends)
//   - run_client_validate bound=101: mirror register_error (passthrough, no
//     client-side pre-validation)
//   - run_client_gen_traces: gen_traces_done with inline itfTraces contents
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

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

ApalacheConfig hourclock_config() {
  ApalacheConfig cfg;
  cfg.spec_path = "HourClock.tla";  // placeholder; the fake ignores it
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 10;
  return cfg;
}

TraceGenerationConfig trace_config() {
  TraceGenerationConfig tc;
  tc.num_traces = 2;
  return tc;
}

}  // namespace

// ---------------------------------------------------------------------------
// register flow: mirror generates + replays traces (same stepping loop).
// ---------------------------------------------------------------------------
TEST_CASE("run_client: register happy path via fake mirror", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_register_happy"));
  REQUIRE(transport != nullptr);

  auto result = run_client(*transport, hourclock_config(), trace_config(),
                           test::hourclock_computer());
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// register with inline spec: the client must send spec (inline sources) AND
// keep apalacheConfig.specPath on the wire (§5.2 / §3.2). The fake asserts both
// and replies protocol_error if either is missing.
// ---------------------------------------------------------------------------
TEST_CASE("run_client: inline spec sent with specPath retained", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_register_inline"));
  REQUIRE(transport != nullptr);

  ApalacheSpec inline_spec;
  inline_spec.sources = {"---- MODULE HourClock ----\n"};

  auto result = run_client(*transport, hourclock_config(), trace_config(),
                           test::hourclock_computer(), std::move(inline_spec));
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// register_validate: valid verdict.
// ---------------------------------------------------------------------------
TEST_CASE("run_client_validate: valid verdict", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_validate_valid"));
  REQUIRE(transport != nullptr);

  auto verdict = run_client_validate(*transport, hourclock_config(), /*bound=*/10);
  REQUIRE(verdict.has_value());
  CHECK(verdict->valid);
  CHECK(verdict->detail.empty());
}

// ---------------------------------------------------------------------------
// register_validate: invalid verdict surfaces the mirror's detail text.
// ---------------------------------------------------------------------------
TEST_CASE("run_client_validate: invalid verdict carries detail", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_validate_invalid"));
  REQUIRE(transport != nullptr);

  auto verdict = run_client_validate(*transport, hourclock_config(), /*bound=*/10);
  REQUIRE(verdict.has_value());
  CHECK_FALSE(verdict->valid);
  CHECK(verdict->detail.find("state invariant violated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// register_validate bound=101: the mirror caps at 100 and replies register_error
// (design §8: "Validate bound cap 100 → register_error"; passthrough, do not
// pre-validate client-side).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_validate: bound=101 -> registration error", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_validate_bound101"));
  REQUIRE(transport != nullptr);

  auto verdict = run_client_validate(*transport, hourclock_config(), /*bound=*/101);
  REQUIRE_FALSE(verdict.has_value());
  REQUIRE(verdict.error().kind == ErrorKind::registration);
  CHECK(verdict.error().message.find("bound") != std::string::npos);
}

// ---------------------------------------------------------------------------
// register_trace_gen: gen_traces_done carries paths + inline ITF contents
// (§5.6 / §3.2; itfTraces may be empty on legacy mirrors).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_gen_traces: paths and inline traces", "[integration][client]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_gen_traces"));
  REQUIRE(transport != nullptr);

  ApalacheSpec inline_spec;
  inline_spec.sources = {"---- MODULE HourClock ----\n"};

  auto result = run_client_gen_traces(*transport, hourclock_config(), trace_config(),
                                      /*dest_path=*/std::nullopt, std::move(inline_spec));
  REQUIRE(result.has_value());
  REQUIRE(result->itf_trace_paths.size() == 2);
  CHECK(result->itf_trace_paths[0].find("fake_trace1") != std::string::npos);
  REQUIRE(result->itf_traces.size() == 2);
  CHECK(result->itf_traces[0].is_object());
}
