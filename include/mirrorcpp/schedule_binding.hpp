#ifndef MIRRORCPP_SCHEDULE_BINDING_HPP
#define MIRRORCPP_SCHEDULE_BINDING_HPP
#include <mirrorcpp/client.hpp>
#include <mirrorcpp/schedule.hpp>

namespace mirrorcpp::schedule {

// A connection-local owner. Constructing it does not construct application state.
// Generated ports call initialize/advance/observation; LocalBinding owns disposal.
class BindingSession {
 public:
  BindingSession(Schedule schedule, Adapter adapter, Policy policy = {}, std::stop_token stop = {});
  BindingSession(const BindingSession&) = delete;
  BindingSession& operator=(const BindingSession&) = delete;
  void initialize();
  void advance(const Step& step);
  nlohmann::json observation() const;
  Result<void> dispose();
  nlohmann::json receipt() const;
  bool fully_completed() const;

 private:
  void retain();
  void require_live() const;
  Schedule schedule_;
  Adapter adapter_;
  Policy policy_;
  std::stop_token stop_;
  std::unique_ptr<Execution> execution_;
  nlohmann::json executions_ = nlohmann::json::array();
  std::size_t receipt_bytes_ = 0;
  std::size_t initializations_ = 0;
  std::size_t disposals_ = 0;
  bool disposed_ = false;
  bool retained_ = false;
  bool receipt_complete_ = true;
  Result<void> disposal_;
};

struct ReplayResult {
  Result<void> client_result;
  nlohmann::json evidence;
  bool passed = false;
};

// Preserve actual peer terminal verdict separately from client/disposal status.
// The selection's factory must bind the supplied session to its generated port.
ReplayResult replay_with_traces(
    Transport& transport, const ApalacheConfig& config,
    const std::vector<std::string>& traces, const CompiledAdapterSelection& selection,
    BindingSession& session);

}  // namespace mirrorcpp::schedule
#endif
