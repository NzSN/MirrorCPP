#include <mirrorcpp/schedule.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <iomanip>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mirrorcpp::schedule {
using nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace {
constexpr std::size_t max_artifact_bytes = 1'048'576;
constexpr std::size_t max_observation_bytes = 65'535;
std::atomic<std::uint64_t> next_generation{1};
thread_local bool in_scheduler_callback = false;
thread_local bool in_scheduler_observer = false;
struct ObserverScope {
  bool previous = in_scheduler_observer;
  ObserverScope() { in_scheduler_observer = true; }
  ~ObserverScope() { in_scheduler_observer = previous; }
};
struct CallbackScope {
  bool previous = in_scheduler_callback;
  CallbackScope() { in_scheduler_callback = true; }
  ~CallbackScope() { in_scheduler_callback = previous; }
};
struct StopActor : std::exception {
  const char* what() const noexcept override { return "checkpoint execution stopped"; }
};
bool identifier(std::string_view text) {
  return !text.empty() && text.size() <= 128 && std::ranges::all_of(text, [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/';
  });
}
bool digest(std::string_view text) {
  return text.size() == 64 && std::ranges::all_of(text, [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool identity_valid(const Identity& value) {
  return digest(value.model_semantic_digest) && digest(value.mapping_sha256) &&
         digest(value.implementation_sha256);
}
bool plain_json(const json& value, unsigned depth = 0) {
  if (depth > 28 || value.is_binary() || value.is_discarded()) return false;
  if (value.is_number_float() && !std::isfinite(value.get<double>())) return false;
  if (value.is_structured())
    for (const auto& child : value) if (!plain_json(child, depth + 1)) return false;
  return true;
}
json identity_json(const Identity& value) {
  return {{"modelSemanticDigest", value.model_semantic_digest},
          {"mappingSha256", value.mapping_sha256},
          {"implementationSha256", value.implementation_sha256}};
}
json schedule_json(const Schedule& value) {
  json steps = json::array();
  for (const auto& step : value.steps)
    steps.push_back({{"actor", step.actor}, {"checkpoint", step.checkpoint}});
  return {{"schema", schedule_schema}, {"profile", value.scheduling_profile},
          {"identity", identity_json(value.identity)}, {"inputs", value.inputs}, {"steps", steps}};
}
void exact(const json& value, std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.size() != keys.size())
    throw std::invalid_argument("schedule object has missing or unknown fields");
  for (const auto key : keys)
    if (!value.contains(key)) throw std::invalid_argument("missing schedule field");
}
bool valid_budget(std::chrono::milliseconds value) {
  return value.count() > 0 && value <= std::chrono::hours(24);
}
}  // namespace

Schedule parse_schedule(std::string_view text) {
  if (text.size() > max_artifact_bytes) throw std::invalid_argument("schedule exceeds byte bound");
  try {
    std::vector<std::set<std::string>> keys;
    unsigned depth = 0;
    const auto callback = [&](int, json::parse_event_t event, json& parsed) {
      switch (event) {
        case json::parse_event_t::object_start:
          keys.emplace_back();
          [[fallthrough]];
        case json::parse_event_t::array_start:
          if (++depth > 32) throw std::invalid_argument("schedule nesting exceeds bound");
          break;
        case json::parse_event_t::object_end:
          keys.pop_back();
          [[fallthrough]];
        case json::parse_event_t::array_end: --depth; break;
        case json::parse_event_t::key:
          if (!keys.back().insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("duplicate schedule JSON key");
          break;
        default: break;
      }
      return true;
    };
    const auto value = json::parse(text.begin(), text.end(), callback);
    exact(value, {"schema", "profile", "identity", "inputs", "steps"});
    if (value.at("schema") != schedule_schema) throw std::invalid_argument("unknown schedule schema");
    const auto& id = value.at("identity");
    exact(id, {"modelSemanticDigest", "mappingSha256", "implementationSha256"});
    Schedule result;
    result.scheduling_profile = value.at("profile").get<std::string>();
    result.identity = {id.at("modelSemanticDigest").get<std::string>(),
                       id.at("mappingSha256").get<std::string>(),
                       id.at("implementationSha256").get<std::string>()};
    result.inputs = value.at("inputs");
    if (!value.at("steps").is_array() || value.at("steps").size() > 65'536)
      throw std::invalid_argument("invalid schedule steps");
    for (const auto& step : value.at("steps")) {
      exact(step, {"actor", "checkpoint"});
      result.steps.push_back({step.at("actor").get<std::string>(), step.at("checkpoint").get<std::string>()});
    }
    if (result.scheduling_profile != profile || !identity_valid(result.identity))
      throw std::invalid_argument("invalid schedule profile or identity");
    for (const auto& step : result.steps)
      if (!identifier(step.actor) || (!identifier(step.checkpoint) && step.checkpoint != completion))
        throw std::invalid_argument("invalid schedule identifier");
    return result;
  } catch (const std::exception& error) {
    throw std::invalid_argument(std::string("schedule decode failed: ") + error.what());
  }
}
std::string encode_schedule(const Schedule& schedule) {
  if (!plain_json(schedule.inputs)) throw std::invalid_argument("inputs must be bounded plain JSON");
  const auto text = schedule_json(schedule).dump();
  (void)parse_schedule(text);
  return text;
}
std::string_view name(Outcome value) {
  switch (value) {
    case Outcome::completed: return "completed";
    case Outcome::invalid_schedule: return "invalid_schedule";
    case Outcome::incompatible_identity: return "incompatible_identity";
    case Outcome::cancelled: return "cancelled";
    case Outcome::timed_out: return "timed_out";
    case Outcome::unexpected_checkpoint: return "unexpected_checkpoint";
    case Outcome::uncontrolled_actor: return "uncontrolled_actor";
    case Outcome::application_failed: return "application_failed";
    case Outcome::observation_failed: return "observation_failed";
    case Outcome::resource_failed: return "resource_failed";
  }
  return "unknown";
}
std::string_view name(Cleanup value) {
  switch (value) {
    case Cleanup::not_started: return "not_started";
    case Cleanup::confirmed: return "confirmed";
    case Cleanup::incomplete: return "incomplete";
    case Cleanup::teardown_failed: return "teardown_failed";
  }
  return "unknown";
}

namespace detail {
enum class Phase { starting, parked, running, finished };
struct ActorState {
  ActorDeclaration declaration;
  Phase phase = Phase::starting;
  std::thread::id thread;
  std::string checkpoint = "$start";
  bool joined = false;
  std::optional<std::string> arrival;
};
struct Shared {
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> cancelled{false};
  bool stopping = false;
  bool failed = false;
  std::vector<ActorState> actors;
  std::set<std::string> checkpoints;
  Report report;
  void fail(Outcome outcome, std::string detail) {
    if (!failed) {
      failed = true;
      report.outcome = outcome;
      report.detail = detail.substr(0, 1024);
    }
    changed.notify_all();
  }
};
}  // namespace detail

Checkpoint::Checkpoint(std::shared_ptr<detail::Shared> shared, std::size_t actor)
    : shared_(std::move(shared)), actor_(actor) {}
bool Checkpoint::stop_requested() const noexcept { return shared_->cancelled.load(); }
void Checkpoint::arrive(std::string_view checkpoint) {
  if (in_scheduler_observer) throw std::logic_error("observer cannot enter a checkpoint");
  auto& state = *shared_;
  std::unique_lock lock(state.mutex);
  auto& actor = state.actors.at(actor_);
  if (actor.thread != std::this_thread::get_id() || actor.phase != detail::Phase::running) {
    state.fail(Outcome::uncontrolled_actor, "checkpoint called outside its owned actor");
    throw StopActor{};
  }
  if (state.stopping) throw StopActor{};
  // Bound even unexpected strings before retaining them as evidence.
  if (!identifier(checkpoint) || !state.checkpoints.contains(std::string(checkpoint))) {
    actor.checkpoint = std::string(checkpoint.substr(0, 128));
    actor.arrival = actor.checkpoint;
    state.fail(Outcome::unexpected_checkpoint, "undeclared checkpoint: " + actor.checkpoint);
    throw StopActor{};
  }
  actor.checkpoint = checkpoint;
  actor.arrival = actor.checkpoint;
  actor.phase = detail::Phase::parked;
  state.changed.notify_all();
  state.changed.wait(lock, [&] { return state.stopping || actor.phase == detail::Phase::running; });
  if (state.stopping) throw StopActor{};
}

struct Execution::Impl {
  std::shared_ptr<detail::Shared> shared = std::make_shared<detail::Shared>();
  Schedule schedule;
  Policy policy;
  Adapter adapter;
  Program program;
  std::size_t observation_bytes = 0;
  bool constructed = false;
  bool initialized = false;
  bool closed = false;
  std::size_t next_step = 0;
  Clock::time_point deadline;
  std::unique_ptr<std::stop_callback<std::function<void()>>> cancellation;
  bool teardown_attempted = false;
  bool teardown_failed = false;
  std::string teardown_detail;
  std::vector<std::thread> threads;
  std::thread::id owner = std::this_thread::get_id();

  bool admit(const Policy& policy) {
    auto& s = *shared;
    auto invalid = [&](std::string why) { s.fail(Outcome::invalid_schedule, std::move(why)); return false; };
    if (in_scheduler_callback) return invalid("scheduler callbacks cannot reenter scheduling");
    if (policy.max_actors == 0 || policy.max_actors > 64 || policy.max_steps == 0 ||
        policy.max_steps > 65'536 || !valid_budget(policy.execution_timeout) ||
        !valid_budget(policy.cleanup_timeout)) return invalid("invalid execution policy bounds");
    if (schedule.scheduling_profile != profile || !identity_valid(schedule.identity) ||
        !identity_valid(adapter.identity)) return invalid("invalid profile or identity syntax");
    if (schedule.identity != adapter.identity) {
      s.fail(Outcome::incompatible_identity, "schedule and adapter identities differ");
      return false;
    }
    if (!adapter.factory || adapter.actors.empty() || adapter.actors.size() > policy.max_actors ||
        adapter.checkpoints.size() > policy.max_steps || schedule.steps.size() > policy.max_steps)
      return invalid("missing factory or actor/step bounds exceeded");
    try { (void)encode_schedule(schedule); }
    catch (const std::exception& error) { return invalid(error.what()); }
    if (schedule.inputs.dump().size() > max_observation_bytes) return invalid("inputs exceed byte bound");
    std::set<std::string> actors, done;
    for (const auto& actor : adapter.actors) {
      if (!identifier(actor.actor) || !identifier(actor.operation) || !actors.insert(actor.actor).second)
        return invalid("invalid or duplicate logical actor");
    }
    for (const auto& checkpoint : adapter.checkpoints)
      if (!identifier(checkpoint) || !s.checkpoints.insert(checkpoint).second)
        return invalid("invalid or duplicate checkpoint declaration");
    for (const auto& step : schedule.steps) {
      if (!actors.contains(step.actor) || done.contains(step.actor))
        return invalid("unknown actor or step after completion");
      if (step.checkpoint == completion) done.insert(step.actor);
      else if (!s.checkpoints.contains(step.checkpoint)) return invalid("undeclared schedule checkpoint");
    }
    if (done != actors) return invalid("every actor requires one terminal completion step");
    for (const auto& actor : adapter.actors) s.actors.push_back({actor, detail::Phase::starting, {}, "$start", false, {}});
    return true;
  }

  bool wait(std::unique_lock<std::mutex>& lock, Clock::time_point deadline,
            const std::function<bool()>& predicate) {
    auto& s = *shared;
    s.changed.wait_until(lock, deadline, [&] { return s.failed || s.cancelled.load() || predicate(); });
    if (!s.failed && s.cancelled.load()) s.fail(Outcome::cancelled, "execution cancelled");
    if (!s.failed && Clock::now() >= deadline) s.fail(Outcome::timed_out, "execution deadline exceeded");
    return !s.failed;
  }

  void event(std::string_view kind, std::size_t step, const detail::ActorState& actor) {
    auto& events = shared->report.events;
    events.push_back({{"ordinal", events.size()}, {"kind", kind}, {"step", step},
                      {"actor", actor.declaration.actor}, {"operation", actor.declaration.operation},
                      {"checkpoint", actor.checkpoint}});
  }
  bool observe(std::size_t index) {
    auto& s = *shared;
    try {
      CallbackScope callback;
      ObserverScope observation;
      auto value = program.observe();
      if (!plain_json(value)) throw std::runtime_error("observation must be bounded plain JSON");
      const auto bytes = value.dump().size();
      if (bytes > max_observation_bytes || observation_bytes + bytes > 8 * max_artifact_bytes)
        throw std::runtime_error("observations exceed byte bound");
      observation_bytes += bytes;
      s.report.observations.push_back({{"afterSteps", index}, {"state", std::move(value)}});
    } catch (const std::exception& error) {
      s.fail(Outcome::observation_failed, error.what());
    } catch (...) { s.fail(Outcome::observation_failed, "nonstandard observer exception"); }
    return !s.failed;
  }
};

Execution::Execution() : impl_(std::make_unique<Impl>()) {}
void Execution::start(const Schedule& schedule, Adapter adapter, const Policy& policy, std::stop_token stop) {
  auto& p = *impl_;
  auto& s = *p.shared;
  p.schedule = schedule;
  p.policy = policy;
  p.adapter = std::move(adapter);
  s.report.generation = next_generation.fetch_add(1);
  std::random_device random;
  std::ostringstream nonce;
  nonce << std::hex << std::setfill('0');
  for (int i = 0; i < 4; ++i) nonce << std::setw(8) << static_cast<std::uint32_t>(random());
  s.report.execution_id = nonce.str();
  if (!p.admit(policy)) return;
  p.cancellation = std::make_unique<std::stop_callback<std::function<void()>>>(stop,
      [shared = p.shared] { shared->cancelled.store(true); shared->changed.notify_all(); });
  if (s.cancelled.load()) { s.fail(Outcome::cancelled, "cancelled before application construction"); return; }
  p.deadline = Clock::now() + policy.execution_timeout;
  const auto deadline = p.deadline;
  try {
    CallbackScope callback;
    p.program = p.adapter.factory(p.schedule.inputs);
    p.constructed = true;
  } catch (const std::exception& error) {
    s.fail(Outcome::application_failed, error.what()); return;
  } catch (...) { s.fail(Outcome::application_failed, "nonstandard factory exception"); return; }
  std::map<std::string, std::function<void(Checkpoint&)>> workers;
  for (auto& worker : p.program.workers) {
    if (!worker.execute || !workers.emplace(worker.actor, std::move(worker.execute)).second)
      s.fail(Outcome::application_failed, "missing worker function or duplicate worker identity");
  }
  if (workers.size() != s.actors.size() || !p.program.observe || !p.program.teardown)
    s.fail(Outcome::application_failed, "factory worker set, observer or teardown missing");
  for (const auto& actor : s.actors)
    if (!workers.contains(actor.declaration.actor)) s.fail(Outcome::application_failed, "factory omitted declared actor");
  if (s.cancelled.load()) s.fail(Outcome::cancelled, "cancelled during construction");
  if (Clock::now() >= deadline) s.fail(Outcome::timed_out, "construction exceeded execution deadline");
  if (s.failed) { (void)cleanup(policy.cleanup_timeout); return; }
  try {
    p.threads.reserve(s.actors.size());
    for (std::size_t i = 0; i < s.actors.size(); ++i) {
      auto function = std::move(workers.at(s.actors[i].declaration.actor));
      p.threads.emplace_back([shared = p.shared, i, function = std::move(function)] {
        auto& state = *shared;
        Checkpoint hook(shared, i);
        try {
          {
            std::unique_lock lock(state.mutex);
            auto& actor = state.actors[i];
            actor.thread = std::this_thread::get_id();
            actor.phase = detail::Phase::parked;
            state.changed.notify_all();
            state.changed.wait(lock, [&] { return state.stopping || actor.phase == detail::Phase::running; });
            if (state.stopping) throw StopActor{};
          }
          CallbackScope callback;
          function(hook);
        } catch (const StopActor&) {
          // Stopping is retained separately; a hook violation already set failure.
        } catch (const std::exception& error) {
          std::lock_guard lock(state.mutex);
          state.fail(Outcome::application_failed, error.what());
        } catch (...) {
          std::lock_guard lock(state.mutex);
          state.fail(Outcome::application_failed, "nonstandard actor exception");
        }
        std::lock_guard lock(state.mutex);
        state.actors[i].checkpoint = std::string(completion);
        if (!state.actors[i].arrival) state.actors[i].arrival = std::string(completion);
        state.actors[i].phase = detail::Phase::finished;
        state.changed.notify_all();
      });
    }
  } catch (const std::exception& error) {
    std::lock_guard lock(s.mutex);
    s.fail(Outcome::resource_failed, error.what());
  }
  {
    std::unique_lock lock(s.mutex);
    if (p.wait(lock, deadline, [&] {
          return std::ranges::all_of(s.actors, [](const auto& actor) { return actor.phase == detail::Phase::parked; });
        })) p.observe(0);
    if (!s.failed && s.cancelled.load()) s.fail(Outcome::cancelled, "execution cancelled");
    if (!s.failed && Clock::now() >= deadline) s.fail(Outcome::timed_out, "execution deadline exceeded");
    p.initialized = !s.failed;
  }
  if (!p.initialized) (void)cleanup(policy.cleanup_timeout);
}

Report Execution::advance(const Step& requested) {
  auto& p = *impl_;
  auto& s = *p.shared;
  if (std::this_thread::get_id() != p.owner || in_scheduler_callback)
    throw std::logic_error("advance requires the owning controller outside scheduler callbacks");
  {
    std::unique_lock lock(s.mutex);
    if (!p.initialized || p.closed || s.failed || s.report.schedule_completed)
      throw std::logic_error("execution cannot advance in its current lifecycle");
    if (s.cancelled.load()) s.fail(Outcome::cancelled, "execution cancelled");
    if (!s.failed && Clock::now() >= p.deadline) s.fail(Outcome::timed_out, "execution deadline exceeded");
    if (!s.failed && (p.next_step >= p.schedule.steps.size() || requested != p.schedule.steps[p.next_step]))
      s.fail(Outcome::invalid_schedule, "callback interval differs from the admitted schedule");
    if (!s.failed) {
      const auto index = p.next_step;
      auto found = std::ranges::find_if(s.actors, [&](const auto& actor) {
        return actor.declaration.actor == requested.actor;
      });
      auto& actor = *found;
      if (actor.phase != detail::Phase::parked) {
        s.fail(Outcome::unexpected_checkpoint, "selected actor is not parked");
      } else {
        p.event("permit", index, actor);
        actor.arrival.reset();
        actor.phase = detail::Phase::running;
        s.changed.notify_all();
        const bool arrived = p.wait(lock, p.deadline, [&] { return actor.phase != detail::Phase::running; });
        if (actor.arrival) {
          auto actual = actor;
          actual.checkpoint = *actor.arrival;
          p.event("arrival", index, actual);
        }
        if (arrived) {
          if (actor.checkpoint != requested.checkpoint) {
            s.fail(Outcome::unexpected_checkpoint, "expected " + requested.checkpoint + ", arrived at " + actor.checkpoint);
          } else {
            // Completion includes thread-local destructors. Observing before
            // joining could race application state written during thread exit.
            if (actor.phase == detail::Phase::finished) {
              const auto actor_index = static_cast<std::size_t>(found - s.actors.begin());
              lock.unlock();
              if (p.threads[actor_index].joinable()) p.threads[actor_index].join();
              lock.lock();
              actor.joined = true;
            }
            ++p.next_step;
            p.observe(p.next_step);
          }
        }
      }
    }
    if (!s.failed && s.cancelled.load()) s.fail(Outcome::cancelled, "execution cancelled");
    if (!s.failed && Clock::now() >= p.deadline) s.fail(Outcome::timed_out, "execution deadline exceeded");
    if (!s.failed && p.next_step == p.schedule.steps.size()) s.report.schedule_completed = true;
  }
  if (s.failed) return cleanup(p.policy.cleanup_timeout);
  return report();
}

Report Execution::finish() {
  auto& p = *impl_;
  auto& s = *p.shared;
  if (std::this_thread::get_id() != p.owner || in_scheduler_callback)
    throw std::logic_error("finish requires the owning controller outside scheduler callbacks");
  {
    std::lock_guard lock(s.mutex);
    if (p.closed || !p.constructed) return s.report;
    if (!s.failed && (!p.initialized || !s.report.schedule_completed))
      s.fail(Outcome::invalid_schedule, "finish requires the complete admitted schedule");
  }
  return cleanup(p.policy.cleanup_timeout);
}

Report Execution::report() const {
  std::lock_guard lock(impl_->shared->mutex);
  return impl_->shared->report;
}
Report Execution::cleanup(std::chrono::milliseconds timeout) {
  auto& p = *impl_;
  auto& s = *p.shared;
  if (std::this_thread::get_id() != p.owner || in_scheduler_callback)
    throw std::logic_error("cleanup requires the owning controller outside callbacks");
  if (!valid_budget(timeout)) throw std::invalid_argument("invalid cleanup budget");
  std::unique_lock lock(s.mutex);
  p.closed = true;
  if (!p.constructed) return s.report;
  s.stopping = true;
  s.cancelled.store(true);
  s.changed.notify_all();
  s.changed.wait_until(lock, Clock::now() + timeout, [&] {
    for (std::size_t i = 0; i < p.threads.size(); ++i)
      if (s.actors[i].phase != detail::Phase::finished) return false;
    return true;
  });
  for (std::size_t i = 0; i < p.threads.size(); ++i) {
    if (s.actors[i].phase == detail::Phase::finished && p.threads[i].joinable()) {
      lock.unlock();
      p.threads[i].join();
      lock.lock();
      s.actors[i].joined = true;
    }
  }
  s.report.remaining_actors.clear();
  for (std::size_t i = 0; i < p.threads.size(); ++i)
    if (p.threads[i].joinable()) s.report.remaining_actors.push_back(s.actors[i].declaration.actor);
  if (s.report.remaining_actors.empty() && !p.teardown_attempted) {
    p.teardown_attempted = true;
    lock.unlock();
    try {
      CallbackScope callback;
      if (!p.program.teardown) throw std::runtime_error("adapter omitted explicit teardown");
      p.program.teardown();
    } catch (const std::exception& error) { p.teardown_failed = true; p.teardown_detail = std::string(error.what()).substr(0, 1024); }
    catch (...) { p.teardown_failed = true; p.teardown_detail = "nonstandard teardown exception"; }
    lock.lock();
  }
  s.report.cleanup = !s.report.remaining_actors.empty() ? Cleanup::incomplete :
      p.teardown_failed ? Cleanup::teardown_failed : Cleanup::confirmed;
  s.report.cleanup_attempts.push_back({{"status", name(s.report.cleanup)},
      {"remainingActors", s.report.remaining_actors}, {"detail", p.teardown_detail}});
  return s.report;
}
Execution::~Execution() {
  auto& p = *impl_;
  auto& s = *p.shared;
  {
    std::lock_guard lock(s.mutex);
    s.stopping = true;
    s.cancelled.store(true);
    s.changed.notify_all();
  }
  for (auto& thread : p.threads) if (thread.joinable()) thread.join();
  if (p.constructed && !p.teardown_attempted) {
    try { CallbackScope callback; if (p.program.teardown) p.program.teardown(); }
    catch (...) { /* Destructors cannot report. Inspect explicit cleanup first. */ }
  }
}
nlohmann::json Execution::receipt() const {
  const auto r = report();
  json actors = json::array();
  for (const auto& actor : impl_->adapter.actors)
    actors.push_back({{"actor", actor.actor}, {"operation", actor.operation}});
  return {{"schema", receipt_schema}, {"schedule", schedule_json(impl_->schedule)},
          {"actors", actors}, {"checkpoints", impl_->adapter.checkpoints},
          {"generation", r.generation}, {"executionId", r.execution_id},
          {"policy", {{"maxActors", impl_->policy.max_actors}, {"maxSteps", impl_->policy.max_steps},
             {"executionTimeoutMs", impl_->policy.execution_timeout.count()},
             {"cleanupTimeoutMs", impl_->policy.cleanup_timeout.count()}}},
          {"outcome", name(r.outcome)}, {"detail", r.detail},
          {"scheduleCompleted", r.schedule_completed}, {"passed", r.passed()},
          {"events", r.events}, {"observations", r.observations},
          {"cleanup", name(r.cleanup)}, {"remainingActors", r.remaining_actors},
          {"cleanupAttempts", r.cleanup_attempts}, {"coverage", "recorded-schedule-only"}};
}
std::unique_ptr<Execution> start_schedule(const Schedule& schedule, Adapter adapter,
                                        const Policy& policy, std::stop_token stop) {
  auto run = std::unique_ptr<Execution>(new Execution());
  run->start(schedule, std::move(adapter), policy, stop);
  return run;
}
std::unique_ptr<Execution> run_schedule(const Schedule& schedule, Adapter adapter,
                                      const Policy& policy, std::stop_token stop) {
  auto run = start_schedule(schedule, std::move(adapter), policy, stop);
  if (!run->impl_->initialized) return run;
  for (const auto& step : schedule.steps) {
    if (run->impl_->closed) break;
    (void)run->advance(step);
  }
  (void)run->finish();
  return run;
}
}  // namespace mirrorcpp::schedule
