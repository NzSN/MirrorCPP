#include <mirrorcpp/schedule_sync.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <exception>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

namespace mirrorcpp::schedule_sync {
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace detail {
struct Pending { std::string op, resource; std::int64_t value = 0; };
struct Actor {
  std::string id;
  std::thread::id thread;
  Pending pending;
  bool parked = false, grant = false, finished = false;
  std::size_t version = 0;
};
struct Owned { Resource declaration; std::string owner; std::deque<std::int64_t> queue; };
struct State {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Actor> actors;
  std::map<std::string, Owned> resources;
  std::thread::id controller;
  Report report;
  bool cancelled = false, execution_ended = false;
  json events = json::array(), observations = json::array(), failure_graph = nullptr;
  Policy policy;
  Clock::time_point deadline = Clock::time_point::max();
  std::string execution_id;
  void fail(std::string outcome, std::string detail) {
    if (report.outcome == "completed") {
      report.outcome = std::move(outcome); report.detail = std::move(detail);
      json owners = json::object(), waits = json::array(), contents = json::object();
      for (const auto& [id, resource] : resources) { owners[id] = resource.owner; contents[id] = resource.queue; }
      for (const auto& actor : actors) if (!actor.finished && actor.parked)
        waits.push_back({{"actor",actor.id},{"operation",actor.pending.op},{"resource",actor.pending.resource}});
      failure_graph = {{"owners",owners},{"waits",waits},{"contents",contents}};
    }
    cancelled = true; cv.notify_all();
  }
  bool execution_expired(const char* where) {
    if (execution_ended || Clock::now() < deadline) return false;
    report.execution_budget_exceeded = true;
    fail("timed_out", where);
    return true;
  }
  void cleanup_expired() {
    report.cleanup_budget_exceeded = true;
    fail("timed_out", "cleanup deadline exceeded");
  }
  bool evidence_fits(const json& destination, const json& row) const {
    const auto separator = destination.empty() ? 0u : 1u;
    return events.dump().size() + observations.dump().size() + row.dump().size() + separator <= policy.max_evidence_bytes;
  }
  bool event(json row) {
    row["ordinal"] = events.size();
    if (!evidence_fits(events, row)) {
      fail("evidence_exhausted", "receipt byte bound"); return false;
    }
    events.push_back(std::move(row)); return true;
  }
  bool enabled(std::size_t i) const {
    const auto& a = actors[i];
    if (!a.parked || a.finished) return false;
    const auto& p = a.pending;
    if (p.op == "start" || p.op == "checkpoint" || p.op == "$done") return true;
    const auto found = resources.find(p.resource);
    if (found == resources.end()) return false;
    const auto& r = found->second;
    if (p.op == "lock") return r.owner.empty();
    if (p.op == "unlock") return r.owner == a.id;
    if (p.op == "send") return r.queue.size() < r.declaration.capacity;
    return p.op == "receive" && !r.queue.empty();
  }
  bool all_parked() const {
    return std::all_of(actors.begin(), actors.end(), [](const auto& a) { return a.parked || a.finished; });
  }
  bool all_finished() const {
    return std::all_of(actors.begin(), actors.end(), [](const auto& a) { return a.finished; });
  }
  std::vector<std::string> enabled_ids() const {
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < actors.size(); ++i) if (enabled(i)) ids.push_back(actors[i].id);
    return ids;
  }
};
} // namespace detail

namespace {
bool identifier(const std::string& id) {
  return !id.empty() && id.size() <= 128 && std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}
bool digest(const std::string& d) {
  return d.size() == 64 && std::all_of(d.begin(), d.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool inert_json(const json& value, unsigned depth = 0) {
  if (depth > 32 || value.is_binary() || value.is_discarded()) return false;
  if (value.is_number_float() && !std::isfinite(value.get<double>())) return false;
  if (value.is_structured()) for (const auto& child : value) if (!inert_json(child, depth+1)) return false;
  return true;
}
std::string fresh_id() {
  static std::atomic<std::uint64_t> sequence{0};
  std::random_device random;
  return std::to_string(random()) + ":" + std::to_string(random()) + ":" + std::to_string(++sequence);
}
} // namespace

Hook::Hook(std::shared_ptr<detail::State> state, std::size_t actor) : state_(std::move(state)), actor_(actor) {}
bool Hook::stop_requested() const { std::lock_guard lock(state_->mutex); return state_->cancelled; }
std::int64_t Hook::request(std::string op, std::string resource, std::int64_t value) {
  auto& s = *state_;
  std::unique_lock lock(s.mutex);
  auto& a = s.actors[actor_];
  if (a.thread != std::this_thread::get_id()) { s.fail("resource_misuse", "foreign hook caller"); throw std::runtime_error("foreign hook"); }
  if (s.cancelled) throw std::runtime_error("controlled execution cancelled");
  if (op != "start" && op != "checkpoint" && op != "$done") {
    const auto found = s.resources.find(resource);
    if (found == s.resources.end()) { s.fail("resource_misuse", "unknown resource"); throw std::runtime_error("unknown resource"); }
    auto& r = found->second;
    const bool mutex_op = op == "lock" || op == "unlock";
    if ((mutex_op && r.declaration.kind != Kind::mutex) || (!mutex_op && r.declaration.kind != Kind::queue) ||
        (op == "lock" && r.owner == a.id) || (op == "unlock" && r.owner != a.id)) {
      s.fail("resource_misuse", "kind, recursive acquire or wrong owner"); throw std::runtime_error("resource misuse");
    }
  }
  a.pending = {op, resource, value}; a.parked = true; a.grant = false; ++a.version; s.cv.notify_all();
  s.cv.wait(lock, [&] { return s.cancelled || a.grant; });
  if (s.cancelled) throw std::runtime_error("controlled execution cancelled");
  a.grant = false; a.parked = false;
  if (op == "lock") s.resources.at(resource).owner = a.id;
  if (op == "unlock") s.resources.at(resource).owner.clear();
  if (op == "send") s.resources.at(resource).queue.push_back(value);
  if (op == "receive") { value = s.resources.at(resource).queue.front(); s.resources.at(resource).queue.pop_front(); }
  s.event({{"kind", "result"}, {"actor", a.id}, {"operation", op}, {"resource", resource}, {"value", value}});
  return value;
}
void Hook::lock(std::string_view r) { (void)request("lock", std::string(r)); }
void Hook::unlock(std::string_view r) { (void)request("unlock", std::string(r)); }
void Hook::send(std::string_view r, std::int64_t v) { (void)request("send", std::string(r), v); }
std::int64_t Hook::receive(std::string_view r) { return request("receive", std::string(r)); }
void Hook::checkpoint(std::string_view name) {
  if (!identifier(std::string(name))) throw std::invalid_argument("invalid checkpoint");
  (void)request("checkpoint", std::string(name));
}
void Hook::unwind_release(std::string_view resource) noexcept {
  std::lock_guard lock(state_->mutex);
  auto found = state_->resources.find(std::string(resource));
  if (found != state_->resources.end() && found->second.owner == state_->actors[actor_].id) found->second.owner.clear();
}
MutexGuard::MutexGuard(Hook& hook, std::string resource) : hook_(hook), resource_(std::move(resource)) { hook_.lock(resource_); }
MutexGuard::~MutexGuard() noexcept {
  if (hook_.stop_requested() || std::uncaught_exceptions()) { hook_.unwind_release(resource_); return; }
  try { hook_.unlock(resource_); } catch (...) { hook_.unwind_release(resource_); }
}

struct Execution::Impl {
  std::shared_ptr<detail::State> state = std::make_shared<detail::State>();
  Adapter adapter;
  Program program;
  std::vector<std::thread> workers;
  bool acquired = false, teardown_attempted = false, observing = false;
  void observe() {
    {
      std::lock_guard lock(state->mutex);
      if (state->execution_expired("execution deadline before observer") || state->cancelled) return;
    }
    json value;
    bool threw = false;
    observing = true;
    try { value = program.observe(); }
    catch (...) { threw = true; }
    observing = false;
    std::lock_guard lock(state->mutex);
    if (threw) state->fail("observation_failed", "observer threw");
    if (state->execution_expired("execution deadline during observer") || state->cancelled) return;
    if (!value.is_object() || !inert_json(value)) {
      state->fail("evidence_exhausted", "observation shape or bound"); return;
    }
    json row = {{"afterDecisions", state->report.decisions}, {"state", std::move(value)}};
    if (!state->evidence_fits(state->observations, row)) {
      state->fail("evidence_exhausted", "observation shape or bound"); return;
    }
    if (state->execution_expired("execution deadline retaining observation")) return;
    state->observations.push_back(std::move(row));
    if (state->execution_expired("execution deadline after observation retention")) {
      state->observations.erase(state->observations.end()-1); return;
    }
    if (state->report.finished) state->execution_ended = true;
  }
};
namespace {
bool valid_policy(const Policy& policy) {
  return policy.max_actors > 0 && policy.max_actors <= 16 && policy.max_resources > 0 && policy.max_resources <= 32 &&
    policy.max_decisions > 0 && policy.max_decisions <= 4096 && policy.max_evidence_bytes >= 256 && policy.max_evidence_bytes <= 32*1024*1024 &&
    policy.execution_timeout.count() > 0 && policy.execution_timeout <= std::chrono::hours(1) &&
    policy.cleanup_timeout.count() > 0 && policy.cleanup_timeout <= std::chrono::hours(1) &&
    policy.exploration_timeout.count() > 0 && policy.exploration_timeout <= std::chrono::hours(1);
}
} // namespace
Execution::Execution(Adapter adapter, Policy policy, Clock::time_point ceiling) : impl_(std::make_unique<Impl>()) {
  auto& i = *impl_; auto s = i.state;
  s->policy = policy; s->controller = std::this_thread::get_id(); s->execution_id = fresh_id();
  i.adapter = std::move(adapter);
  auto refuse = [&](const char* detail) { s->fail("invalid_profile", detail); s->report.cleanup = "not_started"; };
  if (!valid_policy(policy) ||
      !digest(i.adapter.identity.model_semantic_digest) || !digest(i.adapter.identity.mapping_sha256) || !digest(i.adapter.identity.implementation_sha256) ||
      !i.adapter.factory || i.adapter.actors.empty() || i.adapter.actors.size() > policy.max_actors || i.adapter.resources.size() > policy.max_resources) {
    refuse("invalid bounds, identity or factory"); return;
  }
  s->deadline = std::min(Clock::now() + policy.execution_timeout, ceiling);
  std::sort(i.adapter.actors.begin(), i.adapter.actors.end());
  std::set<std::string> unique;
  for (const auto& actor : i.adapter.actors) {
    if (!identifier(actor) || !unique.insert(actor).second) { refuse("invalid or duplicate actor"); return; }
    detail::Actor a; a.id = actor; s->actors.push_back(std::move(a));
  }
  for (const auto& resource : i.adapter.resources) {
    if (!identifier(resource.id) || s->resources.contains(resource.id) ||
        (resource.kind != Kind::mutex && resource.kind != Kind::queue) ||
        (resource.kind == Kind::mutex && resource.capacity != 0) || (resource.kind == Kind::queue && (resource.capacity == 0 || resource.capacity > 64))) {
      refuse("invalid or duplicate resource"); return;
    }
    s->resources.emplace(resource.id, detail::Owned{resource, {}, {}});
  }
  if (s->execution_expired("execution deadline before factory")) return;
  try { i.program = i.adapter.factory(); i.acquired = true; }
  catch (...) { s->fail("application_failed", "factory threw"); }
  if (s->execution_expired("execution deadline during factory") || s->cancelled) return;
  std::sort(i.program.workers.begin(), i.program.workers.end(), [](const auto& a, const auto& b) { return a.actor < b.actor; });
  if (!i.program.observe || !i.program.teardown || i.program.workers.size() != s->actors.size()) { s->fail("application_failed", "invalid program"); return; }
  for (std::size_t index = 0; index < i.program.workers.size(); ++index) {
    if (i.program.workers[index].actor != s->actors[index].id || !i.program.workers[index].run) { s->fail("application_failed", "worker set differs"); return; }
  }
  try {
    for (std::size_t index = 0; index < s->actors.size(); ++index) {
      auto run = i.program.workers[index].run;
      i.workers.emplace_back([s, index, run = std::move(run)] {
        Hook hook(s, index);
        { std::lock_guard lock(s->mutex); s->actors[index].thread = std::this_thread::get_id(); }
        try { (void)hook.request("start", ""); run(hook); (void)hook.request("$done", ""); }
        catch (...) { std::lock_guard lock(s->mutex); if (!s->cancelled) s->fail("application_failed", "worker threw"); }
        std::lock_guard lock(s->mutex);
        auto& a = s->actors[index];
        for (auto& [_, resource] : s->resources) if (resource.owner == a.id) {
          if (!s->cancelled) s->fail("resource_misuse", "actor completed holding mutex");
          resource.owner.clear();
        }
        a.finished = true; a.parked = false; ++a.version; s->cv.notify_all();
      });
    }
  } catch (...) { std::lock_guard lock(s->mutex); s->fail("application_failed", "thread creation failed"); }
  {
    std::unique_lock lock(s->mutex);
    if (!s->cv.wait_until(lock, s->deadline, [&] { return s->cancelled || s->all_parked(); })) s->fail("timed_out", "initial barrier");
    s->execution_expired("execution deadline at initial barrier");
  }
  if (!s->cancelled) i.observe();
}
std::unique_ptr<Execution> start(Adapter adapter, Policy policy) { return std::unique_ptr<Execution>(new Execution(std::move(adapter), policy)); }
Execution::~Execution() {
  { std::lock_guard lock(impl_->state->mutex); impl_->state->cancelled = true; impl_->state->cv.notify_all(); }
  for (auto& worker : impl_->workers) if (worker.joinable()) worker.join();
  if (impl_->acquired && !impl_->teardown_attempted && impl_->program.teardown) {
    impl_->teardown_attempted = true; impl_->observing = true;
    try { impl_->program.teardown(); } catch (...) {}
  }
}
std::vector<std::string> Execution::enabled() {
  std::lock_guard lock(impl_->state->mutex); auto& s = *impl_->state;
  if (s.controller != std::this_thread::get_id() || impl_->observing) throw std::logic_error("controller ownership");
  if (s.execution_expired("execution deadline before enabled query") || s.cancelled || s.report.outcome != "completed") return {};
  auto ids = s.enabled_ids();
  if (ids.empty() && !s.all_finished() && s.all_parked()) s.fail("controlled_deadlock", "all unfinished actors wait on declared resources");
  return ids;
}
Report Execution::advance(std::string_view actor) {
  auto& i = *impl_; auto& s = *i.state;
  std::unique_lock lock(s.mutex);
  if (s.controller != std::this_thread::get_id() || impl_->observing) throw std::logic_error("controller ownership");
  if (s.execution_expired("execution deadline before decision") || s.cancelled) return s.report;
  const auto found = std::find_if(s.actors.begin(), s.actors.end(), [&](const auto& a) { return a.id == actor; });
  const auto index = static_cast<std::size_t>(found - s.actors.begin());
  if (found == s.actors.end() || !s.enabled(index)) { s.fail("disabled_choice", "actor is not enabled"); return s.report; }
  if (s.report.decisions >= s.policy.max_decisions) { s.fail("decision_exhausted", "decision bound"); return s.report; }
  const auto version = found->version;
  if (!s.event({{"kind", "grant"}, {"actor", found->id}, {"operation", found->pending.op},
               {"resource", found->pending.resource}, {"enabled", s.enabled_ids()}})) return s.report;
  ++s.report.decisions; found->grant = true; s.cv.notify_all();
  if (!s.cv.wait_until(lock, s.deadline, [&] { return s.cancelled || found->version != version; })) s.fail("timed_out", "actor did not reach resource boundary");
  s.execution_expired("execution deadline after actor boundary");
  if (s.cancelled) return s.report;
  if (found->finished && i.workers[index].joinable()) {
    // The worker's final notification precedes TLS destruction. Completion
    // observations must wait for actual exit while other actors remain parked.
    if (s.execution_expired("execution deadline before completion join")) return s.report;
    lock.unlock(); i.workers[index].join(); lock.lock();
    s.report.finished = s.all_finished();
    if (s.execution_expired("execution deadline during completion join") || s.cancelled) return s.report;
  }
  s.report.finished = s.all_finished();
  lock.unlock(); i.observe(); return report();
}
Report Execution::report() const { std::lock_guard lock(impl_->state->mutex); return impl_->state->report; }
Report Execution::cleanup(std::chrono::milliseconds timeout) {
  auto& i = *impl_; auto s = i.state;
  if (s->controller != std::this_thread::get_id() || i.observing || timeout.count() <= 0 || timeout > std::chrono::hours(1)) throw std::logic_error("cleanup ownership or bound");
  const auto deadline = Clock::now() + timeout;
  std::unique_lock lock(s->mutex);
  if (s->report.cleanup == "confirmed" || s->report.cleanup == "teardown_failed") return s->report;
  if (s->report.outcome == "completed" && !s->report.finished) {
    s->execution_expired("execution deadline before cleanup");
    if (s->report.outcome == "completed") s->fail("cancelled", "controller cancelled incomplete prefix");
  }
  s->cancelled = true; s->cv.notify_all();
  auto expired = [&] {
    if (Clock::now() < deadline) return false;
    s->cleanup_expired(); s->report.cleanup = "incomplete"; return true;
  };
  // Thread construction failure leaves unstarted actors; wait only for started workers.
  auto ended = [&] { for (std::size_t index = 0; index < i.workers.size(); ++index) if (!s->actors[index].finished) return false; return true; };
  if (!s->cv.wait_until(lock, deadline, ended)) {
    s->cleanup_expired(); s->report.cleanup = "incomplete"; return s->report;
  }
  for (auto& worker : i.workers) if (worker.joinable()) {
    if (expired()) return s->report;
    lock.unlock(); worker.join(); lock.lock();
    // join includes TLS destructors and cannot be interrupted safely.
    if (Clock::now() >= deadline) s->cleanup_expired();
  }
  bool teardown_failed = false;
  if (i.acquired && !i.teardown_attempted) {
    if (expired()) return s->report;
    i.teardown_attempted = true; i.observing = true;
    lock.unlock();
    try { if (!i.program.teardown) throw std::runtime_error("missing teardown"); i.program.teardown(); }
    catch (...) { teardown_failed = true; }
    lock.lock(); i.observing = false;
    if (Clock::now() >= deadline) s->cleanup_expired();
  }
  if (teardown_failed) s->report.cleanup = "teardown_failed";
  else s->report.cleanup = i.acquired ? "confirmed" : "not_started";
  return s->report;
}
json Execution::receipt() const {
  std::lock_guard lock(impl_->state->mutex); const auto& s = *impl_->state;
  json resources = json::array(), pending = json::array();
  for (const auto& [id, r] : s.resources) resources.push_back({{"id",id},{"kind",r.declaration.kind == Kind::mutex ? "mutex" : "queue"},
      {"capacity",r.declaration.capacity},{"owner",r.owner},{"contents",r.queue}});
  for (std::size_t index = 0; index < s.actors.size(); ++index) {
    const auto& a = s.actors[index];
    pending.push_back({{"actor",a.id},{"state",a.finished ? "finished" : a.parked ? (s.enabled(index) ? "enabled" : "blocked") : "running"},
                      {"operation",a.pending.op},{"resource",a.pending.resource}});
  }
  return {{"schema","mirrors.controlled-sync-replay/v1"},{"profile",profile},{"executionId",s.execution_id},
    {"identity",{{"modelSemanticDigest",impl_->adapter.identity.model_semantic_digest},{"mappingSha256",impl_->adapter.identity.mapping_sha256},{"implementationSha256",impl_->adapter.identity.implementation_sha256}}},
    {"outcome",s.report.outcome},{"detail",s.report.detail},{"finished",s.report.finished},{"cleanup",s.report.cleanup},
    {"passed",s.report.passed()},{"budgetExceeded",{{"execution",s.report.execution_budget_exceeded},{"cleanup",s.report.cleanup_budget_exceeded}}},{"decisions",s.report.decisions},{"events",s.events},{"observations",s.observations},
    {"resources",resources},{"actors",pending},{"failureGraph",s.failure_graph},{"coverage","recorded-controlled-resource-decisions-only"},{"modelComparison",false}};
}
Exploration explore(const std::function<Adapter()>& factory, std::size_t max_runs, Policy policy) {
  Exploration result; json rows = json::array();
  if (!factory || max_runs == 0 || max_runs > 4096 || !valid_policy(policy))
    throw std::invalid_argument("exploration bound");
  std::vector<std::vector<std::string>> prefixes{{}};
  bool complete = true, evidence_complete = true, total_budget_exceeded = false;
  std::size_t runs = 0, terminals = 0, deadlocks = 0;
  std::string stop_reason;
  const auto deadline = Clock::now() + policy.exploration_timeout;
  auto evidence = [&] {
    return json{{"schema","mirrors.controlled-sync-exploration/v1"},{"profile",profile},
      {"complete",complete && prefixes.empty()},{"runs",runs},{"terminals",terminals},{"deadlocks",deadlocks},
      {"coverage","finite-enabled-prefix-tree"},{"modelComparison",false},{"evidenceComplete",evidence_complete},
      {"stopReason",stop_reason},{"budgetExceeded",{{"total",total_budget_exceeded}}},{"rows",rows}};
  };
  // Every acquired attempt must fit either its receipt or an explicit bounded
  // omission row. Reserve room also for counters growing and the stop reason.
  constexpr std::size_t failure_reserve = 256;
  if (evidence().dump().size() + failure_reserve > policy.max_evidence_bytes)
    throw std::invalid_argument("exploration evidence bound cannot retain failure metadata");
  auto stop = [&](const char* reason) { complete = false; if (stop_reason.empty()) stop_reason = reason; };
  auto expired = [&] {
    if (Clock::now() < deadline) return false;
    total_budget_exceeded = true; stop("total_timeout"); return true;
  };
  auto retain_row = [&](json row, const char* cleanup) {
    rows.push_back(std::move(row));
    if (evidence().dump().size() + failure_reserve <= policy.max_evidence_bytes) return true;
    rows.erase(rows.end()-1);
    evidence_complete = false; stop("evidence_exhausted");
    rows.push_back({{"ordinal",runs-1},{"evidenceError","receipt_byte_bound_exceeded"},{"cleanup",cleanup}});
    return false;
  };
  while (!prefixes.empty()) {
    if (expired()) break;
    if (runs >= max_runs) { stop("run_bound"); break; }
    if (prefixes.size() > 4096) { stop("prefix_bound"); break; }
    auto prefix = std::move(prefixes.back()); prefixes.pop_back(); ++runs;
    Adapter adapter;
    try { adapter = factory(); }
    catch (...) {
      stop("factory_failed"); expired();
      retain_row({{"ordinal",runs-1},{"prefix",prefix},{"stage","adapter_factory"},
        {"error","adapter factory threw"},{"cleanup","not_started"}}, "not_started");
      break;
    }
    if (expired()) {
      retain_row({{"ordinal",runs-1},{"prefix",prefix},{"stage","adapter_factory"},
        {"error","total deadline during adapter factory"},{"cleanup","not_started"}}, "not_started");
      break;
    }
    auto execution = std::unique_ptr<Execution>(new Execution(std::move(adapter), policy, deadline));
    expired();
    for (const auto& actor : prefix) {
      if (!complete || expired() || execution->report().outcome != "completed") break;
      execution->advance(actor); expired();
    }
    auto enabled = execution->enabled(); auto primary = execution->report();
    expired();
    // Cleanup receives its own drain budget after the exploration deadline.
    auto cleanup = execution->cleanup(policy.cleanup_timeout);
    expired();
    const auto receipt = execution->receipt();
    if (cleanup.cleanup != "confirmed") stop("cleanup_incomplete");
    else if (cleanup.execution_budget_exceeded || cleanup.cleanup_budget_exceeded) stop("callback_timeout");
    else if (primary.outcome != "completed" && primary.outcome != "controlled_deadlock") stop("execution_failed");
    const bool retained = retain_row({{"ordinal",runs-1},{"prefix",prefix},{"enabled",enabled},{"receipt",receipt}}, cleanup.cleanup.c_str());
    // A late final callback or cleanup must not establish coverage even when
    // this attempt exhausted the frontier.
    expired();
    if (cleanup.cleanup == "incomplete") result.retained.push_back(std::move(execution));
    if (!complete || !retained) break;
    if (primary.outcome == "controlled_deadlock") ++deadlocks;
    else if (primary.finished) ++terminals;
    else if (prefix.size() >= policy.max_decisions) { stop("decision_bound"); break; }
    else for (auto choice = enabled.rbegin(); choice != enabled.rend(); ++choice) {
      auto next = prefix; next.push_back(*choice); prefixes.push_back(std::move(next));
    }
  }
  expired();
  result.evidence = evidence();
  if (expired()) result.evidence = evidence();
  return result;
}
} // namespace mirrorcpp::schedule_sync
