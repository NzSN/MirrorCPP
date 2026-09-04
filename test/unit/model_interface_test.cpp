#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace mirrorcpp;

namespace {

constexpr std::string_view digest_hex =
    "193d6cc187d05c18f02ad483a44f8ad0c1634b02083df241df08b9281b045d1c";

constexpr std::string_view counter_contract = R"json({
  "schema":"mirrors.model-interface/v1",
  "interfaceVersion":"1.0.0",
  "model":{"module":"Counter","source":"specs/Counter.tla"},
  "wire":{"actionVariable":"action_taken","parameterVariable":"parameters"},
  "initializers":[{"id":"Initialize","wireAction":"init","wireAliases":[],"inputs":[]}],
  "actions":[{"id":"Tick","wireAction":"tick","wireAliases":[],"inputs":[{"id":"Stride","from":{"root":"stepParameters","path":[{"field":"parameters"},{"field":"stride"}]}}]}],
  "observations":[{"id":"Count","wireName":"count","provenance":"implementation"}]
})json";

class ScriptedTransport final : public Transport {
 public:
  explicit ScriptedTransport(std::vector<std::string> replies)
      : replies_(replies.begin(), replies.end()) {}

  Result<void> send_line(std::string_view line) override {
    sent.emplace_back(line);
    return {};
  }
  Result<std::string> recv_line() override {
    if (replies_.empty()) {
      return std::unexpected(Error(ErrorKind::io, "script exhausted"));
    }
    std::string line = std::move(replies_.front());
    replies_.pop_front();
    return line;
  }
  Result<long> close() override {
    if (!closed_) {
      closed_ = true;
      ++close_calls;
    }
    return 0;
  }

  std::vector<std::string> sent;
  int close_calls = 0;

 private:
  std::deque<std::string> replies_;
  bool closed_ = false;
};

struct Calls {
  int factory = 0;
  int computer = 0;
  int config = 0;
  int dispose = 0;
};

struct SelectionFixture {
  Calls* calls;
  SemanticDigest digest;
  CompiledAdapterRegistry registry;
  CompiledAdapterSelection selection;

  explicit SelectionFixture(Calls& target,
                            NegotiationPolicy policy = NegotiationPolicy::require)
      : calls(&target),
        digest(*semantic_digest_from_hex(digest_hex)),
        registry(std::vector<CompiledAdapterRegistration>{
            CompiledAdapterRegistration{
                CompiledAdapterKey{digest, "counter.mutable/v1",
                    std::string(mirrorcpp_target_profile),
                    std::string(state_computer_contract_version)},
                [this](const ApalacheConfig&) -> Result<LocalBinding> {
                  ++calls->factory;
                  LocalBinding binding;
                  binding.semantic_digest = digest;
                  binding.computer = [this](std::string_view, const State&, const State&) {
                    ++calls->computer;
                    return State{{"count", Value(0)}};
                  };
                  binding.assert_compatible_config = [this](const ApalacheConfig&) {
                    ++calls->config;
                    return Result<void>{};
                  };
                  binding.dispose = [this]() {
                    ++calls->dispose;
                    return Result<void>{};
                  };
                  return binding;
                }}}),
        selection{
            GeneratedModelInterface{std::string(digest_hex), std::string(counter_contract)},
            "counter.mutable/v1",
            std::string(mirrorcpp_target_profile),
            std::string(state_computer_contract_version),
            &registry,
            policy,
            std::nullopt} {}
};

ApalacheConfig config() {
  ApalacheConfig value;
  value.spec_path = "Counter.tla";
  value.invariant = "TraceComplete";
  value.length_bound = 3;
  value.param_vars = "parameters";
  return value;
}

std::string validated(std::string_view status = "matched",
                      std::string_view digest = digest_hex) {
  return nlohmann::json{
      {"proto_step", "spec_validated"},
      {"result", "valid"},
      {"modelInterface", {
          {"schema", model_interface_negotiation_schema},
          {"status", status},
          {"descriptorSchema", model_interface_descriptor_schema},
          {"semanticDigest", "sha256:" + std::string(digest)},
      }},
  }.dump();
}

std::vector<std::string> successful_replay(std::string first) {
  return {
      std::move(first),
      R"({"proto_step":"initial_state","action":"init","state":{}})",
      R"({"proto_step":"step_ok"})",
      R"({"proto_step":"all_steps_done"})",
  };
}

}  // namespace

TEST_CASE("model interface digest is strict and branded at the wire boundary",
          "[model-interface]") {
  auto digest = semantic_digest_from_hex(digest_hex);
  REQUIRE(digest.has_value());
  REQUIRE(semantic_digest_hex(*digest) == digest_hex);
  REQUIRE(render_wire_semantic_digest(*digest) == "sha256:" + std::string(digest_hex));
  auto parsed = parse_wire_semantic_digest(render_wire_semantic_digest(*digest));
  REQUIRE(parsed.has_value());
  REQUIRE(*parsed == *digest);
  REQUIRE_FALSE(semantic_digest_from_hex(std::string(64, 'A')).has_value());
  REQUIRE_FALSE(semantic_digest_from_hex(std::string(63, 'a')).has_value());
  REQUIRE_FALSE(parse_wire_semantic_digest(std::string(digest_hex)).has_value());
}

TEST_CASE("verify request embeds the exact generated contract and digest",
          "[model-interface]") {
  auto request = make_verify_request({std::string(digest_hex), std::string(counter_contract)});
  REQUIRE(request.has_value());
  RegisterTraces registration{config(), {"counter.itf.json"}};
  auto encoded = encode_model_interface_registration(registration, *request);
  REQUIRE(encoded.has_value());
  auto wire = nlohmann::json::parse(*encoded);
  REQUIRE(wire.at("proto_step") == "register_traces");
  REQUIRE(wire.at("modelInterface").at("request") == "verify");
  REQUIRE(wire.at("modelInterface").at("policy") == "require");
  REQUIRE(wire.at("modelInterface").at("expectedSemanticDigest") ==
          "sha256:" + std::string(digest_hex));
  REQUIRE(wire.at("modelInterface").at("contract").at("inline").at("schema") ==
          model_interface_contract_schema);
}

TEST_CASE("generated contract parsing rejects duplicate and unknown versioned fields",
          "[model-interface]") {
  const std::string duplicate =
      R"({"schema":"mirrors.model-interface/v1","schema":"mirrors.model-interface/v1"})";
  auto duplicate_result = make_verify_request({std::string(digest_hex), duplicate});
  REQUIRE_FALSE(duplicate_result.has_value());
  REQUIRE(duplicate_result.error().message.find("duplicate object key") != std::string::npos);

  nlohmann::json contract = nlohmann::json::parse(counter_contract);
  contract["typo"] = true;
  auto unknown = make_verify_request({std::string(digest_hex), contract.dump()});
  REQUIRE_FALSE(unknown.has_value());
  REQUIRE(unknown.error().message.find("unknown field") != std::string::npos);
}

TEST_CASE("matched reply decoding is strict and additive only at the outer message",
          "[model-interface]") {
  nlohmann::json message = nlohmann::json::parse(validated());
  message["futureOuterField"] = true;
  auto decoded = decode_model_interface_registration_reply(message.dump());
  REQUIRE(decoded.has_value());
  REQUIRE(decoded->reply.has_value());
  REQUIRE(decoded->reply->status == ModelInterfaceStatus::matched);
  REQUIRE(decoded->reply->semantic_digest.has_value());
  REQUIRE(*decoded->reply->semantic_digest == *semantic_digest_from_hex(digest_hex));

  std::string duplicate = validated();
  const auto position = duplicate.find("\"status\":\"matched\"");
  REQUIRE(position != std::string::npos);
  duplicate.replace(position, std::string("\"status\":\"matched\"").size(),
                    "\"status\":\"matched\",\"status\":\"unavailable\"");
  auto bad = decode_model_interface_registration_reply(duplicate);
  REQUIRE_FALSE(bad.has_value());
  REQUIRE(bad.error().message.find("duplicate object key") != std::string::npos);

  message = nlohmann::json::parse(validated());
  message["modelInterface"]["typo"] = true;
  REQUIRE_FALSE(decode_model_interface_registration_reply(message.dump()).has_value());
}

TEST_CASE("compiled registry resolves only one exact four-part key",
          "[model-interface]") {
  const auto digest = *semantic_digest_from_hex(digest_hex);
  const CompiledAdapterKey key{digest, "counter", std::string(mirrorcpp_target_profile),
                               std::string(state_computer_contract_version)};
  AdapterFactory factory = [digest](const ApalacheConfig&) -> Result<LocalBinding> {
    LocalBinding binding;
    binding.semantic_digest = digest;
    return binding;
  };
  CompiledAdapterRegistry registry({{key, factory}});
  REQUIRE(registry.resolve(key).has_value());

  auto wrong_target = key;
  wrong_target.target_profile = "other-v1";
  auto target = registry.resolve(wrong_target);
  REQUIRE_FALSE(target.has_value());
  REQUIRE(target.error().code == "target_profile_mismatch");

  CompiledAdapterRegistry ambiguous({{key, factory}, {key, factory}});
  auto duplicate = ambiguous.resolve(key);
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().code == "adapter_ambiguous");
}

TEST_CASE("negotiated runner creates and disposes one binding only after matched",
          "[model-interface]") {
  Calls calls;
  SelectionFixture fixture(calls);
  ScriptedTransport transport(successful_replay(validated()));

  auto result = run_client_with_traces_negotiated(
      transport, config(), {"counter.itf.json"}, fixture.selection);
  REQUIRE(result.has_value());
  REQUIRE(calls.factory == 1);
  REQUIRE(calls.config == 1);
  REQUIRE(calls.computer == 1);
  REQUIRE(calls.dispose == 1);
  REQUIRE(transport.close_calls == 1);
  REQUIRE(transport.sent.size() == 2);
  REQUIRE(transport.sent.front().find("\"modelInterface\"") != std::string::npos);
  REQUIRE(transport.sent.back().find("\"report_state\"") != std::string::npos);
}

TEST_CASE("required missing or wrong negotiation runs no adapter code",
          "[model-interface]") {
  for (const std::string& first : {
      std::string(R"({"proto_step":"spec_validated","result":"valid"})"),
      validated("matched", std::string(64, 'b'))}) {
    Calls calls;
    SelectionFixture fixture(calls);
    ScriptedTransport transport({first});
    auto result = run_client_with_traces_negotiated(
        transport, config(), {"counter.itf.json"}, fixture.selection);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == ErrorKind::model_interface);
    REQUIRE(calls.factory == 0);
    REQUIRE(calls.computer == 0);
    REQUIRE(calls.config == 0);
    REQUIRE(calls.dispose == 0);
    REQUIRE(transport.sent.size() == 1);
    REQUIRE(transport.close_calls == 1);
  }
}

TEST_CASE("prefer uses only an explicit fresh fallback and mismatch never falls back",
          "[model-interface]") {
  Calls primary;
  Calls fallback;
  SelectionFixture fixture(primary, NegotiationPolicy::prefer);
  fixture.selection.fallback_factory = [&fallback, digest = fixture.digest](
      const ApalacheConfig&) -> Result<LocalBinding> {
    ++fallback.factory;
    LocalBinding binding;
    binding.semantic_digest = digest;
    binding.computer = [&fallback](std::string_view, const State&, const State&) {
      ++fallback.computer;
      return State{};
    };
    binding.assert_compatible_config = [&fallback](const ApalacheConfig&) {
      ++fallback.config;
      return Result<void>{};
    };
    binding.dispose = [&fallback]() {
      ++fallback.dispose;
      return Result<void>{};
    };
    return binding;
  };

  ScriptedTransport old_server(successful_replay(
      R"({"proto_step":"spec_validated","result":"valid"})"));
  REQUIRE(run_client_with_traces_negotiated(
      old_server, config(), {"counter.itf.json"}, fixture.selection).has_value());
  REQUIRE(primary.factory == 0);
  REQUIRE(fallback.factory == 1);
  REQUIRE(fallback.dispose == 1);

  Calls mismatch_fallback;
  fixture.selection.fallback_factory = [&mismatch_fallback, digest = fixture.digest](
      const ApalacheConfig&) -> Result<LocalBinding> {
    ++mismatch_fallback.factory;
    LocalBinding binding;
    binding.semantic_digest = digest;
    return binding;
  };
  const nlohmann::json failure = {
      {"proto_step", "register_error"},
      {"error", "model interface digest mismatch"},
      {"modelInterface", {
          {"schema", model_interface_negotiation_schema},
          {"status", "mismatch"},
          {"code", "interface_digest_mismatch"},
          {"expectedSemanticDigest", "sha256:" + std::string(digest_hex)},
      }},
  };
  ScriptedTransport mismatch({failure.dump()});
  auto result = run_client_with_traces_negotiated(
      mismatch, config(), {"counter.itf.json"}, fixture.selection);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::registration);
  REQUIRE(result.error().code == "interface_digest_mismatch");
  REQUIRE(mismatch_fallback.factory == 0);
}

TEST_CASE("binding validation and replay failures dispose exactly once",
          "[model-interface]") {
  Calls calls;
  SelectionFixture fixture(calls);
  fixture.registry = CompiledAdapterRegistry({{
      CompiledAdapterKey{fixture.digest, "counter.mutable/v1",
          std::string(mirrorcpp_target_profile),
          std::string(state_computer_contract_version)},
      [&calls](const ApalacheConfig&) -> Result<LocalBinding> {
        ++calls.factory;
        LocalBinding binding;
        binding.semantic_digest = *semantic_digest_from_hex(std::string(64, 'b'));
        binding.computer = [](std::string_view, const State&, const State&) { return State{}; };
        binding.assert_compatible_config = [](const ApalacheConfig&) { return Result<void>{}; };
        binding.dispose = [&calls]() { ++calls.dispose; return Result<void>{}; };
        return binding;
      }}});
  fixture.selection.registry = &fixture.registry;
  ScriptedTransport transport({validated()});
  auto result = run_client_with_traces_negotiated(
      transport, config(), {"counter.itf.json"}, fixture.selection);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code == "binding_digest_mismatch");
  REQUIRE(calls.factory == 1);
  REQUIRE(calls.dispose == 1);
  REQUIRE(transport.sent.size() == 1);
}

TEST_CASE("negotiated runner converts classified binding throws and disposes",
          "[model-interface]") {
  Calls calls;
  SelectionFixture fixture(calls);
  fixture.registry = CompiledAdapterRegistry({{
      CompiledAdapterKey{fixture.digest, "counter.mutable/v1",
          std::string(mirrorcpp_target_profile),
          std::string(state_computer_contract_version)},
      [&calls, digest = fixture.digest](const ApalacheConfig&) -> Result<LocalBinding> {
        ++calls.factory;
        LocalBinding binding;
        binding.semantic_digest = digest;
        binding.computer = [&calls](std::string_view, const State&, const State&) -> State {
          ++calls.computer;
          throw ModelInterfaceBindingError(
              "input_shape_mismatch", "generated input was malformed");
        };
        binding.assert_compatible_config = [&calls](const ApalacheConfig&) {
          ++calls.config;
          return Result<void>{};
        };
        binding.dispose = [&calls]() {
          ++calls.dispose;
          return Result<void>{};
        };
        return binding;
      }}});
  fixture.selection.registry = &fixture.registry;
  ScriptedTransport transport(successful_replay(validated()));

  auto result = run_client_with_traces_negotiated(
      transport, config(), {"counter.itf.json"}, fixture.selection);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::model_interface);
  REQUIRE(result.error().code == "input_shape_mismatch");
  REQUIRE(calls.factory == 1);
  REQUIRE(calls.computer == 1);
  REQUIRE(calls.dispose == 1);
  REQUIRE(transport.close_calls == 1);
}

TEST_CASE("dispose failure is reported only when there is no primary failure",
          "[model-interface]") {
  auto make_selection = [](Calls& calls) {
    auto fixture = std::make_unique<SelectionFixture>(calls);
    fixture->registry = CompiledAdapterRegistry({{
        CompiledAdapterKey{fixture->digest, "counter.mutable/v1",
            std::string(mirrorcpp_target_profile),
            std::string(state_computer_contract_version)},
        [&calls, digest = fixture->digest](const ApalacheConfig&) -> Result<LocalBinding> {
          ++calls.factory;
          LocalBinding binding;
          binding.semantic_digest = digest;
          binding.computer = [&calls](std::string_view, const State&, const State&) {
            ++calls.computer;
            return State{{"count", Value(0)}};
          };
          binding.assert_compatible_config = [](const ApalacheConfig&) {
            return Result<void>{};
          };
          binding.dispose = [&calls]() -> Result<void> {
            ++calls.dispose;
            return std::unexpected(Error(ErrorKind::io, "cleanup failed"));
          };
          return binding;
        }}});
    fixture->selection.registry = &fixture->registry;
    return fixture;
  };

  Calls success_calls;
  auto success_fixture = make_selection(success_calls);
  ScriptedTransport success_transport(successful_replay(validated()));
  auto cleanup_failure = run_client_with_traces_negotiated(
      success_transport, config(), {"counter.itf.json"},
      success_fixture->selection);
  REQUIRE_FALSE(cleanup_failure.has_value());
  REQUIRE(cleanup_failure.error().code == "adapter_dispose_failed");
  REQUIRE(success_calls.dispose == 1);

  Calls mismatch_calls;
  auto mismatch_fixture = make_selection(mismatch_calls);
  ScriptedTransport mismatch_transport({
      validated(),
      R"({"proto_step":"initial_state","action":"init","state":{}})",
      R"({"proto_step":"step_mismatch","expected":{},"actual":{},"hints":[]})",
  });
  auto primary_failure = run_client_with_traces_negotiated(
      mismatch_transport, config(), {"counter.itf.json"},
      mismatch_fixture->selection);
  REQUIRE_FALSE(primary_failure.has_value());
  REQUIRE(primary_failure.error().kind == ErrorKind::step_mismatch);
  REQUIRE(mismatch_calls.dispose == 1);
}

TEST_CASE("every non-matched verify status fails before adapter construction",
          "[model-interface]") {
  const auto reply = [](std::string_view status) {
    return nlohmann::json{
        {"proto_step", "spec_validated"},
        {"result", "valid"},
        {"modelInterface", {
            {"schema", model_interface_negotiation_schema},
            {"status", status},
        }},
    }.dump();
  };

  for (const std::string& first : {
      reply("resolved"), reply("not_modified"), reply("too_large"),
      reply("mismatch"), reply("unsupported"), reply("unavailable"),
      reply("future_status"), validated("matched", std::string(64, 'A'))}) {
    Calls calls;
    SelectionFixture fixture(calls);
    ScriptedTransport transport({first});
    auto result = run_client_with_traces_negotiated(
        transport, config(), {"counter.itf.json"}, fixture.selection);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(calls.factory == 0);
    REQUIRE(calls.computer == 0);
    REQUIRE(calls.dispose == 0);
    REQUIRE(transport.sent.size() == 1);
    REQUIRE(transport.close_calls == 1);
  }
}

TEST_CASE("missing and ambiguous adapters fail before registration",
          "[model-interface]") {
  Calls missing_calls;
  SelectionFixture missing(missing_calls);
  missing.registry = CompiledAdapterRegistry({});
  missing.selection.registry = &missing.registry;
  ScriptedTransport missing_transport({});
  auto missing_result = run_client_with_traces_negotiated(
      missing_transport, config(), {"counter.itf.json"}, missing.selection);
  REQUIRE_FALSE(missing_result.has_value());
  REQUIRE(missing_result.error().code == "adapter_not_registered");
  REQUIRE(missing_calls.factory == 0);
  REQUIRE(missing_transport.sent.empty());

  Calls ambiguous_calls;
  SelectionFixture ambiguous(ambiguous_calls);
  const CompiledAdapterKey key{
      ambiguous.digest, "counter.mutable/v1", std::string(mirrorcpp_target_profile),
      std::string(state_computer_contract_version)};
  AdapterFactory factory = [&ambiguous_calls](const ApalacheConfig&)
      -> Result<LocalBinding> {
    ++ambiguous_calls.factory;
    return std::unexpected(Error(ErrorKind::io, "must not run"));
  };
  ambiguous.registry = CompiledAdapterRegistry({{key, factory}, {key, factory}});
  ambiguous.selection.registry = &ambiguous.registry;
  ScriptedTransport ambiguous_transport({});
  auto ambiguous_result = run_client_with_traces_negotiated(
      ambiguous_transport, config(), {"counter.itf.json"}, ambiguous.selection);
  REQUIRE_FALSE(ambiguous_result.has_value());
  REQUIRE(ambiguous_result.error().code == "adapter_ambiguous");
  REQUIRE(ambiguous_calls.factory == 0);
  REQUIRE(ambiguous_transport.sent.empty());
}

TEST_CASE("factory and configuration failures never enter replay",
          "[model-interface]") {
  Calls factory_calls;
  SelectionFixture factory_failure(factory_calls);
  factory_failure.registry = CompiledAdapterRegistry({{
      CompiledAdapterKey{factory_failure.digest, "counter.mutable/v1",
          std::string(mirrorcpp_target_profile),
          std::string(state_computer_contract_version)},
      [&factory_calls](const ApalacheConfig&) -> Result<LocalBinding> {
        ++factory_calls.factory;
        return std::unexpected(Error(ErrorKind::io, "construction failed"));
      }}});
  factory_failure.selection.registry = &factory_failure.registry;
  ScriptedTransport factory_transport({validated()});
  auto factory_result = run_client_with_traces_negotiated(
      factory_transport, config(), {"counter.itf.json"},
      factory_failure.selection);
  REQUIRE_FALSE(factory_result.has_value());
  REQUIRE(factory_result.error().code == "adapter_factory_failed");
  REQUIRE(factory_calls.factory == 1);
  REQUIRE(factory_calls.computer == 0);
  REQUIRE(factory_calls.dispose == 0);

  Calls config_calls;
  SelectionFixture config_failure(config_calls);
  config_failure.registry = CompiledAdapterRegistry({{
      CompiledAdapterKey{config_failure.digest, "counter.mutable/v1",
          std::string(mirrorcpp_target_profile),
          std::string(state_computer_contract_version)},
      [&config_calls, digest = config_failure.digest](const ApalacheConfig&)
          -> Result<LocalBinding> {
        ++config_calls.factory;
        LocalBinding binding;
        binding.semantic_digest = digest;
        binding.computer = [&config_calls](std::string_view, const State&, const State&) {
          ++config_calls.computer;
          return State{};
        };
        binding.assert_compatible_config = [](const ApalacheConfig&) {
          return Result<void>(std::unexpected(
              Error(ErrorKind::model_interface, "wrong paramVars")));
        };
        binding.dispose = [&config_calls]() {
          ++config_calls.dispose;
          return Result<void>{};
        };
        return binding;
      }}});
  config_failure.selection.registry = &config_failure.registry;
  ScriptedTransport config_transport({validated()});
  auto config_result = run_client_with_traces_negotiated(
      config_transport, config(), {"counter.itf.json"}, config_failure.selection);
  REQUIRE_FALSE(config_result.has_value());
  REQUIRE(config_result.error().code == "binding_config_mismatch");
  REQUIRE(config_calls.factory == 1);
  REQUIRE(config_calls.computer == 0);
  REQUIRE(config_calls.dispose == 1);
}

TEST_CASE("transport failure after binding construction disposes exactly once",
          "[model-interface]") {
  Calls calls;
  SelectionFixture fixture(calls);
  ScriptedTransport transport({
      validated(),
      R"({"proto_step":"initial_state","action":"init","state":{}})",
  });
  auto result = run_client_with_traces_negotiated(
      transport, config(), {"counter.itf.json"}, fixture.selection);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().kind == ErrorKind::io);
  REQUIRE(calls.factory == 1);
  REQUIRE(calls.computer == 1);
  REQUIRE(calls.dispose == 1);
  REQUIRE(transport.close_calls == 1);
}
