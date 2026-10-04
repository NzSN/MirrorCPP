#ifndef MIRRORCPP_SCHEDULE_EXPLORATION_HPP
#define MIRRORCPP_SCHEDULE_EXPLORATION_HPP
#include <mirrorcpp/schedule_binding.hpp>
#include <functional>

namespace mirrorcpp::schedule {
inline constexpr std::string_view exploration_profile = "mirrorcpp.finite-checkpoint-exploration/v1";
struct ActorChain {
  ActorDeclaration actor;
  std::vector<std::string> checkpoints; // Exactly one final $done.
};
struct FiniteSpace {
  Identity identity;
  std::vector<ActorChain> actors;
  std::vector<nlohmann::json> inputs;
  std::size_t max_preemptions = 64;
  bool require_model_comparison = false;
  std::vector<std::string> state_variables;
  std::vector<std::string> instrumentation_variables;
};
struct ExplorationLimits {
  std::size_t max_enumerated_schedules = 4096;
  std::size_t max_runs = 4096;
  std::chrono::milliseconds time_budget{30'000};
  // Bounds full retained run records; canonical coverage has a separate 8 MiB cap.
  std::size_t max_evidence_bytes = 16 * 1'048'576;
};
struct ExplorationSample {
  nlohmann::json execution;
  nlohmann::json comparison = nullptr;
};
using ExplorationRunner = std::function<ExplorationSample(const Schedule&, std::stop_token)>;

// Uses the real scheduler and a fresh application factory for each schedule.
ExplorationRunner local_exploration_runner(Adapter adapter, Policy policy = {});
// A one-trace DPM-2 result can be used by a reviewed comparison runner.
ExplorationSample comparison_sample(const ReplayResult& result);

// Canonical type-tagged JSON bytes. Sets are extensional; map keys are distinct
// homogeneous string/integer values. Does not alter protocol Value equality.
std::string canonical_state(const State& state);

// Enumerates all eligible merges within declared bounds; no POR. Every returned
// execution must bind the exact schedule. Unknown cleanup stops the campaign.
// Runner callbacks remain trusted prompt-returning code, not forcibly preempted.
nlohmann::json explore_finite(const FiniteSpace& space, const ExplorationRunner& runner,
                             const ExplorationLimits& limits = {}, std::stop_token stop = {});
}  // namespace mirrorcpp::schedule
#endif
