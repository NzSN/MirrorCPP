// mirrorcpp/protocol.hpp — message types, encode/decode, diff hints, phase guard (design §5.2).
//
// Message structs mirror the §5.2 sketches; wire field names are EXACTLY the
// §3.2 names (camelCase in JSON, snake_case in C++). Messages are single-line
// JSON objects with a "proto_step" string discriminant (§3.1). State fields
// are serialized via encode_state / decode_state (never a generic message
// encoder — see the double-wrap warning in §5.2).
//
// DiffHint is defined here, completing the forward declaration in error.hpp.
#ifndef MIRRORCPP_PROTOCOL_HPP
#define MIRRORCPP_PROTOCOL_HPP

#include <mirrorcpp/error.hpp>
#include <mirrorcpp/value.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mirrorcpp {

// ---------------------------------------------------------------------------
// Status enums (§3.2)
// ---------------------------------------------------------------------------
enum class TransitionStatus { enabled, disabled, unknown };
enum class InvariantStatus { satisfied, violated, unknown };

// ---------------------------------------------------------------------------
// Config + spec structs (§5.2)
// ---------------------------------------------------------------------------
struct ApalacheConfig {
  std::string spec_path;                          // ALWAYS serialized, even with inline spec
  std::optional<std::string> init_predicate, next_predicate, const_init;
  std::string invariant;                          // "" allowed
  long long length_bound = 10;
  std::string param_vars;                         // "" allowed; comma-separated names
  bool operator==(const ApalacheConfig&) const = default;
};

struct TraceGenerationConfig {
  long long num_traces = 1;
  std::optional<std::string> view;
  bool operator==(const TraceGenerationConfig&) const = default;
};

struct ApalacheSpec {
  std::vector<std::string> sources;               // sources[0] = root module
  bool operator==(const ApalacheSpec&) const = default;
};

// ---------------------------------------------------------------------------
// Diff hints + path segments (§5.2)
// ---------------------------------------------------------------------------
enum class DiffHintKind {
  value_mismatch,   // path: expected value X, got Y
  missing,          // expected key/elem absent in actual
  extra,            // actual key/elem not in expected
  missing_elem,     // set/seq element missing
  extra_elem,       // set/seq element extra
  type_mismatch,    // expected type T, got type U
  truncated,        // marker: hint list was capped by the mirror
};

struct PathSeg {
  std::variant<std::string, std::int64_t> seg;    // field name or array index
  static PathSeg field(std::string f) { return PathSeg{std::move(f)}; }
  static PathSeg index(std::int64_t i) { return PathSeg{i}; }
  bool is_field() const { return std::holds_alternative<std::string>(seg); }
  bool is_index() const { return std::holds_alternative<std::int64_t>(seg); }
  // Render one segment: "field" for fields, "[i]" for indices.
  std::string render() const;
  bool operator==(const PathSeg&) const = default;
};

// Completes the forward declaration in error.hpp. Wire shape:
// {"path": [...], "kind": "value_mismatch", "expected"?: …, "actual"?: …}.
// The `truncated` kind is a bare {"kind": "truncated"} marker.
struct DiffHint {
  DiffHintKind kind;
  std::vector<PathSeg> path;
  std::optional<Value> expected;   // value_mismatch/type_mismatch/missing_elem
  std::optional<Value> actual;     // value_mismatch/type_mismatch/extra_elem
  bool operator==(const DiffHint&) const = default;
};

// ---------------------------------------------------------------------------
// Client -> mirror messages (§3.2 / §5.2)
// ---------------------------------------------------------------------------
struct Register {
  ApalacheConfig cfg;
  TraceGenerationConfig tc;
  std::optional<ApalacheSpec> spec;
  bool operator==(const Register&) const = default;
};
struct RegisterTraces {
  ApalacheConfig cfg;
  std::vector<std::string> itf_trace_paths;
  bool operator==(const RegisterTraces&) const = default;
};
struct RegisterTraceGen {
  ApalacheConfig cfg;
  TraceGenerationConfig tc;
  std::optional<std::string> dest_path;
  std::optional<ApalacheSpec> spec;
  bool operator==(const RegisterTraceGen&) const = default;
};
struct RegisterExplore {
  ApalacheSpec spec;
  std::vector<std::string> invariants;
  std::vector<std::string> exports;
  std::optional<long long> max_steps;   // wire: maxSteps? (omitted = mirror default)
  bool operator==(const RegisterExplore&) const = default;
};
struct RegisterExploreSession {
  ApalacheSpec spec;
  std::vector<std::string> invariants;
  std::vector<std::string> exports;
  bool operator==(const RegisterExploreSession&) const = default;
};
struct RegisterValidate {
  ApalacheConfig cfg;
  long long bound;
  std::optional<ApalacheSpec> spec;
  bool operator==(const RegisterValidate&) const = default;
};
struct ExploreAssumeTransition { long long transition_id; bool operator==(const ExploreAssumeTransition&) const = default; };
struct ExploreNextStep { bool operator==(const ExploreNextStep&) const = default; };
struct ExploreQueryState { bool operator==(const ExploreQueryState&) const = default; };
struct ExploreCheckInvariant { long long invariant_id; bool operator==(const ExploreCheckInvariant&) const = default; };
struct ExploreAssumeState { State state; bool operator==(const ExploreAssumeState&) const = default; };
struct ExploreRollback { long long snapshot_id; bool operator==(const ExploreRollback&) const = default; };
struct ExploreDone { bool operator==(const ExploreDone&) const = default; };
struct ReportState { State state; bool operator==(const ReportState&) const = default; };

using ClientMessage = std::variant<
    Register, RegisterTraces, RegisterTraceGen, RegisterExplore,
    RegisterExploreSession, RegisterValidate, ExploreAssumeTransition,
    ExploreNextStep, ExploreQueryState, ExploreCheckInvariant,
    ExploreAssumeState, ExploreRollback, ExploreDone, ReportState>;

// ---------------------------------------------------------------------------
// Mirror -> client messages (§3.2 / §5.2)
// ---------------------------------------------------------------------------
struct SpecValidated {
  // monostate = "valid"; string = invalid reason text (wire: {"invalid": text})
  std::variant<std::monostate, std::string> result;
  bool is_valid() const { return std::holds_alternative<std::monostate>(result); }
  const std::string* invalid_text() const {
    return std::holds_alternative<std::string>(result) ? &std::get<std::string>(result) : nullptr;
  }
  bool operator==(const SpecValidated&) const = default;
};
struct InitialState { std::string action; State state; bool operator==(const InitialState&) const = default; };
struct NextStep { std::string action; State parameters; bool operator==(const NextStep&) const = default; };
struct StepOk { bool operator==(const StepOk&) const = default; };
struct AllStepsDone { bool operator==(const AllStepsDone&) const = default; };
struct ExploreSessionDone { bool operator==(const ExploreSessionDone&) const = default; };
struct StepMismatch { State expected; State actual; std::vector<DiffHint> hints; bool operator==(const StepMismatch&) const = default; };
struct GenTracesDone {
  std::vector<std::string> itf_trace_paths;
  std::vector<nlohmann::json> itf_traces;   // inline ITF JSON contents (optional on wire)
  bool operator==(const GenTracesDone&) const = default;
};
struct RegisterError { std::string error; bool operator==(const RegisterError&) const = default; };
struct ProtocolError { std::string error; bool operator==(const ProtocolError&) const = default; };
struct ExplorerReady {
  long long init_transitions, next_transitions, state_invariants;
  bool operator==(const ExplorerReady&) const = default;
};
struct ExploreTransitionStatus { TransitionStatus status; bool operator==(const ExploreTransitionStatus&) const = default; };
struct ExploreStepDone { long long step_no; bool operator==(const ExploreStepDone&) const = default; };
struct ExploreState { State state; bool operator==(const ExploreState&) const = default; };
struct ExploreInvariantStatus { InvariantStatus status; bool operator==(const ExploreInvariantStatus&) const = default; };
struct ExploreAssumeStatus { TransitionStatus status; bool operator==(const ExploreAssumeStatus&) const = default; };
struct ExploreRollbackDone { long long snapshot_id; bool operator==(const ExploreRollbackDone&) const = default; };

using MirrorMessage = std::variant<
    SpecValidated, InitialState, NextStep, StepOk, AllStepsDone,
    ExploreSessionDone, StepMismatch, GenTracesDone, RegisterError,
    ProtocolError, ExplorerReady, ExploreTransitionStatus, ExploreStepDone,
    ExploreState, ExploreInvariantStatus, ExploreAssumeStatus,
    ExploreRollbackDone>;

// ---------------------------------------------------------------------------
// Tag names (proto_step discriminants) — used by errors and the phase guard.
// ---------------------------------------------------------------------------
std::string_view client_message_name(const ClientMessage& msg) noexcept;
std::string_view mirror_message_name(const MirrorMessage& msg) noexcept;

// ---------------------------------------------------------------------------
// Codec (§5.2)
// ---------------------------------------------------------------------------
// Serialize a client message to one compact JSON line WITHOUT a trailing newline.
// Never throws. ReportState's state is serialized via encode_state.
std::string encode_client_message(const ClientMessage& msg);

// Parse one mirror message line (no trailing newline). Never throws: malformed
// JSON / unknown proto_step become Error{json, …} / ProtocolError{…}.
Result<MirrorMessage> decode_mirror_message(std::string_view line);

// ---------------------------------------------------------------------------
// Diff hint rendering (§5.2)
// ---------------------------------------------------------------------------
std::string render_path(const std::vector<PathSeg>& path);
std::string render_diff_hint(const DiffHint& hint);
std::string render_diff_hints(const std::vector<DiffHint>& hints);

// ---------------------------------------------------------------------------
// Phase guard (§5.2): idle --Register*--> validating --spec_validated--> ready
//   --initial_state--> stepping ⇄ (next_step/report_state/step_ok)
//   stepping --all_steps_done | step_mismatch--> done
//   idle --register_explore_session--> exploring --explore_done--> done
// ---------------------------------------------------------------------------
enum class Phase { idle, validating, ready, stepping, exploring, done };

const char* phase_name(Phase p) noexcept;

// A small client-side state machine that keeps flows honest and produces
// precise "expected X, got Y" protocol errors (§5.2). Client flows feed it the
// messages they send and receive.
class PhaseGuard {
 public:
  PhaseGuard() = default;
  explicit PhaseGuard(Phase initial) : phase_(initial) {}

  Phase phase() const noexcept { return phase_; }
  void reset(Phase p = Phase::idle) { phase_ = p; }

  // Record a client message being sent; returns Error{protocol} if the send is
  // illegal in the current phase.
  Result<void> sent(const ClientMessage& msg);

  // Validate an incoming mirror message against the current phase; advances on
  // success. Returns Error{protocol, "expected <X>, got <Y>"} on mismatch.
  Result<void> received(const MirrorMessage& msg);

 private:
  Phase phase_ = Phase::idle;
};

}  // namespace mirrorcpp

#endif  // MIRRORCPP_PROTOCOL_HPP