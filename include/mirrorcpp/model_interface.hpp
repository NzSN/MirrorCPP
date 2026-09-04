// mirrorcpp/model_interface.hpp — version-1 compiled model-interface negotiation.
#ifndef MIRRORCPP_MODEL_INTERFACE_HPP
#define MIRRORCPP_MODEL_INTERFACE_HPP

#include <mirrorcpp/error.hpp>
#include <mirrorcpp/protocol.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mirrorcpp {

inline constexpr std::string_view model_interface_contract_schema =
    "mirrors.model-interface/v1";
inline constexpr std::string_view model_interface_negotiation_schema =
    "mirrors.model-interface-negotiation/v1";
inline constexpr std::string_view model_interface_descriptor_schema =
    "mirrors.model-interface-descriptor/v1";
inline constexpr std::string_view mirrorcpp_target_profile = "mirrorcpp-v1";
inline constexpr std::string_view state_computer_contract_version =
    "mirrors.state-computer/v1";

class SemanticDigest {
 public:
  SemanticDigest() = default;
  explicit SemanticDigest(std::array<std::uint8_t, 32> bytes) : bytes_(bytes) {}

  const std::array<std::uint8_t, 32>& bytes() const noexcept { return bytes_; }
  bool operator==(const SemanticDigest&) const = default;

 private:
  std::array<std::uint8_t, 32> bytes_{};
};

Result<SemanticDigest> semantic_digest_from_hex(std::string_view hex);
Result<SemanticDigest> parse_wire_semantic_digest(std::string_view wire);
std::string semantic_digest_hex(const SemanticDigest& digest);
std::string render_wire_semantic_digest(const SemanticDigest& digest);

enum class NegotiationPolicy { require, prefer };
enum class ModelInterfaceStatus {
  matched,
  resolved,
  not_modified,
  mismatch,
  unsupported,
  unavailable,
  too_large,
};

struct GeneratedModelInterface {
  std::string semantic_digest;
  std::string contract_json;
};

// Generated StateComputer bindings throw this classified internal exception.
// Negotiated public entry points convert it back to ErrorKind::model_interface
// after disposing the session-local binding.
class ModelInterfaceBindingError : public std::runtime_error {
 public:
  ModelInterfaceBindingError(std::string code, std::string message)
      : std::runtime_error(std::move(message)), code_(std::move(code)) {}
  const std::string& code() const noexcept { return code_; }

 private:
  std::string code_;
};

struct ModelInterfaceVerifyRequest {
  NegotiationPolicy policy = NegotiationPolicy::require;
  SemanticDigest expected_semantic_digest;
  nlohmann::json contract;
};

struct ModelInterfaceReply {
  ModelInterfaceStatus status = ModelInterfaceStatus::matched;
  std::optional<std::string> descriptor_schema;
  std::optional<SemanticDigest> semantic_digest;
  std::optional<SemanticDigest> provenance_digest;
};

struct ModelInterfaceFailure {
  ModelInterfaceStatus status = ModelInterfaceStatus::mismatch;
  std::string code;
  std::optional<SemanticDigest> expected_semantic_digest;
  std::optional<SemanticDigest> actual_semantic_digest;
  std::optional<SemanticDigest> provenance_digest;
};

struct DecodedModelInterfaceRegistrationReply {
  MirrorMessage message;
  std::optional<ModelInterfaceReply> reply;
  std::optional<ModelInterfaceFailure> failure;
};

Result<ModelInterfaceVerifyRequest> make_verify_request(
    const GeneratedModelInterface& metadata,
    NegotiationPolicy policy = NegotiationPolicy::require);
Result<std::string> encode_model_interface_registration(
    const Register& registration, const ModelInterfaceVerifyRequest& request);
Result<std::string> encode_model_interface_registration(
    const RegisterTraces& registration, const ModelInterfaceVerifyRequest& request);
Result<DecodedModelInterfaceRegistrationReply>
decode_model_interface_registration_reply(std::string_view line);

struct CompiledAdapterKey {
  SemanticDigest semantic_digest;
  std::string adapter_id;
  std::string target_profile;
  std::string state_computer_contract_version;
  bool operator==(const CompiledAdapterKey&) const = default;
};

struct LocalBinding {
  SemanticDigest semantic_digest;
  StateComputer computer;
  std::function<Result<void>(const ApalacheConfig&)> assert_compatible_config;
  std::function<Result<void>()> dispose;

  LocalBinding() = default;
  LocalBinding(const LocalBinding&) = delete;
  LocalBinding& operator=(const LocalBinding&) = delete;
  LocalBinding(LocalBinding&&) noexcept = default;
  LocalBinding& operator=(LocalBinding&&) noexcept = default;
};

using AdapterFactory =
    std::function<Result<LocalBinding>(const ApalacheConfig&)>;

struct CompiledAdapterRegistration {
  CompiledAdapterKey key;
  AdapterFactory factory;
};

class CompiledAdapterRegistry {
 public:
  explicit CompiledAdapterRegistry(
      std::vector<CompiledAdapterRegistration> registrations);

  Result<const AdapterFactory*> resolve(const CompiledAdapterKey& key) const;

 private:
  std::vector<CompiledAdapterRegistration> registrations_;
};

struct CompiledAdapterSelection {
  GeneratedModelInterface metadata;
  std::string adapter_id;
  std::string target_profile = std::string(mirrorcpp_target_profile);
  std::string state_computer_contract_version =
      std::string(mirrorcpp::state_computer_contract_version);
  const CompiledAdapterRegistry* registry = nullptr;
  NegotiationPolicy policy = NegotiationPolicy::require;
  std::optional<AdapterFactory> fallback_factory;
};

}  // namespace mirrorcpp

#endif  // MIRRORCPP_MODEL_INTERFACE_HPP
