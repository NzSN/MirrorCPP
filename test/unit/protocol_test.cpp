// mirrorcpp/unit/protocol_test.cpp — message codec, diff hints, phase guard (design §8).
//
// Covers: every client message encode (exact wire JSON, no trailing newline,
// optional-field presence/absence, state double-wrap guard), every mirror
// message decode, spec_validated both result shapes, unknown proto_step
// fallback, diff hint decode + render, phase guard transitions and errors.
#include <mirrorcpp/protocol.hpp>

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <string_view>
#include <variant>

using namespace mirrorcpp;

namespace {

// ---- small value/state factories ----

Value iv(long long v) { return Value(v); }
Value bv(bool v) { return Value(v); }
Value sv(std::string v) { return Value(std::move(v)); }
Value seq(std::vector<Value> elems) {
  Value::Seq s;
  s.elems = std::move(elems);
  return Value(std::move(s));
}

State st(std::initializer_list<std::pair<const std::string, Value>> init) {
  return State(init);
}

nlohmann::json parse(const std::string& s) { return nlohmann::json::parse(s); }

MirrorMessage must_decode(std::string_view line) {
  auto res = decode_mirror_message(line);
  REQUIRE(res.has_value());
  return *res;
}

std::string enc(const ClientMessage& m) { return encode_client_message(m); }

}  // namespace

// ===========================================================================
// Client message encode: exact wire JSON
// ===========================================================================

TEST_CASE("register encodes specPath always + optional fields") {
  ApalacheConfig cfg;
  cfg.spec_path = "/tmp/placeholder.tla";
  cfg.invariant = "TypeOK";
  cfg.length_bound = 5;
  cfg.param_vars = "hr,tk";
  TraceGenerationConfig tc;
  tc.num_traces = 3;
  Register msg;
  msg.cfg = cfg;
  msg.tc = tc;
  ApalacheSpec spec;
  spec.sources = {"---- MODULE Root ----", "dep1"};
  msg.spec = spec;
  const nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["proto_step"] == "register");
  REQUIRE(j["apalacheConfig"]["specPath"] == "/tmp/placeholder.tla");
  REQUIRE(j["apalacheConfig"]["invariant"] == "TypeOK");
  REQUIRE(j["apalacheConfig"]["lengthBound"] == 5);
  REQUIRE(j["apalacheConfig"]["paramVars"] == "hr,tk");
  REQUIRE_FALSE(j["apalacheConfig"].contains("initPredicate"));
  REQUIRE_FALSE(j["apalacheConfig"].contains("nextPredicate"));
  REQUIRE_FALSE(j["apalacheConfig"].contains("constInit"));
  REQUIRE(j["traceConfig"]["numTraces"] == 3);
  REQUIRE_FALSE(j["traceConfig"].contains("view"));
  REQUIRE(j["spec"]["sources"].size() == 2);
  REQUIRE(j["spec"]["sources"][0].get<std::string>().rfind("---- MODULE Root", 0) == 0);
}

TEST_CASE("register omits spec when absent; empty invariant/paramVars allowed") {
  ApalacheConfig cfg;
  cfg.spec_path = "x.tla";
  cfg.invariant = "";
  cfg.param_vars = "";
  Register msg;
  msg.cfg = cfg;
  msg.tc = TraceGenerationConfig{};
  const nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE_FALSE(j.contains("spec"));
  REQUIRE(j["apalacheConfig"]["invariant"] == "");
  REQUIRE(j["apalacheConfig"]["paramVars"] == "");
  REQUIRE(j["apalacheConfig"]["lengthBound"] == 10);
  REQUIRE(j["traceConfig"]["numTraces"] == 1);
  REQUIRE_FALSE(j["traceConfig"].contains("view"));
}

TEST_CASE("register_traces encodes itfTracePaths") {
  RegisterTraces msg;
  msg.cfg.spec_path = "s.tla";
  msg.itf_trace_paths = {"/tmp/a.itf.json", "/tmp/b.itf.json"};
  const nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["proto_step"] == "register_traces");
  REQUIRE(j["itfTracePaths"] == nlohmann::json({"/tmp/a.itf.json", "/tmp/b.itf.json"}));
}

TEST_CASE("register_trace_gen optional destPath/spec") {
  RegisterTraceGen msg;
  msg.cfg.spec_path = "s.tla";
  msg.tc.num_traces = 2;
  msg.tc.view = "custom_view";
  nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["proto_step"] == "register_trace_gen");
  REQUIRE(j["traceConfig"]["view"] == "custom_view");
  REQUIRE_FALSE(j.contains("destPath"));
  REQUIRE_FALSE(j.contains("spec"));
  msg.dest_path = "/tmp/out";
  j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["destPath"] == "/tmp/out");
}

TEST_CASE("register_explore maxSteps optional") {
  RegisterExplore msg;
  msg.spec.sources = {"---- MODULE M ----"};
  msg.invariants = {"Inv"};
  msg.exports = {"x", "y"};
  nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["proto_step"] == "register_explore");
  REQUIRE_FALSE(j.contains("maxSteps"));
  msg.max_steps = 42;
  j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["maxSteps"] == 42);
}

TEST_CASE("register_explore_session / register_validate encode") {
  RegisterExploreSession rs;
  rs.spec.sources = {"---- MODULE M ----"};
  rs.invariants = {"Inv"};
  nlohmann::json j = parse(enc(ClientMessage{rs}));
  REQUIRE(j["proto_step"] == "register_explore_session");
  REQUIRE(j["spec"]["sources"].size() == 1);
  REQUIRE(j["invariants"].size() == 1);
  REQUIRE(j["exports"] == nlohmann::json::array());

  RegisterValidate rv;
  rv.cfg.spec_path = "s.tla";
  rv.bound = 7;
  j = parse(enc(ClientMessage{rv}));
  REQUIRE(j["proto_step"] == "register_validate");
  REQUIRE(j["bound"] == 7);   // check length is TOP-LEVEL bound
  // apalacheConfig is always fully serialized (lengthBound is non-optional §3.2);
  // the mirror reads the check length from top-level bound, not lengthBound.
  REQUIRE(j["apalacheConfig"].contains("lengthBound"));
  REQUIRE(j["apalacheConfig"]["lengthBound"] == 10);
  REQUIRE_FALSE(j.contains("spec"));
}

TEST_CASE("report_state uses encode_state (no double wrap)") {
  State s = st({{ "x", iv(1) }, { "name", sv("a") }, { "flag", bv(true) }});
  ReportState msg;
  msg.state = s;
  const std::string out = enc(ClientMessage{msg});
  const nlohmann::json j = parse(out);
  REQUIRE(j["proto_step"] == "report_state");
  REQUIRE(j["state"]["x"] == nlohmann::json({ { "#bigint", "1" } }));
  REQUIRE(j["state"]["name"] == "a");
  REQUIRE(j["state"]["flag"] == true);
  REQUIRE_FALSE(j["state"].contains("proto_step"));
}

TEST_CASE("explore_assume_state state via encode_state") {
  ExploreAssumeState msg;
  msg.state = st({ { "hr", seq({ iv(1), iv(2) }) } });
  const nlohmann::json j = parse(enc(ClientMessage{msg}));
  REQUIRE(j["proto_step"] == "explore_assume_state");
  REQUIRE(j["state"]["hr"].is_array());
  REQUIRE(j["state"]["hr"].size() == 2);
  REQUIRE(j["state"]["hr"][0] == nlohmann::json({ { "#bigint", "1" } }));
}

TEST_CASE("explore commands encode transitionId/invariantId/snapshotId") {
  const nlohmann::json t = parse(enc(ClientMessage{ExploreAssumeTransition{3}}));
  REQUIRE(t["proto_step"] == "explore_assume_transition");
  REQUIRE(t["transitionId"] == 3);
  const nlohmann::json ci = parse(enc(ClientMessage{ExploreCheckInvariant{9}}));
  REQUIRE(ci["proto_step"] == "explore_check_invariant");
  REQUIRE(ci["invariantId"] == 9);
  const nlohmann::json rb = parse(enc(ClientMessage{ExploreRollback{12}}));
  REQUIRE(rb["proto_step"] == "explore_rollback");
  REQUIRE(rb["snapshotId"] == 12);
}

TEST_CASE("empty explore commands encode with no extra fields") {
  for (const ClientMessage& m : { ClientMessage{ExploreNextStep{}}, ClientMessage{ExploreQueryState{}}, ClientMessage{ExploreDone{}} }) {
    const nlohmann::json j = parse(enc(m));
    REQUIRE(j.size() == 1);
  }
}

TEST_CASE("encode output is compact single-line with no trailing newline") {
  const std::string out = enc(ClientMessage{ReportState{st({ { "a", iv(1) } })}});
  REQUIRE_FALSE(out.empty());
  REQUIRE(out.back() != '\n');
  REQUIRE(out.find('\n') == std::string::npos);
}
// ===========================================================================
// Mirror message decode
// ===========================================================================

TEST_CASE("decode spec_validated valid + invalid shapes") {
  auto ok = must_decode(R"({"proto_step":"spec_validated","result":"valid"})");
  REQUIRE(std::holds_alternative<SpecValidated>(ok));
  REQUIRE(std::get<SpecValidated>(ok).is_valid());
  REQUIRE(std::get<SpecValidated>(ok).invalid_text() == nullptr);

  auto bad = must_decode(R"({"proto_step":"spec_validated","result":{"invalid":"Type error at line 3"}})");
  REQUIRE(std::holds_alternative<SpecValidated>(bad));
  const SpecValidated& sv = std::get<SpecValidated>(bad);
  REQUIRE_FALSE(sv.is_valid());
  REQUIRE(sv.invalid_text() != nullptr);
  REQUIRE(*sv.invalid_text() == "Type error at line 3");
}

TEST_CASE("decode initial_state / next_step") {
  auto in = must_decode(R"({"proto_step":"initial_state","action":"Init","state":{"x":{"#bigint":"0"}}})");
  REQUIRE(std::holds_alternative<InitialState>(in));
  const InitialState& is = std::get<InitialState>(in);
  REQUIRE(is.action == "Init");
  REQUIRE(is.state == st({ { "x", iv(0) } }));

  auto nx = must_decode(R"({"proto_step":"next_step","action":"Tick","parameters":{"hr":{"#bigint":"1"}}})");
  REQUIRE(std::holds_alternative<NextStep>(nx));
  REQUIRE(std::get<NextStep>(nx).action == "Tick");
  REQUIRE(std::get<NextStep>(nx).parameters == st({ { "hr", iv(1) } }));
}

TEST_CASE("decode empty mirror messages") {
  REQUIRE(std::holds_alternative<StepOk>(must_decode(R"({"proto_step":"step_ok"})")));
  REQUIRE(std::holds_alternative<AllStepsDone>(must_decode(R"({"proto_step":"all_steps_done"})")));
  REQUIRE(std::holds_alternative<ExploreSessionDone>(must_decode(R"({"proto_step":"explore_session_done"})")));
}

TEST_CASE("decode step_mismatch with and without hints") {
  auto m = must_decode(R"({"proto_step":"step_mismatch","expected":{"x":{"#bigint":"1"}},"actual":{"x":{"#bigint":"2"}}})");
  REQUIRE(std::holds_alternative<StepMismatch>(m));
  const StepMismatch& sm = std::get<StepMismatch>(m);
  REQUIRE(sm.expected == st({ { "x", iv(1) } }));
  REQUIRE(sm.actual == st({ { "x", iv(2) } }));
  REQUIRE(sm.hints.empty());

  auto m2 = must_decode(R"({"proto_step":"step_mismatch","expected":{},"actual":{},"hints":[]})");
  REQUIRE(std::get<StepMismatch>(m2).hints.empty());
}

TEST_CASE("decode gen_traces_done itfTraces optional") {
  auto g = must_decode(R"({"proto_step":"gen_traces_done","itfTracePaths":["/tmp/t.itf.json"]})");
  REQUIRE(std::holds_alternative<GenTracesDone>(g));
  const GenTracesDone& gd = std::get<GenTracesDone>(g);
  REQUIRE(gd.itf_trace_paths == std::vector<std::string>{ "/tmp/t.itf.json" });
  REQUIRE(gd.itf_traces.empty());

  auto g2 = must_decode(R"({"proto_step":"gen_traces_done","itfTracePaths":[],"itfTraces":[{"#meta":{"format":"ITF"},"states":[]}]})");
  REQUIRE(std::get<GenTracesDone>(g2).itf_traces.size() == 1);
  REQUIRE(std::get<GenTracesDone>(g2).itf_traces[0].is_object());
}

TEST_CASE("decode error messages") {
  auto re = must_decode(R"({"proto_step":"register_error","error":"bad config"})");
  REQUIRE(std::holds_alternative<RegisterError>(re));
  REQUIRE(std::get<RegisterError>(re).error == "bad config");
  auto pe = must_decode(R"({"proto_step":"protocol_error","error":"expected register"})");
  REQUIRE(std::holds_alternative<ProtocolError>(pe));
  REQUIRE(std::get<ProtocolError>(pe).error == "expected register");
}

TEST_CASE("decode explorer messages") {
  auto er = must_decode(R"({"proto_step":"explorer_ready","initTransitions":2,"nextTransitions":5,"stateInvariants":1})");
  REQUIRE(std::holds_alternative<ExplorerReady>(er));
  const ExplorerReady& r = std::get<ExplorerReady>(er);
  REQUIRE(r.init_transitions == 2);
  REQUIRE(r.next_transitions == 5);
  REQUIRE(r.state_invariants == 1);

  auto ts = must_decode(R"({"proto_step":"explore_transition_status","status":"ENABLED"})");
  REQUIRE(std::get<ExploreTransitionStatus>(ts).status == TransitionStatus::enabled);
  auto au = must_decode(R"({"proto_step":"explore_assume_status","status":"UNKNOWN"})");
  REQUIRE(std::get<ExploreAssumeStatus>(au).status == TransitionStatus::unknown);
  auto sd = must_decode(R"({"proto_step":"explore_step_done","stepNo":4})");
  REQUIRE(std::get<ExploreStepDone>(sd).step_no == 4);
  auto es = must_decode(R"({"proto_step":"explore_state","state":{"v":{"#bigint":"-5"}}})");
  REQUIRE(std::get<ExploreState>(es).state == st({ { "v", iv(-5) } }));
  auto ivs = must_decode(R"({"proto_step":"explore_invariant_status","status":"VIOLATED"})");
  REQUIRE(std::get<ExploreInvariantStatus>(ivs).status == InvariantStatus::violated);
  auto rd = must_decode(R"({"proto_step":"explore_rollback_done","snapshotId":8})");
  REQUIRE(std::get<ExploreRollbackDone>(rd).snapshot_id == 8);
}

TEST_CASE("decode explore_status values") {
  REQUIRE(std::get<ExploreTransitionStatus>(must_decode(R"({"proto_step":"explore_transition_status","status":"DISABLED"})")).status == TransitionStatus::disabled);
  REQUIRE(std::get<ExploreInvariantStatus>(must_decode(R"({"proto_step":"explore_invariant_status","status":"SATISFIED"})")).status == InvariantStatus::satisfied);
  REQUIRE(std::get<ExploreInvariantStatus>(must_decode(R"({"proto_step":"explore_invariant_status","status":"UNKNOWN"})")).status == InvariantStatus::unknown);
}

// ===========================================================================
// decode errors
// ===========================================================================

TEST_CASE("decode unknown proto_step is ProtocolError (forward-compatible)") {
  auto res = decode_mirror_message(R"({"proto_step":"future_message","x":1})");
  REQUIRE(res.has_value());
  REQUIRE(std::holds_alternative<ProtocolError>(*res));
  REQUIRE(std::get<ProtocolError>(*res).error == "unknown proto_step: future_message");
}

TEST_CASE("decode malformed JSON returns Error{json}") {
  auto res = decode_mirror_message("not json");
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::json);
}

TEST_CASE("decode missing/invalid proto_step returns Error{json}") {
  auto a = decode_mirror_message("{\"x\":1}");
  REQUIRE_FALSE(a.has_value());
  REQUIRE(a.error().kind == ErrorKind::json);
  auto b = decode_mirror_message("[1,2,3]");
  REQUIRE_FALSE(b.has_value());
  REQUIRE(b.error().kind == ErrorKind::json);
}

TEST_CASE("decode bad spec_validated result returns Error{json}") {
  auto res = decode_mirror_message(R"({"proto_step":"spec_validated","result":42})");
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::json);
}

TEST_CASE("decode bad status returns Error{json}") {
  auto res = decode_mirror_message(R"({"proto_step":"explore_transition_status","status":"MAYBE"})");
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::json);
}

// ===========================================================================
// Diff hints: decode + render
// ===========================================================================

TEST_CASE("decode diff hint value_mismatch with path + values") {
  // Path segments are tagged objects on the wire (golden corpus shape):
  // {"field": name} | {"index": n}.
  auto m = must_decode(R"({"proto_step":"step_mismatch","expected":{},"actual":{},"hints":[{"path":[{"field":"hr"},{"index":0},{"field":"salary"}],"kind":"value_mismatch","expected":{"#bigint":"5"},"actual":{"#bigint":"6"}}]})");
  const StepMismatch& sm = std::get<StepMismatch>(m);
  REQUIRE(sm.hints.size() == 1);
  const DiffHint& h = sm.hints[0];
  REQUIRE(h.kind == DiffHintKind::value_mismatch);
  REQUIRE(h.path.size() == 3);
  REQUIRE(h.path[0].is_field());
  REQUIRE(h.path[1].is_index());
  REQUIRE(h.path[2].is_field());
  REQUIRE(h.expected.has_value());
  REQUIRE(*h.expected == iv(5));
  REQUIRE(*h.actual == iv(6));
  REQUIRE(render_path(h.path) == "hr[0].salary");
}

TEST_CASE("decode all diff hint kinds") {
  const char* kinds[] = { "value_mismatch", "missing", "extra", "missing_elem", "extra_elem", "type_mismatch", "truncated" };
  for (const char* k : kinds) {
    const std::string line = std::string(R"({"proto_step":"step_mismatch","expected":{},"actual":{},"hints":[{"path":[{"field":"a"}],"kind":")") + k + R"("}]})";
    auto m = must_decode(line);
    REQUIRE(std::get<StepMismatch>(m).hints.size() == 1);
  }
}

TEST_CASE("truncated hint decodes and renders marker") {
  auto m = must_decode(R"({"proto_step":"step_mismatch","expected":{},"actual":{},"hints":[{"kind":"truncated"}]})");
  const DiffHint& h = std::get<StepMismatch>(m).hints[0];
  REQUIRE(h.kind == DiffHintKind::truncated);
  REQUIRE(h.path.empty());
  REQUIRE_THAT(render_diff_hint(h), Catch::Matchers::ContainsSubstring("truncated"));
}

TEST_CASE("render_path handles fields + indices") {
  REQUIRE(render_path({ PathSeg::field("a"), PathSeg::field("b"), PathSeg::index(0), PathSeg::field("c") }) == "a.b[0].c");
  REQUIRE(render_path({ PathSeg::index(2), PathSeg::field("x") }) == "[2].x");
  REQUIRE(render_path({}) == "");
}

TEST_CASE("render_diff_hint formats each kind") {
  DiffHint vm;
  vm.kind = DiffHintKind::value_mismatch;
  vm.path = { PathSeg::field("x") };
  vm.expected = iv(5);
  vm.actual = iv(6);
  REQUIRE_THAT(render_diff_hint(vm), Catch::Matchers::ContainsSubstring("value mismatch"));
  REQUIRE_THAT(render_diff_hint(vm), Catch::Matchers::ContainsSubstring("5"));
  REQUIRE_THAT(render_diff_hint(vm), Catch::Matchers::ContainsSubstring("6"));

  DiffHint missing;
  missing.kind = DiffHintKind::missing;
  missing.path = { PathSeg::field("y") };
  REQUIRE(render_diff_hint(missing) == "y: missing");
  DiffHint extra;
  extra.kind = DiffHintKind::extra;
  extra.path = { PathSeg::field("z") };
  REQUIRE(render_diff_hint(extra) == "z: extra");
  DiffHint me;
  me.kind = DiffHintKind::missing_elem;
  me.path = { PathSeg::field("s") };
  me.expected = iv(3);
  REQUIRE_THAT(render_diff_hint(me), Catch::Matchers::ContainsSubstring("missing element"));
  DiffHint ee;
  ee.kind = DiffHintKind::extra_elem;
  ee.path = { PathSeg::field("s") };
  ee.actual = iv(4);
  REQUIRE_THAT(render_diff_hint(ee), Catch::Matchers::ContainsSubstring("extra element"));
  DiffHint tm;
  tm.kind = DiffHintKind::type_mismatch;
  tm.path = { PathSeg::field("t") };
  REQUIRE_THAT(render_diff_hint(tm), Catch::Matchers::ContainsSubstring("type mismatch"));
}

TEST_CASE("render_diff_hints joins hints") {
  DiffHint a;
  a.kind = DiffHintKind::missing;
  a.path = { PathSeg::field("x") };
  DiffHint b;
  b.kind = DiffHintKind::extra;
  b.path = { PathSeg::field("y") };
  const std::string out = render_diff_hints({ a, b });
  REQUIRE_THAT(out, Catch::Matchers::ContainsSubstring("x: missing"));
  REQUIRE_THAT(out, Catch::Matchers::ContainsSubstring("y: extra"));
  REQUIRE_THAT(out, Catch::Matchers::ContainsSubstring("; "));
}

// ===========================================================================
// Tag names
// ===========================================================================

TEST_CASE("client_message_name returns proto_step tags") {
  REQUIRE(client_message_name(ClientMessage{ Register{} }) == "register");
  REQUIRE(client_message_name(ClientMessage{ RegisterTraces{} }) == "register_traces");
  REQUIRE(client_message_name(ClientMessage{ RegisterTraceGen{} }) == "register_trace_gen");
  REQUIRE(client_message_name(ClientMessage{ RegisterExplore{} }) == "register_explore");
  REQUIRE(client_message_name(ClientMessage{ RegisterExploreSession{} }) == "register_explore_session");
  REQUIRE(client_message_name(ClientMessage{ RegisterValidate{} }) == "register_validate");
  REQUIRE(client_message_name(ClientMessage{ ExploreAssumeTransition{1} }) == "explore_assume_transition");
  REQUIRE(client_message_name(ClientMessage{ ExploreNextStep{} }) == "explore_next_step");
  REQUIRE(client_message_name(ClientMessage{ ExploreQueryState{} }) == "explore_query_state");
  REQUIRE(client_message_name(ClientMessage{ ExploreCheckInvariant{1} }) == "explore_check_invariant");
  REQUIRE(client_message_name(ClientMessage{ ExploreAssumeState{} }) == "explore_assume_state");
  REQUIRE(client_message_name(ClientMessage{ ExploreRollback{1} }) == "explore_rollback");
  REQUIRE(client_message_name(ClientMessage{ ExploreDone{} }) == "explore_done");
  REQUIRE(client_message_name(ClientMessage{ ReportState{} }) == "report_state");
}

TEST_CASE("mirror_message_name returns proto_step tags") {
  REQUIRE(mirror_message_name(MirrorMessage{ SpecValidated{ std::monostate{} } }) == "spec_validated");
  REQUIRE(mirror_message_name(MirrorMessage{ InitialState{} }) == "initial_state");
  REQUIRE(mirror_message_name(MirrorMessage{ NextStep{} }) == "next_step");
  REQUIRE(mirror_message_name(MirrorMessage{ StepOk{} }) == "step_ok");
  REQUIRE(mirror_message_name(MirrorMessage{ AllStepsDone{} }) == "all_steps_done");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreSessionDone{} }) == "explore_session_done");
  REQUIRE(mirror_message_name(MirrorMessage{ StepMismatch{} }) == "step_mismatch");
  REQUIRE(mirror_message_name(MirrorMessage{ GenTracesDone{} }) == "gen_traces_done");
  REQUIRE(mirror_message_name(MirrorMessage{ RegisterError{} }) == "register_error");
  REQUIRE(mirror_message_name(MirrorMessage{ ProtocolError{} }) == "protocol_error");
  REQUIRE(mirror_message_name(MirrorMessage{ ExplorerReady{} }) == "explorer_ready");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreTransitionStatus{} }) == "explore_transition_status");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreStepDone{} }) == "explore_step_done");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreState{} }) == "explore_state");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreInvariantStatus{} }) == "explore_invariant_status");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreAssumeStatus{} }) == "explore_assume_status");
  REQUIRE(mirror_message_name(MirrorMessage{ ExploreRollbackDone{} }) == "explore_rollback_done");
}

// ===========================================================================
// Phase guard
// ===========================================================================

TEST_CASE("phase guard: stepping flow") {
  PhaseGuard g;
  REQUIRE(g.phase() == Phase::idle);
  REQUIRE(g.sent(ClientMessage{ Register{} }).has_value());
  REQUIRE(g.phase() == Phase::validating);
  REQUIRE(g.received(MirrorMessage{ SpecValidated{ std::monostate{} } }).has_value());
  REQUIRE(g.phase() == Phase::ready);
  REQUIRE(g.received(MirrorMessage{ InitialState{ "Init", st({}) } }).has_value());
  REQUIRE(g.phase() == Phase::stepping);
  REQUIRE(g.sent(ClientMessage{ ReportState{ st({}) } }).has_value());
  REQUIRE(g.received(MirrorMessage{ NextStep{ "Tick", st({}) } }).has_value());
  REQUIRE(g.phase() == Phase::stepping);
  REQUIRE(g.received(MirrorMessage{ StepOk{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ AllStepsDone{} }).has_value());
  REQUIRE(g.phase() == Phase::done);
}

TEST_CASE("phase guard: report_state in wrong phase is protocol error") {
  PhaseGuard g;
  auto res = g.sent(ClientMessage{ ReportState{ st({}) } });
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::protocol);
  REQUIRE_THAT(res.error().message, Catch::Matchers::ContainsSubstring("stepping"));
}

TEST_CASE("phase guard: unexpected mirror message yields expected/got error") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ Register{} }).has_value());
  auto res = g.received(MirrorMessage{ StepOk{} });
  REQUIRE_FALSE(res.has_value());
  REQUIRE(res.error().kind == ErrorKind::protocol);
  REQUIRE_THAT(res.error().message, Catch::Matchers::ContainsSubstring("expected"));
  REQUIRE_THAT(res.error().message, Catch::Matchers::ContainsSubstring("got step_ok"));
  REQUIRE(g.phase() == Phase::validating);
}

TEST_CASE("phase guard: spec_validated invalid is done") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ RegisterValidate{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ SpecValidated{ std::string("bad spec") } }).has_value());
  REQUIRE(g.phase() == Phase::done);
}

TEST_CASE("phase guard: errors accepted at any point") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ Register{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ RegisterError{ "boom" } }).has_value());
  REQUIRE(g.received(MirrorMessage{ ProtocolError{ "boom" } }).has_value());
}

TEST_CASE("phase guard: explorer session flow") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ RegisterExploreSession{} }).has_value());
  REQUIRE(g.phase() == Phase::exploring);
  REQUIRE(g.received(MirrorMessage{ ExplorerReady{1, 2, 3} }).has_value());
  REQUIRE(g.sent(ClientMessage{ ExploreNextStep{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ ExploreState{ st({}) } }).has_value());
  REQUIRE(g.sent(ClientMessage{ ExploreRollback{1} }).has_value());
  REQUIRE(g.received(MirrorMessage{ ExploreRollbackDone{1} }).has_value());
  REQUIRE(g.sent(ClientMessage{ ExploreDone{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ ExploreSessionDone{} }).has_value());
  REQUIRE(g.phase() == Phase::done);
}

TEST_CASE("phase guard: gen_traces_done ends tracegen flow") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ RegisterTraceGen{} }).has_value());
  REQUIRE(g.received(MirrorMessage{ GenTracesDone{} }).has_value());
  REQUIRE(g.phase() == Phase::done);
}

TEST_CASE("phase_name returns readable names") {
  REQUIRE(std::string(phase_name(Phase::idle)) == "idle");
  REQUIRE(std::string(phase_name(Phase::validating)) == "validating");
  REQUIRE(std::string(phase_name(Phase::ready)) == "ready");
  REQUIRE(std::string(phase_name(Phase::stepping)) == "stepping");
  REQUIRE(std::string(phase_name(Phase::exploring)) == "exploring");
  REQUIRE(std::string(phase_name(Phase::done)) == "done");
}

// ===========================================================================
// Async job interface (guide §6)
// ===========================================================================

TEST_CASE("async: register_validate_async encodes corpus shape") {
  ApalacheConfig cfg;
  cfg.spec_path = "s.tla";
  cfg.invariant = "Inv";
  const nlohmann::json j = parse(enc(ClientMessage{ RegisterValidateAsync{cfg, 5, std::nullopt} }));
  REQUIRE(j["proto_step"] == "register_validate_async");
  REQUIRE(j["bound"] == 5);
  REQUIRE(j["apalacheConfig"]["specPath"] == "s.tla");
  REQUIRE_FALSE(j.contains("spec"));        // absent optionals omitted (C14)
  REQUIRE_FALSE(j["apalacheConfig"].contains("constInit"));
}

TEST_CASE("async: register_trace_gen_async encodes corpus shape") {
  ApalacheConfig cfg;
  cfg.spec_path = "s.tla";
  cfg.invariant = "Inv";
  cfg.length_bound = 3;
  TraceGenerationConfig tc;
  tc.num_traces = 2;
  const nlohmann::json j = parse(
      enc(ClientMessage{ RegisterTraceGenAsync{cfg, tc, std::nullopt, std::nullopt} }));
  REQUIRE(j["proto_step"] == "register_trace_gen_async");
  REQUIRE(j["traceConfig"]["numTraces"] == 2);
  REQUIRE_FALSE(j.contains("destPath"));
  REQUIRE_FALSE(j.contains("spec"));
}

TEST_CASE("async: job control messages encode corpus shape") {
  REQUIRE(parse(enc(ClientMessage{ QueryJob{ "job-7f3a" } }))
          == parse(R"({"proto_step":"query_job","jobId":"job-7f3a"})"));
  REQUIRE(parse(enc(ClientMessage{ AwaitJob{ "job-7f3a", 30 } }))
          == parse(R"({"proto_step":"await_job","jobId":"job-7f3a","timeoutSecs":30})"));
  REQUIRE(parse(enc(ClientMessage{ AwaitJob{ "job-7f3a", std::nullopt } }))
          == parse(R"({"proto_step":"await_job","jobId":"job-7f3a"})"));
  REQUIRE(parse(enc(ClientMessage{ CancelJob{ "job-7f3a" } }))
          == parse(R"({"proto_step":"cancel_job","jobId":"job-7f3a"})"));
}

TEST_CASE("async: job_accepted decodes both kinds") {
  {
    auto m = must_decode(R"({"proto_step":"job_accepted","jobId":"job-7f3a","kind":"validate"})");
    const auto& ja = std::get<JobAccepted>(m);
    REQUIRE(ja.job_id == "job-7f3a");
    REQUIRE(ja.kind == JobKind::validate);
  }
  {
    auto m = must_decode(R"({"proto_step":"job_accepted","jobId":"job-9","kind":"gen_traces"})");
    REQUIRE(std::get<JobAccepted>(m).kind == JobKind::gen_traces);
  }
}

TEST_CASE("async: job_status decodes every phase incl. unknown (C21)") {
  const std::pair<const char*, JobPhase> phases[] = {
      {"pending", JobPhase::pending},     {"running", JobPhase::running},
      {"done", JobPhase::done},           {"failed", JobPhase::failed},
      {"cancelled", JobPhase::cancelled}, {"unknown", JobPhase::unknown},
  };
  for (const auto& [tag, phase] : phases) {
    const std::string line =
        std::string(R"({"proto_step":"job_status","jobId":"j","phase":")") + tag + "\"}";
    auto m = must_decode(line);
    REQUIRE(std::get<JobStatus>(m).phase == phase);
  }
}

TEST_CASE("async: job_result decodes all three outcome alternatives") {
  {
    auto m = must_decode(R"({"proto_step":"job_result","jobId":"j","outcome":{"validate":"valid"}})");
    const auto& jr = std::get<JobResult>(m);
    const auto& sv = std::get<SpecValidated>(jr.outcome.value);
    REQUIRE(sv.is_valid());
  }
  {
    auto m = must_decode(
        R"({"proto_step":"job_result","jobId":"j","outcome":{"validate":{"invalid":"boom"}}})");
    const auto& sv = std::get<SpecValidated>(std::get<JobResult>(m).outcome.value);
    REQUIRE_FALSE(sv.is_valid());
    REQUIRE(*sv.invalid_text() == "boom");
  }
  {
    auto m = must_decode(
        R"({"proto_step":"job_result","jobId":"j","outcome":{"genTraces":{"itfTracePaths":["t1.itf.json"],"itfTraces":[]}}})");
    const auto& gt = std::get<GenTracesDone>(std::get<JobResult>(m).outcome.value);
    REQUIRE(gt.itf_trace_paths == std::vector<std::string>{ "t1.itf.json" });
  }
  {
    // infraError (guide §9): NOT a spec verdict; retryable.
    auto m = must_decode(R"({"proto_step":"job_result","jobId":"j","outcome":{"error":"worker died"}})");
    REQUIRE(std::get<std::string>(std::get<JobResult>(m).outcome.value) == "worker died");
  }
}

TEST_CASE("phase guard: async submit stays idle, job control legal in idle/done only") {
  PhaseGuard g;
  REQUIRE(g.sent(ClientMessage{ RegisterValidateAsync{} }).has_value());
  REQUIRE(g.phase() == Phase::idle);   // async submit is NOT a flow
  REQUIRE(g.received(MirrorMessage{ JobAccepted{ "j", JobKind::validate } }).has_value());
  REQUIRE(g.phase() == Phase::idle);
  REQUIRE(g.sent(ClientMessage{ QueryJob{ "j" } }).has_value());
  REQUIRE(g.received(MirrorMessage{ JobStatus{ "j", JobPhase::running } }).has_value());
  REQUIRE(g.sent(ClientMessage{ AwaitJob{ "j", 5 } }).has_value());
  REQUIRE(g.received(MirrorMessage{ JobResult{ "j", JobOutcome{SpecValidated{std::monostate{}}} } }).has_value());

  // Job control is also legal after a finished sync flow (done)…
  PhaseGuard g2;
  REQUIRE(g2.sent(ClientMessage{ RegisterValidate{} }).has_value());
  REQUIRE(g2.received(MirrorMessage{ SpecValidated{ std::monostate{} } }).has_value());
  REQUIRE(g2.phase() == Phase::ready);
  // …but not mid-flow:
  REQUIRE_FALSE(g2.sent(ClientMessage{ QueryJob{ "j" } }).has_value());
  REQUIRE_FALSE(g2.received(MirrorMessage{ JobStatus{ "j", JobPhase::running } }).has_value());

  // …and not during an explore session.
  PhaseGuard g3;
  REQUIRE(g3.sent(ClientMessage{ RegisterExploreSession{} }).has_value());
  REQUIRE_FALSE(g3.sent(ClientMessage{ CancelJob{ "j" } }).has_value());

  // Async submit mid-flow is rejected too.
  PhaseGuard g4;
  REQUIRE(g4.sent(ClientMessage{ Register{} }).has_value());
  REQUIRE_FALSE(g4.sent(ClientMessage{ RegisterTraceGenAsync{} }).has_value());
}

// ===========================================================================
// Round trips
// ===========================================================================

TEST_CASE("register round-trips through JSON shape") {
  ApalacheConfig cfg;
  cfg.spec_path = "p.tla";
  cfg.init_predicate = "Init";
  cfg.next_predicate = "Next";
  cfg.invariant = "TypeOK";
  cfg.length_bound = 100;
  cfg.param_vars = "x";
  Register msg;
  msg.cfg = cfg;
  msg.tc.num_traces = 4;
  msg.tc.view = "v";
  const nlohmann::json j = parse(enc(ClientMessage{ msg }));
  REQUIRE(j["apalacheConfig"]["initPredicate"] == "Init");
  REQUIRE(j["apalacheConfig"]["nextPredicate"] == "Next");
  REQUIRE(j["apalacheConfig"]["lengthBound"] == 100);
  REQUIRE(j["traceConfig"]["numTraces"] == 4);
  REQUIRE(j["traceConfig"]["view"] == "v");
}

