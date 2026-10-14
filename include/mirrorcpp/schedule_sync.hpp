#ifndef MIRRORCPP_SCHEDULE_SYNC_HPP
#define MIRRORCPP_SCHEDULE_SYNC_HPP
#include <mirrorcpp/schedule.hpp>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mirrorcpp::schedule_sync {
inline constexpr std::string_view profile = "mirrorcpp.controlled-sync-experimental/v1";
enum class Kind { mutex, queue };
struct Resource { std::string id; Kind kind = Kind::mutex; std::size_t capacity = 0; };
struct Policy {
  std::size_t max_actors = 16, max_resources = 32, max_decisions = 4096;
  std::size_t max_evidence_bytes = 8 * 1024 * 1024;
  std::chrono::milliseconds execution_timeout{2000}, cleanup_timeout{1000}, exploration_timeout{30000};
};
namespace detail { struct State; }
class Hook {
 public:
  Hook(const Hook&) = delete;
  Hook& operator=(const Hook&) = delete;
  void lock(std::string_view resource);
  void unlock(std::string_view resource);
  void send(std::string_view resource, std::int64_t value);
  std::int64_t receive(std::string_view resource);
  void checkpoint(std::string_view name);
  bool stop_requested() const;
 private:
  Hook(std::shared_ptr<detail::State>, std::size_t);
  std::int64_t request(std::string, std::string, std::int64_t = 0);
  void unwind_release(std::string_view) noexcept;
  std::shared_ptr<detail::State> state_;
  std::size_t actor_;
  friend class Execution;
  friend class MutexGuard;
};
class MutexGuard {
 public:
  MutexGuard(Hook& hook, std::string resource);
  ~MutexGuard() noexcept;
  MutexGuard(const MutexGuard&) = delete;
  MutexGuard& operator=(const MutexGuard&) = delete;
 private:
  Hook& hook_;
  std::string resource_;
};
struct Worker { std::string actor; std::function<void(Hook&)> run; };
struct Program {
  std::vector<Worker> workers;
  std::function<nlohmann::json()> observe;
  std::function<void()> teardown;
};
struct Adapter {
  schedule::Identity identity;
  std::vector<std::string> actors;
  std::vector<Resource> resources;
  std::function<Program()> factory;
};
struct Report {
  std::string outcome = "completed", detail, cleanup = "not_started";
  bool finished = false;
  std::size_t decisions = 0;
  bool execution_budget_exceeded = false, cleanup_budget_exceeded = false;
  bool passed() const {
    return outcome == "completed" && finished && cleanup == "confirmed" &&
      !execution_budget_exceeded && !cleanup_budget_exceeded;
  }
};
struct Exploration;
class Execution {
 public:
  Execution(const Execution&) = delete;
  Execution& operator=(const Execution&) = delete;
  ~Execution(); // Cancels and joins; never detaches. May wait for uncooperative callbacks.
  std::vector<std::string> enabled();
  Report advance(std::string_view actor); // A disabled choice refuses; no implicit repair.
  Report report() const;
  nlohmann::json receipt() const;
  Report cleanup(std::chrono::milliseconds timeout);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Execution(Adapter, Policy, std::chrono::steady_clock::time_point ceiling =
    std::chrono::steady_clock::time_point::max());
  friend Exploration explore(const std::function<Adapter()>&, std::size_t, Policy);
  friend std::unique_ptr<Execution> start(Adapter, Policy);
};
std::unique_ptr<Execution> start(Adapter, Policy = {});
struct Exploration {
  nlohmann::json evidence;
  std::vector<std::unique_ptr<Execution>> retained; // Retain incomplete cleanup for explicit retry.
};
// Fresh prefix replay over dynamically observed enabled sets; bounded, no POR.
Exploration explore(const std::function<Adapter()>& factory, std::size_t max_runs = 256,
                    Policy = {});
} // namespace mirrorcpp::schedule_sync
#endif
