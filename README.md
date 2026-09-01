# MirrorCPP

A C++23 client library for [ModelMirrors](https://github.com/NzSN/ModelMirrors),
the conformance checker that uses [Apalache](https://github.com/apalache-mc/apalache)
to check a client-implemented state machine against a TLA+ specification, state
by state (`diffState`: exact variable-by-variable equality with structured diff
hints). MirrorCPP is the C++ member of the client family, alongside
[MirrorECMA](https://github.com/NzSN/MirrorECMA) (TypeScript) and
[MirrorRust](https://github.com/NzSN/MirrorRust).

The library speaks newline-delimited JSON over **stdio**, **TCP**, or
**mutually-authenticated TLS 1.3** — all six registration flows:
`register`, `register_traces`, `register_trace_gen`, `register_explore`,
`register_explore_session`, and `register_validate`, plus optional
**Consul-compatible service discovery** (fail-closed; location only —
authentication always happens in the mTLS handshake).

The complete design lives in [docs/design.md](docs/design.md).

## Features

- **Faithful value model**: arbitrary-precision integers (`{"#bigint": "…"}`),
  sets, sequences, tuples, records, maps, variants — encoded/decoded exactly
  per the ITF conventions the mirror uses.
- **Blocking API** with `Result<T> = std::expected<T, Error>`; no exceptions
  cross the API boundary.
- **Inline specs**: `spec_from_files` resolves `EXTENDS`/`INSTANCE` closure
  with `TLA_LIBRARY_PATH` support.
- **Explorer sessions**: `assume_transition`, `next_step`, `query_state`,
  `check_invariant`, `assume_state`, `rollback`.
- **Async job interface** (server modes): `submit_validate_async` /
  `submit_trace_gen_async`, `query_job`, long-polling `await_job`, and
  `cancel_job` — jobIds are cross-connection visible, terminal results
  idempotent (guide §6, C17–C22).
- **Golden-corpus verified**: the vendored frozen Haskell wire corpus
  (`test/fixtures/golden/`) is replayed against the codecs in
  `mirrorcpp_golden_corpus_test` (guide §10 item 1).
- **Safety by default**: TLS 1.3 only, SAN hostname/IP verification, cert
  fingerprint pinning, POSIX `0600` client-key enforcement, fail-closed
  registry parsing, no default read timeouts (Apalache can legitimately take
  minutes).
- **Strict framing**: every transport rejects empty lines, embedded LF, and
  payloads above 65,535 bytes before writing the terminating LF.

## Requirements

- CMake ≥ 3.24, a C++23 compiler (tested with GCC 13)
- [nlohmann/json](https://github.com/nlohmann/json) v3.11+ and
  [Boost.Multiprecision](https://www.boost.org/) headers (both are fetched
  automatically by the build when not found)
- OpenSSL 3.0 (only when `MIRRORCPP_WITH_TLS=ON`, the default)
- Python 3 (fake-mirror integration tests)

## Building

```sh
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build
```

Useful CMake options:

| Option | Default | Meaning |
|---|---|---|
| `MIRRORCPP_WITH_TLS` | `ON` | Build mTLS transport (`connect_tls`, `connect_from_registry`) |
| `MIRRORCPP_BUILD_TESTS` | `ON` | Build unit + integration tests |
| `MIRRORCPP_BUILD_EXAMPLES` | `ON` | Build `examples/hourclock` |
| `MIRRORCPP_BUILD_CLI` | `ON` | Build the `mirrorcpp-validate` CLI |
| `MIRRORCPP_SANITIZE` | *(empty)* | Sanitizer flags: `address,undefined` etc. |
| `MIRRORCPP_WERROR` | `OFF` | Treat warnings as errors |

## Quick start (design §10)

```cpp
#include <mirrorcpp/mirrorcpp.hpp>
using namespace mirrorcpp;

int main() {
  auto transport = spawn_mirror("/usr/local/bin/ModelMirrors");  // stdio
  // TCP:    auto transport = connect_tcp("127.0.0.1", 8823);
  // mTLS:   auto transport = connect_tls("127.0.0.1", 8823, tls_opts);
  // Consul: auto transport = connect_from_registry("http://127.0.0.1:8500", tls_opts);

  auto spec = spec_from_files("specs/HourClock.tla");
  ApalacheConfig cfg{ .spec_path = "specs/HourClock.tla",
                      .invariant = "TraceComplete", .length_bound = 13 };

  auto result = run_client(*transport, cfg, {.num_traces = 1},
    [&](std::string_view action, const State& params, const State& prev) {
      if (action == "init") return params;   // adopt the oracle's init
      return hc_tick(prev);                    // your C++ transition
    }, *spec);
  if (!result && result.error().kind == ErrorKind::step_mismatch)
    render_diff_hints(result.error().hints);   // conformance failure details
}
```

The full working example is [examples/hourclock.cpp](examples/hourclock.cpp);
run it with `./build/examples/hourclock /path/to/ModelMirrors specs/HourClock.tla`.

## Consuming the installed package

```sh
cmake --install build --prefix /usr/local
```

```cmake
find_package(mirrorcpp CONFIG REQUIRED)
target_link_libraries(app PRIVATE mirrorcpp::mirrorcpp)
```

The installed `mirrorcppConfig.cmake` re-attaches the public dependencies
(nlohmann_json, Boost headers, OpenSSL when TLS-enabled) to the imported
target. See [test/packaging/](test/packaging/) for the packaging smoke test.

## Integration tests and MIRROR_BIN

Unit tests need no mirror. Integration tests run against either a scripted
**fake mirror** (`test/integration/fake_mirror*.py`, no MIRROR_BIN needed) or
the **real ModelMirrors binary** when `MIRROR_BIN` is set:

```sh
MIRROR_BIN=/path/to/ModelMirrors \
  SPEC=/path/to/authoritative/Counter.tla ctest --test-dir build
```

With `MIRROR_BIN` set, `real_mirror_hourclock` runs (scenarios:
Counter trace generation + replay using `SPEC` (or the checked-in Counter
fixture by default) over stdio, a real TCP server, and a real ephemeral-PKI
mTLS server; an intentionally extra observable state key must produce
terminal `step_mismatch` on all three transports,
`run_client_with_traces`, HourClock `register`, `gen_traces`, `validate` incl.
`bound=101`, `run_client_explore`, `ExploreSession` scripted session +
fatal-`protocol_error` check, async jobs against `mirror --serve`, and a
fault-close check; sync/async verdict congruence and reverse-order async job
awaits); without it the test skips with exit code 77.

TLS scenarios use test certificates generated by ModelMirrors'
`scripts/gen-certs.sh` (CA + server with SAN IP:127.0.0.1 + client; private
keys `0600`). See [specs/README.md](specs/README.md) for the real-mirror
build provenance (upstream commit) and cert/cab paths.

The MBT fixture test (`mirrorcpp_mbt_test`, design §8 “Conformance north
star”) replays the vendored Apalache-generated protocol traces
(`specs/traces/*.itf.json`: `all_steps_done`, `step_mismatch`,
`register_error`, `explore_session`, `explore_cmd`, `fault_close`) through
the trace-driven fake mirror and asserts the client emits the exact
message-code sequences the TLA+ protocol model produces.

## Sanitizers

```sh
cmake -S . -B build-asan -DMIRRORCPP_SANITIZE=address,undefined
cmake --build build-asan -j$(nproc)
ctest --test-dir build-asan
```

## Transports and security model

- **stdio**: the mirror is a child process with piped stdin/stdout; stderr is
  inherited so Apalache logs stay visible. One session per child.
- **TCP**: `ModelMirrors --serve <port>`; plain newline-delimited JSON.
- **mTLS**: TLS 1.3 with client certificate + optional SHA-256 fingerprint
  pinning (explicit pin wins over registry-advertised pins). SAN verification
  is always on and CN fallback is disabled; IP literals require an IP SAN and
  are not sent as SNI. There is no “insecure” toggle.
- **Registry**: Consul health API (`/v1/health/service/modelmirrors?passing=true`),
  fail-closed parsing, candidate iteration with per-candidate diagnostics.

## License

See the upstream projects' licenses; MirrorCPP is published under the same
terms as [ModelMirrors](https://github.com/NzSN/ModelMirrors).
