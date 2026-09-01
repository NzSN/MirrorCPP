#!/usr/bin/env python3
"""Fake ModelMirrors over stdio for MirrorCPP integration tests (design §8).

Reads newline-delimited JSON protocol messages from stdin and plays a scripted
mirror session against a MirrorCPP client. Scenario is selected by the thin
executable wrappers in this directory, each importing this module and calling
run("<scenario>").

Scenarios (t5 stepping + t6 one-shots + t10 explore):
  happy / happy_wrap — register_traces happy path (start hr=1 / wrap 12->1)
  mismatch  — after a tick report, send step_mismatch with hints
  protoerr  — protocol_error right after register_traces
  unexpected— all_steps_done right after register_traces (not spec_validated)
  invalid   — spec_validated {"invalid": ...} right after register_traces
  register_happy  — register flow happy path (same stepping session)
  register_inline — register WITH inline spec; asserts spec + specPath present
  validate_valid / validate_invalid — register_validate verdicts
  validate_bound101 — register_validate with bound=101 -> register_error
  gen_traces — register_trace_gen -> gen_traces_done with inline itfTraces
  explore_session — register_explore_session -> explorer_ready, then generic
      command/reply alternation until explore_done -> explore_session_done
  explore_bad_command — first command -> protocol_error; client must close
      a valid reply, then done
  explore_stepping — register_explore happy path (full-state parameters)
"""

import json
import os
import sys

NUMBER_OF_TICKS = 5


def big(n):
    return {"#bigint": str(n)}


def hc_state(hr, latest_hr, ticked, action_taken, step_count):
    return {
        "hr": big(hr),
        "latest_hr": big(latest_hr),
        "ticked": ticked,
        "action_taken": action_taken,
        "nondet_picks": {"start_hr": big(1), "start_latest_hr": big(1)},
        "step_count": big(step_count),
    }


def expected_states():
    states = [hc_state(1, 1, False, "init", 0)]
    hr, latest_hr, step = 1, 1, 0
    for _ in range(NUMBER_OF_TICKS):
        next_hr = hr % 12 + 1
        latest_hr = hr
        step += 1
        states.append(hc_state(next_hr, latest_hr, True, "tick", step))
        hr = next_hr
    return states


def read_msg():
    line = sys.stdin.readline()
    if not line:
        return None
    try:
        return json.loads(line)
    except json.JSONDecodeError as e:
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: malformed JSON from client: %s" % e})
        return None


def write_msg(msg):
    sys.stdout.write(json.dumps(msg, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def state_of(report):
    """Extract the reported state object from a report_state message."""
    return report.get("state")


def hc_vars_equal(actual, expected):
    for key in ("hr", "latest_hr", "ticked", "action_taken", "nondet_picks", "step_count"):
        if actual.get(key) != expected.get(key):
            return False
    return True


def expect_step(expected_step):
    """Read the first client message and require it to be expected_step."""
    msg = read_msg()
    if msg is None:
        sys.stderr.write(("fake mirror: client closed before %s\n") % expected_step)
        return None
    if msg.get("proto_step") != expected_step:
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: expected %s, got %s" %
                            (expected_step, str(msg.get("proto_step")))});
        return None
    return msg


def expect_register_traces():
    return expect_step("register_traces") is not None


def drive_stepping(start_hr):
    """Run a happy stepping session from start_hr (init + N ticks -> all_steps_done)."""
    write_msg({"proto_step": "spec_validated", "result": "valid"})
    init = hc_state(start_hr, start_hr, False, "init", 0)
    states = [init]
    hr, latest_hr, step = start_hr, start_hr, 0
    for _ in range(NUMBER_OF_TICKS):
        next_hr = hr % 12 + 1
        latest_hr = hr
        step += 1
        states.append(hc_state(next_hr, latest_hr, True, "tick", step))
        hr = next_hr

    write_msg({"proto_step": "initial_state", "action": "init", "state": states[0]})
    report = read_msg()
    if report is None or report.get("proto_step") != "report_state":
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: expected report_state after initial_state"})
        return 1
    if not hc_vars_equal(state_of(report), states[0]):
        write_msg({"proto_step": "step_mismatch", "expected": states[0],
                   "actual": state_of(report),
                   "hints": [{"path": [{"field": "hr"}], "kind": "value_mismatch",
                              "expected": states[0]["hr"],
                              "actual": state_of(report).get("hr")}]})
        return 1

    for i in range(1, NUMBER_OF_TICKS + 1):
        write_msg({"proto_step": "step_ok"})
        write_msg({"proto_step": "next_step", "action": "tick", "parameters": {}})
        report = read_msg()
        if report is None or report.get("proto_step") != "report_state":
            write_msg({"proto_step": "protocol_error",
                       "error": "fake mirror: expected report_state after next_step"})
            return 1
        if not hc_vars_equal(state_of(report), states[i]):
            write_msg({"proto_step": "step_mismatch", "expected": states[i],
                       "actual": state_of(report),
                       "hints": [{"path": [{"field": "hr"}], "kind": "value_mismatch",
                                  "expected": states[i]["hr"],
                                  "actual": state_of(report).get("hr")}]})
            return 1

    write_msg({"proto_step": "all_steps_done"})
    while sys.stdin.readline():
        pass
    return 0


def run_happy():
    if not expect_register_traces():
        return 1
    return drive_stepping(1)


def run_happy_wrap():
    if not expect_register_traces():
        return 1
    return drive_stepping(12)


def run_mismatch():
    if not expect_register_traces():
        return 1
    write_msg({"proto_step": "spec_validated", "result": "valid"})
    states = expected_states()
    write_msg({"proto_step": "initial_state", "action": "init", "state": states[0]})
    report = read_msg()
    if report is None or report.get("proto_step") != "report_state":
        return 1
    write_msg({"proto_step": "step_ok"})
    write_msg({"proto_step": "next_step", "action": "tick", "parameters": {}})
    report = read_msg()
    if report is None or report.get("proto_step") != "report_state":
        return 1
    expected = states[1]
    actual = state_of(report)
    write_msg({"proto_step": "step_mismatch",
               "expected": expected,
               "actual": actual,
               "hints": [
                   {"path": [{"field": "hr"}], "kind": "value_mismatch",
                    "expected": expected["hr"], "actual": actual.get("hr")},
                   {"path": [{"field": "ticked"}], "kind": "value_mismatch",
                    "expected": expected["ticked"], "actual": actual.get("ticked")},
               ]})
    while sys.stdin.readline():
        pass
    return 0


def run_protoerr():
    if not expect_register_traces():
        return 1
    write_msg({"proto_step": "protocol_error",
               "error": "fake mirror: spec rejected: unknown operator FOO"})
    while sys.stdin.readline():
        pass
    return 0


def run_unexpected():
    if not expect_register_traces():
        return 1
    write_msg({"proto_step": "all_steps_done"})
    while sys.stdin.readline():
        pass
    return 0


def run_invalid():
    if not expect_register_traces():
        return 1
    write_msg({"proto_step": "spec_validated",
               "result": {"invalid": "fake mirror: type error in init predicate"}})
    while sys.stdin.readline():
        pass
    return 0


def run_register_happy():
    if expect_step("register") is None:
        return 1
    return drive_stepping(1)


def run_register_inline():
    msg = expect_step("register")
    if msg is None:
        return 1
    if "spec" not in msg or not msg.get("spec", {}).get("sources"):
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: register with inline spec missing spec"})
        return 1
    apalache = msg.get("apalacheConfig", {})
    if "specPath" not in apalache:
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: register missing apalacheConfig.specPath"})
        return 1
    return drive_stepping(1)


def run_validate_valid():
    msg = expect_step("register_validate")
    if msg is None:
        return 1
    if msg.get("bound") != 10:
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: validate bound != 10"})
        return 1
    write_msg({"proto_step": "spec_validated", "result": "valid"})
    while sys.stdin.readline():
        pass
    return 0


def run_validate_invalid():
    if expect_step("register_validate") is None:
        return 1
    write_msg({"proto_step": "spec_validated",
               "result": {"invalid": "fake mirror: state invariant violated"}})
    while sys.stdin.readline():
        pass
    return 0


def run_validate_bound101():
    msg = expect_step("register_validate")
    if msg is None:
        return 1
    if msg.get("bound") != 101:
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: expected bound=101"})
        return 1
    write_msg({"proto_step": "register_error",
               "error": "fake mirror: bound must be <= 100 (got 101)"})
    while sys.stdin.readline():
        pass
    return 0


def run_gen_traces():
    msg = expect_step("register_trace_gen")
    if msg is None:
        return 1
    if "spec" not in msg or not msg.get("spec", {}).get("sources"):
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: register_trace_gen missing spec"})
        return 1
    trace = {"#meta": {"format": "ITF"}, "vars": [], "states": []}
    write_msg({"proto_step": "gen_traces_done",
               "itfTracePaths": ["/tmp/fake_trace1.itf.json", "/tmp/fake_trace2.itf.json"],
               "itfTraces": [trace, trace]})
    while sys.stdin.readline():
        pass
    return 0


def run_explore_session():
    """Scripted explorer session: explorer_ready, then generic command/reply
    alternation until explore_done."""
    if expect_step("register_explore_session") is None:
        return 1
    write_msg({"proto_step": "explorer_ready",
               "initTransitions": 2, "nextTransitions": 1, "stateInvariants": 1})
    while True:
        msg = read_msg()
        if msg is None:
            return 1
        step = msg.get("proto_step")
        if step == "explore_done":
            write_msg({"proto_step": "explore_session_done"})
            while sys.stdin.readline():
                pass
            return 0
        if step == "explore_assume_transition":
            write_msg({"proto_step": "explore_transition_status", "status": "ENABLED"})
        elif step == "explore_next_step":
            write_msg({"proto_step": "explore_step_done", "stepNo": 3})
        elif step == "explore_query_state":
            write_msg({"proto_step": "explore_state",
                       "state": hc_state(1, 1, False, "init", 0)})
        elif step == "explore_check_invariant":
            write_msg({"proto_step": "explore_invariant_status", "status": "SATISFIED"})
        elif step == "explore_assume_state":
            write_msg({"proto_step": "explore_assume_status", "status": "ENABLED"})
        elif step == "explore_rollback":
            write_msg({"proto_step": "explore_rollback_done",
                       "snapshotId": msg.get("snapshotId", 0)})
        else:
            write_msg({"proto_step": "protocol_error",
                       "error": "fake mirror: unexpected explore command " + step})
    return 0


def run_explore_bad_command():
    """Deliberate bad command -> fatal protocol_error, then expect EOF."""
    if expect_step("register_explore_session") is None:
        return 1
    write_msg({"proto_step": "explorer_ready",
               "initTransitions": 2, "nextTransitions": 1, "stateInvariants": 1})
    # First command: any -> protocol_error (fatal for this connection).
    first = read_msg()
    if first is None:
        return 1
    if first.get("proto_step") == "explore_done":
        write_msg({"proto_step": "explore_session_done"})
        return 0
    write_msg({"proto_step": "protocol_error",
               "error": "fake mirror: bad transition id " + str(first.get("transitionId"))})
    while sys.stdin.readline():
        return 1
    return 0


def run_explore_stepping():
    """register_explore happy path: full expected state in next_step.parameters."""
    msg = expect_step("register_explore")
    if msg is None:
        return 1
    if "spec" not in msg or not msg.get("spec", {}).get("sources"):
        write_msg({"proto_step": "protocol_error",
                   "error": "fake mirror: register_explore missing spec"})
        return 1
    write_msg({"proto_step": "spec_validated", "result": "valid"})
    states = expected_states()
    write_msg({"proto_step": "initial_state", "action": "init", "state": states[0]})
    report = read_msg()
    if report is None or report.get("proto_step") != "report_state":
        return 1
    if not hc_vars_equal(state_of(report), states[0]):
        return 1
    for i in range(1, NUMBER_OF_TICKS + 1):
        write_msg({"proto_step": "step_ok"})
        write_msg({"proto_step": "next_step", "action": "tick",
                   "parameters": states[i]})
        report = read_msg()
        if report is None or report.get("proto_step") != "report_state":
            return 1
        if not hc_vars_equal(state_of(report), states[i]):
            return 1
    write_msg({"proto_step": "all_steps_done"})
    while sys.stdin.readline():
        pass
    return 0



def run_trace():
    """Trace-driven conformance replay (design §8 north star).

    Reads an ITF protocol trace (path in env MIRRORCPP_TRACE) and replays the
    mirror side of it: each trace state records the single in-flight message —
    a non-empty client_to_mirror means the CLIENT just sent code X (we assert
    it), a non-empty mirror_to_client means the MIRROR sends code Y (we emit
    it). The client flow under test is driven by the C++ side (mbt_test.cpp).
    """
    path = os.environ.get("MIRRORCPP_TRACE", "")
    if not path:
        sys.stderr.write("fake mirror: MIRRORCPP_TRACE not set\n")
        return 2
    try:
        with open(path) as f:
            trace = json.load(f)
    except OSError as e:
        sys.stderr.write("fake mirror: %s\n" % e)
        return 2

    # Message-code tables from the protocol model (MirrorProtocol.tla), as
    # embedded in the vendored specs/traces/*.itf.json fixtures:
    #   client->mirror: 0 register, 2 report_state, 10 register_traces,
    #       13 register_explore, 14 register_explore_session, 16 explore cmd,
    #       18 explore_done
    #   mirror->client: 1 register_error, 3 spec_validated, 4 initial_state,
    #       7 step_mismatch, 8 all_steps_done, 15 explorer_ready,
    #       17 explore result, 18 explore_session_done
    CODE_OF_CLIENT_STEP = {
        "register": 0, "report_state": 2, "register_traces": 10,
        "register_explore": 13, "register_explore_session": 14,
        "explore_done": 18,
    }
    EXPLORE_CMDS = {"explore_assume_transition", "explore_next_step",
                    "explore_query_state", "explore_check_invariant",
                    "explore_assume_state", "explore_rollback"}

    def client_code(msg):
        step = msg.get("proto_step")
        if step in EXPLORE_CMDS:
            return 16
        return CODE_OF_CLIENT_STEP.get(step, -1)

    def code_val(c):
        return c["#bigint"] if isinstance(c, dict) else str(c)

    def send_mirror(code, last_cmd=None):
        if code == 3:
            write_msg({"proto_step": "spec_validated", "result": "valid"})
        elif code == 4:
            write_msg({"proto_step": "initial_state", "action": "init",
                       "state": hc_state(1, 1, False, "init", 0)})
        elif code == 8:
            write_msg({"proto_step": "all_steps_done"})
        elif code == 1:
            write_msg({"proto_step": "register_error",
                       "error": "fake mirror: trace-driven register_error"})
        elif code == 15:
            write_msg({"proto_step": "explorer_ready",
                       "initTransitions": 1, "nextTransitions": 1, "stateInvariants": 0})
        elif code == 7:
            exp = expected_states()[0]
            act = hc_state(2, 1, True, "tick", 1)
            write_msg({"proto_step": "step_mismatch", "expected": exp, "actual": act,
                       "hints": [{"path": [{"field": "hr"}], "kind": "value_mismatch",
                                  "expected": exp["hr"], "actual": act["hr"]}]})
        elif code == 17:
            # explore result: reply depends on the command received
            if last_cmd == "explore_assume_transition":
                write_msg({"proto_step": "explore_transition_status", "status": "ENABLED"})
            elif last_cmd == "explore_next_step":
                write_msg({"proto_step": "explore_step_done", "stepNo": 1})
            elif last_cmd == "explore_query_state":
                write_msg({"proto_step": "explore_state",
                           "state": hc_state(1, 1, False, "init", 0)})
            elif last_cmd == "explore_check_invariant":
                write_msg({"proto_step": "explore_invariant_status", "status": "SATISFIED"})
            elif last_cmd == "explore_assume_state":
                write_msg({"proto_step": "explore_assume_status", "status": "ENABLED"})
            elif last_cmd == "explore_rollback":
                write_msg({"proto_step": "explore_rollback_done", "snapshotId": 0})
            else:
                write_msg({"proto_step": "protocol_error",
                           "error": "fake mirror: trace result code 17 without a command"})
                return False
        elif code == 18:
            write_msg({"proto_step": "explore_session_done"})
        else:
            write_msg({"proto_step": "protocol_error",
                       "error": "fake mirror: trace wants unknown mirror code %s" % code})
            return False
        return True

    last_cmd = None
    for state in trace.get("states", []):
        ctm = state.get("client_to_mirror") or []
        mtc = state.get("mirror_to_client") or []
        for code in ctm:
            msg = read_msg()
            if msg is None:
                sys.stderr.write("fake mirror: client closed before client code %s\n" % code_val(code))
                return 1
            got = client_code(msg)
            want = code_val(code)
            if str(got) != str(want):
                write_msg({"proto_step": "protocol_error",
                           "error": "fake mirror: trace expects client code %s, got %s (%s)" %
                                    (want, got, msg.get("proto_step"))})
                return 1
            if msg.get("proto_step") in EXPLORE_CMDS:
                last_cmd = msg.get("proto_step")
        for code in mtc:
            if not send_mirror(int(code_val(code)), last_cmd):
                return 1

    # Trace exhausted. Explore sessions are closed by the client with
    # explore_done (+ session-done ack) after the modelled command window.
    msg = read_msg()
    if msg is not None:
        if msg.get("proto_step") == "explore_done":
            write_msg({"proto_step": "explore_session_done"})
        elif msg.get("proto_step") in EXPLORE_CMDS:
            # client issued one more command than the trace models: honour it
            write_msg({"proto_step": "explore_transition_status", "status": "ENABLED"})
            msg = read_msg()
            if msg is not None and msg.get("proto_step") == "explore_done":
                write_msg({"proto_step": "explore_session_done"})
        else:
            write_msg({"proto_step": "protocol_error",
                       "error": "fake mirror: unexpected message after trace: %s" %
                                msg.get("proto_step")})
            return 1
    # Drain: the client closes its transport on every terminal path.
    while sys.stdin.readline():
        pass
    return 0

def run(scenario):
    table = {
        "happy": run_happy,
        "happy_wrap": run_happy_wrap,
        "mismatch": run_mismatch,
        "protoerr": run_protoerr,
        "unexpected": run_unexpected,
        "invalid": run_invalid,
        "register_happy": run_register_happy,
        "register_inline": run_register_inline,
        "validate_valid": run_validate_valid,
        "validate_invalid": run_validate_invalid,
        "validate_bound101": run_validate_bound101,
        "gen_traces": run_gen_traces,
        "explore_session": run_explore_session,
        "explore_bad_command": run_explore_bad_command,
        "explore_stepping": run_explore_stepping,
        "trace": run_trace,
    }
    fn = table.get(scenario)
    if fn is None:
        sys.stderr.write("fake mirror: unknown scenario %r\n" % scenario)
        return 2
    return fn()


if __name__ == "__main__":
    sys.exit(run(sys.argv[1] if len(sys.argv) > 1 else "happy"))
