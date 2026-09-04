// mirrorcpp/model_interface.cpp — strict compiled verification codecs and registry.
#include <mirrorcpp/model_interface.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace mirrorcpp {

using nlohmann::json;
using std::unexpected;

namespace {

Error mi_error(std::string code, std::string message) {
  return Error(ErrorKind::model_interface, std::move(message), std::move(code));
}

struct StrictJsonFailure : std::runtime_error {
  using std::runtime_error::runtime_error;
};

Result<json> strict_parse(std::string_view text) {
  if (text.size() > 65'535) {
    return unexpected(mi_error(
        "negotiation_status_unexpected",
        "strict JSON exceeds the 65535-byte protocol payload limit"));
  }
  std::vector<std::unordered_set<std::string>> object_keys;
  std::size_t containers = 0;
  try {
    auto callback = [&](int, json::parse_event_t event, json& parsed) {
      switch (event) {
        case json::parse_event_t::object_start:
          if (++containers > 128) throw StrictJsonFailure("JSON nesting exceeds 128");
          object_keys.emplace_back();
          break;
        case json::parse_event_t::array_start:
          if (++containers > 128) throw StrictJsonFailure("JSON nesting exceeds 128");
          break;
        case json::parse_event_t::object_end:
          if (!object_keys.empty()) object_keys.pop_back();
          if (containers > 0) --containers;
          break;
        case json::parse_event_t::array_end:
          if (containers > 0) --containers;
          break;
        case json::parse_event_t::key: {
          if (object_keys.empty()) throw StrictJsonFailure("object key outside object");
          const std::string key = parsed.get<std::string>();
          if (!object_keys.back().insert(key).second) {
            throw StrictJsonFailure("duplicate object key '" + key + "'");
          }
          break;
        }
        case json::parse_event_t::value:
          break;
      }
      return true;
    };
    return json::parse(text.begin(), text.end(), callback, true, false);
  } catch (const std::exception& error) {
    return unexpected(mi_error(
        "negotiation_status_unexpected",
        std::string("strict JSON decode failed: ") + error.what()));
  }
}

Result<void> exact_object(const json& value, std::string_view path,
                          std::initializer_list<std::string_view> allowed,
                          std::initializer_list<std::string_view> required) {
  if (!value.is_object()) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": object expected"));
  }
  for (const auto& [key, _] : value.items()) {
    const bool known = std::ranges::any_of(allowed, [&](std::string_view item) {
      return item == key;
    });
    if (!known) {
      return unexpected(mi_error("negotiation_status_unexpected",
          std::string(path) + ": unknown field '" + key + "'"));
    }
  }
  for (std::string_view key : required) {
    if (!value.contains(std::string(key))) {
      return unexpected(mi_error("negotiation_status_unexpected",
          std::string(path) + ": missing required field '" + std::string(key) + "'"));
    }
  }
  return {};
}

Result<std::string> string_field(const json& object, std::string_view key,
                                 std::string_view path) {
  const auto it = object.find(std::string(key));
  if (it == object.end() || !it->is_string()) {
    return unexpected(mi_error("negotiation_status_unexpected",
        std::string(path) + "." + std::string(key) + ": string expected"));
  }
  return it->get<std::string>();
}

Result<std::optional<SemanticDigest>> optional_digest(
    const json& object, std::string_view key, std::string_view path) {
  const auto it = object.find(std::string(key));
  if (it == object.end() || it->is_null()) return std::optional<SemanticDigest>{};
  if (!it->is_string()) {
    return unexpected(mi_error("descriptor_digest_invalid",
        std::string(path) + "." + std::string(key) + ": string expected"));
  }
  auto digest = parse_wire_semantic_digest(it->get_ref<const std::string&>());
  if (!digest) return unexpected(digest.error());
  return std::optional<SemanticDigest>{*digest};
}

Result<void> short_string(const json& value, std::string_view path) {
  if (!value.is_string()) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": string expected"));
  }
  if (value.get_ref<const std::string&>().size() > 256) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": exceeds 256 UTF-8 bytes"));
  }
  return {};
}

Result<void> validate_model_type(const json& value, std::size_t depth,
                                 std::size_t& nodes, std::string_view path) {
  if (depth >= 32) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": type depth exceeds 32"));
  }
  if (++nodes > 8'192) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": type nodes exceed 8192"));
  }
  if (!value.is_object() || !value.contains("kind") || !value.at("kind").is_string()) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": tagged model type expected"));
  }
  const std::string kind = value.at("kind").get<std::string>();
  if (kind == "int" || kind == "bool" || kind == "str" || kind == "null") {
    return exact_object(value, path, {"kind"}, {"kind"});
  }
  if (kind == "set" || kind == "seq") {
    if (auto result = exact_object(value, path, {"kind", "element"},
                                   {"kind", "element"}); !result) return result;
    return validate_model_type(value.at("element"), depth + 1, nodes,
                               std::string(path) + ".element");
  }
  if (kind == "tuple") {
    if (auto result = exact_object(value, path, {"kind", "elements"},
                                   {"kind", "elements"}); !result) return result;
    if (!value.at("elements").is_array()) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ".elements: array expected"));
    }
    for (std::size_t i = 0; i < value.at("elements").size(); ++i) {
      if (auto result = validate_model_type(value.at("elements")[i], depth + 1,
          nodes, std::string(path) + ".elements[]"); !result) return result;
    }
    return {};
  }
  if (kind == "record" || kind == "variant") {
    const char* list_name = kind == "record" ? "fields" : "cases";
    if (auto result = exact_object(value, path, {"kind", list_name},
                                   {"kind", list_name}); !result) return result;
    const auto& entries = value.at(list_name);
    if (!entries.is_array()) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ": array expected"));
    }
    std::set<std::string> names;
    for (const auto& entry : entries) {
      const char* name = kind == "record" ? "wireName" : "tag";
      const char* payload = kind == "record" ? "type" : "payload";
      if (auto result = exact_object(entry, path, {name, payload}, {name, payload});
          !result) return result;
      if (auto result = short_string(entry.at(name), path); !result) return result;
      const std::string item = entry.at(name).get<std::string>();
      if (item.empty() || !names.insert(item).second) {
        return unexpected(mi_error("negotiation_status_unexpected",
                                   std::string(path) + ": duplicate or empty label"));
      }
      if (auto result = validate_model_type(entry.at(payload), depth + 1, nodes,
                                            path); !result) return result;
    }
    return {};
  }
  if (kind == "map") {
    if (auto result = exact_object(value, path, {"kind", "key", "value"},
                                   {"kind", "key", "value"}); !result) return result;
    if (auto result = validate_model_type(value.at("key"), depth + 1, nodes, path);
        !result) return result;
    return validate_model_type(value.at("value"), depth + 1, nodes, path);
  }
  if (kind == "opaqueItf") {
    if (auto result = exact_object(value, path, {"kind", "description"},
                                   {"kind", "description"}); !result) return result;
    return short_string(value.at("description"), path);
  }
  return unexpected(mi_error("negotiation_status_unexpected",
                             std::string(path) + ": unknown model type " + kind));
}

Result<void> validate_path_segment(const json& value, std::string_view path) {
  if (!value.is_object() || value.size() != 1) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": exactly one selector required"));
  }
  const auto selector = value.begin();
  const std::string key = selector.key();
  const auto& payload = selector.value();
  if (key == "field" || key == "variantValue") return short_string(payload, path);
  if (key == "index") {
    if (!payload.is_number_unsigned() &&
        !(payload.is_number_integer() && payload.get<long long>() >= 0)) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ": nonnegative index expected"));
    }
    return {};
  }
  if (key == "mapKey") {
    // Static mirrorcpp-v1 rejects map-key paths during target emission, but the
    // contract grammar remains strict and data-only here.
    if (!payload.is_object()) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ": canonical literal expected"));
    }
    return {};
  }
  return unexpected(mi_error("negotiation_status_unexpected",
                             std::string(path) + ": unknown path selector"));
}

Result<void> validate_contract_action(const json& value, std::string_view path,
                                      std::size_t& nodes) {
  if (auto result = exact_object(value, path,
      {"id", "wireAction", "wireAliases", "inputs"},
      {"id", "wireAction", "wireAliases", "inputs"}); !result) return result;
  if (auto result = short_string(value.at("id"), path); !result) return result;
  if (auto result = short_string(value.at("wireAction"), path); !result) return result;
  if (!value.at("wireAliases").is_array() || value.at("wireAliases").size() > 16 ||
      !value.at("inputs").is_array() || value.at("inputs").size() > 128) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string(path) + ": action resource limit exceeded"));
  }
  for (const auto& alias : value.at("wireAliases")) {
    if (auto result = short_string(alias, path); !result) return result;
  }
  for (const auto& input : value.at("inputs")) {
    if (auto result = exact_object(input, path, {"id", "from", "expectedType"},
                                   {"id", "from"}); !result) return result;
    if (auto result = short_string(input.at("id"), path); !result) return result;
    const auto& from = input.at("from");
    if (auto result = exact_object(from, path, {"root", "path"}, {"root", "path"});
        !result) return result;
    auto root = string_field(from, "root", path);
    if (!root || (*root != "initialState" && *root != "stepParameters")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ": invalid path root"));
    }
    if (!from.at("path").is_array() || from.at("path").size() > 32) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 std::string(path) + ": path limit exceeded"));
    }
    for (const auto& segment : from.at("path")) {
      if (auto result = validate_path_segment(segment, path); !result) return result;
    }
    if (input.contains("expectedType") && !input.at("expectedType").is_null()) {
      if (auto result = validate_model_type(input.at("expectedType"), 0, nodes, path);
          !result) return result;
    }
  }
  return {};
}

Result<void> validate_contract(const json& value) {
  constexpr std::string_view path = "modelInterface.contract.inline";
  if (auto result = exact_object(value, path,
      {"schema", "interfaceVersion", "model", "wire", "initializers", "actions", "observations"},
      {"schema", "interfaceVersion", "model", "wire", "initializers", "actions", "observations"});
      !result) return result;
  auto schema = string_field(value, "schema", path);
  if (!schema || *schema != model_interface_contract_schema) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "modelInterface contract schema is unsupported"));
  }
  if (auto result = short_string(value.at("interfaceVersion"), path); !result) return result;
  if (auto result = exact_object(value.at("model"), path, {"module", "source"},
                                 {"module", "source"}); !result) return result;
  if (auto result = short_string(value.at("model").at("module"), path); !result) return result;
  if (auto result = short_string(value.at("model").at("source"), path); !result) return result;
  if (auto result = exact_object(value.at("wire"), path,
      {"actionVariable", "parameterVariable"},
      {"actionVariable", "parameterVariable"}); !result) return result;
  if (auto result = short_string(value.at("wire").at("actionVariable"), path); !result) return result;
  const auto& parameter = value.at("wire").at("parameterVariable");
  if (!parameter.is_null()) {
    if (auto result = short_string(parameter, path); !result) return result;
  }
  if (!value.at("initializers").is_array() || value.at("initializers").size() > 32 ||
      !value.at("actions").is_array() || value.at("actions").size() > 256 ||
      !value.at("observations").is_array() || value.at("observations").size() > 1'024) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "modelInterface contract resource limit exceeded"));
  }
  std::size_t nodes = 0;
  for (const auto& action : value.at("initializers")) {
    if (auto result = validate_contract_action(action, path, nodes); !result) return result;
  }
  for (const auto& action : value.at("actions")) {
    if (auto result = validate_contract_action(action, path, nodes); !result) return result;
  }
  for (const auto& observation : value.at("observations")) {
    if (auto result = exact_object(observation, path,
        {"id", "wireName", "provenance", "expectedType"},
        {"id", "wireName", "provenance"}); !result) return result;
    if (auto result = short_string(observation.at("id"), path); !result) return result;
    if (auto result = short_string(observation.at("wireName"), path); !result) return result;
    auto provenance = string_field(observation, "provenance", path);
    if (!provenance || (*provenance != "implementation" && *provenance != "oracle" &&
                        *provenance != "derived")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "invalid observation provenance"));
    }
    if (observation.contains("expectedType") && !observation.at("expectedType").is_null()) {
      if (auto result = validate_model_type(observation.at("expectedType"), 0, nodes, path);
          !result) return result;
    }
  }
  return {};
}

Result<ModelInterfaceStatus> decode_status(const json& object) {
  auto status = string_field(object, "status", "modelInterface");
  if (!status) return unexpected(status.error());
  if (*status == "matched") return ModelInterfaceStatus::matched;
  if (*status == "resolved") return ModelInterfaceStatus::resolved;
  if (*status == "not_modified") return ModelInterfaceStatus::not_modified;
  if (*status == "mismatch") return ModelInterfaceStatus::mismatch;
  if (*status == "unsupported") return ModelInterfaceStatus::unsupported;
  if (*status == "unavailable") return ModelInterfaceStatus::unavailable;
  if (*status == "too_large") return ModelInterfaceStatus::too_large;
  return unexpected(mi_error("negotiation_status_unexpected",
                             "unknown model-interface status: " + *status));
}

bool absent(const json& object, std::string_view key) {
  auto it = object.find(std::string(key));
  return it == object.end() || it->is_null();
}

Result<ModelInterfaceReply> decode_reply(const json& extension) {
  if (auto result = exact_object(extension, "modelInterface",
      {"schema", "status", "descriptorSchema", "semanticDigest", "provenanceDigest",
       "descriptorBytes", "descriptor"}, {"schema", "status"}); !result) {
    return unexpected(result.error());
  }
  auto schema = string_field(extension, "schema", "modelInterface");
  if (!schema || *schema != model_interface_negotiation_schema) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "unsupported model-interface negotiation schema"));
  }
  auto status = decode_status(extension);
  if (!status) return unexpected(status.error());
  ModelInterfaceReply reply;
  reply.status = *status;
  auto semantic = optional_digest(extension, "semanticDigest", "modelInterface");
  if (!semantic) return unexpected(semantic.error());
  auto provenance = optional_digest(extension, "provenanceDigest", "modelInterface");
  if (!provenance) return unexpected(provenance.error());
  reply.semantic_digest = *semantic;
  reply.provenance_digest = *provenance;
  if (!absent(extension, "descriptorSchema")) {
    auto descriptor_schema = string_field(extension, "descriptorSchema", "modelInterface");
    if (!descriptor_schema) return unexpected(descriptor_schema.error());
    reply.descriptor_schema = *descriptor_schema;
  }

  if (*status == ModelInterfaceStatus::matched) {
    if (!reply.descriptor_schema || *reply.descriptor_schema != model_interface_descriptor_schema ||
        !reply.semantic_digest || !absent(extension, "descriptor") ||
        !absent(extension, "descriptorBytes")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "invalid matched model-interface reply"));
    }
    return reply;
  }
  if (*status == ModelInterfaceStatus::unsupported ||
      *status == ModelInterfaceStatus::unavailable) {
    if (reply.descriptor_schema || reply.semantic_digest || reply.provenance_digest ||
        !absent(extension, "descriptor") || !absent(extension, "descriptorBytes")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "unavailable negotiation carried descriptor identity"));
    }
    return reply;
  }
  if (*status == ModelInterfaceStatus::mismatch) {
    if (!absent(extension, "descriptor") || !absent(extension, "descriptorBytes")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "mismatch reply carried descriptor data"));
    }
    return reply;
  }
  return unexpected(mi_error(
      "negotiation_status_unexpected",
      "descriptor-mode status is invalid for a compiled verify client"));
}

Result<ModelInterfaceFailure> decode_failure(const json& extension) {
  if (auto result = exact_object(extension, "modelInterface",
      {"schema", "status", "code", "expectedSemanticDigest", "actualSemanticDigest",
       "provenanceDigest", "descriptorBytes"}, {"schema", "status", "code"}); !result) {
    return unexpected(result.error());
  }
  auto schema = string_field(extension, "schema", "modelInterface");
  if (!schema || *schema != model_interface_negotiation_schema) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "unsupported model-interface failure schema"));
  }
  auto status = decode_status(extension);
  if (!status) return unexpected(status.error());
  if (*status != ModelInterfaceStatus::mismatch &&
      *status != ModelInterfaceStatus::unsupported &&
      *status != ModelInterfaceStatus::unavailable) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "unexpected status on verify register_error"));
  }
  auto code = string_field(extension, "code", "modelInterface");
  if (!code || code->empty()) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "model-interface failure code is empty"));
  }
  ModelInterfaceFailure failure;
  failure.status = *status;
  failure.code = *code;
  auto expected = optional_digest(extension, "expectedSemanticDigest", "modelInterface");
  auto actual = optional_digest(extension, "actualSemanticDigest", "modelInterface");
  auto provenance = optional_digest(extension, "provenanceDigest", "modelInterface");
  if (!expected) return unexpected(expected.error());
  if (!actual) return unexpected(actual.error());
  if (!provenance) return unexpected(provenance.error());
  failure.expected_semantic_digest = *expected;
  failure.actual_semantic_digest = *actual;
  failure.provenance_digest = *provenance;
  if (*status == ModelInterfaceStatus::mismatch && !failure.expected_semantic_digest) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "mismatch failure lacks expectedSemanticDigest"));
  }
  return failure;
}

json request_json(const ModelInterfaceVerifyRequest& request) {
  return json{
      {"schema", model_interface_negotiation_schema},
      {"request", "verify"},
      {"policy", request.policy == NegotiationPolicy::require ? "require" : "prefer"},
      {"acceptDescriptorSchemas", json::array({model_interface_descriptor_schema})},
      {"expectedSemanticDigest", render_wire_semantic_digest(request.expected_semantic_digest)},
      {"contract", json{{"inline", request.contract}}},
  };
}

template <class Registration>
Result<std::string> encode_registration(const Registration& registration,
                                        const ModelInterfaceVerifyRequest& request) {
  try {
    json outer = json::parse(encode_client_message(ClientMessage{registration}));
    if (outer.contains("modelInterface")) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "registration already contains modelInterface"));
    }
    outer["modelInterface"] = request_json(request);
    std::string encoded = outer.dump();
    if (encoded.size() > 65'535) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "model-interface registration exceeds 65535 bytes"));
    }
    return encoded;
  } catch (const std::exception& error) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               std::string("cannot encode registration: ") + error.what()));
  }
}

}  // namespace

Result<SemanticDigest> semantic_digest_from_hex(std::string_view hex) {
  if (hex.size() != 64) {
    return unexpected(mi_error("descriptor_digest_invalid",
                               "semantic digest must contain 64 lowercase hex characters"));
  }
  std::array<std::uint8_t, 32> bytes{};
  auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
    return -1;
  };
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    const int high = nibble(hex[index * 2]);
    const int low = nibble(hex[index * 2 + 1]);
    if (high < 0 || low < 0) {
      return unexpected(mi_error("descriptor_digest_invalid",
                                 "semantic digest must contain lowercase hexadecimal"));
    }
    bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
  }
  return SemanticDigest(bytes);
}

Result<SemanticDigest> parse_wire_semantic_digest(std::string_view wire) {
  constexpr std::string_view prefix = "sha256:";
  if (!wire.starts_with(prefix)) {
    return unexpected(mi_error("descriptor_digest_invalid",
                               "semantic digest must start with sha256:"));
  }
  return semantic_digest_from_hex(wire.substr(prefix.size()));
}

std::string semantic_digest_hex(const SemanticDigest& digest) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (std::uint8_t byte : digest.bytes()) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 0x0f]);
  }
  return result;
}

std::string render_wire_semantic_digest(const SemanticDigest& digest) {
  return "sha256:" + semantic_digest_hex(digest);
}

Result<ModelInterfaceVerifyRequest> make_verify_request(
    const GeneratedModelInterface& metadata, NegotiationPolicy policy) {
  auto digest = semantic_digest_from_hex(metadata.semantic_digest);
  if (!digest) return unexpected(digest.error());
  auto contract = strict_parse(metadata.contract_json);
  if (!contract) return unexpected(contract.error());
  if (auto valid = validate_contract(*contract); !valid) return unexpected(valid.error());
  return ModelInterfaceVerifyRequest{policy, *digest, std::move(*contract)};
}

Result<std::string> encode_model_interface_registration(
    const Register& registration, const ModelInterfaceVerifyRequest& request) {
  return encode_registration(registration, request);
}

Result<std::string> encode_model_interface_registration(
    const RegisterTraces& registration, const ModelInterfaceVerifyRequest& request) {
  return encode_registration(registration, request);
}

Result<DecodedModelInterfaceRegistrationReply>
decode_model_interface_registration_reply(std::string_view line) {
  auto raw = strict_parse(line);
  if (!raw) return unexpected(raw.error());
  if (!raw->is_object()) {
    return unexpected(mi_error("negotiation_status_unexpected",
                               "mirror registration reply must be an object"));
  }
  auto legacy = decode_mirror_message(line);
  if (!legacy) return unexpected(legacy.error());
  DecodedModelInterfaceRegistrationReply decoded{std::move(*legacy), std::nullopt,
                                                   std::nullopt};
  const auto extension = raw->find("modelInterface");
  if (extension == raw->end() || extension->is_null()) return decoded;
  auto tag = string_field(*raw, "proto_step", "mirror message");
  if (!tag) return unexpected(tag.error());
  if (*tag == "spec_validated") {
    auto reply = decode_reply(*extension);
    if (!reply) return unexpected(reply.error());
    const auto* validated = std::get_if<SpecValidated>(&decoded.message);
    if (!validated || !validated->is_valid()) {
      return unexpected(mi_error("negotiation_status_unexpected",
                                 "negotiation reply requires a valid specification"));
    }
    decoded.reply = std::move(*reply);
    return decoded;
  }
  if (*tag == "register_error") {
    auto failure = decode_failure(*extension);
    if (!failure) return unexpected(failure.error());
    decoded.failure = std::move(*failure);
    return decoded;
  }
  return unexpected(mi_error("negotiation_status_unexpected",
                             "modelInterface is allowed only on registration replies"));
}

CompiledAdapterRegistry::CompiledAdapterRegistry(
    std::vector<CompiledAdapterRegistration> registrations)
    : registrations_(std::move(registrations)) {}

Result<const AdapterFactory*> CompiledAdapterRegistry::resolve(
    const CompiledAdapterKey& key) const {
  std::vector<const CompiledAdapterRegistration*> exact;
  for (const auto& registration : registrations_) {
    if (registration.key == key) exact.push_back(&registration);
  }
  if (exact.size() > 1) {
    return unexpected(mi_error("adapter_ambiguous",
                               "multiple exact adapters are registered for " + key.adapter_id));
  }
  if (exact.size() == 1) return &exact.front()->factory;

  bool identity = false;
  bool target = false;
  for (const auto& registration : registrations_) {
    if (registration.key.semantic_digest == key.semantic_digest &&
        registration.key.adapter_id == key.adapter_id) {
      identity = true;
      if (registration.key.target_profile == key.target_profile) target = true;
    }
  }
  if (identity && !target) {
    return unexpected(mi_error("target_profile_mismatch",
                               "adapter target profile does not match " + key.target_profile));
  }
  if (target) {
    return unexpected(mi_error("state_computer_contract_mismatch",
                               "adapter StateComputer contract does not match " +
                                   key.state_computer_contract_version));
  }
  return unexpected(mi_error("adapter_not_registered",
                             "adapter is not registered: " + key.adapter_id));
}

}  // namespace mirrorcpp
