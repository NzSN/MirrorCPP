#include <mirrorcpp/schedule_exploration.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace mirrorcpp::schedule {
using nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
bool id(std::string_view text) {
  return !text.empty() && text.size() <= 128 && std::ranges::all_of(text, [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.' || ch == '/';
  });
}
void require(bool condition, std::string_view message) {
  if (!condition) throw std::invalid_argument(std::string(message));
}
json canonical_value(const Value& value, std::size_t depth, std::size_t& nodes) {
  require(depth <= 32 && ++nodes <= 65'536, "canonical value exceeds structural bound");
  using Kind = Value::Kind;
  switch (value.kind()) {
    case Kind::null: return json::array({"null"});
    case Kind::integer: return json::array({"integer", value.get<Value::Int>().str()});
    case Kind::boolean: return json::array({"boolean", value.get<bool>()});
    case Kind::string: return json::array({"string", value.get<std::string>()});
    case Kind::unserializable: return json::array({"unserializable", value.get<Value::Unserializable>().text});
    case Kind::variant: {
      const auto& v = value.get<Value::Variant>();
      return json::array({"variant", v.tag, canonical_value(*v.value, depth + 1, nodes)});
    }
    case Kind::record: {
      json fields = json::array();
      for (const auto& [key, item] : value.get<Value::Record>().fields)
        fields.push_back(json::array({key, canonical_value(item, depth + 1, nodes)}));
      return json::array({"record", fields});
    }
    case Kind::map: {
      std::map<std::string, json> entries;
      std::optional<Kind> key_kind;
      for (const auto& [key, item] : value.get<Value::Map>().entries) {
        require(key.kind() == Kind::string || key.kind() == Kind::integer, "unsupported canonical map key");
        if (!key_kind) key_kind = key.kind();
        require(*key_kind == key.kind(), "mixed canonical map keys");
        const auto canonical_key = canonical_value(key, depth + 1, nodes);
        const auto pair = json::array({canonical_key, canonical_value(item, depth + 1, nodes)});
        require(entries.emplace(canonical_key.dump(), pair).second, "duplicate canonical map key");
      }
      json rows = json::array();
      for (const auto& [_, pair] : entries) rows.push_back(pair);
      return json::array({"map", rows});
    }
    case Kind::set: {
      std::map<std::string, json> members;
      for (const auto& item : value.get<Value::Set>().elems) {
        auto canonical = canonical_value(item, depth + 1, nodes);
        const auto key = canonical.dump();
        members.emplace(key, std::move(canonical));
      }
      json rows = json::array();
      for (const auto& [_, item] : members) rows.push_back(item);
      return json::array({"set", rows});
    }
    case Kind::seq:
    case Kind::tuple: {
      json items = json::array();
      const auto& values = value.kind() == Kind::seq ? value.get<Value::Seq>().elems : value.get<Value::Tuple>().elems;
      for (const auto& item : values) items.push_back(canonical_value(item, depth + 1, nodes));
      return json::array({value.kind() == Kind::seq ? "sequence" : "tuple", items});
    }
  }
  throw std::invalid_argument("unknown canonical value");
}
struct Enumeration {
  std::vector<std::vector<Step>> schedules;
  bool exhausted = true;
  std::string stop_reason;
};
Enumeration enumerate(const std::vector<ActorChain>& actors, std::size_t max_preemptions,
                       std::size_t cap, Clock::time_point deadline, std::stop_token stop) {
  Enumeration result;
  std::vector<std::size_t> positions(actors.size(), 0);
  std::vector<Step> current;
  std::size_t length = 0;
  for (const auto& actor : actors) length += actor.checkpoints.size();
  std::function<void(std::optional<std::size_t>, std::size_t)> visit;
  visit = [&](std::optional<std::size_t> prior, std::size_t preemptions) {
    if (!result.exhausted) return;
    if (stop.stop_requested() || Clock::now() >= deadline) {
      result.exhausted = false;
      result.stop_reason = stop.stop_requested() ? "cancelled" : "time_budget";
      return;
    }
    if (current.size() == length) {
      if (result.schedules.size() == cap) {
        result.exhausted = false; result.stop_reason = "enumeration_limit";
      } else result.schedules.push_back(current);
      return;
    }
    for (std::size_t index = 0; index < actors.size(); ++index) {
      if (positions[index] == actors[index].checkpoints.size()) continue;
      const bool preemption = prior && *prior != index && positions[*prior] < actors[*prior].checkpoints.size();
      if (preemptions + (preemption ? 1 : 0) > max_preemptions) continue;
      const auto offset = positions[index]++;
      current.push_back({actors[index].actor.actor, actors[index].checkpoints[offset]});
      visit(index, preemptions + (preemption ? 1 : 0));
      current.pop_back(); --positions[index];
      if (!result.exhausted) return;
    }
  };
  visit(std::nullopt, 0);
  return result;
}
std::map<std::string, std::string> actor_map(const json& actors) {
  require(actors.is_array(), "missing execution actors");
  std::map<std::string, std::string> result;
  for (const auto& actor : actors) {
    require(actor.is_object() && actor.size() == 2 && actor.contains("actor") && actor.contains("operation"), "invalid execution actor");
    require(result.emplace(actor.at("actor").get<std::string>(), actor.at("operation").get<std::string>()).second,
            "duplicate execution actor");
  }
  return result;
}
std::string category(const json& execution, const json& comparison) {
  if (execution.at("cleanup") != "confirmed" || !execution.at("remainingActors").empty()) return "cleanup_failed";
  if (!comparison.is_null()) {
    require(comparison.at("schema") == "mirrors.scheduled-comparison/v1", "invalid comparison sample");
    const auto& records = comparison.at("binding").at("executions");
    require(records.size() == 1 && records.at(0) == execution, "comparison and schedule evidence differ");
    if (comparison.at("comparison") == "step_mismatch") {
      require(comparison.at("passed") == false && comparison.at("client").at("kind") == "step_mismatch", "invalid mismatch sample");
      return "model_mismatch";
    }
    if (comparison.at("comparison") != "matched" || comparison.at("passed") != true) return "comparison_failed";
  }
  if (execution.at("passed") == true) return "passed";
  const auto outcome = execution.at("outcome").get<std::string>();
  if (outcome == "timed_out") return "timed_out";
  if (outcome == "cancelled") return "cancelled";
  if (outcome == "unexpected_checkpoint" || outcome == "invalid_schedule" || outcome == "incompatible_identity") return "schedule_failed";
  return "execution_failed";
}
void validate_success(const json& execution, const Schedule& schedule) {
  require(execution.at("scheduleCompleted") == true && execution.at("outcome") == "completed", "false completed schedule");
  const auto& events = execution.at("events");
  require(events.is_array() && events.size() == schedule.steps.size() * 2, "wrong accepted interval denominator");
  for (std::size_t i = 0; i < schedule.steps.size(); ++i) {
    for (std::size_t offset = 0; offset < 2; ++offset) {
      const auto& event = events.at(i * 2 + offset);
      require(event.at("ordinal") == i * 2 + offset && event.at("step") == i && event.at("actor") == schedule.steps[i].actor,
              "execution event identity differs");
      require(event.at("kind") == (offset == 0 ? "permit" : "arrival"), "execution event order differs");
      if (offset == 1) require(event.at("checkpoint") == schedule.steps[i].checkpoint, "execution arrival differs");
    }
  }
  require(execution.at("observations").size() == schedule.steps.size() + 1, "wrong observation denominator");
}
}  // namespace

std::string canonical_state(const State& state) {
  json variables = json::array(); std::size_t nodes = 0;
  for (const auto& [key, value] : state) variables.push_back(json::array({key, canonical_value(value, 0, nodes)}));
  const auto result = json::array({"mirrors.canonical-state/v1", variables}).dump();
  require(result.size() <= 1'048'576, "canonical state exceeds byte bound");
  return result;
}
ExplorationRunner local_exploration_runner(Adapter adapter, Policy policy) {
  return [adapter=std::move(adapter), policy](const Schedule& schedule, std::stop_token stop) {
    auto execution = run_schedule(schedule, adapter, policy, stop);
    return ExplorationSample{execution->receipt(), nullptr};
  };
}
ExplorationSample comparison_sample(const ReplayResult& result) {
  const auto& executions = result.evidence.at("binding").at("executions");
  require(executions.is_array() && executions.size() == 1, "exploration comparison needs exactly one execution");
  return {executions.at(0), result.evidence};
}
json explore_finite(const FiniteSpace& space, const ExplorationRunner& runner,
                    const ExplorationLimits& limits, std::stop_token stop) {
  const auto start = Clock::now();
  json result = {{"schema", "mirrors.finite-exploration/v1"}, {"profile", exploration_profile},
    {"complete", false}, {"enumerationExhausted", false}, {"denominatorKnown", false},
    {"eligibleRuns", nullptr}, {"attemptedRuns", 0}, {"completedRuns", 0},
    {"runs", json::array()}, {"states", json::array()}, {"transitions", json::array()},
    {"categories", json::object()}, {"firstCounterexample", nullptr}, {"firstFailure", nullptr}, {"por", "disabled"},
    {"projection", {{"baseVariables", space.state_variables}, {"instrumentationVariables", space.instrumentation_variables}}},
    {"comparison", "not_requested"}, {"comparisonRequired", space.require_model_comparison}, {"comparisonRuns", 0}};
  try {
    require(static_cast<bool>(runner), "missing exploration runner");
    require(!space.actors.empty() && space.actors.size() <= 8 && !space.inputs.empty() && space.inputs.size() <= 64,
            "finite actor/input bound exceeded");
    require(space.max_preemptions <= 64 && limits.max_enumerated_schedules > 0 && limits.max_enumerated_schedules <= 4096 &&
            limits.max_runs <= 65'536 && limits.time_budget.count() > 0 && limits.time_budget <= std::chrono::hours(24) &&
            limits.max_evidence_bytes > 0 && limits.max_evidence_bytes <= 64 * 1'048'576, "invalid exploration limits");
    auto actors = space.actors;
    std::ranges::sort(actors, {}, [](const auto& actor) { return actor.actor.actor; });
    std::map<std::string, std::string> declarations;
    Schedule check; check.identity = space.identity;
    std::size_t total_steps = 0;
    for (const auto& actor : actors) {
      require(id(actor.actor.actor) && id(actor.actor.operation) && declarations.emplace(actor.actor.actor, actor.actor.operation).second,
              "duplicate/invalid exploration actor");
      require(!actor.checkpoints.empty() && actor.checkpoints.back() == completion, "actor chain needs terminal completion");
      for (std::size_t i = 0; i < actor.checkpoints.size(); ++i) {
        const auto& point = actor.checkpoints[i];
        require(i + 1 == actor.checkpoints.size() ? point == completion : id(point), "invalid actor chain checkpoint");
        check.steps.push_back({actor.actor.actor, point});
      }
      total_steps += actor.checkpoints.size();
    }
    require(total_steps <= 64, "exploration step bound exceeded");
    std::set<std::string> input_keys, variables, ignored;
    for (const auto& input : space.inputs) {
      check.inputs = input;
      (void)encode_schedule(check);
      require(input.dump().size() <= 65'535, "exploration input exceeds byte bound");
      require(input_keys.insert(input.dump()).second, "duplicate declared input assignment");
    }
    require(!space.state_variables.empty() && space.state_variables.size() <= 1024 && space.instrumentation_variables.size() <= 1024, "bounded base observation variables required");
    for (const auto& key : space.state_variables)
      require(id(key) && variables.insert(key).second, "duplicate/invalid state variable");
    for (const auto& key : space.instrumentation_variables)
      require(id(key) && !variables.contains(key) && ignored.insert(key).second, "invalid instrumentation projection");
    const auto deadline = start + limits.time_budget;
    const auto enumeration = enumerate(actors, space.max_preemptions, limits.max_enumerated_schedules, deadline, stop);
    result["denominatorKnown"] = enumeration.exhausted;
    result["enumeratedScheduleShapes"] = enumeration.schedules.size();
    result["inputAssignments"] = space.inputs.size();
    result["maxPreemptions"] = space.max_preemptions;
    result["declaredInputs"] = space.inputs;
    result["limits"] = {{"maxEnumeratedSchedules", limits.max_enumerated_schedules}, {"maxRuns", limits.max_runs},
                         {"timeBudgetMs", limits.time_budget.count()}, {"maxEvidenceBytes", limits.max_evidence_bytes}};
    const auto eligible = enumeration.schedules.size() * space.inputs.size();
    result["eligibleRunsLowerBound"] = eligible;
    if (enumeration.exhausted) result["eligibleRuns"] = eligible;
    std::string stop_reason = enumeration.stop_reason;
    std::map<std::string, std::size_t> states;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> transitions;
    std::set<std::string> executions;
    std::size_t attempted = 0, completed = 0, bytes = 0, coverage_bytes = 0, comparison_runs = 0;
    bool halt = false;
    for (const auto& steps : enumeration.schedules) {
      for (std::size_t input_index = 0; input_index < space.inputs.size(); ++input_index) {
        if (stop.stop_requested() || Clock::now() >= deadline || attempted >= limits.max_runs) {
          stop_reason = stop.stop_requested() ? "cancelled" : Clock::now() >= deadline ? "time_budget" : "run_limit";
          halt = true; break;
        }
        Schedule candidate; candidate.identity = space.identity; candidate.steps = steps; candidate.inputs = space.inputs[input_index];
        json row = {{"ordinal", attempted}, {"inputIndex", input_index}, {"schedule", json::parse(encode_schedule(candidate))}};
        ++attempted;
        std::string disposition;
        bool cleanup_known = false;
        try {
          auto sample = runner(candidate, stop);
          auto& execution = sample.execution;
          row["execution"] = execution;
          if (!sample.comparison.is_null()) row["comparison"] = sample.comparison;
          require(execution.at("schema") == receipt_schema && execution.at("schedule") == row.at("schedule"),
                  "runner evidence is not bound to the selected schedule");
          require(actor_map(execution.at("actors")) == declarations, "runner actor declarations differ");
          const auto execution_id = execution.at("executionId").get<std::string>();
          require(execution_id.size() == 32 && std::ranges::all_of(execution_id, [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
          }) && executions.insert(execution_id).second, "invalid or duplicate execution identity");
          cleanup_known = execution.at("cleanup") == "confirmed" && execution.at("remainingActors").empty();
          disposition = category(execution, sample.comparison);
          if (space.require_model_comparison && sample.comparison.is_null()) disposition = "comparison_missing";
          row["execution"] = execution;
          if (!sample.comparison.is_null()) { ++comparison_runs; row["comparison"] = sample.comparison; result["comparison"] = "requested"; }
          if (disposition == "passed") {
            validate_success(execution, candidate);
            std::optional<std::size_t> prior;
            for (std::size_t i = 0; i < execution.at("observations").size(); ++i) {
              const auto& observation = execution.at("observations").at(i);
              require(observation.at("afterSteps") == i, "observation position differs");
              auto state = decode_state(observation.at("state"));
              require(state.size() == variables.size() + ignored.size(), "observation projection omits or adds fields");
              for (const auto& key : ignored) require(state.erase(key) == 1, "declared instrumentation variable absent");
              for (const auto& key : variables) require(state.contains(key), "base observation variable absent");
              const auto canonical = canonical_state(state);
              if (!states.contains(canonical)) {
                if (coverage_bytes + canonical.size() + 128 > 8 * 1'048'576) throw std::length_error("coverage byte bound exceeded");
                coverage_bytes += canonical.size() + 128;
              }
              auto [entry, inserted] = states.emplace(canonical, states.size());
              if (inserted) result["states"].push_back({{"id", entry->second}, {"canonical", json::parse(canonical)}});
              if (prior) {
                const auto edge = std::pair{*prior, entry->second};
                if (!transitions.contains(edge)) {
                  if (coverage_bytes + 128 > 8 * 1'048'576) throw std::length_error("coverage byte bound exceeded");
                  coverage_bytes += 128;
                }
                ++transitions[edge];
              }
              prior = entry->second;
            }
            ++completed;
          } else if (disposition == "model_mismatch" && result["firstCounterexample"].is_null()) {
            result["firstCounterexample"] = {{"ordinal", attempted - 1}, {"category", disposition}, {"schedule", row.at("schedule")}};
          }
        } catch (const std::length_error& error) {
          disposition = "coverage_failed"; row["error"] = error.what(); stop_reason = "coverage_limit"; halt = true;
        } catch (const std::exception& error) {
          disposition = "runner_or_evidence_failed";
          row["error"] = std::string(error.what()).substr(0, 1024);
        } catch (...) {
          disposition = "runner_or_evidence_failed"; row["error"] = "nonstandard runner failure";
        }
        row["category"] = disposition;
        if (disposition != "passed" && result["firstFailure"].is_null())
          result["firstFailure"] = {{"ordinal", attempted - 1}, {"category", disposition}, {"schedule", row.at("schedule")}};
        auto& count = result["categories"][disposition]; count = count.is_number_integer() ? count.get<std::size_t>() + 1 : 1;
        const auto row_bytes = row.dump().size();
        if (row_bytes > limits.max_evidence_bytes - std::min(bytes, limits.max_evidence_bytes)) {
          result["runs"].push_back({{"ordinal", attempted - 1}, {"category", disposition}, {"evidenceOmitted", "byte_budget"}});
          stop_reason = "evidence_limit"; halt = true; break;
        }
        bytes += row_bytes; result["runs"].push_back(std::move(row));
        if (halt) break;
        if (!cleanup_known) { stop_reason = "unconfirmed_cleanup"; halt = true; break; }
        if (Clock::now() >= deadline) { stop_reason = "time_budget"; halt = true; break; }
      }
      if (halt) break;
    }
    for (const auto& [edge, count] : transitions)
      result["transitions"].push_back({{"from", edge.first}, {"to", edge.second}, {"count", count}});
    result["comparisonRuns"] = comparison_runs;
    result["attemptedRuns"] = attempted;
    result["completedRuns"] = completed;
    result["enumerationExhausted"] = enumeration.exhausted && attempted == eligible;
    result["complete"] = result["enumerationExhausted"] == true && completed == eligible && !halt;
    result["stopReason"] = stop_reason.empty() ? (completed == eligible ? "complete" : "non_pass_runs") : stop_reason;
    result["status"] = result["complete"] == true ? "complete" : "incomplete";
  } catch (const std::exception& error) {
    result["status"] = "invalid_declaration"; result["stopReason"] = "invalid_declaration";
    result["error"] = std::string(error.what()).substr(0, 1024);
  }
  result["elapsedMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  return result;
}
}  // namespace mirrorcpp::schedule
