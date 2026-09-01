// MirrorCPP ExploreSession + run_client_explore integration tests against the
// python fake-mirror harness (design §8, t10).
//
// Covers the §3.3 / §5.6 explorer-session contract:
//   - ExploreSession::open -> explorer_ready (init/next/invariant counts)
//   - strict alternation: assume_transition -> next_step -> query_state ->
//     check_invariant -> rollback -> done
//   - a protocol_error reply poisons the session; the next command is rejected
//   - run_client_explore: register_explore stepping happy path, where
//     next_step.parameters carries the FULL expected state (§3.2)
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

ApalacheSpec hourclock_spec() {
  ApalacheSpec spec;
  spec.sources = {"---- MODULE HourClock ----"};
  return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// Scripted explorer session: open + every command, ending with done.
// ---------------------------------------------------------------------------
TEST_CASE("ExploreSession: full scripted session via fake mirror",
          "[integration][explore]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_explore_session"));
  REQUIRE(transport != nullptr);

  auto session = ExploreSession::open(std::move(transport), hourclock_spec(),
                                      {"Inv"}, {"Actions"});
  REQUIRE(session.has_value());
  CHECK(session->ready().init_transitions == 2);
  CHECK(session->ready().next_transitions == 1);
  CHECK(session->ready().state_invariants == 1);

  auto st = session->assume_transition(1);
  REQUIRE(st.has_value());
  CHECK(*st == TransitionStatus::enabled);

  auto step = session->next_step();
  REQUIRE(step.has_value());
  CHECK(*step == 3);

  auto state = session->query_state();
  REQUIRE(state.has_value());
  CHECK(state->contains("hr"));

  auto inv = session->check_invariant(0);
  REQUIRE(inv.has_value());
  CHECK(*inv == InvariantStatus::satisfied);

  auto rb = session->rollback(7);
  REQUIRE(rb.has_value());
  CHECK(*rb == 7);

  auto done = session->done();
  REQUIRE(done.has_value());
}

// ---------------------------------------------------------------------------
// assume_state: ExploreAssumeState -> ExploreAssumeStatus.
// ---------------------------------------------------------------------------
TEST_CASE("ExploreSession: assume_state returns transition status",
          "[integration][explore]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_explore_session"));
  REQUIRE(transport != nullptr);

  auto session = ExploreSession::open(std::move(transport), hourclock_spec(),
                                      {}, {});
  REQUIRE(session.has_value());

  State s;
  s["hr"] = Value(Value::Int(1));
  auto st = session->assume_state(s);
  REQUIRE(st.has_value());
  CHECK(*st == TransitionStatus::enabled);

  auto done = session->done();
  REQUIRE(done.has_value());
}

// ---------------------------------------------------------------------------
// Fatal protocol_error: the connection is unusable for this flow (guide §9).
// ---------------------------------------------------------------------------
TEST_CASE("ExploreSession: protocol_error poisons the session", "[integration][explore]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_explore_bad_command"));
  REQUIRE(transport != nullptr);

  auto session = ExploreSession::open(std::move(transport), hourclock_spec(),
                                      {}, {});
  REQUIRE(session.has_value());

  // Bad command -> protocol_error surfaced as a per-call error.
  auto bad = session->assume_transition(99);
  REQUIRE_FALSE(bad.has_value());
  CHECK(bad.error().kind == ErrorKind::protocol);

  // The next command is rejected locally; no further flow bytes are sent.
  auto good = session->assume_transition(1);
  REQUIRE_FALSE(good.has_value());
  CHECK(good.error().kind == ErrorKind::io);
}

// ---------------------------------------------------------------------------
// register_explore stepping happy path (full expected state in parameters).
// ---------------------------------------------------------------------------
TEST_CASE("run_client_explore: stepping happy path via fake mirror",
          "[integration][explore]") {
  auto transport = spawn_mirror(fake_mirror_path("fake_mirror_explore_stepping"));
  REQUIRE(transport != nullptr);

  auto result = run_client_explore(*transport, hourclock_spec(),
                                   {"TraceComplete"}, {"Actions"}, /*max_steps=*/10,
                                   test::hourclock_computer());
  REQUIRE(result.has_value());
}

// ---------------------------------------------------------------------------
// ExploreSession destructor sends best-effort done() when not explicitly
// called (no crash, no hang).
// ---------------------------------------------------------------------------
TEST_CASE("ExploreSession: destructor best-effort done", "[integration][explore]") {
  {
    auto transport = spawn_mirror(fake_mirror_path("fake_mirror_explore_session"));
    REQUIRE(transport != nullptr);
    auto session = ExploreSession::open(std::move(transport), hourclock_spec(),
                                        {}, {});
    REQUIRE(session.has_value());
    // session goes out of scope without done() -> destructor calls it.
  }
}
