#include <catch2/catch_test_macros.hpp>
#include <mirrorcpp/schedule_exploration.hpp>
#include "scheduled_counter.hpp"
#include <atomic>
#include <set>
#include <thread>
using namespace std::chrono_literals;
namespace sc = mirrorcpp::schedule;
using nlohmann::json;
namespace {
sc::FiniteSpace space() {
  sc::FiniteSpace value;
  value.identity = scheduled_counter::identity();
  value.actors = {{{"a", "increment-a"}, {"read", "write", "$done"}},
                  {{"b", "increment-b"}, {"read", "write", "$done"}}};
  value.inputs = {json{{"initial", 0}}, json{{"initial", 5}}};
  value.state_variables = {"value"};
  return value;
}
std::set<std::string> independently_enumerated(std::size_t bound) {
  std::set<std::string> values;
  for (unsigned mask = 0; mask < 64; ++mask) {
    unsigned ones = 0;
    for (unsigned i = 0; i < 6; ++i) ones += (mask >> i) & 1;
    if (ones != 3) continue;
    int counts[2] = {0, 0}, prior = -1; std::size_t preemptions = 0;
    std::string order;
    for (unsigned i = 0; i < 6; ++i) {
      const int actor = (mask >> i) & 1;
      if (prior >= 0 && prior != actor && counts[prior] < 3) ++preemptions;
      ++counts[actor]; prior = actor; order.push_back(actor ? 'b' : 'a');
    }
    if (preemptions <= bound) values.insert(order);
  }
  return values;
}
}
TEST_CASE("finite exploration exhausts the independent two-actor denominator", "[schedule-exploration]") {
  for (const std::size_t bound : {0U, 1U, 2U, 3U, 64U}) {
    auto declaration = space(); declaration.max_preemptions = bound;
    const auto result = sc::explore_finite(declaration, sc::local_exploration_runner(scheduled_counter::adapter()));
    REQUIRE(result["complete"] == true);
    const auto expected = independently_enumerated(bound);
    REQUIRE(result["eligibleRuns"] == expected.size() * 2);
    REQUIRE(result["attemptedRuns"] == expected.size() * 2);
    REQUIRE(result["completedRuns"] == expected.size() * 2);
    REQUIRE(result["categories"]["passed"] == expected.size() * 2);
    std::set<std::string> actual;
    for (const auto& row : result["runs"]) {
      std::string order;
      for (const auto& step : row["schedule"]["steps"]) order += step["actor"].get<std::string>();
      actual.insert(order);
    }
    REQUIRE(actual == expected);
  }
  REQUIRE(independently_enumerated(64).size() == 20);
  REQUIRE(independently_enumerated(0).size() == 2);
}
TEST_CASE("finite exploration never credits a bounded prefix as complete", "[schedule-exploration]") {
  auto runner = sc::local_exploration_runner(scheduled_counter::adapter());
  sc::ExplorationLimits limits; limits.max_runs = 3;
  auto result = sc::explore_finite(space(), runner, limits);
  REQUIRE(result["complete"] == false);
  REQUIRE(result["denominatorKnown"] == true);
  REQUIRE(result["eligibleRuns"] == 40);
  REQUIRE(result["attemptedRuns"] == 3);
  REQUIRE(result["stopReason"] == "run_limit");
  limits = {}; limits.max_enumerated_schedules = 3;
  result = sc::explore_finite(space(), runner, limits);
  REQUIRE(result["complete"] == false);
  REQUIRE(result["denominatorKnown"] == false);
  REQUIRE(result["eligibleRuns"].is_null());
  REQUIRE(result["stopReason"] == "enumeration_limit");
  limits = {}; limits.max_evidence_bytes = 1;
  result = sc::explore_finite(space(), runner, limits);
  REQUIRE(result["complete"] == false);
  REQUIRE(result["stopReason"] == "evidence_limit");
}
TEST_CASE("invalid exploration declarations acquire no application", "[schedule-exploration]") {
  int calls = 0;
  sc::ExplorationRunner runner = [&](const sc::Schedule&, std::stop_token) { ++calls; return sc::ExplorationSample{}; };
  for (int scenario = 0; scenario < 4; ++scenario) {
    auto declaration = space();
    if (scenario == 0) declaration.actors.push_back(declaration.actors.front());
    if (scenario == 1) declaration.inputs.push_back(declaration.inputs.front());
    if (scenario == 2) declaration.actors.front().checkpoints = {"$done", "read", "$done"};
    if (scenario == 3) declaration.instrumentation_variables = {"value"};
    REQUIRE(sc::explore_finite(declaration, runner)["status"] == "invalid_declaration");
  }
  REQUIRE(calls == 0);
}
TEST_CASE("canonical coverage preserves semantic map and set equality", "[schedule-exploration]") {
  auto key = [](const json& value) { return sc::canonical_state(mirrorcpp::decode_state(json{{"actual", value}})); };
  REQUIRE(key(json{{"#set", json::array({1, 2, 1})}}) == key(json{{"#set", json::array({2, 1})}}));
  REQUIRE(key(json{{"#map", json::array({json::array({"b", 2}), json::array({"a", 1})})}}) ==
          key(json{{"#map", json::array({json::array({"a", 1}), json::array({"b", 2})})}}));
  REQUIRE(key(json::array({1, 2})) != key(json::array({2, 1})));
  REQUIRE(key(json{{"#tup", json::array({1, 2})}}) != key(json::array({1, 2})));
  REQUIRE(key(json{{"#set", json::array({1})}, {"ordinary", true}}) != key(json{{"#set", json::array({1})}}));
  REQUIRE(key(json{{"#map", json::array({json::array({1, true})})}}) !=
          key(json{{"#map", json::array({json::array({"1", true})})}}));
  REQUIRE_THROWS_AS(key(json{{"#map", json::array({json::array({"a", 1}), json::array({"a", 2})})}}), std::invalid_argument);
  REQUIRE_THROWS_AS(key(json{{"#map", json::array({json::array({"a", 1}), json::array({2, 2})})}}), std::invalid_argument);
}
TEST_CASE("instrumentation exclusion is explicit and cannot hide undeclared fields", "[schedule-exploration]") {
  auto declaration = space(); declaration.instrumentation_variables = {"instrumentation"};
  auto adapter = scheduled_counter::adapter(); auto factory = adapter.factory;
  adapter.factory = [factory](const json& inputs) {
    auto program = factory(inputs); auto observe = program.observe;
    program.observe = [observe, count=0]() mutable { auto state = observe(); state["instrumentation"] = ++count; return state; };
    return program;
  };
  auto runner = sc::local_exploration_runner(adapter);
  const auto included = sc::explore_finite(declaration, runner);
  REQUIRE(included["complete"] == true);
  const auto normal = sc::explore_finite(space(), sc::local_exploration_runner(scheduled_counter::adapter()));
  REQUIRE(included["states"] == normal["states"]);
  const auto undeclared = sc::explore_finite(space(), runner);
  REQUIRE(undeclared["complete"] == false);
  REQUIRE(undeclared["categories"]["runner_or_evidence_failed"] == 40);
}
TEST_CASE("cancellation and time budget stop finite exploration honestly", "[schedule-exploration]") {
  auto runner = sc::local_exploration_runner(scheduled_counter::adapter());
  std::stop_source stop; stop.request_stop();
  auto result = sc::explore_finite(space(), runner, {}, stop.get_token());
  REQUIRE(result["complete"] == false);
  REQUIRE(result["attemptedRuns"] == 0);
  REQUIRE(result["stopReason"] == "cancelled");
  sc::ExplorationLimits limits; limits.time_budget = 1ms;
  result = sc::explore_finite(space(), [&](const auto& plan, std::stop_token token) {
    auto sample = runner(plan, token); std::this_thread::sleep_for(3ms); return sample;
  }, limits);
  REQUIRE(result["complete"] == false);
  REQUIRE(result["stopReason"] == "time_budget");
}
TEST_CASE("unknown cleanup and reused execution evidence cannot qualify exploration", "[schedule-exploration]") {
  auto runner = sc::local_exploration_runner(scheduled_counter::adapter());
  auto result = sc::explore_finite(space(), [&](const auto& plan, std::stop_token token) {
    auto sample = runner(plan, token); sample.execution["cleanup"] = "incomplete"; return sample;
  });
  REQUIRE(result["complete"] == false);
  REQUIRE(result["attemptedRuns"] == 1);
  REQUIRE(result["stopReason"] == "unconfirmed_cleanup");
  result = sc::explore_finite(space(), [&](const auto& plan, std::stop_token token) {
    auto sample = runner(plan, token); sample.execution["executionId"] = std::string(32, 'a'); return sample;
  });
  REQUIRE(result["complete"] == false);
  REQUIRE(result["attemptedRuns"] == 2);
  REQUIRE(result["stopReason"] == "unconfirmed_cleanup");
}
TEST_CASE("required model comparison cannot be satisfied by scheduling alone", "[schedule-exploration]") {
  auto declaration = space(); declaration.require_model_comparison = true;
  auto result = sc::explore_finite(declaration, sc::local_exploration_runner(scheduled_counter::adapter()));
  REQUIRE(result["complete"] == false);
  REQUIRE(result["enumerationExhausted"] == true);
  REQUIRE(result["completedRuns"] == 0);
  REQUIRE(result["comparisonRuns"] == 0);
  REQUIRE(result["categories"]["comparison_missing"] == 40);
}
