#ifndef MIRRORCPP_EXAMPLE_SCHEDULED_COUNTER_HPP
#define MIRRORCPP_EXAMPLE_SCHEDULED_COUNTER_HPP
#include <mirrorcpp/schedule.hpp>
#include <memory>

namespace scheduled_counter {
namespace sc = mirrorcpp::schedule;
// Fixture labels, not build attestations. The acceptance runner hashes real artifacts.
inline sc::Identity identity() { return {std::string(64, '1'), std::string(64, '2'), std::string(64, '3')}; }
inline sc::Schedule plan(bool overlap) {
  sc::Schedule value;
  value.identity = identity();
  value.inputs = {{"initial", 0}};
  value.steps = overlap
      ? std::vector<sc::Step>{{"a", "read"}, {"b", "read"}, {"a", "write"},
          {"b", "write"}, {"a", "$done"}, {"b", "$done"}}
      : std::vector<sc::Step>{{"a", "read"}, {"a", "write"}, {"a", "$done"},
          {"b", "read"}, {"b", "write"}, {"b", "$done"}};
  return value;
}
inline sc::Adapter adapter(bool mutate = false) {
  sc::Adapter result;
  result.identity = identity();
  result.actors = {{"a", "increment-a"}, {"b", "increment-b"}};
  result.checkpoints = {"read", "write"};
  result.factory = [mutate](const nlohmann::json& inputs) {
    struct State { int value; };
    if (!inputs.is_object() || inputs.size() != 1 || !inputs.contains("initial") ||
        !inputs.at("initial").is_number_integer() || inputs.at("initial") < -100 || inputs.at("initial") > 100)
      throw std::invalid_argument("fixture requires one bounded integer initial input");
    const auto initial = inputs.at("initial").get<int>();
    if (initial < -100 || initial > 100) throw std::invalid_argument("fixture initial bound exceeded");
    auto state = std::make_shared<State>(State{initial});
    auto increment = [state, mutate](sc::Checkpoint& hook) {
      const int saved = state->value;
      hook.arrive("read");
      state->value = saved + (mutate ? 2 : 1);
      hook.arrive("write");
    };
    return sc::Program{{{"a", increment}, {"b", increment}},
        [state] { return nlohmann::json{{"value", state->value}}; }, [] {}};
  };
  return result;
}
}  // namespace scheduled_counter
#endif
