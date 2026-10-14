#include <mirrorcpp/schedule_sync.hpp>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
using namespace std::chrono_literals;
namespace ss = mirrorcpp::schedule_sync;
using json = nlohmann::json;
namespace {
ss::Adapter base(std::vector<std::string> actors, std::vector<ss::Resource> resources,
                 std::function<ss::Program()> factory) {
  return {{std::string(64,'a'),std::string(64,'b'),std::string(64,'c')},std::move(actors),std::move(resources),std::move(factory)};
}
void drain(ss::Execution& execution) {
  for (int step=0; step<128 && !execution.report().finished; ++step) {
    auto enabled=execution.enabled();
    REQUIRE_FALSE(enabled.empty());
    REQUIRE(execution.advance(enabled.front()).outcome=="completed");
  }
  REQUIRE(execution.report().finished);
  REQUIRE(execution.cleanup(1s).passed());
}
ss::Adapter mutex_fixture(std::shared_ptr<int> count, std::shared_ptr<int> teardown) {
  return base({"a","b"},{{"m",ss::Kind::mutex,0}},[count,teardown] {
    ss::Program p;
    for (const auto actor : {"a","b"}) p.workers.push_back({actor,[count](ss::Hook& hook) {
      ss::MutexGuard guard(hook,"m"); auto prior=*count; hook.checkpoint("held"); *count=prior+1;
    }});
    p.observe=[count]{return json{{"count",*count}};}; p.teardown=[teardown]{++*teardown;}; return p;
  });
}
}
TEST_CASE("controlled mutex holder remains enabled while contender blocks", "[sync]") {
  auto count=std::make_shared<int>(0), teardown=std::make_shared<int>(0);
  auto execution=ss::start(mutex_fixture(count,teardown));
  REQUIRE(execution->advance("a").outcome=="completed"); // start -> lock
  REQUIRE(execution->advance("a").outcome=="completed"); // lock -> checkpoint
  REQUIRE(execution->advance("b").outcome=="completed"); // start -> blocked lock
  REQUIRE(execution->enabled()==std::vector<std::string>{"a"});
  REQUIRE(execution->receipt()["resources"][0]["owner"]=="a");
  drain(*execution); REQUIRE(*count==2); REQUIRE(*teardown==1);
  auto receipt=execution->receipt(); REQUIRE(receipt["cleanup"]=="confirmed");
  REQUIRE(receipt["actors"][0]["state"]=="finished");
}
TEST_CASE("fixed replay refuses a disabled actor without running the owner", "[sync]") {
  auto count=std::make_shared<int>(0), teardown=std::make_shared<int>(0);
  auto execution=ss::start(mutex_fixture(count,teardown));
  execution->advance("a"); execution->advance("a"); execution->advance("b");
  REQUIRE(execution->advance("b").outcome=="disabled_choice");
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed"); REQUIRE(*count==0); REQUIRE(*teardown==1);
  REQUIRE_FALSE(execution->report().passed());
}
TEST_CASE("bounded queue parks full senders and preserves actual FIFO", "[sync]") {
  auto received=std::make_shared<std::vector<std::int64_t>>();
  auto adapter=base({"producer","consumer"},{{"q",ss::Kind::queue,1}},[received] {
    ss::Program p;
    p.workers={{"producer",[](ss::Hook& h){h.send("q",10);h.send("q",20);}},
               {"consumer",[received](ss::Hook& h){received->push_back(h.receive("q"));received->push_back(h.receive("q"));}}};
    p.observe=[received]{return json{{"received",*received}};};p.teardown=[]{};return p;
  });
  auto execution=ss::start(adapter);
  execution->advance("producer"); execution->advance("producer"); // queue full
  REQUIRE(execution->enabled()==std::vector<std::string>{"consumer"});
  drain(*execution); REQUIRE(*received==std::vector<std::int64_t>{10,20});
}
TEST_CASE("declared resource wait cycle is deadlock and retains cleanup", "[sync]") {
  auto execution=ss::start(base({"a","b"},{{"x",ss::Kind::mutex,0},{"y",ss::Kind::mutex,0}},[] {
    ss::Program p;
    p.workers={{"a",[](ss::Hook& h){ss::MutexGuard x(h,"x");h.checkpoint("held");ss::MutexGuard y(h,"y");}},
               {"b",[](ss::Hook& h){ss::MutexGuard y(h,"y");h.checkpoint("held");ss::MutexGuard x(h,"x");}}};
    p.observe=[]{return json::object();};p.teardown=[]{};return p;
  }));
  for(auto actor: {"a","a","b","b","a","b"}) REQUIRE(execution->advance(actor).outcome=="completed");
  REQUIRE(execution->enabled().empty()); REQUIRE(execution->report().outcome=="controlled_deadlock");
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed");
  REQUIRE(execution->report().outcome=="controlled_deadlock");
  REQUIRE(execution->receipt()["failureGraph"]["owners"]["x"]=="a");
  REQUIRE(execution->receipt()["failureGraph"]["owners"]["y"]=="b");
  REQUIRE_FALSE(execution->report().passed());
}
TEST_CASE("resource misuse and malformed declarations refuse", "[sync]") {
  for (const auto mode : {"recursive","wrong-owner","wrong-kind","unknown"}) {
    auto execution=ss::start(base({"a"},{{"m",ss::Kind::mutex,0}},[mode] {
      ss::Program p;p.workers={{"a",[mode](ss::Hook& h){
        if(std::string(mode)=="recursive"){h.lock("m");h.lock("m");}
        else if(std::string(mode)=="wrong-owner")h.unlock("m");
        else if(std::string(mode)=="wrong-kind")h.send("m",1);
        else h.lock("absent");
      }}};p.observe=[]{return json::object();};p.teardown=[]{};return p;
    }));
    execution->advance("a"); if(std::string(mode)=="recursive")execution->advance("a");
    REQUIRE(execution->report().outcome=="resource_misuse");
    REQUIRE(execution->cleanup(1s).cleanup=="confirmed");
  }
  int acquired=0;
  auto invalid=ss::start(base({"a","a"},{},[&]{++acquired;return ss::Program{};}));
  REQUIRE(invalid->report().outcome=="invalid_profile"); REQUIRE(acquired==0);
}
TEST_CASE("fresh prefix exploration closes a small enabled state space", "[sync]") {
  auto factory=[] { return base({"a","b"},{{"q",ss::Kind::queue,1}},[] {
    ss::Program p;p.workers={{"a",[](ss::Hook& h){h.send("q",42);}}, {"b",[](ss::Hook& h){if(h.receive("q")!=42)throw std::runtime_error("wrong actual value");}}};
    p.observe=[]{return json::object();};p.teardown=[]{};return p;
  });};
  auto result=ss::explore(factory,256); REQUIRE(result.retained.empty());
  REQUIRE(result.evidence["complete"]==true); REQUIRE(result.evidence["terminals"].get<int>()>0);
  REQUIRE(result.evidence["deadlocks"]==0); REQUIRE(result.evidence["modelComparison"]==false);
  auto truncated=ss::explore(factory,1); REQUIRE(truncated.evidence["complete"]==false);
}
TEST_CASE("timeout is distinct from controlled deadlock and cleanup can retry", "[sync]") {
  auto release=std::make_shared<std::atomic<bool>>(false);
  ss::Policy policy;policy.execution_timeout=20ms;policy.cleanup_timeout=1ms;
  auto execution=ss::start(base({"a"},{},[release] {
    ss::Program p;p.workers={{"a",[release](ss::Hook&){while(!release->load())std::this_thread::yield();}}};
    p.observe=[]{return json::object();};p.teardown=[]{};return p;
  }),policy);
  auto primary=execution->advance("a"); auto first=execution->cleanup(1ms);
  release->store(true);auto retried=execution->cleanup(1s);
  REQUIRE(primary.outcome=="timed_out"); REQUIRE(first.cleanup=="incomplete");
  REQUIRE(retried.cleanup=="confirmed");REQUIRE(retried.outcome=="timed_out");
}
TEST_CASE("teardown failure and evidence exhaustion never pass", "[sync]") {
  auto adapter=base({"a"},{},[] {
    ss::Program p;p.workers={{"a",[](ss::Hook& h){h.checkpoint("one");}}};
    p.observe=[]{return json::object();};p.teardown=[]{throw std::runtime_error("teardown");};return p;
  });
  auto execution=ss::start(adapter);
  while(!execution->report().finished)execution->advance("a");
  REQUIRE(execution->cleanup(1s).cleanup=="teardown_failed");
  REQUIRE(execution->cleanup(1s).cleanup=="teardown_failed"); REQUIRE_FALSE(execution->report().passed());
  ss::Policy policy;policy.max_evidence_bytes=256;
  auto bounded=ss::start(adapter,policy); bounded->advance("a"); bounded->advance("a");
  REQUIRE(bounded->report().outcome=="evidence_exhausted");
  REQUIRE_FALSE(bounded->cleanup(1s).passed());
}
TEST_CASE("actual early-release mutation exposes overlapping logical critical sections", "[sync]") {
  for(bool mutant : {false,true}) {
    auto active=std::make_shared<std::atomic<int>>(0);auto overlap=std::make_shared<std::atomic<bool>>(false);
    auto execution=ss::start(base({"a","b"},{{"m",ss::Kind::mutex,0}},[active,overlap,mutant] {
      ss::Program p;
      for(auto actor : {"a","b"})p.workers.push_back({actor,[active,overlap,mutant](ss::Hook& h){
        h.lock("m");if(active->fetch_add(1)!=0)overlap->store(true);
        if(mutant) { h.unlock("m"); }
        h.checkpoint("critical");
        active->fetch_sub(1);if(!mutant)h.unlock("m");
      }});
      p.observe=[overlap]{return json{{"overlap",overlap->load()}};};p.teardown=[]{};return p;
    }));
    execution->advance("a");execution->advance("a");
    if(mutant)execution->advance("a");
    execution->advance("b");
    if(mutant)execution->advance("b");
    REQUIRE(overlap->load()==mutant);drain(*execution);
  }
}
TEST_CASE("queue duplicate and reorder application mutations violate independent FIFO specification", "[sync]") {
  for(auto values : {std::vector<std::int64_t>{10,20},std::vector<std::int64_t>{10,10},std::vector<std::int64_t>{20,10}}) {
    auto actual=std::make_shared<std::vector<std::int64_t>>();
    auto execution=ss::start(base({"a","b"},{{"q",ss::Kind::queue,1}},[actual,values] {
      ss::Program p;p.workers={{"a",[values](ss::Hook& h){for(auto value:values)h.send("q",value);}},
                               {"b",[actual](ss::Hook& h){actual->push_back(h.receive("q"));actual->push_back(h.receive("q"));}}};
      p.observe=[actual]{return json{{"actual",*actual}};};p.teardown=[]{};return p;
    }));
    drain(*execution);REQUIRE(*actual==values);
    REQUIRE((*actual==std::vector<std::int64_t>{10,20})==(values==std::vector<std::int64_t>{10,20}));
  }
}
TEST_CASE("early cancellation joins initial workers without entering the application", "[sync]") {
  auto entered=std::make_shared<std::atomic<int>>(0);auto disposed=std::make_shared<int>(0);
  auto execution=ss::start(base({"a"},{},[entered,disposed] {
    ss::Program p;p.workers={{"a",[entered](ss::Hook&){++*entered;}}};
    p.observe=[]{return json::object();};p.teardown=[disposed]{++*disposed;};return p;
  }));
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed"); REQUIRE(execution->report().outcome=="cancelled");
  REQUIRE(*entered==0);REQUIRE(*disposed==1);REQUIRE_FALSE(execution->report().passed());
}

namespace {
ss::Program idle_program(std::function<json()> observer = [] { return json::object(); },
                         std::function<void()> teardown = [] {}) {
  ss::Program program;
  program.workers = {{"a", [](ss::Hook&) {}}};
  program.observe = std::move(observer); program.teardown = std::move(teardown);
  return program;
}
void finish_idle(ss::Execution& execution) {
  for (int step=0; step<2 && execution.report().outcome=="completed"; ++step)
    execution.advance("a");
}
struct ReleaseFlag {
  std::shared_ptr<std::atomic<bool>> flag;
  ~ReleaseFlag() { flag->store(true); }
};
struct SlowThreadExit {
  std::shared_ptr<std::atomic<bool>> entered;
  ~SlowThreadExit() { entered->store(true); std::this_thread::sleep_for(120ms); }
};
struct FinalThreadEffect {
  std::atomic<int>* value;
  ~FinalThreadEffect() { std::this_thread::sleep_for(120ms); value->store(99); }
};
}

TEST_CASE("late controlled factory starts no workers and retains one teardown", "[sync][deadline]") {
  ss::Policy policy; policy.execution_timeout=40ms;
  int entered=0, disposed=0;
  auto execution=ss::start(base({"a"},{},[&] {
    std::this_thread::sleep_for(120ms);
    auto program=idle_program([] { return json::object(); }, [&] { ++disposed; });
    program.workers={{"a",[&](ss::Hook&) { ++entered; }}};
    return program;
  }),policy);
  REQUIRE(execution->report().outcome=="timed_out");
  REQUIRE(execution->report().execution_budget_exceeded);
  REQUIRE(execution->receipt()["observations"].empty());
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed");
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed");
  REQUIRE(entered==0); REQUIRE(disposed==1); REQUIRE_FALSE(execution->report().passed());
}

TEST_CASE("initial intermediate and final controlled observers cannot return late and pass", "[sync][deadline]") {
  for (int late_at : {1,2,3}) {
    ss::Policy policy; policy.execution_timeout=40ms;
    int observations=0, disposed=0;
    auto execution=ss::start(base({"a"},{},[&] {
      return idle_program([&] {
        if (++observations==late_at) std::this_thread::sleep_for(120ms);
        return json{{"observation",observations}};
      },[&] { ++disposed; });
    }),policy);
    finish_idle(*execution);
    REQUIRE(execution->report().outcome=="timed_out");
    REQUIRE(execution->report().execution_budget_exceeded);
    REQUIRE(execution->receipt()["observations"].size()==static_cast<std::size_t>(late_at-1));
    REQUIRE(execution->cleanup(1s).cleanup=="confirmed");
    REQUIRE(disposed==1); REQUIRE_FALSE(execution->report().passed());
  }
}

TEST_CASE("callback exceptions retain primary attribution when they also return late", "[sync][deadline]") {
  ss::Policy policy; policy.execution_timeout=40ms;
  auto failed_factory=ss::start(base({"a"},{},[]() -> ss::Program {
    std::this_thread::sleep_for(120ms); throw std::runtime_error("factory");
  }),policy);
  REQUIRE(failed_factory->report().outcome=="application_failed");
  REQUIRE(failed_factory->report().execution_budget_exceeded);
  REQUIRE(failed_factory->cleanup(1s).cleanup=="not_started");
  auto failed_observer=ss::start(base({"a"},{},[] {
    return idle_program([]() -> json {
      std::this_thread::sleep_for(120ms); throw std::runtime_error("observer");
    });
  }),policy);
  REQUIRE(failed_observer->report().outcome=="observation_failed");
  REQUIRE(failed_observer->report().execution_budget_exceeded);
  REQUIRE(failed_observer->cleanup(1s).cleanup=="confirmed");
}

TEST_CASE("late teardown confirms physical cleanup once but never restores pass", "[sync][deadline]") {
  for (bool throws : {false,true}) {
    int disposed=0;
    auto execution=ss::start(base({"a"},{},[&] {
      return idle_program([] { return json::object(); },[&] {
        ++disposed; std::this_thread::sleep_for(120ms);
        if (throws) throw std::runtime_error("teardown");
      });
    }));
    finish_idle(*execution);
    const auto cleaned=execution->cleanup(10ms);
    REQUIRE(cleaned.cleanup==(throws?"teardown_failed":"confirmed"));
    REQUIRE(cleaned.cleanup_budget_exceeded);
    REQUIRE_FALSE(cleaned.execution_budget_exceeded);
    REQUIRE(cleaned.outcome=="timed_out"); REQUIRE_FALSE(cleaned.passed());
    REQUIRE(execution->cleanup(1s).cleanup==cleaned.cleanup);
    REQUIRE(disposed==1);
    REQUIRE(execution->receipt()["budgetExceeded"]["cleanup"]==true);
  }
}

TEST_CASE("execution timing ends after final observation independently of cleanup", "[sync][deadline]") {
  ss::Policy policy; policy.execution_timeout=100ms;
  auto execution=ss::start(base({"a"},{},[] { return idle_program(); }),policy);
  finish_idle(*execution);
  REQUIRE(execution->report().finished);
  std::this_thread::sleep_for(150ms);
  REQUIRE(execution->enabled().empty());
  REQUIRE(execution->cleanup(1s).passed());
  REQUIRE_FALSE(execution->report().execution_budget_exceeded);
}

TEST_CASE("late cleanup preserves primary resource failure and original graph", "[sync][deadline]") {
  int disposed=0;
  auto execution=ss::start(base({"a"},{{"m",ss::Kind::mutex,0}},[&] {
    auto program=idle_program([] { return json::object(); },[&] {
      ++disposed; std::this_thread::sleep_for(120ms);
    });
    program.workers={{"a",[](ss::Hook& hook) { hook.unlock("m"); }}};
    return program;
  }));
  REQUIRE(execution->advance("a").outcome=="resource_misuse");
  const auto graph=execution->receipt()["failureGraph"];
  const auto cleaned=execution->cleanup(10ms);
  REQUIRE(cleaned.outcome=="resource_misuse"); REQUIRE(cleaned.cleanup=="confirmed");
  REQUIRE(cleaned.cleanup_budget_exceeded); REQUIRE_FALSE(cleaned.passed());
  REQUIRE(execution->receipt()["failureGraph"]==graph);
  REQUIRE(execution->cleanup(1s).outcome=="resource_misuse"); REQUIRE(disposed==1);
}

TEST_CASE("cancelled worker TLS join consumes cleanup budget and teardown retries once", "[sync][deadline]") {
  auto entered=std::make_shared<std::atomic<bool>>(false);
  int disposed=0;
  auto execution=ss::start(base({"a"},{},[&] {
    auto program=idle_program([] { return json::object(); },[&] { ++disposed; });
    program.workers={{"a",[entered](ss::Hook& hook) {
      thread_local SlowThreadExit exit{entered}; hook.checkpoint("ready");
    }}};
    return program;
  }));
  REQUIRE(execution->advance("a").outcome=="completed");
  REQUIRE_FALSE(execution->report().finished);
  auto first=execution->cleanup(10ms);
  REQUIRE(first.cleanup=="incomplete"); REQUIRE(first.cleanup_budget_exceeded);
  REQUIRE(first.outcome=="cancelled"); REQUIRE(disposed==0);
  auto second=execution->cleanup(1s);
  REQUIRE(second.cleanup=="confirmed"); REQUIRE(second.cleanup_budget_exceeded);
  REQUIRE(second.outcome=="cancelled"); REQUIRE_FALSE(second.passed()); REQUIRE(disposed==1);
  REQUIRE(entered->load());
  REQUIRE(execution->cleanup(1s).cleanup=="confirmed"); REQUIRE(disposed==1);
}

TEST_CASE("each completion observation includes actual thread exit effects", "[sync][deadline]") {
  for (bool another_actor : {false,true}) {
    std::atomic<int> value{0};
    auto execution=ss::start(base(another_actor?std::vector<std::string>{"a","b"}:std::vector<std::string>{"a"},{},[&] {
      auto program=idle_program([&] { return json{{"value",value.load()}}; });
      program.workers={{"a",[&](ss::Hook&) { thread_local FinalThreadEffect effect{&value}; }}};
      if (another_actor) program.workers.push_back({"b",[](ss::Hook&) {}});
      return program;
    }));
    REQUIRE(execution->advance("a").outcome=="completed");
    REQUIRE(execution->advance("a").outcome=="completed");
    REQUIRE(value.load()==99);
    REQUIRE(execution->receipt()["observations"].back()["state"]["value"]==99);
    REQUIRE(execution->report().finished==!another_actor);
    if (another_actor) {
      REQUIRE(execution->advance("b").outcome=="completed");
      REQUIRE(execution->advance("b").outcome=="completed");
    }
    REQUIRE(execution->cleanup(1s).passed());
    REQUIRE(execution->receipt()["observations"].back()["state"]["value"]==value.load());
  }
}

TEST_CASE("late completion TLS join cannot retain an observation or pass", "[sync][deadline]") {
  ss::Policy policy; policy.execution_timeout=40ms;
  std::atomic<int> value{0}; int disposed=0;
  auto execution=ss::start(base({"a"},{},[&] {
    auto program=idle_program([&] { return json{{"value",value.load()}}; },[&] { ++disposed; });
    program.workers={{"a",[&](ss::Hook&) { thread_local FinalThreadEffect effect{&value}; }}};
    return program;
  }),policy);
  REQUIRE(execution->advance("a").outcome=="completed");
  const auto completion=execution->advance("a");
  REQUIRE(completion.outcome=="timed_out"); REQUIRE(completion.execution_budget_exceeded);
  REQUIRE(value.load()==99);
  REQUIRE(execution->receipt()["observations"].size()==2);
  const auto cleaned=execution->cleanup(1s);
  REQUIRE(cleaned.cleanup=="confirmed"); REQUIRE_FALSE(cleaned.cleanup_budget_exceeded);
  REQUIRE(cleaned.execution_budget_exceeded); REQUIRE_FALSE(cleaned.passed()); REQUIRE(disposed==1);
}

TEST_CASE("review final observer and teardown overrun reproduction cannot pass", "[sync][deadline]") {
  ss::Policy policy; policy.execution_timeout=100ms;
  int observations=0, disposed=0;
  auto execution=ss::start(base({"a"},{},[&] {
    return idle_program([&] {
      if (++observations==3) std::this_thread::sleep_for(250ms);
      return json::object();
    },[&] { ++disposed; std::this_thread::sleep_for(150ms); });
  }),policy);
  finish_idle(*execution);
  auto first=execution->cleanup(1ms);
  REQUIRE(first.outcome=="timed_out"); REQUIRE(first.execution_budget_exceeded);
  REQUIRE(first.cleanup_budget_exceeded); REQUIRE_FALSE(first.passed());
  auto final=execution->cleanup(1s);
  REQUIRE(final.cleanup=="confirmed"); REQUIRE(final.cleanup_budget_exceeded);
  REQUIRE_FALSE(final.passed()); REQUIRE(disposed==1);
}

TEST_CASE("exploration last prefix observer and cleanup obey the total deadline", "[sync][deadline]") {
  for (bool late_cleanup : {false,true}) {
    ss::Policy policy; policy.execution_timeout=1s; policy.exploration_timeout=100ms;
    int acquisitions=0;
    auto result=ss::explore([&] {
      const int attempt=++acquisitions;
      return base({"a"},{},[attempt,late_cleanup] {
        auto observations=std::make_shared<int>(0);
        return idle_program([observations,attempt,late_cleanup] {
          if (++*observations==3 && attempt==3 && !late_cleanup) std::this_thread::sleep_for(200ms);
          return json::object();
        },[attempt,late_cleanup] {
          if (attempt==3 && late_cleanup) std::this_thread::sleep_for(200ms);
        });
      });
    },16,policy);
    REQUIRE(result.evidence["complete"]==false);
    REQUIRE(result.evidence["stopReason"]=="total_timeout");
    REQUIRE(result.evidence["runs"]==3); REQUIRE(result.evidence["rows"].size()==3);
    REQUIRE(result.evidence["rows"].back()["receipt"]["passed"]==late_cleanup);
    REQUIRE(result.evidence["modelComparison"]==false);
  }
}

TEST_CASE("exploration retains outer factory errors and factory deadline attempts", "[sync][deadline]") {
  for (bool throws : {false,true}) {
    ss::Policy policy; policy.exploration_timeout=40ms;
    auto result=ss::explore([throws]() -> ss::Adapter {
      std::this_thread::sleep_for(120ms);
      if (throws) throw std::runtime_error("outer factory");
      return base({"a"},{},[] { return idle_program(); });
    },16,policy);
    REQUIRE(result.evidence["complete"]==false); REQUIRE(result.evidence["runs"]==1);
    REQUIRE(result.evidence["rows"].size()==1);
    REQUIRE(result.evidence["rows"][0]["stage"]=="adapter_factory");
    REQUIRE_FALSE(result.evidence["rows"][0].contains("receipt"));
    REQUIRE(result.evidence["stopReason"]==(throws?"factory_failed":"total_timeout"));
  }
}

TEST_CASE("exploration cleanup failure retains the failed row and retryable execution", "[sync][deadline]") {
  auto release=std::make_shared<std::atomic<bool>>(false);
  ss::Exploration result;
  ReleaseFlag guard{release}; // Release before result's retained execution is destroyed on any assertion.
  ss::Policy policy; policy.execution_timeout=100ms; policy.cleanup_timeout=100ms;
  int disposed=0;
  result=ss::explore([&] {
    return base({"a"},{},[&] {
      auto program=idle_program([] { return json::object(); },[&] { ++disposed; });
      program.workers={{"a",[release](ss::Hook&) { while (!release->load()) std::this_thread::yield(); }}};
      return program;
    });
  },16,policy);
  REQUIRE(result.evidence["complete"]==false); REQUIRE(result.evidence["runs"]==2);
  REQUIRE(result.evidence["rows"].size()==2); REQUIRE(result.retained.size()==1);
  REQUIRE(result.evidence["rows"].back()["receipt"]["cleanup"]=="incomplete");
  release->store(true);
  auto cleanup=result.retained[0]->cleanup(1s);
  REQUIRE(cleanup.cleanup=="confirmed"); REQUIRE(cleanup.cleanup_budget_exceeded);
  REQUIRE_FALSE(cleanup.passed()); REQUIRE(disposed==2);
}

TEST_CASE("exploration retains teardown failures and bounded omission metadata", "[sync][deadline]") {
  auto failed=ss::explore([] {
    return base({"a"},{},[] {
      return idle_program([] { return json::object(); },[] { throw std::runtime_error("teardown"); });
    });
  });
  REQUIRE(failed.evidence["complete"]==false); REQUIRE(failed.evidence["runs"]==1);
  REQUIRE(failed.evidence["rows"].size()==1);
  REQUIRE(failed.evidence["rows"][0]["receipt"]["cleanup"]=="teardown_failed");
  ss::Policy policy; policy.max_evidence_bytes=1024;
  auto bounded=ss::explore([] { return base({"a"},{},[] { return idle_program(); }); },16,policy);
  REQUIRE(bounded.evidence["complete"]==false);
  REQUIRE(bounded.evidence["evidenceComplete"]==false);
  REQUIRE(bounded.evidence["rows"].size()==bounded.evidence["runs"].get<std::size_t>());
  REQUIRE(bounded.evidence["rows"].back()["evidenceError"]=="receipt_byte_bound_exceeded");
  REQUIRE(bounded.evidence.dump().size()<=policy.max_evidence_bytes);
  policy.max_evidence_bytes=256;
  int acquisitions=0;
  REQUIRE_THROWS_AS(ss::explore([&] { ++acquisitions; return base({"a"},{},[] { return idle_program(); }); },16,policy),std::invalid_argument);
  REQUIRE(acquisitions==0);
}

TEST_CASE("final observation admission measures its complete row at the exact byte limit", "[sync][evidence]") {
  auto adapter=[] {
    return base({"a"},{},[] {
      auto calls=std::make_shared<int>(0);
      return idle_program([calls] {
        if (++*calls==3) return json{{"payload",std::string(128,'x')}};
        return json::object();
      });
    });
  };
  auto baseline=ss::start(adapter());
  finish_idle(*baseline);
  REQUIRE(baseline->cleanup(1s).passed());
  const auto receipt=baseline->receipt();
  // Only the retained event/observation arrays count, including their brackets.
  const auto exact=receipt["events"].dump().size()+receipt["observations"].dump().size();
  REQUIRE(exact>=256);
  for (const auto excess : {0u,1u}) {
    ss::Policy policy; policy.max_evidence_bytes=exact-excess;
    auto execution=ss::start(adapter(),policy);
    finish_idle(*execution);
    const auto cleaned=execution->cleanup(1s);
    const auto actual=execution->receipt();
    const auto retained=actual["events"].dump().size()+actual["observations"].dump().size();
    REQUIRE(cleaned.cleanup=="confirmed");
    REQUIRE(retained<=policy.max_evidence_bytes);
    if (excess==0) {
      REQUIRE(cleaned.passed()); REQUIRE(retained==exact);
      REQUIRE(actual["observations"].size()==3);
    } else {
      REQUIRE(cleaned.outcome=="evidence_exhausted"); REQUIRE_FALSE(cleaned.passed());
      REQUIRE(actual["observations"].size()==2);
    }
  }
}

TEST_CASE("event admission charges the separator in a nonempty array", "[sync][evidence]") {
  auto adapter=[] { return base({"a"},{},[] { return idle_program(); }); };
  auto baseline=ss::start(adapter());
  finish_idle(*baseline);
  REQUIRE(baseline->cleanup(1s).passed());
  const auto receipt=baseline->receipt();
  REQUIRE(receipt["events"].size()==4); REQUIRE(receipt["observations"].size()==3);
  auto events=receipt["events"]; events.erase(events.end()-1);
  auto observations=receipt["observations"]; observations.erase(observations.end()-1);
  // Measure through the final grant; the following result must need more space.
  const auto exact=events.dump().size()+observations.dump().size();
  REQUIRE(exact>=256);
  for (const auto excess : {0u,1u}) {
    ss::Policy policy; policy.max_evidence_bytes=exact-excess;
    auto execution=ss::start(adapter(),policy);
    REQUIRE(execution->advance("a").outcome=="completed");
    REQUIRE(execution->advance("a").outcome=="evidence_exhausted");
    const auto cleaned=execution->cleanup(1s);
    const auto actual=execution->receipt();
    const auto retained=actual["events"].dump().size()+actual["observations"].dump().size();
    REQUIRE(cleaned.cleanup=="confirmed"); REQUIRE_FALSE(cleaned.passed());
    REQUIRE(actual["observations"].size()==2);
    REQUIRE(actual["events"].size()==(excess==0?3:2));
    REQUIRE(retained<=policy.max_evidence_bytes);
    if (excess==0) REQUIRE(retained==exact);
  }
}
