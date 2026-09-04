#include "CounterMirror.generated.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>
#include <tuple>
#include <variant>

using namespace mirrorcpp;
using namespace mirrors_generated::counter;

namespace {

ApalacheConfig counter_config() {
  ApalacheConfig config;
  config.spec_path = "Counter.tla";
  config.invariant = "TraceComplete";
  config.length_bound = 3;
  config.param_vars = "parameters";
  return config;
}

State tick_payload(long long stride) {
  return {{"parameters", Value(Value::Record{
      {{"stride", Value(stride)}}})}};
}

class MutableCounter final : public CounterPort {
 public:
  void initialize() override {
    ++actions;
    count = 0;
  }
  void tick(const TickInput& input) override {
    ++actions;
    count += input.stride;
  }
  CounterObservation observe() override {
    ++observations;
    if (fail_observation) throw std::runtime_error("observation failed");
    return {count + offset};
  }

  Value::Int count = 0;
  Value::Int offset = 0;
  int actions = 0;
  int observations = 0;
  bool fail_observation = false;
};

const Value::Int& reported_count(const State& state) {
  return state.at("count").get<Value::Int>();
}

}  // namespace

TEST_CASE("generated Counter binding dispatches typed inputs and observations",
          "[model-interface][generated]") {
  MutableCounter port;
  auto binding = bind_counter(port, counter_config());

  REQUIRE(reported_count(binding.computer("init", {}, {})) == 0);
  REQUIRE(reported_count(binding.computer("tick", tick_payload(2), {})) == 2);
  REQUIRE(reported_count(binding.computer("tick", tick_payload(3), {})) == 5);
  REQUIRE(port.actions == 3);
  REQUIRE(port.observations == 3);
  REQUIRE(binding.coverage().at("Initialize") == 1);
  REQUIRE(binding.coverage().at("Tick") == 2);
  REQUIRE_NOTHROW(binding.assert_all_actions_covered());
  REQUIRE(CounterSemanticDigest ==
          "193d6cc187d05c18f02ad483a44f8ad0c1634b02083df241df08b9281b045d1c");
  REQUIRE(CounterModelInterface.semantic_digest == CounterSemanticDigest);
}

TEST_CASE("generated binding validates every input before adapter mutation",
          "[model-interface][generated]") {
  MutableCounter port;
  auto binding = bind_counter(port, counter_config());
  (void)binding.computer("init", {}, {});
  const int before = port.actions;

  try {
    (void)binding.computer("tick", {{"parameters", Value(Value::Record{})}}, {});
    FAIL("missing stride must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "input_shape_mismatch");
  }
  REQUIRE(port.actions == before);
  REQUIRE_THROWS_AS(binding.computer("tick", tick_payload(1), {}), BindingError);
}

TEST_CASE("generated binding enforces lifecycle, poisoning, and configuration",
          "[model-interface][generated]") {
  MutableCounter port;
  auto wrong = counter_config();
  wrong.param_vars.clear();
  REQUIRE_THROWS_AS(bind_counter(port, wrong), BindingError);
  REQUIRE(port.actions == 0);
  REQUIRE(port.observations == 0);

  auto binding = bind_counter(port, counter_config());
  try {
    (void)binding.computer("tick", tick_payload(1), {});
    FAIL("transition before init must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "transition_before_initialization");
  }
  REQUIRE(port.actions == 0);
  REQUIRE_THROWS_AS(binding.computer("init", {}, {}), BindingError);
}

TEST_CASE("generated binding classifies adapter and observation failures",
          "[model-interface][generated]") {
  class ThrowingPort final : public CounterPort {
   public:
    void initialize() override { throw std::runtime_error("adapter failed"); }
    void tick(const TickInput&) override {}
    CounterObservation observe() override { return {0}; }
  } throwing;
  auto adapter = bind_counter(throwing, counter_config());
  try {
    (void)adapter.computer("init", {}, {});
    FAIL("adapter throw must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "adapter_failure");
  }

  MutableCounter port;
  port.fail_observation = true;
  auto observation = bind_counter(port, counter_config());
  try {
    (void)observation.computer("init", {}, {});
    FAIL("observer throw must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "observation_shape_mismatch");
  }
  REQUIRE(port.actions == 1);
  REQUIRE(port.observations == 1);
  REQUIRE_THROWS_AS(observation.computer("init", {}, {}), BindingError);
}

TEST_CASE("generated binding cannot be reentered or spoof mechanical errors",
          "[model-interface][generated]") {
  class ReentrantPort final : public CounterPort {
   public:
    void initialize() override {
      try {
        (void)(*computer)("init", {}, {});
      } catch (const BindingError&) {
        // The outer call must still observe the poisoned lifecycle.
      }
    }
    void tick(const TickInput&) override {}
    CounterObservation observe() override { return {0}; }
    StateComputer* computer = nullptr;
  } reentrant;
  auto binding = bind_counter(reentrant, counter_config());
  reentrant.computer = &binding.computer;
  try {
    (void)binding.computer("init", {}, {});
    FAIL("swallowed reentrancy must poison the outer call");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "adapter_failure");
  }
  REQUIRE(binding.coverage().at("Initialize") == 0);
  REQUIRE_THROWS_AS(binding.computer("init", {}, {}), BindingError);

  class SpoofingPort final : public CounterPort {
   public:
    void initialize() override { throw binding_error("unknown_action", "spoofed"); }
    void tick(const TickInput&) override {}
    CounterObservation observe() override { return {0}; }
  } spoofing;
  auto spoofed = bind_counter(spoofing, counter_config());
  try {
    (void)spoofed.computer("init", {}, {});
    FAIL("adapter BindingError must not spoof a mechanical error");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "adapter_failure");
  }
}

TEST_CASE("generated native codecs cover the portable model-interface types",
          "[model-interface][generated][types]") {
  using Int = Value::Int;
  using Flag = RecordField<"flag", bool>;
  using Count = RecordField<"count", Int>;
  using RichRecord = MirrorRecord<Flag, Count>;
  using None = VariantCase<"none", MirrorNull>;
  using Some = VariantCase<"some", Int>;
  using RichVariant = MirrorVariant<None, Some>;

  const MirrorSet<Int> set{{Int{1}, Int{2}}};
  const auto set_wire = encode_native(set, "set");
  REQUIRE(set_wire.is_set());
  REQUIRE(decode_native<MirrorSet<Int>>(set_wire, "set").values.size() == 2);

  const MirrorSeq<std::string> seq{{"first", "second"}};
  const auto seq_wire = encode_native(seq, "seq");
  REQUIRE(seq_wire.is_seq());
  REQUIRE(decode_native<MirrorSeq<std::string>>(seq_wire, "seq").values.at(1) ==
          "second");

  const MirrorTuple<Int, bool> tuple{std::tuple{Int{7}, true}};
  const auto tuple_wire = encode_native(tuple, "tuple");
  REQUIRE(tuple_wire.is_tuple());
  const auto tuple_back = decode_native<MirrorTuple<Int, bool>>(tuple_wire, "tuple");
  REQUIRE(std::get<0>(tuple_back.values) == 7);
  REQUIRE(std::get<1>(tuple_back.values));

  const RichRecord record{std::tuple{Flag{true}, Count{Int{11}}}};
  const auto record_wire = encode_native(record, "record");
  REQUIRE(record_wire.is_record());
  const auto record_back = decode_native<RichRecord>(record_wire, "record");
  REQUIRE(std::get<Flag>(record_back.fields).value);
  REQUIRE(std::get<Count>(record_back.fields).value == 11);

  const MirrorMap<Int> map{{{"one", Int{1}}, {"two", Int{2}}}};
  const auto map_wire = encode_native(map, "map");
  REQUIRE(map_wire.is_map());
  const auto map_back = decode_native<MirrorMap<Int>>(map_wire, "map");
  REQUIRE(map_back.entries.at(1).first == "two");
  REQUIRE(map_back.entries.at(1).second == 2);

  const RichVariant variant{Some{Int{23}}};
  const auto variant_wire = encode_native(variant, "variant");
  REQUIRE(variant_wire.is_variant());
  const auto variant_back = decode_native<RichVariant>(variant_wire, "variant");
  REQUIRE(std::holds_alternative<Some>(variant_back.value));
  REQUIRE(std::get<Some>(variant_back.value).value == 23);

  REQUIRE(encode_native(MirrorNull{}, "null").is_null());
  REQUIRE_NOTHROW(decode_native<MirrorNull>(Value(nullptr), "null"));

  try {
    (void)encode_native(MirrorSet<Int>{{Int{1}, Int{1}}}, "set");
    FAIL("duplicate set observations must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "observation_shape_mismatch");
  }
  try {
    (void)encode_native(MirrorMap<Int>{{{"same", Int{1}}, {"same", Int{2}}}},
                        "map");
    FAIL("duplicate string map keys must fail");
  } catch (const BindingError& error) {
    REQUIRE(error.code() == "observation_shape_mismatch");
  }
  REQUIRE_THROWS_AS(
      decode_native<MirrorSet<Int>>(
          Value(Value::Set{{Value(1), Value(1)}}), "set"),
      BindingError);
  REQUIRE_THROWS_AS(
      decode_native<RichVariant>(
          Value(Value::Variant{"other", Box<Value>(Value(nullptr))}), "variant"),
      BindingError);
}
