// mirrorcpp/protocol.cpp — message encode/decode, diff hints, phase guard (design §5.2).
//
// Wire format: single-line JSON objects with a "proto_step" string discriminant
// (§3.1). Field names are EXACTLY the §3.2 names. States are serialized via
// encode_state / decode_state — never a generic message encoder (the §5.2
// double-wrap warning). decode_mirror_message never throws: malformed JSON and
// bad payloads become Error{json, …}; unknown proto_step decodes as a
// ProtocolError{"unknown proto_step: …"} for forward compatibility.
#include <mirrorcpp/protocol.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace mirrorcpp {

namespace {

// ---------------------------------------------------------------------------
// Small JSON helpers (internal)
// ---------------------------------------------------------------------------

nlohmann::json require_object(const nlohmann::json& j, std::string_view what) {
  if (!j.is_object()) throw JsonError(std::string(what) + ": expected object");
  return j;
}

std::string require_string(const nlohmann::json& j, std::string_view what) {
  if (!j.is_string()) throw JsonError(std::string(what) + ": expected string");
  return j.get<std::string>();
}

long long require_int(const nlohmann::json& j, std::string_view what) {
  if (!j.is_number_integer() && !j.is_number_unsigned())
    throw JsonError(std::string(what) + ": expected integer");
  return j.get<long long>();
}

std::vector<std::string> require_string_array(const nlohmann::json& j, std::string_view what) {
  if (!j.is_array()) throw JsonError(std::string(what) + ": expected array");
  std::vector<std::string> out;
  out.reserve(j.size());
  for (const auto& e : j) out.push_back(require_string(e, what));
  return out;
}

// std::unexpected(Error(...)) builders so Result-returning functions can return
// the error branch directly (std::expected has no implicit ctor from E).
std::unexpected<Error> err_json(std::string m) {
  return std::unexpected(Error(ErrorKind::json, std::move(m)));
}
std::unexpected<Error> err_protocol(std::string m) {
  return std::unexpected(Error(ErrorKind::protocol, std::move(m)));
}

// ---------------------------------------------------------------------------
// ApalacheConfig / TraceGenerationConfig / ApalacheSpec encode (wire names §3.2)
// ---------------------------------------------------------------------------

nlohmann::json encode_apalache_config(const ApalacheConfig& c) {
  nlohmann::json j = nlohmann::json::object();
  j["specPath"] = c.spec_path;                    // ALWAYS serialized
  if (c.init_predicate) j["initPredicate"] = *c.init_predicate;
  if (c.next_predicate) j["nextPredicate"] = *c.next_predicate;
  if (c.const_init) j["constInit"] = *c.const_init;
  j["invariant"] = c.invariant;                   // may be ""
  j["lengthBound"] = c.length_bound;
  j["paramVars"] = c.param_vars;                  // may be ""
  return j;
}

nlohmann::json encode_trace_config(const TraceGenerationConfig& t) {
  nlohmann::json j = nlohmann::json::object();
  j["numTraces"] = t.num_traces;
  if (t.view) j["view"] = *t.view;
  return j;
}

nlohmann::json encode_spec(const ApalacheSpec& s) {
  nlohmann::json j = nlohmann::json::object();
  j["sources"] = s.sources;                       // sources[0] = root
  return j;
}

// ---------------------------------------------------------------------------
// DiffHint decode (wire: {path, kind, expected?, actual?})
// ---------------------------------------------------------------------------

DiffHintKind diff_hint_kind_from_string(std::string_view s) {
  if (s == "value_mismatch") return DiffHintKind::value_mismatch;
  if (s == "missing") return DiffHintKind::missing;
  if (s == "extra") return DiffHintKind::extra;
  if (s == "missing_elem") return DiffHintKind::missing_elem;
  if (s == "extra_elem") return DiffHintKind::extra_elem;
  if (s == "type_mismatch") return DiffHintKind::type_mismatch;
  if (s == "truncated") return DiffHintKind::truncated;
  throw JsonError(std::string("unknown diff hint kind: ") + std::string(s));
}

PathSeg decode_path_seg(const nlohmann::json& j) {
  // Wire shape (pinned by the golden corpus): tagged objects
  // {"field": name} | {"index": n}.
  if (j.is_object()) {
    if (auto it = j.find("field"); it != j.end() && it->is_string())
      return PathSeg::field(it->get<std::string>());
    if (auto it = j.find("index");
        it != j.end() && (it->is_number_integer() || it->is_number_unsigned()))
      return PathSeg::index(it->get<std::int64_t>());
  }
  throw JsonError("diff hint path segment must be {\"field\": name} or {\"index\": n}");
}

DiffHint decode_diff_hint(const nlohmann::json& j) {
  require_object(j, "diff hint");
  DiffHint h;
  h.kind = diff_hint_kind_from_string(require_string(j.at("kind"), "diff hint kind"));
  if (auto it = j.find("path"); it != j.end() && it->is_array()) {
    h.path.reserve(it->size());
    for (const auto& seg : *it) h.path.push_back(decode_path_seg(seg));
  }
  if (auto it = j.find("expected"); it != j.end() && !it->is_null()) h.expected = decode_value(*it);
  if (auto it = j.find("actual"); it != j.end() && !it->is_null()) h.actual = decode_value(*it);
  return h;
}

// ---------------------------------------------------------------------------
// Mirror message decode (wire names §3.2)
// ---------------------------------------------------------------------------

State decode_state_field(const nlohmann::json& j, std::string_view key) {
  auto it = j.find(key);
  if (it == j.end()) throw JsonError(std::string("missing ") + std::string(key));
  return decode_state(*it);
}

// A ValidateResult payload ("valid" | {"invalid": text}) — shared by
// spec_validated.result and the async job_result outcome.validate (C20).
SpecValidated decode_validate_result(const nlohmann::json& result) {
  if (result.is_string() && result.get<std::string>() == "valid")
    return SpecValidated{std::monostate{}};
  if (result.is_object() && result.contains("invalid") && result.at("invalid").is_string())
    return SpecValidated{result.at("invalid").get<std::string>()};
  throw JsonError("validate result: expected \"valid\" or {\"invalid\": text}");
}

SpecValidated decode_spec_validated(const nlohmann::json& j) {
  require_object(j, "spec_validated");
  return decode_validate_result(j.at("result"));
}

StepMismatch decode_step_mismatch(const nlohmann::json& j) {
  require_object(j, "step_mismatch");
  StepMismatch m;
  m.expected = decode_state_field(j, "expected");
  m.actual = decode_state_field(j, "actual");
  if (auto it = j.find("hints"); it != j.end() && it->is_array()) {
    m.hints.reserve(it->size());
    for (const auto& h : *it) m.hints.push_back(decode_diff_hint(h));
  }
  return m;
}

// A genTraces payload ({itfTracePaths, itfTraces?}) — shared by
// gen_traces_done and the async job_result outcome.genTraces.
GenTracesDone decode_gen_traces_payload(const nlohmann::json& j) {
  GenTracesDone g;
  g.itf_trace_paths = require_string_array(j.at("itfTracePaths"), "genTraces.itfTracePaths");
  if (auto it = j.find("itfTraces"); it != j.end() && it->is_array()) {
    g.itf_traces = *it;    // inline ITF JSON contents, kept as raw JSON
  }
  return g;
}

GenTracesDone decode_gen_traces_done(const nlohmann::json& j) {
  require_object(j, "gen_traces_done");
  return decode_gen_traces_payload(j);
}

// ---------------------------------------------------------------------------
// Async job reply decode (guide §6; wire shapes pinned by the golden corpus)
// ---------------------------------------------------------------------------

JobKind decode_job_kind(const nlohmann::json& j) {
  const std::string s = require_string(j, "kind");
  if (s == "validate") return JobKind::validate;
  if (s == "gen_traces") return JobKind::gen_traces;
  throw JsonError("kind: expected validate/gen_traces");
}

JobPhase decode_job_phase(const nlohmann::json& j) {
  const std::string s = require_string(j, "phase");
  if (s == "pending") return JobPhase::pending;
  if (s == "running") return JobPhase::running;
  if (s == "done") return JobPhase::done;
  if (s == "failed") return JobPhase::failed;
  if (s == "cancelled") return JobPhase::cancelled;
  if (s == "unknown") return JobPhase::unknown;
  throw JsonError("phase: expected pending/running/done/failed/cancelled/unknown");
}

JobOutcome decode_job_outcome(const nlohmann::json& j) {
  require_object(j, "job_result.outcome");
  if (auto it = j.find("validate"); it != j.end())
    return JobOutcome{decode_validate_result(*it)};
  if (auto it = j.find("genTraces"); it != j.end()) {
    require_object(*it, "job_result.outcome.genTraces");
    return JobOutcome{decode_gen_traces_payload(*it)};
  }
  if (auto it = j.find("error"); it != j.end())
    return JobOutcome{require_string(*it, "job_result.outcome.error")};
  throw JsonError("job_result.outcome: expected validate/genTraces/error");
}

TransitionStatus decode_transition_status(const nlohmann::json& j) {
  const std::string s = require_string(j, "status");
  if (s == "ENABLED") return TransitionStatus::enabled;
  if (s == "DISABLED") return TransitionStatus::disabled;
  if (s == "UNKNOWN") return TransitionStatus::unknown;
  throw JsonError("status: expected ENABLED/DISABLED/UNKNOWN");
}

InvariantStatus decode_invariant_status(const nlohmann::json& j) {
  const std::string s = require_string(j, "status");
  if (s == "SATISFIED") return InvariantStatus::satisfied;
  if (s == "VIOLATED") return InvariantStatus::violated;
  if (s == "UNKNOWN") return InvariantStatus::unknown;
  throw JsonError("status: expected SATISFIED/VIOLATED/UNKNOWN");
}

}  // namespace

// ---------------------------------------------------------------------------
// Tag names
// ---------------------------------------------------------------------------

std::string_view client_message_name(const ClientMessage& msg) noexcept {
  return std::visit([](const auto& m) -> std::string_view {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, Register>) return "register";
    else if constexpr (std::is_same_v<T, RegisterTraces>) return "register_traces";
    else if constexpr (std::is_same_v<T, RegisterTraceGen>) return "register_trace_gen";
    else if constexpr (std::is_same_v<T, RegisterExplore>) return "register_explore";
    else if constexpr (std::is_same_v<T, RegisterExploreSession>) return "register_explore_session";
    else if constexpr (std::is_same_v<T, RegisterValidate>) return "register_validate";
    else if constexpr (std::is_same_v<T, RegisterValidateAsync>) return "register_validate_async";
    else if constexpr (std::is_same_v<T, RegisterTraceGenAsync>) return "register_trace_gen_async";
    else if constexpr (std::is_same_v<T, QueryJob>) return "query_job";
    else if constexpr (std::is_same_v<T, AwaitJob>) return "await_job";
    else if constexpr (std::is_same_v<T, CancelJob>) return "cancel_job";
    else if constexpr (std::is_same_v<T, ExploreAssumeTransition>) return "explore_assume_transition";
    else if constexpr (std::is_same_v<T, ExploreNextStep>) return "explore_next_step";
    else if constexpr (std::is_same_v<T, ExploreQueryState>) return "explore_query_state";
    else if constexpr (std::is_same_v<T, ExploreCheckInvariant>) return "explore_check_invariant";
    else if constexpr (std::is_same_v<T, ExploreAssumeState>) return "explore_assume_state";
    else if constexpr (std::is_same_v<T, ExploreRollback>) return "explore_rollback";
    else if constexpr (std::is_same_v<T, ExploreDone>) return "explore_done";
    else return "report_state";
  }, msg);
}

std::string_view mirror_message_name(const MirrorMessage& msg) noexcept {
  return std::visit([](const auto& m) -> std::string_view {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, SpecValidated>) return "spec_validated";
    else if constexpr (std::is_same_v<T, InitialState>) return "initial_state";
    else if constexpr (std::is_same_v<T, NextStep>) return "next_step";
    else if constexpr (std::is_same_v<T, StepOk>) return "step_ok";
    else if constexpr (std::is_same_v<T, AllStepsDone>) return "all_steps_done";
    else if constexpr (std::is_same_v<T, ExploreSessionDone>) return "explore_session_done";
    else if constexpr (std::is_same_v<T, StepMismatch>) return "step_mismatch";
    else if constexpr (std::is_same_v<T, GenTracesDone>) return "gen_traces_done";
    else if constexpr (std::is_same_v<T, RegisterError>) return "register_error";
    else if constexpr (std::is_same_v<T, ProtocolError>) return "protocol_error";
    else if constexpr (std::is_same_v<T, ExplorerReady>) return "explorer_ready";
    else if constexpr (std::is_same_v<T, ExploreTransitionStatus>) return "explore_transition_status";
    else if constexpr (std::is_same_v<T, ExploreStepDone>) return "explore_step_done";
    else if constexpr (std::is_same_v<T, ExploreState>) return "explore_state";
    else if constexpr (std::is_same_v<T, ExploreInvariantStatus>) return "explore_invariant_status";
    else if constexpr (std::is_same_v<T, ExploreAssumeStatus>) return "explore_assume_status";
    else if constexpr (std::is_same_v<T, JobAccepted>) return "job_accepted";
    else if constexpr (std::is_same_v<T, JobStatus>) return "job_status";
    else if constexpr (std::is_same_v<T, JobResult>) return "job_result";
    else return "explore_rollback_done";
  }, msg);
}

// ---------------------------------------------------------------------------
// PathSeg render
// ---------------------------------------------------------------------------

std::string PathSeg::render() const {
  if (is_field()) return std::get<std::string>(seg);
  return "[" + std::to_string(std::get<std::int64_t>(seg)) + "]";
}

// ---------------------------------------------------------------------------
// render_path / render_diff_hint / render_diff_hints (§5.2)
// ---------------------------------------------------------------------------

std::string render_path(const std::vector<PathSeg>& path) {
  std::string out;
  for (const auto& seg : path) {
    if (seg.is_field()) {
      if (!out.empty()) out += '.';
      out += std::get<std::string>(seg.seg);
    } else {
      out += seg.render();   // "[i]"
    }
  }
  return out;
}

std::string render_diff_hint(const DiffHint& h) {
  const std::string p = render_path(h.path);
  const std::string lead = p.empty() ? std::string("state") : p;
  switch (h.kind) {
    case DiffHintKind::value_mismatch:
      return lead + ": value mismatch (expected " + (h.expected ? to_string(*h.expected) : "?")
             + ", got " + (h.actual ? to_string(*h.actual) : "?") + ")";
    case DiffHintKind::missing:
      return lead + ": missing";
    case DiffHintKind::extra:
      return lead + ": extra";
    case DiffHintKind::missing_elem:
      return lead + ": missing element" + (h.expected ? std::string(" ") + to_string(*h.expected) : "");
    case DiffHintKind::extra_elem:
      return lead + ": extra element" + (h.actual ? std::string(" ") + to_string(*h.actual) : "");
    case DiffHintKind::type_mismatch:
      return lead + ": type mismatch (expected " + (h.expected ? to_string(*h.expected) : "?")
             + ", got " + (h.actual ? to_string(*h.actual) : "?") + ")";
    case DiffHintKind::truncated:
      return "… (hints truncated)";
  }
  return lead;
}

std::string render_diff_hints(const std::vector<DiffHint>& hints) {
  std::string out;
  for (const auto& h : hints) {
    if (!out.empty()) out += "; ";
    out += render_diff_hint(h);
  }
  return out;
}

// ---------------------------------------------------------------------------
// encode_client_message (§5.2)
// ---------------------------------------------------------------------------

std::string encode_client_message(const ClientMessage& msg) {
  nlohmann::json j = nlohmann::json::object();
  j["proto_step"] = client_message_name(msg);
  std::visit([&j](const auto& m) {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, Register>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["traceConfig"] = encode_trace_config(m.tc);
      if (m.spec) j["spec"] = encode_spec(*m.spec);
    } else if constexpr (std::is_same_v<T, RegisterTraces>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["itfTracePaths"] = m.itf_trace_paths;
    } else if constexpr (std::is_same_v<T, RegisterTraceGen>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["traceConfig"] = encode_trace_config(m.tc);
      if (m.dest_path) j["destPath"] = *m.dest_path;
      if (m.spec) j["spec"] = encode_spec(*m.spec);
    } else if constexpr (std::is_same_v<T, RegisterExplore>) {
      j["spec"] = encode_spec(m.spec);
      j["invariants"] = m.invariants;
      j["exports"] = m.exports;
      if (m.max_steps) j["maxSteps"] = *m.max_steps;
    } else if constexpr (std::is_same_v<T, RegisterExploreSession>) {
      j["spec"] = encode_spec(m.spec);
      j["invariants"] = m.invariants;
      j["exports"] = m.exports;
    } else if constexpr (std::is_same_v<T, RegisterValidate>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["bound"] = m.bound;
      if (m.spec) j["spec"] = encode_spec(*m.spec);
    } else if constexpr (std::is_same_v<T, RegisterValidateAsync>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["bound"] = m.bound;
      if (m.spec) j["spec"] = encode_spec(*m.spec);
    } else if constexpr (std::is_same_v<T, RegisterTraceGenAsync>) {
      j["apalacheConfig"] = encode_apalache_config(m.cfg);
      j["traceConfig"] = encode_trace_config(m.tc);
      if (m.dest_path) j["destPath"] = *m.dest_path;
      if (m.spec) j["spec"] = encode_spec(*m.spec);
    } else if constexpr (std::is_same_v<T, QueryJob>) {
      j["jobId"] = m.job_id;
    } else if constexpr (std::is_same_v<T, AwaitJob>) {
      j["jobId"] = m.job_id;
      if (m.timeout_secs) j["timeoutSecs"] = *m.timeout_secs;
    } else if constexpr (std::is_same_v<T, CancelJob>) {
      j["jobId"] = m.job_id;
    } else if constexpr (std::is_same_v<T, ExploreAssumeTransition>) {
      j["transitionId"] = m.transition_id;
    } else if constexpr (std::is_same_v<T, ExploreNextStep>) {
      /* no fields */
    } else if constexpr (std::is_same_v<T, ExploreQueryState>) {
      /* no fields */
    } else if constexpr (std::is_same_v<T, ExploreCheckInvariant>) {
      j["invariantId"] = m.invariant_id;
    } else if constexpr (std::is_same_v<T, ExploreAssumeState>) {
      j["state"] = encode_state(m.state);     // encode_state, NOT a generic encoder
    } else if constexpr (std::is_same_v<T, ExploreRollback>) {
      j["snapshotId"] = m.snapshot_id;
    } else if constexpr (std::is_same_v<T, ExploreDone>) {
      /* no fields */
    } else {   // ReportState
      j["state"] = encode_state(m.state);     // encode_state, NOT a generic encoder
    }
  }, msg);
  return j.dump();     // compact, single-line, no trailing newline
}

// ---------------------------------------------------------------------------
// decode_mirror_message (§5.2)
// ---------------------------------------------------------------------------

Result<MirrorMessage> decode_mirror_message(std::string_view line) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(line);
  } catch (const nlohmann::json::parse_error& e) {
    return err_json(std::string("malformed JSON: ") + e.what());
  }
  if (!j.is_object()) return err_json("mirror message: expected object");
  auto tag_it = j.find("proto_step");
  if (tag_it == j.end() || !tag_it->is_string())
    return err_json("mirror message: missing string proto_step");
  const std::string tag = tag_it->get<std::string>();
  try {
    if (tag == "spec_validated") return MirrorMessage{decode_spec_validated(j)};
    if (tag == "initial_state") {
      require_object(j, "initial_state");
      return MirrorMessage{InitialState{require_string(j.at("action"), "action"), decode_state_field(j, "state")}};
    }
    if (tag == "next_step") {
      require_object(j, "next_step");
      return MirrorMessage{NextStep{require_string(j.at("action"), "action"), decode_state_field(j, "parameters")}};
    }
    if (tag == "step_ok") return MirrorMessage{StepOk{}};
    if (tag == "all_steps_done") return MirrorMessage{AllStepsDone{}};
    if (tag == "explore_session_done") return MirrorMessage{ExploreSessionDone{}};
    if (tag == "step_mismatch") return MirrorMessage{decode_step_mismatch(j)};
    if (tag == "gen_traces_done") return MirrorMessage{decode_gen_traces_done(j)};
    if (tag == "register_error") {
      require_object(j, "register_error");
      return MirrorMessage{RegisterError{require_string(j.at("error"), "error")}};
    }
    if (tag == "protocol_error") {
      require_object(j, "protocol_error");
      return MirrorMessage{ProtocolError{require_string(j.at("error"), "error")}};
    }
    if (tag == "explorer_ready") {
      require_object(j, "explorer_ready");
      return MirrorMessage{ExplorerReady{require_int(j.at("initTransitions"), "initTransitions"),
                                          require_int(j.at("nextTransitions"), "nextTransitions"),
                                          require_int(j.at("stateInvariants"), "stateInvariants")}};
    }
    if (tag == "explore_transition_status") {
      require_object(j, "explore_transition_status");
      return MirrorMessage{ExploreTransitionStatus{decode_transition_status(j.at("status"))}};
    }
    if (tag == "explore_assume_status") {
      require_object(j, "explore_assume_status");
      return MirrorMessage{ExploreAssumeStatus{decode_transition_status(j.at("status"))}};
    }
    if (tag == "explore_step_done") {
      require_object(j, "explore_step_done");
      return MirrorMessage{ExploreStepDone{require_int(j.at("stepNo"), "stepNo")}};
    }
    if (tag == "explore_state") {
      require_object(j, "explore_state");
      return MirrorMessage{ExploreState{decode_state_field(j, "state")}};
    }
    if (tag == "explore_invariant_status") {
      require_object(j, "explore_invariant_status");
      return MirrorMessage{ExploreInvariantStatus{decode_invariant_status(j.at("status"))}};
    }
    if (tag == "explore_rollback_done") {
      require_object(j, "explore_rollback_done");
      return MirrorMessage{ExploreRollbackDone{require_int(j.at("snapshotId"), "snapshotId")}};
    }
    if (tag == "job_accepted") {
      require_object(j, "job_accepted");
      return MirrorMessage{JobAccepted{require_string(j.at("jobId"), "jobId"),
                                       decode_job_kind(j.at("kind"))}};
    }
    if (tag == "job_status") {
      require_object(j, "job_status");
      return MirrorMessage{JobStatus{require_string(j.at("jobId"), "jobId"),
                                     decode_job_phase(j.at("phase"))}};
    }
    if (tag == "job_result") {
      require_object(j, "job_result");
      return MirrorMessage{JobResult{require_string(j.at("jobId"), "jobId"),
                                     decode_job_outcome(j.at("outcome"))}};
    }
    // Unknown tag: forward-compatible (§3.3 / §5.2).
    return MirrorMessage{ProtocolError{"unknown proto_step: " + tag}};
  } catch (const JsonError& e) {
    return err_json(e.what());
  } catch (const nlohmann::json::exception& e) {
    return err_json(std::string("bad mirror message: ") + e.what());
  }
}

// ---------------------------------------------------------------------------
// PhaseGuard (§5.2)
// ---------------------------------------------------------------------------

const char* phase_name(Phase p) noexcept {
  switch (p) {
    case Phase::idle:       return "idle";
    case Phase::validating: return "validating";
    case Phase::ready:      return "ready";
    case Phase::stepping:   return "stepping";
    case Phase::exploring:  return "exploring";
    case Phase::done:       return "done";
  }
  return "unknown";
}

namespace {

// Expected incoming message tags for a phase (used for "expected X, got Y").
const char* expected_tags(Phase p) noexcept {
  switch (p) {
    case Phase::idle:       return "Register* | register_*_async | query_job | await_job | cancel_job";
    case Phase::validating: return "spec_validated | gen_traces_done";
    case Phase::ready:      return "initial_state";
    case Phase::stepping:   return "next_step | step_ok | all_steps_done | step_mismatch";
    case Phase::exploring:  return "explore_* reply | explore_session_done";
    case Phase::done:       return "(no further messages)";
  }
  return "?";
}

}  // namespace

Result<void> PhaseGuard::sent(const ClientMessage& msg) {
  const std::string_view tag = client_message_name(msg);
  auto is_reg = [](std::string_view t) {
    return t == "register" || t == "register_traces" || t == "register_trace_gen"
        || t == "register_explore" || t == "register_explore_session" || t == "register_validate";
  };
  if (is_reg(tag)) {
    if (phase_ != Phase::idle)
      return err_protocol(std::string("cannot send ") + std::string(tag)
                        + " in phase " + phase_name(phase_));
    phase_ = (tag == "register_explore_session") ? Phase::exploring : Phase::validating;
    return {};
  }
  // Async job submissions (guide §6): the reply (job_accepted | register_error)
  // is synchronous and the job lives server-side, so this is NOT a flow — the
  // session stays idle and may submit more jobs or start a sync flow.
  if (tag == "register_validate_async" || tag == "register_trace_gen_async") {
    if (phase_ != Phase::idle)
      return err_protocol(std::string("cannot send ") + std::string(tag)
                        + " in phase " + phase_name(phase_));
    return {};
  }
  // Job control is phase-independent on the wire but MUST NOT be interleaved
  // with a live sync/explore flow on the same connection (the server would
  // reject it out-of-phase); idle and done are the quiescent phases.
  if (tag == "query_job" || tag == "await_job" || tag == "cancel_job") {
    if (phase_ != Phase::idle && phase_ != Phase::done)
      return err_protocol(std::string("cannot send ") + std::string(tag)
                        + " in phase " + phase_name(phase_));
    return {};
  }
  // Non-register client messages: allowed only in their flow phase.
  if (tag == "report_state") {
    if (phase_ != Phase::stepping)
      return err_protocol("report_state requires phase stepping, got "
                          + std::string(phase_name(phase_)));
    return {};
  }
  if (tag.rfind("explore_", 0) == 0) {
    if (phase_ != Phase::exploring)
      return err_protocol(std::string("explore_* requires phase exploring, got ")
                          + phase_name(phase_));
    return {};
  }
  return err_protocol(std::string("unexpected client message ") + std::string(tag));
}

Result<void> PhaseGuard::received(const MirrorMessage& msg) {
  const std::string_view tag = mirror_message_name(msg);
  // Errors may arrive at any point and end/short-circuit a session (§3.3).
  if (tag == "register_error" || tag == "protocol_error") return {};
  const bool ok = std::visit([this](const auto& m) -> bool {
    using T = std::decay_t<decltype(m)>;
    if constexpr (std::is_same_v<T, SpecValidated>) {
      if (phase_ != Phase::validating) return false;
      phase_ = m.is_valid() ? Phase::ready : Phase::done;
      return true;
    } else if constexpr (std::is_same_v<T, GenTracesDone>) {
      if (phase_ != Phase::validating) return false;
      phase_ = Phase::done;
      return true;
    } else if constexpr (std::is_same_v<T, InitialState>) {
      // ready (first trace) OR stepping (the mirror starts the NEXT trace
      // directly — all_steps_done comes once, at the very end of the flow;
      // verified against the real mirror, which replays multiple traces per
      // register run).
      if (phase_ != Phase::ready && phase_ != Phase::stepping) return false;
      phase_ = Phase::stepping;
      return true;
    } else if constexpr (std::is_same_v<T, NextStep> || std::is_same_v<T, StepOk>) {
      return phase_ == Phase::stepping;
    } else if constexpr (std::is_same_v<T, AllStepsDone> || std::is_same_v<T, StepMismatch>) {
      if (phase_ != Phase::stepping) return false;
      phase_ = Phase::done;
      return true;
    } else if constexpr (std::is_same_v<T, ExplorerReady>) {
      return phase_ == Phase::exploring;
    } else if constexpr (std::is_same_v<T, ExploreTransitionStatus> ||
                         std::is_same_v<T, ExploreAssumeStatus> ||
                         std::is_same_v<T, ExploreStepDone> ||
                         std::is_same_v<T, ExploreState> ||
                         std::is_same_v<T, ExploreInvariantStatus> ||
                         std::is_same_v<T, ExploreRollbackDone>) {
      return phase_ == Phase::exploring;
    } else if constexpr (std::is_same_v<T, ExploreSessionDone>) {
      if (phase_ != Phase::exploring) return false;
      phase_ = Phase::done;
      return true;
    } else if constexpr (std::is_same_v<T, JobAccepted> ||
                         std::is_same_v<T, JobStatus> ||
                         std::is_same_v<T, JobResult>) {
      // Async job replies arrive only in the quiescent phases (see sent()).
      return phase_ == Phase::idle || phase_ == Phase::done;
    } else {
      return false;
    }
  }, msg);
  if (!ok)
    return err_protocol(std::string("expected ") + expected_tags(phase_)
                      + ", got " + std::string(tag));
  return {};
}

}  // namespace mirrorcpp