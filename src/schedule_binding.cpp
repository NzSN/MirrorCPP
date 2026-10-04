#include <mirrorcpp/schedule_binding.hpp>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace mirrorcpp::schedule {
using nlohmann::json;
namespace {
constexpr std::size_t max_executions = 64;
constexpr std::size_t max_receipt_bytes = 16 * 1'048'576;
Error cleanup_error(std::string text) {
  return Error(ErrorKind::model_interface, std::move(text), "schedule_cleanup_failed");
}
void schedule_error(std::string code, const Report& report) {
  throw ModelInterfaceBindingError(std::move(code), std::string(name(report.outcome)) + ": " + report.detail);
}
class ObservedTransport final : public Transport {
 public:
  explicit ObservedTransport(Transport& transport) : transport_(transport) {}
  Result<void> send_line(std::string_view line) override { return transport_.send_line(line); }
  Result<std::string> recv_line() override {
    auto line = transport_.recv_line();
    if (line) {
      auto message = decode_mirror_message(*line);
      if (message) {
        if (std::holds_alternative<AllStepsDone>(*message)) terminal = "all_steps_done";
        else if (std::holds_alternative<StepMismatch>(*message)) terminal = "step_mismatch";
        if (!terminal.empty()) raw_terminal = *line;
      }
    }
    return line;
  }
  Result<long> close() override { return transport_.close(); }
  bool async_capable() const noexcept override { return transport_.async_capable(); }
  std::string terminal, raw_terminal;
 private:
  Transport& transport_;
};
json client_evidence(const Result<void>& result) {
  if (result) return {{"status", "succeeded"}};
  const auto& error = result.error();
  json value = {{"status", "failed"}, {"kind", error_kind_name(error.kind)}, {"message", error.message}};
  if (error.code) value["code"] = *error.code;
  if (error.expected) value["expected"] = encode_state(*error.expected);
  if (error.actual) value["actual"] = encode_state(*error.actual);
  value["orderedHints"] = json::array();
  if (error.hints) for (const auto& hint : *error.hints) value["orderedHints"].push_back(render_diff_hint(hint));
  return value;
}
}  // namespace

BindingSession::BindingSession(Schedule schedule, Adapter adapter, Policy policy, std::stop_token stop)
    : schedule_(std::move(schedule)), adapter_(std::move(adapter)), policy_(policy), stop_(stop) {}
void BindingSession::require_live() const {
  if (disposed_ || !execution_)
    throw ModelInterfaceBindingError("schedule_lifecycle_invalid", "binding session is not live");
}
void BindingSession::retain() {
  if (!execution_ || retained_) return;
  auto value = execution_->receipt();
  const auto bytes = value.dump().size();
  if (receipt_bytes_ + bytes > max_receipt_bytes) {
    receipt_complete_ = false;
    executions_.push_back({{"passed", false}, {"evidenceError", "receipt_byte_bound_exceeded"}});
  } else {
    receipt_bytes_ += bytes;
    executions_.push_back(std::move(value));
  }
  retained_ = true;
}
void BindingSession::initialize() {
  if (disposed_ || initializations_ >= max_executions)
    throw ModelInterfaceBindingError("schedule_lifecycle_invalid", "binding initialization limit or disposed session");
  if (execution_) {
    const auto prior = execution_->finish();
    retain();
    if (!prior.passed() || !receipt_complete_)
      throw ModelInterfaceBindingError("schedule_previous_incomplete", "previous execution did not complete and clean up");
    execution_.reset();
  }
  ++initializations_;
  retained_ = false;
  execution_ = start_schedule(schedule_, adapter_, policy_, stop_);
  const auto report = execution_->report();
  if (report.outcome != Outcome::completed) schedule_error("schedule_admission_failed", report);
}
void BindingSession::advance(const Step& step) {
  require_live();
  const auto report = execution_->advance(step);
  if (report.outcome != Outcome::completed) schedule_error("schedule_interval_failed", report);
}
json BindingSession::observation() const {
  require_live();
  const auto report = execution_->report();
  if (report.outcome != Outcome::completed || report.observations.empty())
    schedule_error("schedule_observation_unavailable", report);
  return report.observations.back().at("state");
}
Result<void> BindingSession::dispose() {
  if (disposed_) return disposal_;
  disposed_ = true;
  ++disposals_;
  try {
    if (execution_) {
      const auto before = execution_->report();
      const auto after = before.schedule_completed ? execution_->finish() : execution_->cleanup(policy_.cleanup_timeout);
      retain();
      if (after.cleanup != Cleanup::confirmed)
        disposal_ = std::unexpected(cleanup_error(std::string(name(after.cleanup))));
      else if (!receipt_complete_)
        disposal_ = std::unexpected(cleanup_error("schedule receipt limit exceeded"));
    }
  } catch (const std::exception& error) {
    disposal_ = std::unexpected(cleanup_error(error.what()));
  } catch (...) { disposal_ = std::unexpected(cleanup_error("nonstandard scheduled disposal failure")); }
  return disposal_;
}
json BindingSession::receipt() const {
  auto records = executions_;
  if (execution_ && !retained_) {
    auto current = execution_->receipt();
    if (receipt_bytes_ + current.dump().size() <= max_receipt_bytes) records.push_back(std::move(current));
    else records.push_back({{"passed", false}, {"evidenceError", "receipt_byte_bound_exceeded"}});
  }
  return {{"schema", "mirrors.scheduled-binding/v1"}, {"initializations", initializations_},
          {"disposals", disposals_}, {"disposed", disposed_}, {"receiptComplete", receipt_complete_},
          {"executions", records}};
}
bool BindingSession::fully_completed() const {
  return disposed_ && initializations_ != 0 && receipt_complete_ && disposal_.has_value() &&
      !executions_.empty() && std::ranges::all_of(executions_, [](const auto& item) {
        return item.contains("passed") && item.at("passed") == true;
      });
}
ReplayResult replay_with_traces(Transport& transport, const ApalacheConfig& config,
    const std::vector<std::string>& traces, const CompiledAdapterSelection& selection,
    BindingSession& session) {
  const auto before = session.receipt();
  if (before.at("initializations") != 0 || before.at("disposals") != 0 || before.at("disposed") != false) {
    auto error = Error(ErrorKind::model_interface, "scheduled replay requires a fresh binding session", "schedule_session_reused");
    Result<void> rejected = std::unexpected(error);
    (void)transport.close();
    return {rejected, {{"schema", "mirrors.scheduled-comparison/v1"}, {"passed", false},
      {"comparison", "incomplete"}, {"peerTerminal", ""}, {"peerTerminalRaw", ""},
      {"client", client_evidence(rejected)}, {"binding", before}}, false};
  }
  ObservedTransport observed(transport);
  auto result = run_client_with_traces_negotiated(observed, config, traces, selection);
  std::string comparison = "incomplete";
  if (observed.terminal == "step_mismatch" && !result && result.error().is_step_mismatch()) comparison = "step_mismatch";
  if (observed.terminal == "all_steps_done" &&
      (result || (result.error().kind == ErrorKind::model_interface && result.error().code == "adapter_dispose_failed")))
    comparison = "matched";
  const bool passed = result.has_value() && comparison == "matched" && session.fully_completed();
  json evidence = {{"schema", "mirrors.scheduled-comparison/v1"}, {"passed", passed},
      {"comparison", comparison}, {"peerTerminal", observed.terminal},
      {"peerTerminalRaw", observed.raw_terminal}, {"client", client_evidence(result)}, {"binding", session.receipt()}};
  return {std::move(result), std::move(evidence), passed};
}
}  // namespace mirrorcpp::schedule
