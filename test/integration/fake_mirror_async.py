#!/usr/bin/env python3
"""Fake ModelMirrors async job server over TCP for MirrorCPP integration tests
(guide §6, drafts/conformance-gap-plan.md G1.6).

Listens on 127.0.0.1:<port>, accepts <nconns> connections (a thread each, so
the cross-connection job-control case works), and answers the async job
protocol from a shared in-memory job table — jobIds are cross-connection
visible (C17).

Scenarios:
  validate   — register_validate_async -> job_accepted; query -> running;
               await with timeoutSecs -> job_status running (non-terminal);
               await without -> job_result {"validate":"valid"}; terminal
               results are cached (idempotent re-await, C18)
  gen        — register_trace_gen_async -> job_result genTraces payload
  queue_full — any async submit -> register_error "job queue full" (C22)
  infra      — await -> job_result {"error":"worker died"} (infraError, §9)

Common behavior (all scenarios): unknown jobIds answer phase "unknown" (C21);
cancel_job flips a live job to cancelled and answers job_status.

Usage: fake_mirror_async.py <port> <scenario> <nconns>
Exits 0 when every accepted connection was served and closed cleanly.
"""
import json
import socket
import sys
import threading

JOBS = {}
LOCK = threading.Lock()
NEXT_ID = [0]
SCENARIO = "validate"


def write_msg(conn, msg):
    conn.sendall((json.dumps(msg, separators=(",", ":")) + "\n").encode())


def job_status(jid):
    with LOCK:
        phase = JOBS.get(jid, {}).get("phase", "unknown")
    return {"proto_step": "job_status", "jobId": jid, "phase": phase}


def terminal_result(jid):
    with LOCK:
        job = JOBS[jid]
        if job.get("result") is None:
            if SCENARIO == "infra":
                job["result"] = {"error": "worker died"}
            elif job["kind"] == "validate":
                job["result"] = {"validate": "valid"}
            else:
                job["result"] = {"genTraces": {
                    "itfTracePaths": ["out/itf/t1.itf.json"], "itfTraces": []}}
        job["phase"] = "done" if "error" not in job["result"] else "failed"
        return {"proto_step": "job_result", "jobId": jid, "outcome": job["result"]}


def handle_message(msg):
    """One request -> one reply dict (None = protocol_error already handled)."""
    step = msg.get("proto_step")
    if step in ("register_validate_async", "register_trace_gen_async"):
        if SCENARIO == "queue_full":
            return {"proto_step": "register_error",
                    "error": "job queue full"}
        with LOCK:
            NEXT_ID[0] += 1
            jid = "job-%d" % NEXT_ID[0]
            kind = "validate" if step == "register_validate_async" else "gen_traces"
            JOBS[jid] = {"phase": "running", "kind": kind, "result": None}
        return {"proto_step": "job_accepted", "jobId": jid, "kind": kind}
    if step == "query_job":
        return job_status(msg.get("jobId", ""))
    if step == "await_job":
        jid = msg.get("jobId", "")
        with LOCK:
            job = JOBS.get(jid)
        if job is None:
            return job_status(jid)  # unknown (C21)
        if job["phase"] == "cancelled":
            return job_status(jid)
        if job.get("result") is not None:
            return {"proto_step": "job_result", "jobId": jid, "outcome": job["result"]}
        if "timeoutSecs" in msg:
            return job_status(jid)  # timeout: non-terminal, never an error (C18)
        return terminal_result(jid)
    if step == "cancel_job":
        jid = msg.get("jobId", "")
        reply = None
        with LOCK:
            job = JOBS.get(jid)
            if job is not None:
                if job.get("result") is not None:
                    reply = {"proto_step": "job_result", "jobId": jid,
                             "outcome": job["result"]}
                else:
                    job["phase"] = "cancelled"
        return reply if reply is not None else job_status(jid)
    return {"proto_step": "protocol_error",
            "error": "fake async mirror: unexpected " + str(step)}


def serve_conn(conn):
    ok = True
    buf = b""
    try:
        while True:
            chunk = conn.recv(4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except json.JSONDecodeError:
                    write_msg(conn, {"proto_step": "protocol_error",
                                     "error": "fake async mirror: bad JSON"})
                    ok = False
                    continue
                write_msg(conn, handle_message(msg))
    except OSError:
        ok = False
    finally:
        conn.close()
    return ok


def main(port, scenario, nconns):
    global SCENARIO
    SCENARIO = scenario
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(nconns)
    srv.settimeout(30)
    threads = []
    results = []
    try:
        for _ in range(nconns):
            conn, _ = srv.accept()
            t = threading.Thread(target=lambda c=conn: results.append(serve_conn(c)))
            t.start()
            threads.append(t)
    except socket.timeout:
        srv.close()
        return 2
    srv.close()
    for t in threads:
        t.join(30)
    return 0 if all(results) and len(results) == nconns else 1


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.stderr.write("usage: fake_mirror_async.py <port> <scenario> <nconns>\n")
        sys.exit(2)
    sys.exit(main(int(sys.argv[1]), sys.argv[2], int(sys.argv[3])))
