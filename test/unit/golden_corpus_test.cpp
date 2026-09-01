// golden_corpus_test.cpp — replay the vendored golden wire corpus
// (test/fixtures/golden/, frozen from Haskell ModelMirrors@3496251) against
// MirrorCPP's codecs (guide §10 item 1, drafts/conformance-gap-plan.md G2).
//
// Direction-aware replay:
//   - mirror_messages.jsonl  — DECODE-side: every line must decode, and the
//     decoded variant must match the constructor named in manifest.json.
//   - client_messages.jsonl  — ENCODE-side: the corpus line is parsed into a
//     ClientMessage, re-encoded, and compared semantically (absent ≡ explicit
//     null for the known optional keys — the decode_only.jsonl asymmetry).
//   - decode_only.jsonl      — JS-shape decode→re-encode vectors, incl. the
//     bare-integral-number → #bigint acceptance (C11 decode parity).
//   - consul_payloads.jsonl  — "health" lines replayed through the registry
//     entry parser; "register" lines are server-side (informational skips).
//   - diff_cases.jsonl       — 500 decode/equality vectors over every Value
//     constructor: (expected == actual) ⇔ haskell.tag == "match".
#include <mirrorcpp/mirrorcpp.hpp>
#include <mirrorcpp/registry.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace mirrorcpp;
using nlohmann::json;

namespace {

const std::string kGoldenDir = std::string(MIRRORCPP_TEST_FIXTURES_DIR) + "/golden";

std::vector<std::string> read_lines(const std::string& path) {
  std::ifstream f(path);
  REQUIRE(f.good());
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(f, line))
    if (!line.empty()) lines.push_back(line);
  return lines;
}

// ---------------------------------------------------------------------------
// Normalization: absent ≡ explicit null for the known optional keys (the
// corpus pins Haskell's explicit-null shape for register/register_trace_gen
// while MirrorCPP omits them — both conforming per guide C14). Scrubbing is
// restricted to the message envelope + apalacheConfig/traceConfig so ITF null
// values inside states are never touched.
// ---------------------------------------------------------------------------
const std::set<std::string> kOptionalKeys = {
    "spec", "destPath", "view", "initPredicate", "nextPredicate",
    "constInit", "maxSteps", "timeoutSecs", "itfTraces"};

void scrub(json& o) {
  for (const auto& k : kOptionalKeys)
    if (o.contains(k) && o.at(k).is_null()) o.erase(k);
}

json normalize_msg(json j) {
  scrub(j);
  if (j.contains("apalacheConfig")) scrub(j["apalacheConfig"]);
  if (j.contains("traceConfig")) scrub(j["traceConfig"]);
  return j;
}

// ---------------------------------------------------------------------------
// Test-local ClientMessage builder from a parsed corpus line. Throws
// JsonError (via decode_state) on values outside the ITF grammar — the
// decode_only "error:" cases rely on that.
// ---------------------------------------------------------------------------
ApalacheConfig cfg_from(const json& j) {
  ApalacheConfig c;
  c.spec_path = j.at("specPath").get<std::string>();
  auto opt = [&](const char* k) -> std::optional<std::string> {
    auto it = j.find(k);
    if (it == j.end() || it->is_null()) return std::nullopt;
    return it->get<std::string>();
  };
  c.init_predicate = opt("initPredicate");
  c.next_predicate = opt("nextPredicate");
  c.const_init = opt("constInit");
  c.invariant = j.value("invariant", "");
  c.length_bound = j.value("lengthBound", 10LL);
  c.param_vars = j.value("paramVars", "");
  return c;
}

TraceGenerationConfig tc_from(const json& j) {
  TraceGenerationConfig t;
  t.num_traces = j.value("numTraces", 1LL);
  auto it = j.find("view");
  if (it != j.end() && !it->is_null()) t.view = it->get<std::string>();
  return t;
}

std::optional<ApalacheSpec> opt_spec_from(const json& j) {
  auto it = j.find("spec");
  if (it == j.end() || it->is_null()) return std::nullopt;
  ApalacheSpec s;
  s.sources = it->at("sources").get<std::vector<std::string>>();
  return s;
}

ApalacheSpec req_spec_from(const json& j) {
  ApalacheSpec s;
  s.sources = j.at("spec").at("sources").get<std::vector<std::string>>();
  return s;
}

std::optional<std::string> opt_string(const json& j, const char* k) {
  auto it = j.find(k);
  if (it == j.end() || it->is_null()) return std::nullopt;
  return it->get<std::string>();
}

ClientMessage client_message_from_json(const json& j) {
  const std::string tag = j.at("proto_step").get<std::string>();
  if (tag == "register")
    return Register{cfg_from(j.at("apalacheConfig")), tc_from(j.at("traceConfig")),
                    opt_spec_from(j)};
  if (tag == "register_traces")
    return RegisterTraces{cfg_from(j.at("apalacheConfig")),
                          j.at("itfTracePaths").get<std::vector<std::string>>()};
  if (tag == "register_trace_gen")
    return RegisterTraceGen{cfg_from(j.at("apalacheConfig")), tc_from(j.at("traceConfig")),
                            opt_string(j, "destPath"), opt_spec_from(j)};
  if (tag == "register_explore") {
    RegisterExplore m{req_spec_from(j),
                      j.at("invariants").get<std::vector<std::string>>(),
                      j.at("exports").get<std::vector<std::string>>(), std::nullopt};
    if (auto it = j.find("maxSteps"); it != j.end() && !it->is_null())
      m.max_steps = it->get<long long>();
    return m;
  }
  if (tag == "register_explore_session")
    return RegisterExploreSession{req_spec_from(j),
                                  j.at("invariants").get<std::vector<std::string>>(),
                                  j.at("exports").get<std::vector<std::string>>()};
  if (tag == "register_validate")
    return RegisterValidate{cfg_from(j.at("apalacheConfig")), j.at("bound").get<long long>(),
                            opt_spec_from(j)};
  if (tag == "register_validate_async")
    return RegisterValidateAsync{cfg_from(j.at("apalacheConfig")),
                                 j.at("bound").get<long long>(), opt_spec_from(j)};
  if (tag == "register_trace_gen_async")
    return RegisterTraceGenAsync{cfg_from(j.at("apalacheConfig")),
                                 tc_from(j.at("traceConfig")),
                                 opt_string(j, "destPath"), opt_spec_from(j)};
  if (tag == "query_job") return QueryJob{j.at("jobId").get<std::string>()};
  if (tag == "await_job") {
    AwaitJob m{j.at("jobId").get<std::string>(), std::nullopt};
    if (auto it = j.find("timeoutSecs"); it != j.end() && !it->is_null())
      m.timeout_secs = it->get<long long>();
    return m;
  }
  if (tag == "cancel_job") return CancelJob{j.at("jobId").get<std::string>()};
  if (tag == "explore_assume_transition")
    return ExploreAssumeTransition{j.at("transitionId").get<long long>()};
  if (tag == "explore_next_step") return ExploreNextStep{};
  if (tag == "explore_query_state") return ExploreQueryState{};
  if (tag == "explore_check_invariant")
    return ExploreCheckInvariant{j.at("invariantId").get<long long>()};
  if (tag == "explore_assume_state") return ExploreAssumeState{decode_state(j.at("state"))};
  if (tag == "explore_rollback") return ExploreRollback{j.at("snapshotId").get<long long>()};
  if (tag == "explore_done") return ExploreDone{};
  if (tag == "report_state") return ReportState{decode_state(j.at("state"))};
  FAIL("unknown client proto_step in corpus: " + tag);
  return ExploreDone{};  // unreachable
}

// Haskell ctor name ("SpecValidated.ok") -> wire tag ("spec_validated").
std::string ctor_to_tag(std::string ctor) {
  const auto dot = ctor.find('.');
  if (dot != std::string::npos) ctor.resize(dot);
  static const std::map<std::string, std::string> kMirror = {
      {"SpecValidated", "spec_validated"},   {"InitialState", "initial_state"},
      {"NextStep", "next_step"},             {"StepOk", "step_ok"},
      {"StepMismatch", "step_mismatch"},     {"AllStepsDone", "all_steps_done"},
      {"GenTracesDone", "gen_traces_done"},  {"RegisterError", "register_error"},
      {"ProtocolError", "protocol_error"},   {"ExplorerReady", "explorer_ready"},
      {"ExploreTransitionStatus", "explore_transition_status"},
      {"ExploreStepDone", "explore_step_done"},
      {"ExploreState", "explore_state"},
      {"ExploreInvariantStatus", "explore_invariant_status"},
      {"ExploreAssumeStatus", "explore_assume_status"},
      {"ExploreRollbackDone", "explore_rollback_done"},
      {"ExploreSessionDone", "explore_session_done"},
      {"JobAccepted", "job_accepted"},       {"JobStatus", "job_status"},
      {"JobResult", "job_result"},
  };
  auto it = kMirror.find(ctor);
  REQUIRE(it != kMirror.end());
  return it->second;
}

}  // namespace

TEST_CASE("golden: mirror_messages.jsonl decodes to the manifest constructors", "[golden]") {
  const auto lines = read_lines(kGoldenDir + "/mirror_messages.jsonl");
  std::ifstream mf(kGoldenDir + "/manifest.json");
  REQUIRE(mf.good());
  const json manifest = json::parse(mf);

  for (const auto& entry : manifest.at("mirror")) {
    const std::size_t line_no = entry.at("line").get<std::size_t>();  // 1-based
    INFO("line " + std::to_string(line_no) + " ctor " + entry.at("ctor").get<std::string>());
    REQUIRE(line_no >= 1);
    REQUIRE(line_no <= lines.size());
    auto msg = decode_mirror_message(lines[line_no - 1]);
    REQUIRE(msg.has_value());
    CHECK(std::string(mirror_message_name(*msg)) == ctor_to_tag(entry.at("ctor")));
  }
}

TEST_CASE("golden: client_messages.jsonl re-encodes semantically", "[golden]") {
  const auto lines = read_lines(kGoldenDir + "/client_messages.jsonl");
  std::ifstream mf(kGoldenDir + "/manifest.json");
  REQUIRE(mf.good());
  const json manifest = json::parse(mf);

  for (const auto& entry : manifest.at("client")) {
    const std::size_t line_no = entry.at("line").get<std::size_t>();  // 1-based
    INFO("line " + std::to_string(line_no) + " ctor " + entry.at("ctor").get<std::string>());
    REQUIRE(line_no >= 1);
    REQUIRE(line_no <= lines.size());
    const json corpus = json::parse(lines[line_no - 1]);
    const ClientMessage msg = client_message_from_json(corpus);
    const json ours = json::parse(encode_client_message(msg));
    CHECK(normalize_msg(ours) == normalize_msg(corpus));
  }
}

TEST_CASE("golden: decode_only.jsonl JS-shape decode→re-encode vectors", "[golden]") {
  const auto lines = read_lines(kGoldenDir + "/decode_only.jsonl");
  std::size_t ok_count = 0, err_count = 0;
  for (const auto& line : lines) {
    const json rec = json::parse(line);
    const std::string wire = rec.at("json").get<std::string>();
    const std::string expect = rec.at("expect").get<std::string>();
    INFO("json " + wire);
    if (expect.rfind("ok:", 0) == 0) {
      ++ok_count;
      const json corpus = json::parse(wire);
      const ClientMessage msg = client_message_from_json(corpus);  // must not throw
      const json ours = json::parse(encode_client_message(msg));
      const json canonical = json::parse(expect.substr(3));
      CHECK(normalize_msg(ours) == normalize_msg(canonical));
    } else {
      ++err_count;
      // Reference rejects this input; MirrorCPP must reject it too.
      CHECK_THROWS(client_message_from_json(json::parse(wire)));
    }
  }
  CHECK(ok_count > 0);
  CHECK(err_count > 0);  // the bare 0.5 case pins rejection of non-integral floats
}

// MirrorCPP's documented discovery rule (design 5.4): an entry is a candidate
// iff Service.Address is non-blank, Service.Port is an integer in 1..65535,
// and a PRESENT cert-sha256 is a well-formed 64-hex pin (a malformed pin skips
// the entry — no unpinned fallback). The reference health parser accepts the
// entry and defers pin validation to connect time; the corpus's "fp1" entry
// pins that divergence. This predicate mirrors OUR rule so the test asserts
// MirrorCPP returns exactly the well-formed subset.
bool well_formed_entry(const json& entry) {
  auto svc = entry.find("Service");
  if (svc == entry.end() || !svc->is_object()) return false;
  auto addr = svc->find("Address");
  if (addr == svc->end() || !addr->is_string()) return false;
  {
    const std::string a = addr->get<std::string>();
    if (a.find_first_not_of(" \t\r\n") == std::string::npos) return false;
  }
  auto port = svc->find("Port");
  if (port == svc->end() || !port->is_number_integer()) return false;
  const auto p = port->get<long long>();
  if (p < 1 || p > 65535) return false;
  auto meta = svc->find("Meta");
  if (meta != svc->end() && meta->is_object()) {
    auto pin = meta->find("cert-sha256");
    if (pin != meta->end()) {
      if (!pin->is_string()) return false;
      const std::string s = pin->get<std::string>();
      if (s.size() != 64) return false;
      for (const char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
  }
  return true;
}

TEST_CASE("golden: consul_payloads.jsonl health responses parse fail-closed", "[golden]") {
  const auto lines = read_lines(kGoldenDir + "/consul_payloads.jsonl");
  std::size_t health = 0, skipped_register = 0;
  for (const auto& line : lines) {
    const json rec = json::parse(line);
    const std::string kind = rec.at("kind").get<std::string>();
    const std::string body = rec.at("json").get<std::string>();
    const std::string expect = rec.at("expect").get<std::string>();
    INFO("kind " + kind + " body " + body);
    if (kind == "register") {
      ++skipped_register;  // server-side advertisement validation — N/A to a client
      continue;
    }
    ++health;
    const auto infos = detail::parse_registry_entries(body);
    if (expect.rfind("ok:", 0) == 0) {
      // Assert against the well-formed subset of the INPUT (our documented
      // rule), which may be smaller than the reference's accepted list (the
      // "fp1" divergence above).
      const json input = json::parse(body);
      std::vector<json> expected;
      for (const auto& e : input)
        if (well_formed_entry(e)) expected.push_back(e.at("Service"));
      REQUIRE(infos.size() == expected.size());
      for (std::size_t i = 0; i < infos.size(); ++i) {
        CHECK(infos[i].host == expected[i].at("Address").get<std::string>());
        CHECK(infos[i].port == expected[i].at("Port").get<long long>());
        auto meta = expected[i].find("Meta");
        if (meta != expected[i].end() && meta->contains("cert-sha256")) {
          REQUIRE(infos[i].cert_sha256.has_value());
          std::string pin = meta->at("cert-sha256").get<std::string>();
          for (auto& c : pin) c = static_cast<char>(std::tolower(c));
          CHECK(*infos[i].cert_sha256 == pin);
        }
      }
    } else {
      // Reference rejects the response; fail-closed parsing yields no candidates.
      CHECK(infos.empty());
    }
  }
  CHECK(health == 4);
  CHECK(skipped_register == 3);
}

// The mirror's filterMeta (guide C9): top-level keys starting with '#', plus
// "action_taken" and "parameters", are excluded from state comparison. The
// diff corpus's "match" verdicts are computed AFTER this filter, so the
// equality replay must apply it too.
State filter_meta(const State& s) {
  State out;
  for (const auto& [k, v] : s)
    if (!k.empty() && k[0] != '#' && k != "action_taken" && k != "parameters")
      out.emplace(k, v);
  return out;
}

TEST_CASE("golden: diff_cases.jsonl value decode + equality semantics", "[golden]") {
  const auto lines = read_lines(kGoldenDir + "/diff_cases.jsonl");
  REQUIRE(lines.size() == 500);
  std::size_t matches = 0;
  for (const auto& line : lines) {
    const json rec = json::parse(line);
    INFO("line " + line);
    const State expected = filter_meta(decode_state(rec.at("expected")));
    const State actual = filter_meta(decode_state(rec.at("actual")));
    const bool is_match = rec.at("haskell").at("tag").get<std::string>() == "match";
    if (is_match) ++matches;
    CHECK((expected == actual) == is_match);
  }
  CHECK(matches == 257);  // pinned corpus shape (257 match / 243 mismatch)
}
