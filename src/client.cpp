// mirrorcpp/client.cpp — run_client* flows, one-shot flows, ExploreSession, preset_client
// (design §5.6).
//
// t5/t6 implement the shared stepping loop, run_client_with_traces, run_client,
// run_client_gen_traces, run_client_validate, and preset_client. t10 implements
// run_client_explore and ExploreSession. All §5.6 flows are now implemented.
#include <mirrorcpp/client.hpp>

#include <stdexcept>
#include <utility>

namespace mirrorcpp {

using std::unexpected;

namespace {

// Build a protocol error naming the offending tag ("expected X, got Y",
// design §5.2 / §5.6 item 1).
Error unexpected_tag_error(const MirrorMessage& msg, std::string_view expected) {
  return Error(ErrorKind::protocol,
               "expected " + std::string(expected) + ", got " +
               std::string(mirror_message_name(msg)));
}

// Serialize + send one client message over the transport.
Result<void> send_message(Transport& transport, const ClientMessage& msg) {
  return transport.send_line(encode_client_message(msg));
}

// The shared stepping loop (design §5.6 items 1–5, identical to MirrorECMA
// mainLoop). The transport is closed on EVERY terminal path (§5.6 item 5) —
// including when the state computer throws (RAII hygiene, §4.4).
//
// Caller must have sent the Register* message already and fed it to `guard`
// (guard.sent); this function handles everything from spec_validated onward,
// feeding every received/sent message to the guard (C4/C5 enforcement).
Result<void> run_stepping_loop(Transport& transport, const StateComputer& compute,
                               PhaseGuard& guard) {
  // ---- item 1: await spec_validated ----
  auto first_line = transport.recv_line();
  if (!first_line) {
    (void)transport.close();
    return unexpected(first_line.error());
  }
  auto first = decode_mirror_message(*first_line);
  if (!first) {
    (void)transport.close();
    return unexpected(first.error());
  }
  MirrorMessage msg = std::move(*first);
  if (auto g = guard.received(msg); !g) {
    (void)transport.close();
    return unexpected(g.error());
  }

  if (auto* pv = std::get_if<SpecValidated>(&msg)) {
    if (!pv->is_valid()) {
      const std::string text = pv->invalid_text() ? *pv->invalid_text() : std::string();
      (void)transport.close();
      return unexpected(Error(ErrorKind::spec_invalid,
                              "spec was not validated: " + text));
    }
  } else if (auto* re = std::get_if<RegisterError>(&msg)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::registration, re->error));
  } else if (auto* pe = std::get_if<ProtocolError>(&msg)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::protocol, pe->error));
  } else {
    (void)transport.close();
    return unexpected(unexpected_tag_error(msg, "spec_validated"));
  }

  // ---- items 2–4: stepping loop ----
  State prev;                       // computer's last returned state
  std::string last_action;          // for the step_mismatch message (§5.6 item 4)

  for (;;) {
    auto line = transport.recv_line();
    if (!line) {
      (void)transport.close();
      return unexpected(line.error());
    }
    auto decoded = decode_mirror_message(*line);
    if (!decoded) {
      (void)transport.close();
      return unexpected(decoded.error());
    }
    MirrorMessage m = std::move(*decoded);
    if (auto g = guard.received(m); !g) {
      (void)transport.close();
      return unexpected(g.error());
    }

    if (auto* init = std::get_if<InitialState>(&m)) {
      last_action = init->action;
      State next;
      try {
        next = compute(init->action, init->state, State{});   // item 2
      } catch (...) {
        (void)transport.close();
        throw;
      }
      prev = next;
      ReportState report{std::move(next)};
      if (auto g = guard.sent(report); !g) {                  // item 3
        (void)transport.close();
        return unexpected(g.error());
      }
      auto sr = send_message(transport, report);
      if (!sr) {
        (void)transport.close();
        return unexpected(sr.error());
      }
    } else if (auto* ns = std::get_if<NextStep>(&m)) {
      last_action = ns->action;
      State next;
      try {
        next = compute(ns->action, ns->parameters, prev);     // item 2
      } catch (...) {
        (void)transport.close();
        throw;
      }
      prev = next;
      ReportState report{std::move(next)};
      if (auto g = guard.sent(report); !g) {                  // item 3
        (void)transport.close();
        return unexpected(g.error());
      }
      auto sr = send_message(transport, report);
      if (!sr) {
        (void)transport.close();
        return unexpected(sr.error());
      }
    } else if (std::get_if<StepOk>(&m)) {
      // ack of the previous report; keep going
    } else if (std::get_if<AllStepsDone>(&m)) {
      (void)transport.close();                                 // item 4: success
      return {};
    } else if (auto* sm = std::get_if<StepMismatch>(&m)) {
      auto hints = std::make_shared<const std::vector<DiffHint>>(sm->hints);
      std::string text = "conformance failure";
      if (!last_action.empty()) text += " at action '" + last_action + "'";
      if (!sm->hints.empty()) text += ": " + render_diff_hints(sm->hints);
      (void)transport.close();                                 // item 4: failure
      return unexpected(Error(ErrorKind::step_mismatch, std::move(text),
                              sm->expected, sm->actual, std::move(hints)));
    } else if (auto* re = std::get_if<RegisterError>(&m)) {
      (void)transport.close();
      return unexpected(Error(ErrorKind::registration, re->error));
    } else if (auto* pe = std::get_if<ProtocolError>(&m)) {
      (void)transport.close();
      return unexpected(Error(ErrorKind::protocol, pe->error));
    } else {
      (void)transport.close();
      return unexpected(unexpected_tag_error(
          m, "next_step | step_ok | all_steps_done | step_mismatch"));
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// register_traces flow (t5)
// ---------------------------------------------------------------------------
Result<void> run_client_with_traces(Transport& transport, const ApalacheConfig& config,
                                    const std::vector<std::string>& itf_trace_paths,
                                    StateComputer compute) {
  if (!compute)
    return unexpected(Error(ErrorKind::protocol,
                            "run_client_with_traces: null StateComputer"));
  PhaseGuard guard;
  RegisterTraces reg{config, itf_trace_paths};
  if (auto g = guard.sent(reg); !g) return unexpected(g.error());
  auto sr = send_message(transport, reg);
  if (!sr) return unexpected(sr.error());
  return run_stepping_loop(transport, compute, guard);
}

// ---------------------------------------------------------------------------
// register flow (t6): the mirror generates traces with Apalache, then replays
// them — identical stepping loop to register_traces.
// ---------------------------------------------------------------------------
Result<void> run_client(Transport& transport, const ApalacheConfig& config,
                        const TraceGenerationConfig& trace_config, StateComputer compute,
                        std::optional<ApalacheSpec> inline_spec) {
  if (!compute)
    return unexpected(Error(ErrorKind::protocol,
                            "run_client: null StateComputer"));
  PhaseGuard guard;
  Register reg{config, trace_config, std::move(inline_spec)};
  if (auto g = guard.sent(reg); !g) return unexpected(g.error());
  auto sr = send_message(transport, reg);
  if (!sr) return unexpected(sr.error());
  return run_stepping_loop(transport, compute, guard);
}

// ---------------------------------------------------------------------------
// register_trace_gen flow (t6): generate ITF trace files; exactly one reply
// (gen_traces_done), then the session ends (§3.3).
// ---------------------------------------------------------------------------
Result<GenTracesResult> run_client_gen_traces(Transport& transport,
                                              const ApalacheConfig& config,
                                              const TraceGenerationConfig& trace_config,
                                              std::optional<std::string> dest_path,
                                              std::optional<ApalacheSpec> inline_spec) {
  PhaseGuard guard;
  RegisterTraceGen reg{config, trace_config, std::move(dest_path),
                       std::move(inline_spec)};
  if (auto g = guard.sent(reg); !g) return unexpected(g.error());
  auto sr = send_message(transport, reg);
  if (!sr) return unexpected(sr.error());

  auto line = transport.recv_line();
  if (!line) {
    (void)transport.close();
    return unexpected(line.error());
  }
  auto decoded = decode_mirror_message(*line);
  if (!decoded) {
    (void)transport.close();
    return unexpected(decoded.error());
  }
  MirrorMessage m = std::move(*decoded);
  if (auto g = guard.received(m); !g) {
    (void)transport.close();
    return unexpected(g.error());
  }
  if (auto* g = std::get_if<GenTracesDone>(&m)) {
    (void)transport.close();
    return GenTracesResult{std::move(g->itf_trace_paths), std::move(g->itf_traces)};
  }
  if (auto* re = std::get_if<RegisterError>(&m)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::registration, re->error));
  }
  if (auto* pe = std::get_if<ProtocolError>(&m)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::protocol, pe->error));
  }
  (void)transport.close();
  return unexpected(unexpected_tag_error(m, "gen_traces_done"));
}

// ---------------------------------------------------------------------------
// register_validate flow (t6): typecheck + bounded model check; exactly one
// reply (spec_validated or register_error), then the session ends (§3.3).
// Bound is the top-level `bound` (NOT apalacheConfig.lengthBound); the mirror
// caps it at 100 and replies register_error out of range — pass through, do not
// pre-validate client-side.
// ---------------------------------------------------------------------------
Result<ValidateVerdict> run_client_validate(Transport& transport, const ApalacheConfig& config,
                                            long long bound,
                                            std::optional<ApalacheSpec> inline_spec) {
  PhaseGuard guard;
  RegisterValidate reg{config, bound, std::move(inline_spec)};
  if (auto g = guard.sent(reg); !g) return unexpected(g.error());
  auto sr = send_message(transport, reg);
  if (!sr) return unexpected(sr.error());

  auto line = transport.recv_line();
  if (!line) {
    (void)transport.close();
    return unexpected(line.error());
  }
  auto decoded = decode_mirror_message(*line);
  if (!decoded) {
    (void)transport.close();
    return unexpected(decoded.error());
  }
  MirrorMessage m = std::move(*decoded);
  if (auto g = guard.received(m); !g) {
    (void)transport.close();
    return unexpected(g.error());
  }

  if (auto* pv = std::get_if<SpecValidated>(&m)) {
    (void)transport.close();
    if (pv->is_valid()) {
      ValidateVerdict v;
      v.valid = true;
      return v;
    }
    ValidateVerdict v;
    v.valid = false;
    v.detail = pv->invalid_text() ? *pv->invalid_text() : std::string();
    return v;
  }
  if (auto* re = std::get_if<RegisterError>(&m)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::registration, re->error));
  }
  if (auto* pe = std::get_if<ProtocolError>(&m)) {
    (void)transport.close();
    return unexpected(Error(ErrorKind::protocol, pe->error));
  }
  (void)transport.close();
  return unexpected(unexpected_tag_error(m, "spec_validated"));
}

// ---------------------------------------------------------------------------
// register_explore flow (t10): mirror-driven symbolic exploration; the same
// stepping loop as register, but next_step.parameters carries the FULL expected
// state (§3.2) — the loop already passes it to the computer uniformly.
// ---------------------------------------------------------------------------
Result<void> run_client_explore(Transport& transport, const ApalacheSpec& spec,
                                std::vector<std::string> invariants,
                                std::vector<std::string> exports, long long max_steps,
                                StateComputer compute) {
  if (!compute)
    return unexpected(Error(ErrorKind::protocol,
                            "run_client_explore: null StateComputer"));
  RegisterExplore msg;
  msg.spec = spec;
  msg.invariants = std::move(invariants);
  msg.exports = std::move(exports);
  // maxSteps is ALWAYS sent: the Lean mirror decodes it with reqNat (absent →
  // "field maxSteps: natural expected"); the Haskell reference accepts an
  // explicit value too, so unconditional send is compatible with both.
  msg.max_steps = max_steps;
  PhaseGuard guard;
  if (auto g = guard.sent(msg); !g) return unexpected(g.error());
  auto sr = send_message(transport, msg);
  if (!sr) return unexpected(sr.error());
  return run_stepping_loop(transport, compute, guard);
}

// ---------------------------------------------------------------------------
// preset_client test helper (design §5.6)
// ---------------------------------------------------------------------------
StateComputer preset_client(std::vector<State> states) {
  auto queue = std::make_shared<std::vector<State>>(std::move(states));
  return [queue](std::string_view, const State&, const State&) -> State {
    if (queue->empty()) throw std::runtime_error("preset_client exhausted");
    State s = std::move(queue->front());
    queue->erase(queue->begin());
    return s;
  };
}

// ---------------------------------------------------------------------------
// ExploreSession (t10): register_explore_session flow — client commands and
// mirror replies STRICTLY alternate until explore_done -> explore_session_done.
// Any protocol_error or malformed/impossible exchange poisons the physical
// connection (client guide §9). Every successful command sends exactly one
// client message and consumes exactly one mirror reply.
// ---------------------------------------------------------------------------
namespace {

// Await one mirror reply and classify it.
Result<MirrorMessage> recv_explore_reply(Transport& transport) {
  auto line = transport.recv_line();
  if (!line) return unexpected(line.error());
  auto decoded = decode_mirror_message(*line);
  if (!decoded) return unexpected(decoded.error());
  MirrorMessage m = std::move(*decoded);
  if (auto* pe = std::get_if<ProtocolError>(&m))
    return unexpected(Error(ErrorKind::protocol, pe->error));
  return m;
}

}  // namespace

ExploreSession::ExploreSession(std::unique_ptr<Transport> transport, Ready ready,
                               PhaseGuard guard)
    : transport_(std::move(transport)), ready_(ready), guard_(guard) {}
ExploreSession::ExploreSession(ExploreSession&&) noexcept = default;
ExploreSession& ExploreSession::operator=(ExploreSession&&) noexcept = default;
ExploreSession::~ExploreSession() {
  if (transport_ && !done_) {
    // Best-effort done() so the mirror leaves exploring; ignore failures.
    // If done() fails the transport_ unique_ptr destroys the transport, closing
    // the child's stdin so it exits (§4.4).
    (void)done();
  }
}

Result<ExploreSession> ExploreSession::open(std::unique_ptr<Transport> transport,
                                            const ApalacheSpec& spec,
                                            std::vector<std::string> invariants,
                                            std::vector<std::string> exports) {
  if (!transport)
    return unexpected(Error(ErrorKind::io,
                            "ExploreSession::open: null transport"));
  PhaseGuard guard;
  RegisterExploreSession reg{spec, std::move(invariants), std::move(exports)};
  if (auto g = guard.sent(reg); !g) {
    (void)transport->close();
    return unexpected(g.error());
  }
  auto sr = send_message(*transport, reg);
  if (!sr) {
    (void)transport->close();
    return unexpected(sr.error());
  }
  auto line = transport->recv_line();
  if (!line) {
    (void)transport->close();
    return unexpected(line.error());
  }
  auto decoded = decode_mirror_message(*line);
  if (!decoded) {
    (void)transport->close();
    return unexpected(decoded.error());
  }
  MirrorMessage m = std::move(*decoded);
  if (auto g = guard.received(m); !g) {
    (void)transport->close();
    return unexpected(g.error());
  }

  if (auto* er = std::get_if<ExplorerReady>(&m)) {
    Ready ready{er->init_transitions, er->next_transitions, er->state_invariants};
    return ExploreSession(std::move(transport), ready, guard);
  }
  if (auto* re = std::get_if<RegisterError>(&m)) {
    (void)transport->close();
    return unexpected(Error(ErrorKind::registration, re->error));
  }
  if (auto* pe = std::get_if<ProtocolError>(&m)) {
    (void)transport->close();
    return unexpected(Error(ErrorKind::protocol, pe->error));
  }
  (void)transport->close();
  return unexpected(unexpected_tag_error(m, "explorer_ready"));
}

Result<MirrorMessage> ExploreSession::command(const ClientMessage& msg) {
  if (!transport_ || done_)
    return unexpected(Error(ErrorKind::io,
                            "ExploreSession: session is closed"));
  auto abort = [&](Error error) -> Result<MirrorMessage> {
    poison();
    return unexpected(std::move(error));
  };
  if (auto g = guard_.sent(msg); !g) return abort(g.error());
  auto sr = send_message(*transport_, msg);
  if (!sr) return abort(sr.error());
  auto r = recv_explore_reply(*transport_);
  if (!r) return abort(r.error());
  if (auto g = guard_.received(*r); !g) return abort(g.error());
  return r;
}

void ExploreSession::poison() noexcept {
  done_ = true;
  if (transport_) (void)transport_->close();
}

Result<TransitionStatus> ExploreSession::assume_transition(long long transition_id) {
  auto r = command(ExploreAssumeTransition{transition_id});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreTransitionStatus>(&*r)) return s->status;
  auto error = unexpected_tag_error(*r, "explore_transition_status");
  poison();
  return unexpected(std::move(error));
}

Result<long long> ExploreSession::next_step() {
  auto r = command(ExploreNextStep{});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreStepDone>(&*r)) return s->step_no;
  auto error = unexpected_tag_error(*r, "explore_step_done");
  poison();
  return unexpected(std::move(error));
}

Result<State> ExploreSession::query_state() {
  auto r = command(ExploreQueryState{});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreState>(&*r)) return s->state;
  auto error = unexpected_tag_error(*r, "explore_state");
  poison();
  return unexpected(std::move(error));
}

Result<InvariantStatus> ExploreSession::check_invariant(long long invariant_id) {
  auto r = command(ExploreCheckInvariant{invariant_id});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreInvariantStatus>(&*r)) return s->status;
  auto error = unexpected_tag_error(*r, "explore_invariant_status");
  poison();
  return unexpected(std::move(error));
}

Result<TransitionStatus> ExploreSession::assume_state(const State& state) {
  auto r = command(ExploreAssumeState{state});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreAssumeStatus>(&*r)) return s->status;
  auto error = unexpected_tag_error(*r, "explore_assume_status");
  poison();
  return unexpected(std::move(error));
}

Result<long long> ExploreSession::rollback(long long snapshot_id) {
  auto r = command(ExploreRollback{snapshot_id});
  if (!r) return unexpected(r.error());
  if (auto* s = std::get_if<ExploreRollbackDone>(&*r)) return s->snapshot_id;
  auto error = unexpected_tag_error(*r, "explore_rollback_done");
  poison();
  return unexpected(std::move(error));
}

Result<void> ExploreSession::done() {
  if (!transport_ || done_) return {};
  auto r = command(ExploreDone{});
  if (!r) return unexpected(r.error());
  if (!std::get_if<ExploreSessionDone>(&*r)) {
    auto error = unexpected_tag_error(*r, "explore_session_done");
    poison();
    return unexpected(std::move(error));
  }
  auto cr = transport_->close();
  done_ = true;
  transport_.reset();
  if (!cr) return unexpected(cr.error());
  return {};
}

// ---------------------------------------------------------------------------
// Asynchronous job interface (guide §6, C17–C22). Server-mode transports only.
// Successful job control and register_error leave the borrowed transport open
// for subsequent polls/submissions. Transport failures, protocol_error, and
// malformed/impossible replies close the connection as poisoned. A fresh guard
// per call validates the immediate exchange;
// callers MUST NOT interleave job control with a live sync/explore flow on the
// same connection.
// ---------------------------------------------------------------------------
namespace {

// Send one job message, await its synchronous reply, run both past a guard.
Result<MirrorMessage> job_exchange(Transport& transport, const ClientMessage& msg) {
  auto abort = [&](Error error) -> Result<MirrorMessage> {
    (void)transport.close();
    return unexpected(std::move(error));
  };
  PhaseGuard guard;
  if (auto g = guard.sent(msg); !g) return abort(g.error());
  auto sr = send_message(transport, msg);
  if (!sr) return abort(sr.error());
  auto line = transport.recv_line();
  if (!line) return abort(line.error());
  auto decoded = decode_mirror_message(*line);
  if (!decoded) return abort(decoded.error());
  if (auto g = guard.received(*decoded); !g) return abort(g.error());
  if (auto* re = std::get_if<RegisterError>(&*decoded))
    return unexpected(Error(ErrorKind::registration, re->error));   // e.g. queue full (C22)
  if (auto* pe = std::get_if<ProtocolError>(&*decoded))
    return abort(Error(ErrorKind::protocol, pe->error));
  return decoded;
}

Result<JobAccepted> submit_async(Transport& transport, const ClientMessage& msg) {
  if (!transport.async_capable())
    return unexpected(Error(ErrorKind::protocol,
                            "async jobs require a server-mode (TCP/TLS) transport"));
  auto r = job_exchange(transport, msg);
  if (!r) return unexpected(r.error());
  if (auto* ja = std::get_if<JobAccepted>(&*r)) return *ja;
  auto error = unexpected_tag_error(*r, "job_accepted");
  (void)transport.close();
  return unexpected(std::move(error));
}

// Classify a job-control reply: JobResult is terminal, JobStatus is not (C18).
Result<AwaitResult> classify_job_reply(Transport& transport, Result<MirrorMessage> r,
                                       std::string_view expected) {
  if (!r) return unexpected(r.error());
  if (auto* jr = std::get_if<JobResult>(&*r)) return AwaitResult{*jr};
  if (auto* js = std::get_if<JobStatus>(&*r)) return AwaitResult{*js};
  auto error = unexpected_tag_error(*r, expected);
  (void)transport.close();
  return unexpected(std::move(error));
}

}  // namespace

Result<JobAccepted> submit_validate_async(Transport& transport, const ApalacheConfig& config,
                                          long long bound,
                                          std::optional<ApalacheSpec> inline_spec) {
  return submit_async(transport,
                      RegisterValidateAsync{config, bound, std::move(inline_spec)});
}

Result<JobAccepted> submit_trace_gen_async(Transport& transport,
                                           const ApalacheConfig& config,
                                           const TraceGenerationConfig& trace_config,
                                           std::optional<std::string> dest_path,
                                           std::optional<ApalacheSpec> inline_spec) {
  return submit_async(transport,
                      RegisterTraceGenAsync{config, trace_config, std::move(dest_path),
                                            std::move(inline_spec)});
}

Result<JobStatus> query_job(Transport& transport, std::string_view job_id) {
  auto r = job_exchange(transport, QueryJob{std::string(job_id)});
  if (!r) return unexpected(r.error());
  if (auto* js = std::get_if<JobStatus>(&*r)) return *js;
  auto error = unexpected_tag_error(*r, "job_status");
  (void)transport.close();
  return unexpected(std::move(error));
}

Result<AwaitResult> await_job(Transport& transport, std::string_view job_id,
                              std::optional<long long> timeout_secs) {
  return classify_job_reply(transport,
      job_exchange(transport, AwaitJob{std::string(job_id), timeout_secs}),
      "job_result | job_status");
}

Result<AwaitResult> cancel_job(Transport& transport, std::string_view job_id) {
  return classify_job_reply(transport,
      job_exchange(transport, CancelJob{std::string(job_id)}),
      "job_result | job_status");
}

}  // namespace mirrorcpp
