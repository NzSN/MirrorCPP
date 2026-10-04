#include <catch2/catch_test_macros.hpp>
#include <mirrorcpp/schedule_binding.hpp>
#include <chrono>
#include <memory>
#include <thread>

namespace sc = mirrorcpp::schedule;
using nlohmann::json;
using namespace std::chrono_literals;
namespace {
sc::Schedule plan() {
  sc::Schedule value;
  value.identity = {std::string(64, '1'), std::string(64, '2'), std::string(64, '3')};
  value.steps = {{"a", "point"}, {"a", "$done"}};
  return value;
}
sc::Adapter adapter(int& constructed, int& disposed, bool fail_cleanup = false) {
  return {plan().identity, {{"a", "operation"}}, {"point"},
      [&constructed, &disposed, fail_cleanup](const json&) { ++constructed; return sc::Program{
        {{"a", [](sc::Checkpoint& hook) { hook.arrive("point"); }}},
        [] { return json{{"actual", true}}; }, [&disposed, fail_cleanup] {
          ++disposed; if (fail_cleanup) throw std::runtime_error("cleanup failed");
        }}; }};
}
class UnusedTransport : public mirrorcpp::Transport {
 public:
  int sent = 0, received = 0, closed = 0;
  mirrorcpp::Result<void> send_line(std::string_view) override { ++sent; return {}; }
  mirrorcpp::Result<std::string> recv_line() override {
    ++received; return std::unexpected(mirrorcpp::Error(mirrorcpp::ErrorKind::io, "unused"));
  }
  mirrorcpp::Result<long> close() override { ++closed; return 0; }
};
}
TEST_CASE("scheduled binding is deferred and disposes once across trace generations", "[schedule-binding]") {
  int constructed = 0, disposed = 0;
  sc::BindingSession session(plan(), adapter(constructed, disposed));
  REQUIRE(constructed == 0);
  for (int i = 0; i < 2; ++i) {
    session.initialize();
    REQUIRE(session.observation() == json{{"actual", true}});
    for (const auto& step : plan().steps) session.advance(step);
  }
  REQUIRE(session.dispose().has_value());
  REQUIRE(session.dispose().has_value());
  REQUIRE(constructed == 2);
  REQUIRE(disposed == 2);
  REQUIRE(session.fully_completed());
  const auto receipt = session.receipt();
  REQUIRE(receipt["initializations"] == 2);
  REQUIRE(receipt["disposals"] == 1);
  REQUIRE(receipt["executions"][0]["executionId"] != receipt["executions"][1]["executionId"]);
  REQUIRE_THROWS_AS(session.initialize(), mirrorcpp::ModelInterfaceBindingError);
  REQUIRE_THROWS_AS(session.observation(), mirrorcpp::ModelInterfaceBindingError);
}
TEST_CASE("scheduled binding cannot discard an unfinished generation", "[schedule-binding]") {
  int constructed = 0, disposed = 0;
  sc::BindingSession session(plan(), adapter(constructed, disposed));
  session.initialize();
  REQUIRE_THROWS_AS(session.initialize(), mirrorcpp::ModelInterfaceBindingError);
  REQUIRE(constructed == 1);
  REQUIRE(disposed == 1);
  REQUIRE(session.dispose().has_value());
  REQUIRE_FALSE(session.fully_completed());
}
TEST_CASE("scheduled disposal failure remains visible on repeated disposal", "[schedule-binding]") {
  int constructed = 0, disposed = 0;
  sc::BindingSession session(plan(), adapter(constructed, disposed, true));
  session.initialize();
  for (const auto& step : plan().steps) session.advance(step);
  REQUIRE_FALSE(session.dispose().has_value());
  REQUIRE_FALSE(session.dispose().has_value());
  REQUIRE(disposed == 1);
  REQUIRE_FALSE(session.fully_completed());
  REQUIRE(session.receipt()["executions"][0]["cleanup"] == "teardown_failed");
}
TEST_CASE("scheduled deadline includes the gap between generated callbacks", "[schedule-binding]") {
  int constructed = 0, disposed = 0;
  sc::Policy policy; policy.execution_timeout = 50ms;
  sc::BindingSession session(plan(), adapter(constructed, disposed), policy);
  session.initialize();
  std::this_thread::sleep_for(70ms);
  REQUIRE_THROWS_AS(session.advance(plan().steps.front()), mirrorcpp::ModelInterfaceBindingError);
  REQUIRE(session.dispose().has_value());
  REQUIRE(session.receipt()["executions"][0]["outcome"] == "timed_out");
  REQUIRE(disposed == 1);
}
TEST_CASE("old scheduled binding evidence cannot qualify a new connection", "[schedule-binding]") {
  int constructed = 0, disposed = 0;
  sc::BindingSession session(plan(), adapter(constructed, disposed));
  session.initialize();
  for (const auto& step : plan().steps) session.advance(step);
  REQUIRE(session.dispose().has_value());
  UnusedTransport transport;
  const auto result = sc::replay_with_traces(transport, {}, {}, {}, session);
  REQUIRE_FALSE(result.passed);
  REQUIRE_FALSE(result.client_result.has_value());
  REQUIRE(result.client_result.error().code == "schedule_session_reused");
  REQUIRE(transport.sent == 0);
  REQUIRE(transport.received == 0);
  REQUIRE(transport.closed == 1);
}
