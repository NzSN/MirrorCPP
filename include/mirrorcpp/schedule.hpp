#ifndef MIRRORCPP_SCHEDULE_HPP
#define MIRRORCPP_SCHEDULE_HPP

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace mirrorcpp::schedule {

inline constexpr std::string_view profile = "mirrorcpp.cooperative-checkpoints/v1";
inline constexpr std::string_view schedule_schema = "mirrors.checkpoint-schedule/v1";
inline constexpr std::string_view receipt_schema = "mirrors.checkpoint-replay/v1";
inline constexpr std::string_view completion = "$done";

struct Identity {
  std::string model_semantic_digest;
  std::string mapping_sha256;
  std::string implementation_sha256;
  bool operator==(const Identity&) const = default;
};

struct Step {
  std::string actor;
  std::string checkpoint;
  bool operator==(const Step&) const = default;
};

struct Schedule {
  std::string scheduling_profile{profile};
  Identity identity;
  nlohmann::json inputs = nlohmann::json::object();
  std::vector<Step> steps;
};

// Strict versioned JSON: unknown/duplicate keys and excessive nesting/size reject.
// Throws std::invalid_argument; no application factory is invoked by parsing.
Schedule parse_schedule(std::string_view text);
std::string encode_schedule(const Schedule& schedule);

struct ActorDeclaration {
  std::string actor;
  std::string operation;
};

struct Policy {
  std::size_t max_actors = 16;
  std::size_t max_steps = 4096;
  std::chrono::milliseconds execution_timeout{2000};
  std::chrono::milliseconds cleanup_timeout{1000};
};

enum class Outcome {
  completed, invalid_schedule, incompatible_identity, cancelled, timed_out,
  unexpected_checkpoint, uncontrolled_actor, application_failed,
  observation_failed, resource_failed
};

enum class Cleanup { not_started, confirmed, incomplete, teardown_failed };
std::string_view name(Outcome outcome);
std::string_view name(Cleanup cleanup);

struct Report {
  Outcome outcome = Outcome::completed;
  std::string detail;
  bool schedule_completed = false;
  Cleanup cleanup = Cleanup::not_started;
  std::vector<std::string> remaining_actors;
  std::uint64_t generation = 0;
  std::string execution_id;
  nlohmann::json events = nlohmann::json::array();
  nlohmann::json observations = nlohmann::json::array();
  nlohmann::json cleanup_attempts = nlohmann::json::array();

  bool passed() const noexcept {
    return outcome == Outcome::completed && schedule_completed && cleanup == Cleanup::confirmed;
  }
};

namespace detail { struct Shared; }

class Checkpoint {
 public:
  Checkpoint(const Checkpoint&) = delete;
  Checkpoint& operator=(const Checkpoint&) = delete;
  // Parks until the next permit. Cancellation unwinds the actor with an exception.
  // Do not call from another thread, a signal/VEH handler, or a suspended-target region.
  void arrive(std::string_view checkpoint);
  bool stop_requested() const noexcept;

 private:
  Checkpoint(std::shared_ptr<detail::Shared> shared, std::size_t actor);
  std::shared_ptr<detail::Shared> shared_;
  std::size_t actor_;
  friend class Execution;
};

struct Worker {
  std::string actor;
  std::function<void(Checkpoint&)> execute;
};

struct Program {
  std::vector<Worker> workers;
  // Called only while every declared actor is parked or finished. Must not wait
  // for actors, call hooks, or reenter the scheduler. Return actual SUT state.
  std::function<nlohmann::json()> observe;
  // Called once after all actors are joined, including after primary failure.
  std::function<void()> teardown;
};

struct Adapter {
  Identity identity;
  std::vector<ActorDeclaration> actors;
  std::vector<std::string> checkpoints;
  // Called after static admission, before starting workers. Capture owning state;
  // factory/observer/teardown and thread destructors must return promptly.
  std::function<Program(const nlohmann::json& inputs)> factory;
};

class Execution {
 public:
  Execution(const Execution&) = delete;
  Execution& operator=(const Execution&) = delete;
  Execution(Execution&&) = delete;
  Execution& operator=(Execution&&) = delete;
  // Never detaches. Cancels and joins remaining threads; may block for an
  // uncooperative application. Retain the handle and use process isolation when
  // hard termination is required. See docs/deterministic-scheduling.md.
  ~Execution();

  // Incremental control for serialized generated callbacks. Each requested step
  // must equal the next fully admitted interval. No new schedule choices here.
  Report advance(const Step& requested);
  // Require all admitted intervals; idempotent once closed. Early comparison
  // failure may instead call cleanup(), retaining scheduleCompleted=false.
  Report finish();
  Report report() const;
  nlohmann::json receipt() const;
  // Retry bounded coordinator cleanup after an incomplete attempt. Preserve the
  // primary failure. Call on the owning controller thread, not from callbacks.
  Report cleanup(std::chrono::milliseconds timeout);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Execution();
  void start(const Schedule&, Adapter, const Policy&, std::stop_token);
  friend std::unique_ptr<Execution> start_schedule(
      const Schedule&, Adapter, const Policy&, std::stop_token);
  friend std::unique_ptr<Execution> run_schedule(
      const Schedule&, Adapter, const Policy&, std::stop_token);
};

// Admit the whole plan and park workers before application code. Observe the
// real initial state. The total execution deadline includes time between advances.
std::unique_ptr<Execution> start_schedule(
    const Schedule& schedule, Adapter adapter, const Policy& policy = {},
    std::stop_token stop = {});

// Drives a fixed schedule; no exploration, generated binding or oracle is implied.
// The returned handle owns incomplete executions as well as successful ones.
std::unique_ptr<Execution> run_schedule(
    const Schedule& schedule, Adapter adapter, const Policy& policy = {},
    std::stop_token stop = {});

}  // namespace mirrorcpp::schedule
#endif
