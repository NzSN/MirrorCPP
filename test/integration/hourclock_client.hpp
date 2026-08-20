// HourClock C++ client for MirrorCPP integration tests (design §8 / §10).
//
// Ports the TLA+ HourClock state machine (specs/HourClock.tla): the computer
// adopts the oracle's init state verbatim, then each "tick" advances the
// 1..12 hour cycle. Report_state includes EVERY state variable and omits
// paramVars (none for HourClock) — §3.5 rules.
#ifndef MIRRORCPP_TEST_HOURCLOCK_CLIENT_HPP
#define MIRRORCPP_TEST_HOURCLOCK_CLIENT_HPP

#include <mirrorcpp/mirrorcpp.hpp>

namespace mirrorcpp::test {

// The full HourClock state as a Value map (all six spec vars).
inline State hourclock_state(Value::Int hr, Value::Int latest_hr, bool ticked,
                             std::string action_taken, Value::Int step_count) {
  Value::Record nondet;
  nondet.fields["start_hr"] = Value(hr);
  nondet.fields["start_latest_hr"] = Value(latest_hr);
  State s;
  s["hr"] = Value(hr);
  s["latest_hr"] = Value(latest_hr);
  s["ticked"] = Value(ticked);
  s["action_taken"] = Value(std::move(action_taken));
  s["nondet_picks"] = Value(std::move(nondet));
  s["step_count"] = Value(step_count);
  return s;
}

// Adopt the oracle's init state verbatim (design §10).
inline State hc_adopt_init(const State& oracle_init) { return oracle_init; }

// Compute the next HourClock state after one tick (hc_tick, design §10).
// Per HourClock.tla HCnext: hr advances modulo 12 (12 -> 1);
// latest_hr' = hr (the PREVIOUS hr); ticked' = true; action_taken' = "tick";
// nondet_picks unchanged; step_count increments by 1.
inline State hc_tick(const State& prev) {
  const Value::Int hr = prev.at("hr").as_int().value_or(Value::Int(1));
  const Value::Int latest = prev.at("latest_hr").as_int().value_or(Value::Int(1));
  const Value::Int count = prev.at("step_count").as_int().value_or(Value::Int(0));

  Value::Int next_hr = hr + 1;
  if (next_hr > 12) next_hr = 1;
  const Value::Int next_count = count + 1;

  Value::Record nondet;
  if (const Value::Record* r = prev.at("nondet_picks").as_record())
    nondet = *r;
  else {
    nondet.fields["start_hr"] = Value(hr);
    nondet.fields["start_latest_hr"] = Value(latest);
  }

  State s;
  s["hr"] = Value(next_hr);
  s["latest_hr"] = Value(hr);      // latest_hr' = hr (old hr), per HCnext
  s["ticked"] = Value(true);
  s["action_taken"] = Value(std::string("tick"));
  s["nondet_picks"] = Value(std::move(nondet));
  s["step_count"] = Value(next_count);
  return s;
}

// A StateComputer for the HourClock machine.
//   action == "init"  -> adopt the oracle state (params_or_state).
//   action == "tick"  -> hc_tick(prev_state).
inline StateComputer hourclock_computer() {
  return [](std::string_view action, const State& params_or_state,
            const State& prev_state) -> State {
    if (action == "init") return hc_adopt_init(params_or_state);
    return hc_tick(prev_state);
  };
}

}  // namespace mirrorcpp::test

#endif  // MIRRORCPP_TEST_HOURCLOCK_CLIENT_HPP
