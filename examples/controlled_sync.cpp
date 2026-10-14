#include <mirrorcpp/schedule_sync.hpp>
#include <iostream>
#include <memory>
#include <vector>

int main() {
  namespace sync = mirrorcpp::schedule_sync;
  using nlohmann::json;
  auto count = std::make_shared<int>(0);
  auto received = std::make_shared<std::vector<std::int64_t>>();
  sync::Adapter adapter;
  // Fixture labels; the acceptance manifest separately hashes the actual binary.
  adapter.identity = {std::string(64, 'a'), std::string(64, 'b'), std::string(64, 'c')};
  adapter.actors = {"producer", "consumer"};
  adapter.resources = {{"mutex", sync::Kind::mutex, 0}, {"queue", sync::Kind::queue, 1}};
  adapter.factory = [count, received] {
    sync::Program program;
    program.workers = {
      {"producer", [count](sync::Hook& hook) {
        { sync::MutexGuard guard(hook, "mutex"); ++*count; hook.checkpoint("critical"); }
        hook.send("queue", 10); hook.send("queue", 20);
      }},
      {"consumer", [count, received](sync::Hook& hook) {
        received->push_back(hook.receive("queue"));
        received->push_back(hook.receive("queue"));
        { sync::MutexGuard guard(hook, "mutex"); ++*count; hook.checkpoint("critical"); }
      }}
    };
    program.observe = [count, received] { return json{{"count", *count}, {"received", *received}}; };
    program.teardown = [] {};
    return program;
  };
  auto execution = sync::start(std::move(adapter));
  for (unsigned i = 0; i < 64 && !execution->report().finished; ++i) {
    const auto enabled = execution->enabled();
    if (enabled.empty() || execution->advance(enabled.front()).outcome != "completed") break;
  }
  const auto report = execution->cleanup(std::chrono::seconds(1));
  std::cout << execution->receipt().dump() << '\n';
  return report.passed() && *count == 2 && *received == std::vector<std::int64_t>{10, 20} ? 0 : 1;
}
