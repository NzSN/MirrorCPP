#include <catch2/catch_test_macros.hpp>
#include <mirrorcpp/schedule.hpp>
#include "scheduled_counter.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <limits>
#include <thread>

using namespace std::chrono_literals;
namespace sc = mirrorcpp::schedule;
using nlohmann::json;

namespace {
sc::Schedule one_plan() {
  sc::Schedule plan;
  plan.identity = scheduled_counter::identity();
  plan.steps = {{"a", "point"}, {"a", "$done"}};
  return plan;
}
sc::Adapter one_adapter(std::function<void(sc::Checkpoint&)> worker = [](auto& hook) { hook.arrive("point"); }) {
  return {scheduled_counter::identity(), {{"a", "operation-1"}}, {"point", "other"},
    [worker](const json&) { return sc::Program{{{"a", worker}}, [] { return json{{"actual", true}}; }, [] {}}; }};
}
json replay_projection(const sc::Execution& execution) {
  auto receipt = execution.receipt();
  receipt.erase("generation");
  receipt.erase("executionId");
  return receipt;
}
struct ReleaseOnExit {
  std::shared_ptr<std::atomic<bool>> released;
  ~ReleaseOnExit() { released->store(true); }
};
}

TEST_CASE("scheduled workers retain real stacks and replay both interleavings", "[schedule]") {
  for (const bool overlap : {false, true}) {
    const auto plan = sc::parse_schedule(sc::encode_schedule(scheduled_counter::plan(overlap)));
    json first;
    std::uint64_t previous_generation = 0;
    for (int i = 0; i < 40; ++i) {
      auto run = sc::run_schedule(plan, scheduled_counter::adapter());
      REQUIRE(run->report().passed());
      REQUIRE(run->report().remaining_actors.empty());
      REQUIRE(run->report().generation != previous_generation);
      previous_generation = run->report().generation;
      const auto receipt = replay_projection(*run);
      REQUIRE(receipt["observations"].back()["state"]["value"] == (overlap ? 1 : 2));
      REQUIRE(receipt["events"].size() == 12);
      if (i == 0) first = receipt;
      else REQUIRE(receipt == first);
    }
  }
}

TEST_CASE("scheduler observation records actual behavior under a fixture mutation", "[schedule]") {
  const auto plan = scheduled_counter::plan(true);
  auto correct = sc::run_schedule(plan, scheduled_counter::adapter());
  auto mutant = sc::run_schedule(plan, scheduled_counter::adapter(true));
  REQUIRE(correct->report().passed());
  REQUIRE(mutant->report().passed()); // scheduling success is not oracle conformance
  REQUIRE(correct->report().observations != mutant->report().observations);
  REQUIRE(mutant->report().observations.back()["state"]["value"] == 2);
}

TEST_CASE("invalid schedules and identity changes reject before factory", "[schedule]") {
  for (int scenario = 0; scenario < 9; ++scenario) {
    auto plan = one_plan();
    auto adapter = one_adapter();
    sc::Policy policy;
    int constructed = 0;
    adapter.factory = [&](const json&) { ++constructed; return sc::Program{}; };
    switch (scenario) {
      case 0: plan.scheduling_profile = "unknown"; break;
      case 1: plan.identity.mapping_sha256 = std::string(64, '9'); break;
      case 2: adapter.actors.push_back(adapter.actors.front()); break;
      case 3: plan.steps.front().actor = "unknown"; break;
      case 4: plan.steps.pop_back(); break;
      case 5: plan.steps.push_back({"a", "point"}); break;
      case 6: plan.steps.front().checkpoint = "undeclared"; break;
      case 7: policy.max_steps = 1; break;
      case 8: policy.execution_timeout = 0ms; break;
    }
    CAPTURE(scenario);
    auto run = sc::run_schedule(plan, adapter, policy);
    REQUIRE_FALSE(run->report().passed());
    REQUIRE(constructed == 0);
    REQUIRE(run->report().events.empty());
    REQUIRE(run->report().cleanup == sc::Cleanup::not_started);
  }
}

TEST_CASE("schedule JSON rejects duplicate unknown malformed and excessive fields", "[schedule]") {
  const auto good = sc::encode_schedule(one_plan());
  REQUIRE(sc::encode_schedule(sc::parse_schedule(good)) == good);
  auto value = json::parse(good);
  value["extra"] = 1;
  REQUIRE_THROWS_AS(sc::parse_schedule(value.dump()), std::invalid_argument);
  REQUIRE_THROWS_AS(sc::parse_schedule("{\"schema\":0," + good.substr(1)), std::invalid_argument);
  value = json::parse(good);
  value["steps"][0]["actor"] = 1;
  REQUIRE_THROWS_AS(sc::parse_schedule(value.dump()), std::invalid_argument);
  value = json::parse(good);
  value["identity"]["mappingSha256"] = "bad";
  REQUIRE_THROWS_AS(sc::parse_schedule(value.dump()), std::invalid_argument);
  REQUIRE_THROWS_AS(sc::parse_schedule(std::string(1'048'577, ' ')), std::invalid_argument);
  value = json::parse(good);
  auto nested = json::array();
  for (int i = 0; i < 40; ++i) nested = json::array({nested});
  value["inputs"] = nested;
  REQUIRE_THROWS_AS(sc::parse_schedule(value.dump()), std::invalid_argument);
}

TEST_CASE("unexpected and premature arrivals preserve actual checkpoint evidence", "[schedule]") {
  for (const std::string checkpoint : {"other", "undeclared", ""}) {
    auto run = sc::run_schedule(one_plan(), one_adapter([checkpoint](auto& hook) {
      if (!checkpoint.empty()) hook.arrive(checkpoint);
    }));
    REQUIRE(run->report().outcome == sc::Outcome::unexpected_checkpoint);
    REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
    REQUIRE(run->report().events.back()["checkpoint"] == (checkpoint.empty() ? "$done" : checkpoint));
  }
}

TEST_CASE("foreign hook callers fail without corrupting actor ownership", "[schedule]") {
  auto run = sc::run_schedule(one_plan(), one_adapter([](auto& hook) {
    std::thread foreign([&] { try { hook.arrive("point"); } catch (...) {} });
    foreign.join();
  }));
  REQUIRE(run->report().outcome == sc::Outcome::uncontrolled_actor);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
}

TEST_CASE("actor and observer failures remain distinct from teardown failures", "[schedule]") {
  auto adapter = one_adapter([](auto&) { throw std::runtime_error("actor broke"); });
  auto factory = adapter.factory;
  adapter.factory = [factory](const json& inputs) {
    auto program = factory(inputs);
    program.teardown = [] { throw std::runtime_error("teardown broke"); };
    return program;
  };
  auto run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().outcome == sc::Outcome::application_failed);
  REQUIRE(run->report().detail == "actor broke");
  REQUIRE(run->report().cleanup == sc::Cleanup::teardown_failed);
  REQUIRE(run->report().cleanup_attempts.back()["detail"] == "teardown broke");
  adapter = one_adapter();
  factory = adapter.factory;
  adapter.factory = [factory](const json& inputs) {
    auto program = factory(inputs);
    program.observe = []() -> json { throw std::runtime_error("observer broke"); };
    return program;
  };
  run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().outcome == sc::Outcome::observation_failed);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
}

TEST_CASE("factory worker mismatch unwinds the constructed application once", "[schedule]") {
  int teardown = 0;
  auto adapter = one_adapter();
  adapter.factory = [&](const json&) {
    return sc::Program{{{"different", [](auto&) {}}}, [] { return json{}; }, [&] { ++teardown; }};
  };
  auto run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().outcome == sc::Outcome::application_failed);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
  REQUIRE(teardown == 1);
  run.reset();
  REQUIRE(teardown == 1);
}

TEST_CASE("cancellation before factory and at every observation releases owned waits", "[schedule]") {
  for (int phase = -1; phase <= 2; ++phase) {
    std::stop_source stop;
    int constructed = 0;
    auto adapter = one_adapter();
    auto factory = adapter.factory;
    adapter.factory = [&](const json& inputs) {
      ++constructed;
      auto program = factory(inputs);
      program.observe = [&, observed = 0]() mutable {
        if (observed++ == phase) stop.request_stop();
        return json{{"observed", observed}};
      };
      return program;
    };
    if (phase == -1) stop.request_stop();
    auto run = sc::run_schedule(one_plan(), adapter, {}, stop.get_token());
    REQUIRE(run->report().outcome == sc::Outcome::cancelled);
    REQUIRE_FALSE(run->report().passed());
    REQUIRE(constructed == (phase == -1 ? 0 : 1));
    REQUIRE(run->report().remaining_actors.empty());
  }
}

TEST_CASE("execution deadline cancels a cooperative actor without inventing an arrival", "[schedule]") {
  sc::Policy policy;
  policy.execution_timeout = 150ms;
  auto run = sc::run_schedule(one_plan(), one_adapter([](auto& hook) {
    while (!hook.stop_requested()) std::this_thread::yield();
  }), policy);
  REQUIRE(run->report().outcome == sc::Outcome::timed_out);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
  REQUIRE(run->report().events.size() == 1);
  REQUIRE(run->report().events.front()["kind"] == "permit");
}

TEST_CASE("incomplete cleanup retains thread ownership and permits an honest retry", "[schedule]") {
  auto released = std::make_shared<std::atomic<bool>>(false);
  sc::Policy policy;
  policy.execution_timeout = 150ms;
  policy.cleanup_timeout = 10ms;
  auto run = sc::run_schedule(one_plan(), one_adapter([released](auto&) {
    while (!released->load()) std::this_thread::yield();
  }), policy);
  ReleaseOnExit ensure_release{released}; // must precede run destruction on assertion failure
  REQUIRE(run->report().outcome == sc::Outcome::timed_out);
  REQUIRE(run->report().cleanup == sc::Cleanup::incomplete);
  REQUIRE(run->report().remaining_actors == std::vector<std::string>{"a"});
  released->store(true);
  const auto result = run->cleanup(1000ms);
  REQUIRE(result.cleanup == sc::Cleanup::confirmed);
  REQUIRE(result.remaining_actors.empty());
  REQUIRE(result.outcome == sc::Outcome::timed_out);
  REQUIRE(result.cleanup_attempts.size() == 2);
  REQUIRE(result.cleanup_attempts.front()["status"] == "incomplete");
  REQUIRE_FALSE(result.passed());
}

TEST_CASE("nested execution is rejected before constructing another application", "[schedule]") {
  bool nested_rejected = false;
  auto adapter = one_adapter([&](auto& hook) {
    auto nested = sc::run_schedule(one_plan(), one_adapter());
    nested_rejected = nested->report().outcome == sc::Outcome::invalid_schedule;
    hook.arrive("point");
  });
  auto run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().passed());
  REQUIRE(nested_rejected);
}

TEST_CASE("observer hook reentry fails instead of blocking the coordinator", "[schedule]") {
  sc::Checkpoint* current = nullptr;
  auto adapter = one_adapter([&](auto& hook) { current = &hook; hook.arrive("point"); });
  auto factory = adapter.factory;
  adapter.factory = [&](const json& inputs) {
    auto program = factory(inputs);
    program.observe = [&]() -> json { if (current) current->arrive("point"); return json{}; };
    return program;
  };
  auto run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().outcome == sc::Outcome::observation_failed);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
}

TEST_CASE("cancellation during construction and running actors preserves cleanup", "[schedule]") {
  for (bool during_factory : {false, true}) {
    std::stop_source stop;
    int entered = 0, disposed = 0;
    auto adapter = one_adapter([&](auto& hook) {
      ++entered;
      stop.request_stop();
      hook.arrive("point");
    });
    auto factory = adapter.factory;
    adapter.factory = [&](const json& inputs) {
      auto program = factory(inputs);
      program.teardown = [&] { ++disposed; };
      if (during_factory) stop.request_stop();
      return program;
    };
    auto run = sc::run_schedule(one_plan(), adapter, {}, stop.get_token());
    REQUIRE(run->report().outcome == sc::Outcome::cancelled);
    REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
    REQUIRE(entered == (during_factory ? 0 : 1));
    REQUIRE(disposed == 1);
  }
}

TEST_CASE("observation size and teardown failure cannot produce a passing receipt", "[schedule]") {
  for (bool oversized : {false, true}) {
    auto adapter = one_adapter();
    auto factory = adapter.factory;
    adapter.factory = [=](const json& inputs) {
      auto program = factory(inputs);
      if (oversized) program.observe = [] { return json(std::string(70'000, 'x')); };
      else program.teardown = [] { throw std::runtime_error("teardown failed"); };
      return program;
    };
    auto run = sc::run_schedule(one_plan(), adapter);
    REQUIRE_FALSE(run->report().passed());
    REQUIRE(run->receipt()["passed"] == false);
    if (oversized) REQUIRE(run->report().outcome == sc::Outcome::observation_failed);
    else {
      REQUIRE(run->report().outcome == sc::Outcome::completed);
      REQUIRE(run->report().schedule_completed);
      REQUIRE(run->report().cleanup == sc::Cleanup::teardown_failed);
    }
  }
}

TEST_CASE("timeout releases a parked lock owner before joining a blocked actor", "[schedule]") {
  auto mutex = std::make_shared<std::mutex>();
  sc::Schedule plan;
  plan.identity = scheduled_counter::identity();
  plan.steps = {{"holder", "holding"}, {"waiter", "acquired"},
                {"holder", "$done"}, {"waiter", "$done"}};
  sc::Adapter adapter{plan.identity, {{"holder", "lock-holder"}, {"waiter", "lock-waiter"}},
      {"holding", "acquired"}, [mutex](const json&) {
        return sc::Program{{
          {"holder", [mutex](auto& hook) { std::lock_guard lock(*mutex); hook.arrive("holding"); }},
          {"waiter", [mutex](auto& hook) { std::lock_guard lock(*mutex); hook.arrive("acquired"); }}},
          [] { return json{{"observation", "does-not-lock-application-mutex"}}; }, [] {}};
      }};
  sc::Policy policy;
  policy.execution_timeout = 150ms;
  auto run = sc::run_schedule(plan, adapter, policy);
  REQUIRE(run->report().outcome == sc::Outcome::timed_out);
  REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
  REQUIRE(run->report().remaining_actors.empty());
  REQUIRE(run->report().observations.size() == 2);
}

TEST_CASE("non-JSON inputs reject before factory and observations fail without lossy serialization", "[schedule]") {
  for (const auto& value : std::vector<json>{json::binary({1, 2}),
                                           json(std::numeric_limits<double>::quiet_NaN())}) {
    auto plan = one_plan();
    plan.inputs = {{"nested", value}};
    int constructed = 0;
    auto adapter = one_adapter();
    adapter.factory = [&](const json&) { ++constructed; return sc::Program{}; };
    auto run = sc::run_schedule(plan, adapter);
    REQUIRE(run->report().outcome == sc::Outcome::invalid_schedule);
    REQUIRE(constructed == 0);
    REQUIRE_THROWS_AS(sc::encode_schedule(plan), std::invalid_argument);
    adapter = one_adapter();
    auto factory = adapter.factory;
    adapter.factory = [=](const json& inputs) {
      auto program = factory(inputs);
      program.observe = [value] { return value; };
      return program;
    };
    run = sc::run_schedule(one_plan(), adapter);
    REQUIRE(run->report().outcome == sc::Outcome::observation_failed);
    REQUIRE(run->report().cleanup == sc::Cleanup::confirmed);
    REQUIRE(run->report().observations.empty());
  }
}

TEST_CASE("an omitted teardown is a failed cleanup rather than inferred success", "[schedule]") {
  auto adapter = one_adapter();
  auto factory = adapter.factory;
  adapter.factory = [factory](const json& inputs) {
    auto program = factory(inputs);
    program.teardown = {};
    return program;
  };
  auto run = sc::run_schedule(one_plan(), adapter);
  REQUIRE(run->report().outcome == sc::Outcome::application_failed);
  REQUIRE(run->report().cleanup == sc::Cleanup::teardown_failed);
  REQUIRE_FALSE(run->report().passed());
  REQUIRE(run->report().events.empty());
}

TEST_CASE("incremental callbacks advance only admitted intervals and finish once", "[schedule]") {
  const auto plan = scheduled_counter::plan(true);
  auto run = sc::start_schedule(plan, scheduled_counter::adapter());
  REQUIRE(run->report().outcome == sc::Outcome::completed);
  REQUIRE(run->report().events.empty());
  REQUIRE(run->report().observations.size() == 1);
  for (const auto& step : plan.steps) {
    const auto report = run->advance(step);
    REQUIRE(report.outcome == sc::Outcome::completed);
  }
  REQUIRE(run->report().schedule_completed);
  REQUIRE(run->report().cleanup == sc::Cleanup::not_started);
  REQUIRE(run->finish().passed());
  const auto evidence = run->receipt();
  REQUIRE(run->finish().passed());
  REQUIRE(run->receipt() == evidence);
  REQUIRE_THROWS_AS(run->advance(plan.steps.front()), std::logic_error);
  REQUIRE(run->receipt() == evidence);
}

TEST_CASE("incremental wrong interval and early finish release all parked actors", "[schedule]") {
  auto plan = one_plan();
  auto run = sc::start_schedule(plan, one_adapter());
  auto report = run->advance({"a", "other"});
  REQUIRE(report.outcome == sc::Outcome::invalid_schedule);
  REQUIRE(report.events.empty());
  REQUIRE(report.cleanup == sc::Cleanup::confirmed);
  run = sc::start_schedule(plan, one_adapter());
  report = run->finish();
  REQUIRE(report.outcome == sc::Outcome::invalid_schedule);
  REQUIRE(report.cleanup == sc::Cleanup::confirmed);
}

TEST_CASE("incremental stop token remains active between generated callbacks", "[schedule]") {
  std::stop_source stop;
  auto plan = one_plan();
  auto run = sc::start_schedule(plan, one_adapter(), {}, stop.get_token());
  stop.request_stop();
  const auto report = run->advance(plan.steps.front());
  REQUIRE(report.outcome == sc::Outcome::cancelled);
  REQUIRE(report.cleanup == sc::Cleanup::confirmed);
  REQUIRE(report.events.empty());
}

TEST_CASE("terminal observation occurs after actor thread-local teardown", "[schedule]") {
  auto value = std::make_shared<std::atomic<int>>(0);
  auto adapter = one_adapter([value](auto& hook) {
    struct Finalizer {
      std::shared_ptr<std::atomic<int>> value;
      ~Finalizer() { value->store(2); }
    };
    thread_local std::unique_ptr<Finalizer> finalizer;
    finalizer = std::make_unique<Finalizer>();
    finalizer->value = value;
    value->store(1);
    hook.arrive("point");
  });
  auto factory = adapter.factory;
  adapter.factory = [factory, value](const json& inputs) {
    auto program = factory(inputs);
    program.observe = [value] { return json{{"actual", value->load()}}; };
    return program;
  };
  auto execution = sc::start_schedule(one_plan(), adapter);
  REQUIRE(execution->advance({"a", "point"}).observations.back()["state"]["actual"] == 1);
  REQUIRE(execution->advance({"a", "$done"}).observations.back()["state"]["actual"] == 2);
  REQUIRE(execution->finish().passed());
}
