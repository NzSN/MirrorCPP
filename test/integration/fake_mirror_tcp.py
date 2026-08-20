#!/usr/bin/env python3
"""Fake ModelMirrors over TCP for the mirrorcpp-validate CLI tests (design §8).

Listens on 127.0.0.1:<port>, accepts ONE connection, reads one client message,
and replies per scenario, then closes: it emulates the register_validate flow
(exactly one reply, session ends).

Scenarios:
  valid         — spec_validated {"result":"valid"}
  invalid       — spec_validated {"result":{"invalid":"<apalache output>"}}
  register_error— register_error {"error":"<message>"}

Usage: fake_mirror_tcp.py <port> <scenario> [--wait-close]
The server exits 0 when it replied cleanly.
"""
import json
import socket
import sys


def main(port, scenario):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    conn, _ = srv.accept()
    # Read one newline-delimited JSON message.
    buf = b""
    while b"\n" not in buf:
        chunk = conn.recv(4096)
        if not chunk:
            break
        buf += chunk
    line = buf.split(b"\n", 1)[0]
    if not line:
        conn.close()
        srv.close()
        return 2
    try:
        msg = json.loads(line)
    except json.JSONDecodeError:
        msg = {}
    # Sanity: the client must have sent register_validate.
    if msg.get("proto_step") != "register_validate":
        conn.close()
        srv.close()
        return 2
    if scenario == "valid":
        reply = {"proto_step": "spec_validated", "result": "valid"}
    elif scenario == "invalid":
        reply = {"proto_step": "spec_validated",
                 "result": {"invalid": "state invariant violated at line 12"}}
    else:
        reply = {"proto_step": "register_error",
                 "error": "bound must be <= 100 (got 101)"}
    conn.sendall((json.dumps(reply, separators=(",", ":")) + "\n").encode())
    conn.close()
    srv.close()
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        sys.stderr.write("usage: fake_mirror_tcp.py <port> <scenario>\n")
        sys.exit(2)
    sys.exit(main(int(sys.argv[1]), sys.argv[2]))
