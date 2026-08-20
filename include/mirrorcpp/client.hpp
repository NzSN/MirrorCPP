// mirrorcpp/client.hpp — run_client* flows, one-shot flows, ExploreSession, preset_client
// (design §5.6).
//
// The client ties the protocol codec (§5.2) to a Transport (§5.3): it sends
// Register* messages, drives the stepping loop against a caller-supplied
// state computer, and surfaces mirror errors as Error values (no exceptions
// cross the API boundary — §4.3).
//
// Behavioral contract of the stepping loop (identical to MirrorECMA mainLoop,
// §5.6 items 1–5):
//   1. await spec_validated; invalid → spec_invalid error; register_error /
//      protocol_error propagate; anything else → protocol error naming the tag.
//   2. initial_state → computer(action, state, {}); next_step →
//      computer(action, parameters, prev) — remembering that in
//      register_explore, parameters is the full expected state.
//   3. reply report_state with the returned state (client code is responsible
//      for the include/omit rules of §3.5; the library documents but cannot
//      enforce them).
//   4. step_mismatch → step_mismatch error with expected/actual/hints and the
//      last action name in the message; all_steps_done → success.
//   5. EVERY terminal path closes the transport (and thus reaps the stdio
//      child — §4.4).
#ifndef MIRRORCPP_CLIENT_HPP
#define MIRRORCPP_CLIENT_HPP

#include <mirrorcpp/error.hpp>
#include <mirrorcpp/protocol.hpp>
#include <mirrorcpp/transport.hpp>
#include <mirrorcpp/value.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mirrorcpp {

// ---------------------------------------------------------------------------
// StateComputer (design §5.6)
// ---------------------------------------------------------------------------
// Computes the state to report for one step. Called with:
//   - action:        the step's action name (initial_state/next_step action).
//   - params_or_state: for initial_state, the FULL oracle state; for next_step,
//     the mirror-extracted parameters (in register_explore, the full expected
//     state — §3.2). Callers should adopt the oracle init state and derive
//     subsequent states from it (see §3.5 report_state rules: include every
//     state var, omit paramVars, include action_taken when the spec defines it).
//   - prev_state:    the state the computer returned for the previous step
//                    (empty {} on the first call).
using StateComputer = std::function<State(std::string_view action,
                                          const State& params_or_state,
                                          const State& prev_state)>;

// ---------------------------------------------------------------------------
// Stepping flows (block until all_steps_done / mismatch / error) — §5.6
// ---------------------------------------------------------------------------

// register_traces flow: replay pre-generated ITF trace files (mirror-local
// paths) against the client. Returns success on all_steps_done.
Result<void> run_client_with_traces(Transport& transport, const ApalacheConfig& config,
                                    const std::vector<std::string>& itf_trace_paths,
                                    StateComputer compute);

// register flow: the mirror generates traces with Apalache, then replays them.
Result<void> run_client(Transport& transport, const ApalacheConfig& config,
                        const TraceGenerationConfig& trace_config, StateComputer compute,
                        std::optional<ApalacheSpec> inline_spec = std::nullopt);

// register_explore flow: mirror-driven symbolic exploration; next_step carries
// the full expected state (§3.2).
Result<void> run_client_explore(Transport& transport, const ApalacheSpec& spec,
                                std::vector<std::string> invariants,
                                std::vector<std::string> exports, long long max_steps,
                                StateComputer compute);

// ---------------------------------------------------------------------------
// One-shot flows — §5.6
// ---------------------------------------------------------------------------

// register_trace_gen: generate ITF trace files, return their paths + optional
// inline contents. Session ends after one reply.
struct GenTracesResult {
  std::vector<std::string> itf_trace_paths;
  std::vector<nlohmann::json> itf_traces;  // inline ITF JSON contents (may be empty)
};
Result<GenTracesResult> run_client_gen_traces(Transport& transport,
                                              const ApalacheConfig& config,
                                              const TraceGenerationConfig& trace_config,
                                              std::optional<std::string> dest_path = std::nullopt,
                                              std::optional<ApalacheSpec> inline_spec = std::nullopt);

// register_validate: typecheck + bounded model check; exactly one reply.
struct ValidateVerdict {
  bool valid = false;
  std::string detail;  // apalache output when invalid
};
Result<ValidateVerdict> run_client_validate(Transport& transport, const ApalacheConfig& config,
                                            long long bound,
                                            std::optional<ApalacheSpec> inline_spec = std::nullopt);

// ---------------------------------------------------------------------------
// Client-driven symbolic sessions (ExploreSession) — §5.6
// ---------------------------------------------------------------------------

// register_explore_session flow: client commands and mirror replies strictly
// alternate, in any order, until explore_done → explore_session_done. A
// protocol_error rejects the offending command but the session STAYS OPEN —
// surfaced as a recoverable per-call error, not a fatal one (§3.3).
class ExploreSession {
public:
  struct Ready {
    long long init_transitions = 0;
    long long next_transitions = 0;
    long long state_invariants = 0;
  };

  // register_explore_session → await explorer_ready; returns a session ready
  // for commands.
  static Result<ExploreSession> open(std::unique_ptr<Transport> transport,
                                     const ApalacheSpec& spec,
                                     std::vector<std::string> invariants,
                                     std::vector<std::string> exports);

  ExploreSession(ExploreSession&&) noexcept;
  ExploreSession& operator=(ExploreSession&&) noexcept;
  ~ExploreSession();  // best-effort done() if still open

  const Ready& ready() const noexcept { return ready_; }

  Result<TransitionStatus> assume_transition(long long transition_id);
  Result<long long>        next_step();
  Result<State>            query_state();
  Result<InvariantStatus>  check_invariant(long long invariant_id);
  Result<TransitionStatus> assume_state(const State& state);
  Result<long long>        rollback(long long snapshot_id);
  Result<void>             done();  // explore_done → explore_session_done, then close

private:
  ExploreSession(std::unique_ptr<Transport> transport, Ready ready);
  Result<MirrorMessage> command(const ClientMessage& msg);  // send + recv

  std::unique_ptr<Transport> transport_;
  Ready ready_;
  bool done_ = false;
};

// ---------------------------------------------------------------------------
// Test helper (design §5.6)
// ---------------------------------------------------------------------------

// Returns a StateComputer that reports the given states in sequence, then
// throws std::runtime_error("preset_client exhausted") on further calls.
StateComputer preset_client(std::vector<State> states);

}  // namespace mirrorcpp

#endif  // MIRRORCPP_CLIENT_HPP
