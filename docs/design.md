# MirrorCPP Design

**Status:** Design proposal (no implementation yet)
**Audience:** Contributors to MirrorCPP
**Upstream protocol owner:** [ModelMirrors](https://github.com/NzSN/ModelMirrors)
**Sibling clients:** [MirrorRust](https://github.com/NzSN/MirrorRust) (Rust), [MirrorECMA](https://github.com/NzSN/MirrorECMA) (TypeScript)

---

## 1. Background

[ModelMirrors](https://github.com/NzSN/ModelMirrors) is a conformance checker: it uses [Apalache](https://github.com/apalache-mc/apalache) as an oracle for a
TLA+ specification and checks a client-implemented state machine against it, state by state
(`diffState`: exact variable-by-variable equality with structured diff hints). The mirror is a
standalone process; clients speak **newline-delimited JSON** over **stdio**, **TCP**, or
**mutually-authenticated TLS 1.3**, with optional **Consul-compatible service discovery**.

The mirror supports six registration flows:

| Registration | Shape | Purpose |
|---|---|---|
| `register` | request → stepping loop | Generate traces with Apalache, then replay them against the client |
| `register_traces` | request → stepping loop | Replay pre-generated ITF trace files (mirror-local paths only) |
| `register_trace_gen` | request → one reply | Generate ITF trace files, return their paths + inline contents |
| `register_explore` | request → stepping loop | Mirror-driven symbolic exploration; expected states computed live by the explorer server |
| `register_explore_session` | request → command/reply session | Client-driven symbolic checking (`assumeTransition`, `nextStep`, `queryState`, `checkInvariant`, `assumeState`, `rollback`) |
| `register_validate` | request → one reply | Typecheck + bounded model check only; verdict, then session ends |

**MirrorCPP** is the C++ member of this client family: a library (plus a small CLI) that lets a
C++ state machine be conformance-checked by any ModelMirrors mirror. This document is the complete
design. It is derived from the authoritative sources in ModelMirrors (`src/Protocol/Format/Json.hs`,
`src/Protocol/Core.hs`, `src/Apalache/Types.hs`, `src/Engine/Core.hs`, `docs/protocol-spec.md`,
README) and from the architecture of the MirrorECMA sibling client.

## 2. Goals and non-goals

### Goals

1. **Full protocol coverage.** All six registration flows, all transports (stdio subprocess, TCP,
   mTLS), Consul registry discovery, inline spec sources with `EXTENDS`/`INSTANCE` closure
   resolution, and explorer sessions.
2. **A library first.** `libmirrorcpp` is consumable from any C++ build via CMake
   (`find_package(mirrorcpp)` or `add_subdirectory`/`FetchContent`). A thin
   `mirrorcpp-validate` CLI (CI-friendly, exit 0/1/2) is an optional build artifact.
3. **Faithful value model.** Arbitrary-precision integers (`{"#bigint": "…"}`), sets, sequences,
   tuples, records, maps, variants — encoded/decoded exactly per the ITF conventions the mirror uses.
4. **Safety by default.** TLS 1.3 only, SAN hostname verification, certificate fingerprint pinning,
   POSIX `0600` client-key enforcement, fail-closed registry parsing, no default read timeouts
   (Apalache can legitimately take minutes).
5. **Testable against the real mirror.** Unit tests for codecs/parsers; integration tests that run
   every flow against a real ModelMirrors binary (`MIRROR_BIN`), over all three transports.

### Non-goals

- **No mirror implementation.** MirrorCPP never runs Apalache and never acts as a server.
- **No async runtime.** The protocol strictly alternates request/reply; the public API is blocking.
  Callers wanting concurrency run independent sessions on their own threads. (Note: the mirror's
  asynchronous *job* interface — `register_*_async`/`query_job`/`await_job`/`cancel_job`, guide §6 —
  IS implemented; it is ordinary blocking request/reply over server-mode connections, not a
  client-side async runtime.)
- **No general-purpose JSON-RPC/Consul client.** Only the single registry endpoint and the session
  protocol are implemented.
- **Header-only distribution is not a goal** (a compiled static/shared library keeps TLS and
  process-spawn code out of headers).

## 3. Protocol contract (what MirrorCPP must implement)

This section condenses the normative contract from ModelMirrors. Where the prose and code disagree,
`src/Protocol/Format/Json.hs` is authoritative.

### 3.1 Framing and transports

- Messages are single-line JSON objects, UTF-8, terminated by `
`. **No embedded raw newlines**
  (standard JSON string escaping handles this; serializers must produce compact, single-line output).
- Every message is an object with a `proto_step` string discriminant.
- The **first message on a connection must be a `Register*` message**; anything else receives
  `protocol_error` and the connection closes. There is no greeting, banner, or version exchange —
  in mTLS mode the handshake carries all setup.
- Transports:
  - **stdio** — mirror runs as a child process; one session, then the mirror exits. Child
    `stderr` is inherited so Apalache diagnostics reach the user's console.
  - **TCP** — `ModelMirrors --serve <port>`; one session per connection.
  - **mTLS** — `ModelMirrors --server <port> --tls …`; TLS 1.3 **only**, client certificate
    required, signed by the server CA.
- **No default read timeout.** Trace generation and symbolic exploration spawn a JVM per session;
  blocking reads must not time out aggressively. Connect/handshake timeouts are configurable
  (default 10 s).

### 3.2 Message inventory

Client → mirror:

| `proto_step` | Fields |
|---|---|
| `register` | `apalacheConfig`, `traceConfig`, `spec?` |
| `register_traces` | `apalacheConfig`, `itfTracePaths` |
| `register_trace_gen` | `apalacheConfig`, `traceConfig`, `destPath?`, `spec?` |
| `register_explore` | `spec` (inline, required), `invariants`, `exports`, `maxSteps?` |
| `register_explore_session` | `spec` (inline, required), `invariants`, `exports` |
| `register_validate` | `apalacheConfig`, `bound`, `spec?` |
| `register_validate_async` (server modes) | `apalacheConfig`, `bound`, `spec?` |
| `register_trace_gen_async` (server modes) | `apalacheConfig`, `traceConfig`, `destPath?`, `spec?` |
| `query_job` (server modes) | `jobId` |
| `await_job` (server modes) | `jobId`, `timeoutSecs?` |
| `cancel_job` (server modes) | `jobId` |
| `report_state` | `state` |
| `explore_assume_transition` | `transitionId` |
| `explore_next_step` | — |
| `explore_query_state` | — |
| `explore_check_invariant` | `invariantId` |
| `explore_assume_state` | `state` |
| `explore_rollback` | `snapshotId` |
| `explore_done` | — |

Mirror → client:

| `proto_step` | Fields |
|---|---|
| `spec_validated` | `result`: `"valid"` or `{"invalid": "<text>"}` |
| `initial_state` | `action`, `state` (full state) |
| `next_step` | `action`, `parameters` (paramVars-extracted; **full state** in `register_explore`) |
| `step_ok` | — |
| `step_mismatch` | `expected`, `actual`, `hints?` |
| `all_steps_done` | — |
| `gen_traces_done` | `itfTracePaths`, `itfTraces?` (inline ITF JSON contents) |
| `register_error` | `error` |
| `protocol_error` | `error` |
| `explorer_ready` | `initTransitions`, `nextTransitions`, `stateInvariants` (counts) |
| `explore_transition_status` / `explore_assume_status` | `status`: `ENABLED` \| `DISABLED` \| `UNKNOWN` |
| `explore_step_done` | `stepNo` |
| `explore_state` | `state` |
| `explore_invariant_status` | `status`: `SATISFIED` \| `VIOLATED` \| `UNKNOWN` |
| `explore_rollback_done` | `snapshotId` |
| `explore_session_done` | — |
| `job_accepted` (server modes) | `jobId`, `kind`: `validate` \| `gen_traces` |
| `job_status` (server modes) | `jobId`, `phase`: `pending` \| `running` \| `done` \| `failed` \| `cancelled` \| `unknown` |
| `job_result` (server modes) | `jobId`, `outcome`: `{"validate": <ValidateResult>}` \| `{"genTraces": {…}}` \| `{"error": "…"}` (infraError — not a spec verdict, retryable) |

`apalacheConfig` fields: `specPath` (string, **always sent** — when an inline `spec` is present the
mirror materializes the sources and ignores the path, so a placeholder is fine), `initPredicate?`,
`nextPredicate?`, `constInit?`, `invariant` (string, may be empty), `lengthBound` (int),
`paramVars` (comma-separated string, may be empty).

`traceConfig` fields: `numTraces` (int), `view?` (string).

`spec` is `{"sources": ["---- MODULE Root ----\n…", "…dep…"]}`; **`sources[0]` is the root
module**, the rest are dependencies. The mirror names materialized files after their `MODULE`
headers so `EXTENDS` resolves.

`register_validate` takes the check length in the top-level `bound` field (not
`apalacheConfig.lengthBound`); the mirror caps accepted bounds at `maxValidateBound = 100` and
replies `register_error` out of range.

### 3.3 Session flows

**Stepping flows** (`register`, `register_traces`, `register_explore`):

```
client → Register*            mirror → spec_validated ("valid")
mirror → initial_state        client → report_state
mirror → step_ok | step_mismatch
mirror → next_step            client → report_state
…                             …
mirror → all_steps_done
```

- On `step_mismatch` the run aborts; the error carries `expected`, `actual`, and path-based
  `hints` (mirror caps hints at 50, appending a `truncated` marker).
- `register_error` / `protocol_error` may arrive at any point and end the session.

**Explorer sessions** (`register_explore_session`):

- After `explorer_ready`, client commands and mirror replies **strictly alternate**, in any order,
  until `explore_done` → `explore_session_done`.
- A `protocol_error` identifies a client/session bug: the library closes and
  poisons the connection, and subsequent calls fail locally.

**One-shot flows**: `register_validate` → exactly one reply (`spec_validated` or
`register_error`), session ends. `register_trace_gen` → `gen_traces_done` (paths + optional
inline trace contents), session ends.

### 3.4 Value encoding (ITF)

| TLA+ value | JSON encoding |
|---|---|
| `Int` | `{"#bigint": "42"}` — digits as string; negatives `{"#bigint": "-5"}`; empty string decodes as null |
| `Bool` | `true` / `false` |
| `Str` | `"hello"` |
| Set | `{"#set": […]}` (the mirror also *accepts* bare arrays as sequences on input) |
| Sequence | bare JSON array `[…]` |
| Tuple | `{"#tup": […]}` |
| Record | JSON object `{"k1": …}` |
| Map (function) | `{"#map": [[k, v], …]}` |
| Variant | `{"tag": "…", "value": …}` |
| Unserializable | `{"#unserializable": "…"}` |
| Null | `null` |

Client-relevant subtleties:

- **Arbitrary precision is mandatory.** Integers must never round-trip through an IEEE double.
- **`#map` keys**: on decode the mirror parses each key as a value and stringifies it
  (`VInt 1` → `"1"`, `VStr s` → `s`). MirrorCPP decodes both plain-string and `#bigint` key
  forms and preserves keys as full `Value`s (like MirrorECMA); on encode it emits keys as encoded
  values — the mirror accepts both forms.
- **Set equality is unordered** in the mirror's diff (`length ==` + mutual membership). Client-side
  `Value` equality (used only in tests/preset matching) follows the same rule for sets.
- A bare JSON number decodes as an integer (it appears in hand-written fixtures).

### 3.5 Conformance semantics (`diffState`)

The mirror compares the client's reported state against the trace/explorer state with these rules
(`Engine/Core.hs`):

- Expected state = the step's `stateVars` **plus** `action_taken` (re-inserted); `paramVars` are
  **not** part of the expected state.
- **Meta keys are excluded from the diff on both sides**: any key starting with `#`, plus
  `action_taken` and `parameters`.
- Therefore a MirrorCPP `report_state` **must**:
  1. contain **every state variable** of the spec (missing keys → `missing` hints),
  2. **omit** every variable listed in `apalacheConfig.paramVars` (they were moved to step
     `parameters`; reporting them yields `extra` hints),
  3. include `action_taken` when the spec defines it — the mirror derives action names from it in
     the explore flows even though the diff itself skips it.

### 3.6 Discovery and mTLS contract

Full procedure (ModelMirrors `docs/protocol-spec.md`, "Discovery and mTLS (Client Guide)"):

1. **Discover**: `GET <registry>/v1/health/service/modelmirrors?passing=true` over plain HTTP.
   Parse only the `Service` object of each entry. Skip entries with empty `Address` or
   missing/zero `Port`. `Service.Meta["cert-sha256"]` is optional. **Any registry error
   (unreachable, non-200, malformed JSON) means "no servers"** — fail closed.
2. **Handshake**: TLS 1.3 only; verify the server chain against the pinned CA; verify the SAN
   against the host (IP literals checked as IP SANs); present the client certificate.
3. **Pin**: if the entry advertised `cert-sha256`, compute SHA-256 over the peer **leaf**
   certificate's **DER** bytes, render lowercase hex, compare; mismatch → close and try next entry.
   (Defense in depth — the handshake already authenticates.)
4. **Session**: identical to stdio/TCP; the first message must be `Register*`.

Client-side hygiene mirrored from the Haskell/TS clients: the **client private key file must be
mode `0600`** on POSIX (`mode & 0o077 != 0` → refuse to connect); skipped on Windows.

## 4. Architecture

### 4.1 Layering

```
┌──────────────────────────────────────────────────────────────┐
│ examples/ (hourclock)        mirrorcpp-validate (CLI, opt.)  │
├──────────────────────────────────────────────────────────────┤
│ mirrorcpp/client.hpp      ← run_client* flows, ExploreSession│
│ mirrorcpp/spec.hpp        ← EXTENDS/INSTANCE closure         │
│ mirrorcpp/registry.hpp    ← Consul discovery + connect       │
├──────────────────────────────────────────────────────────────┤
│ mirrorcpp/protocol.hpp    ← message types, encode/decode,    │
│                             diff hints, state machine guard  │
│ mirrorcpp/value.hpp       ← Value variant, ITF codec         │
├──────────────────────────────────────────────────────────────┤
│ mirrorcpp/transport.hpp   ← Transport iface + Stdio/Tcp/Tls  │
│ mirrorcpp/error.hpp       ← Error, Result<T>                 │
├────────────────────────────── internal ──────────────────────┤
│ detail/net.hpp     blocking socket wrapper (POSIX/Winsock)   │
│ detail/process.hpp subprocess spawn + pipes                  │
│ detail/tls.hpp     OpenSSL setup, handshake, pin check       │
│ detail/http.hpp    minimal HTTP/1.1 GET (registry only)      │
├──────────────────────────────────────────────────────────────┤
│ deps: nlohmann_json · Boost.Multiprecision · OpenSSL 3       │
└──────────────────────────────────────────────────────────────┘
```

Dependency direction is strictly downward: `value`/`error` know nothing about transports;
`protocol` knows `value`; `transport` knows neither (it moves *lines*, like MirrorECMA's
`Transport`); `client` ties them together. This mirrors MirrorECMA's split
(`protocol.ts`/`transport.ts`/`client.ts`/`spec.ts`/`registry.ts`) and keeps the session
protocol testable without any I/O.

### 4.2 Naming and language baseline

- **C++23** (`std::expected`, `std::string_view` ergonomics, ranges). Compilers: GCC 13+,
  Clang 17+, MSVC 19.38+.
- Namespace `mirrorcpp`. Types `PascalCase`, functions `snake_case` — e.g. `run_client`,
  `spec_from_files`, `discover_mirrors`, matching the sibling clients' API surface
  (`runClient`, `specFromFiles`, `discoverMirrors`).
- Umbrella header `mirrorcpp/mirrorcpp.hpp` re-exports the public API.

### 4.3 Error model

Public functions return `Result<T> = std::expected<T, Error>`; **no exceptions cross the API
boundary**. Internally, exceptions from nlohmann-json or Boost may propagate but are caught and
converted at module boundaries.

```cpp
enum class ErrorKind {
  io,            // socket/pipe/read-write failure, unexpected EOF
  spawn,         // child process failed to start / exited abnormally (stdio transport)
  json,          // malformed JSON on the wire
  protocol,      // unexpected message for the current phase; mirror-sent protocol_error
  registration,  // mirror-sent register_error
  spec_invalid,  // spec_validated with {"invalid": …}
  step_mismatch, // conformance failure; carries expected/actual/hints
  tls,           // handshake/verification/pin failure
  registry,      // discovery/connect-from-registry exhausted candidates
  spec_source,   // spec_from_files: missing/ambiguous module
};

struct DiffHint; struct State;   // from protocol.hpp / value.hpp

struct Error {
  ErrorKind kind;
  std::string message;                    // human-readable, includes mirror text when present
  // Populated only when kind == step_mismatch:
  std::optional<State> expected;
  std::optional<State> actual;
  std::vector<DiffHint> hints;
};

template <class T> using Result = std::expected<T, Error>;
```

A `step_mismatch` error message renders hints via `render_diff_hints` and names the last action,
exactly like MirrorECMA's `mainLoop`.

### 4.4 Threading and lifecycle

- Everything is **blocking and single-threaded**. One `Transport` = one session; transports and
  sessions are **not thread-safe**; concurrent checking = multiple threads each owning their own
  transport/session.
- The stdio transport owns the child process: `close()` closes stdin, waits, and reaps the exit
  code. Dropping the transport without closing terminates the child (best effort) so Apalache JVMs
  never leak.
- RAII throughout: sockets, `SSL*`, pipe handles, and child handles live in RAII wrappers; a
  failed session always leaves the OS resources reclaimed.

## 5. Module designs

### 5.1 `mirrorcpp/value.hpp` — Value and the ITF codec

```cpp
namespace mirrorcpp {

class Value {
public:
  using Int  = boost::multiprecision::cpp_int;
  struct Null   {};
  struct Set    { std::vector<Value> elems; };                    // unordered equality
  struct Seq    { std::vector<Value> elems; };                    // ordered
  struct Tuple  { std::vector<Value> elems; };
  struct Record { std::map<std::string, Value> fields; };
  struct Map    { std::vector<std::pair<Value, Value>> entries; };
  struct Variant   { std::string tag; Box<Value> value; };
  struct Unserializable { std::string text; };

  using Storage = std::variant<Null, Int, bool, std::string,
                               Box<Set>, Box<Seq>, Box<Tuple>,
                               Box<Record>, Box<Map>, Box<Variant>, Unserializable>;
  // … constructors, accessors (is<T>/get<T>), operator==
};

using State = std::map<std::string, Value>;

// ITF codec (nlohmann::json as the DOM):
nlohmann::json encode_value(const Value&);          // §3.4 rules
Value          decode_value(const nlohmann::json&); // throws JsonError internally
nlohmann::json encode_state(const State&);
State          decode_state(const nlohmann::json&);
```

Design notes:

- **Recursion.** `std::variant` alternatives must be complete types, and `std::map`/`std::vector`
  of an incomplete `Value` are not portable before C++26. Recursive alternatives are wrapped in
  `Box<T>` — a 30-line deep-copying `std::unique_ptr` wrapper (copy ctor/assign clone the pointee)
  so `Value` stays a normal copyable value type with value semantics.
- **Equality.** Structural, except `Set` (length + mutual membership), matching the mirror's
  `Eq Value`. Used only client-side (tests, `preset_client`); the mirror is always the
  conformance authority.
- **Decode rules** (mirror of `Apalache/Types.hs` / MirrorECMA `walk`): `#bigint` (""→null,
  optional `-`, digits — anything else is a parse error), `#tup`, `#set`, `#map` (both key
  forms), `#unserializable`, two-key `{tag,value}` → variant, other objects → record, arrays →
  seq, numbers → int, `null` → null.
- **Encode** always wraps ints as `{"#bigint": to_string(i)}` — never as JSON numbers.
- Convenience accessors: `as_int` (→ `optional<Int>`), `as_str`, `as_record`, and
  `get_param`/`get_param_int` for paramVars extraction — same roles as MirrorECMA's helpers.

### 5.2 `mirrorcpp/protocol.hpp` — messages, hints, phase guard

Message types are plain structs aggregated into two variants:

```cpp
struct ApalacheConfig {
  std::string spec_path;
  std::optional<std::string> init_predicate, next_predicate, const_init;
  std::string invariant;      // "" allowed
  long long length_bound = 10;
  std::string param_vars;     // "" allowed; comma-separated names
};
struct TraceGenerationConfig { long long num_traces = 1; std::optional<std::string> view; };
struct ApalacheSpec { std::vector<std::string> sources; };  // sources[0] = root module

struct Register            { ApalacheConfig cfg; TraceGenerationConfig tc; std::optional<ApalacheSpec> spec; };
struct RegisterTraces      { ApalacheConfig cfg; std::vector<std::string> itf_trace_paths; };
struct RegisterTraceGen    { ApalacheConfig cfg; TraceGenerationConfig tc; std::optional<std::string> dest_path; std::optional<ApalacheSpec> spec; };
struct RegisterExplore     { ApalacheSpec spec; std::vector<std::string> invariants; std::vector<std::string> exports; long long max_steps = 10; };
struct RegisterExploreSession { ApalacheSpec spec; std::vector<std::string> invariants; std::vector<std::string> exports; };
struct RegisterValidate    { ApalacheConfig cfg; long long bound; std::optional<ApalacheSpec> spec; };
struct ExploreAssumeTransition { long long transition_id; };
struct ExploreNextStep {};  struct ExploreQueryState {};  struct ExploreDone {};
struct ExploreCheckInvariant { long long invariant_id; };
struct ExploreAssumeState { State state; };
struct ExploreRollback { long long snapshot_id; };
struct ReportState { State state; };
using ClientMessage = std::variant<Register, …, ReportState>;

struct SpecValidated { std::variant<std::monostate, std::string> result; }; // valid | invalid(text)
struct InitialState  { std::string action; State state; };
struct NextStep      { std::string action; State parameters; };
struct StepOk {};     struct AllStepsDone {};  struct ExploreSessionDone {};
struct StepMismatch  { State expected; State actual; std::vector<DiffHint> hints; };
struct GenTracesDone { std::vector<std::string> itf_trace_paths; std::vector<nlohmann::json> itf_traces; };
struct RegisterError { std::string error; };
struct ProtocolError { std::string error; };
struct ExplorerReady { long long init_transitions, next_transitions, state_invariants; };
struct ExploreTransitionStatus { TransitionStatus status; }; // ENABLED/DISABLED/UNKNOWN
struct ExploreStepDone  { long long step_no; };
struct ExploreState     { State state; };
struct ExploreInvariantStatus { InvariantStatus status; };   // SATISFIED/VIOLATED/UNKNOWN
struct ExploreAssumeStatus    { TransitionStatus status; };
struct ExploreRollbackDone    { long long snapshot_id; };
using MirrorMessage = std::variant<SpecValidated, …, ExploreSessionDone>;

std::string        encode_client_message(const ClientMessage&);  // one line, no trailing 

Result<MirrorMessage> decode_mirror_message(std::string_view line);

// Diff hints (kinds: value_mismatch, missing, extra, missing_elem, extra_elem,
// type_mismatch, truncated), path segments {field} | {index}:
std::string render_path(const std::vector<PathSeg>&);
std::string render_diff_hint(const DiffHint&);
std::string render_diff_hints(const std::vector<DiffHint>&);
```

Serialization rules that must hold (all verified against `Format/Json.hs`):

- Unknown `proto_step` from the mirror decodes as `ProtocolError{"unknown proto_step: …"}` —
  forward-compatible with mirror additions (same as MirrorECMA).
- `spec_validated.result`: string `"valid"` vs object `{"invalid": …}`.
- `step_mismatch.hints` and `gen_traces_done.itfTraces` are optional on the wire.
- `register` with an inline spec still sends `apalacheConfig.specPath` (placeholder).
- `encode_state`, not `encode_client_message`, must serialize states — state values are already
  in tagged ITF form (the double-wrap bug MirrorECMA's AGENTS.md warns about). In C++ this is
  structural: `ReportState{state}` delegates to `encode_state`.

**Phase guard.** A small client-side state machine keeps flows honest and produces precise errors
("expected spec_validated, got step_ok"):

```
enum class Phase { idle, validating, ready, stepping, exploring, done };
idle --Register*--> validating --spec_validated(valid)--> ready
ready --initial_state--> stepping ⇄ (next_step/report_state/step_ok)
stepping --all_steps_done | step_mismatch--> done
idle --register_explore_session--> [explorer_ready]--> exploring
exploring --explore_done--> done
```

### 5.3 `mirrorcpp/transport.hpp` — transports

```cpp
class Transport {
public:
  virtual ~Transport() = default;
  virtual Result<void>        send_line(std::string_view line) = 0; // appends '
', flushes
  virtual Result<std::string> recv_line() = 0;                      // one line, no '
'
  virtual Result<long>        close() = 0;                          // exit code (stdio) or 0
};

struct SpawnOptions   { /* env, cwd — future */ };
struct ConnectOptions { std::chrono::milliseconds connect_timeout{10s}; };
struct TlsOptions {
  std::filesystem::path ca_path, cert_path, key_path; // PEM; key 0600 on POSIX
  std::optional<std::string> pin;                     // 64 lowercase hex chars, normalized
  std::optional<std::string> servername;              // SNI/SAN name; defaults to host
  std::chrono::milliseconds handshake_timeout{10s};
};

std::unique_ptr<Transport> spawn_mirror(const std::filesystem::path& mirror_bin,
                                        SpawnOptions = {});
Result<std::unique_ptr<Transport>> connect_tcp(std::string_view host, uint16_t port,
                                               ConnectOptions = {});
class TlsTransport;  // Transport + peer_fingerprint() (lowercase hex SHA-256 of leaf DER)
Result<std::unique_ptr<TlsTransport>> connect_tls(std::string_view host, uint16_t port,
                                                  const TlsOptions&);
```

Implementation notes:

- **`detail/net.hpp`** — a ~250-line blocking-socket RAII wrapper over POSIX sockets / Winsock2:
  `connect` (with timeout via non-blocking + `poll`/`WSAPoll`), buffered `read_line`,
  `write_all`, `close`. Reused by TCP, TLS (via OpenSSL `SSL_set_fd`), and the registry HTTP
  client. No Asio dependency (see §12).
- **`detail/process.hpp`** — subprocess spawn: POSIX `posix_spawn` with `pipe` + `dup2`
  (stdin/stdout piped, **stderr inherited**); Windows `CreateProcess` with inheritable anonymous
  pipe handles. `close()` closes stdin, then `waitpid`/`WaitForSingleObject`, returning the
  exit code.
- **`detail/tls.hpp`** — OpenSSL 3:
  - `SSL_CTX_set_min_proto_version`/`max` to `TLS1_3_VERSION` — 1.3 only.
  - `SSL_CTX_load_verify_locations(ca)`; `SSL_CTX_use_certificate_chain_file(cert)`;
    `SSL_CTX_use_PrivateKey_file(key)`; `SSL_CTX_check_private_key`.
  - POSIX: `stat(key_path)` and refuse when `mode & 0777 & 0o077 != 0` (mirrors
    `Protocol/Transport/Tls.hs`); skipped on Windows.
  - Hostname verification: `SSL_set1_host` for DNS names, `SSL_set1_ip_asc` for IP literals
    (registry entries are usually IPs); `SSL_VERIFY_PEER`.
  - Handshake with deadline; afterwards compute the pin:
    `SSL_get0_peer_certificate` → `i2d_X509` (DER) → `EVP_sha256` → lowercase hex; compare with
    the normalized pin (trim + lowercase + validate `^[0-9a-f]{64}$`).
  - After connect, the same line framing attaches (the mirror sends nothing until `Register*`).
- **Framing invariants**: `send_line` rejects inputs containing `\n` (defensive; the JSON
  encoder never produces them); `recv_line` has **no default timeout** (Apalache latency is
  legitimate); a clean peer EOF mid-session is an `io` error ("transport closed unexpectedly").

### 5.4 `mirrorcpp/registry.hpp` — discovery

```cpp
struct MirrorServiceInfo {
  std::string id, host;
  uint16_t port;
  std::optional<std::string> cert_sha256;  // normalized lowercase
};
struct RegistryOptions { std::chrono::milliseconds timeout{5s}; };

Result<std::vector<MirrorServiceInfo>> discover_mirrors(std::string_view registry_url,
                                                        RegistryOptions = {});
Result<std::unique_ptr<TlsTransport>> connect_from_registry(std::string_view registry_url,
                                                            const TlsOptions&,
                          /* pin override (wins over Meta) */ std::optional<std::string> pin = {},
                                                            RegistryOptions = {});
```

- **`detail/http.hpp`** — a deliberately minimal HTTP/1.1 client over `detail/net`: plain
  `http://` only, sends `GET <path> HTTP/1.1` + `Host` + `Accept: application/json` +
  `Connection: close`, requires `200`, reads `Content-Length` or EOF-terminated bodies.
  (Chunked encoding is a documented limitation; Consul answers this endpoint with a length.)
- **Parsing** is a total function ported from MirrorECMA `parseServiceEntry`: malformed entries
  are skipped, never thrown; `Address` trimmed non-empty; `Port` integer in 1–65535; a
  **malformed present** `cert-sha256` skips the entry (no unpinned fallback); absent pin allowed.
- **Fail closed**: any network/HTTP/JSON error → empty list. `connect_from_registry` tries
  candidates in registry order with `pin = override ?? entry.cert_sha256`, collecting per-candidate
  diagnostics; empty list → "no mirror candidates discovered"; exhausted list → one `registry`
  error with all diagnostics.

### 5.5 `mirrorcpp/spec.hpp` — EXTENDS/INSTANCE closure

```cpp
Result<ApalacheSpec> spec_from_files(const std::filesystem::path& root,
                                     std::vector<std::filesystem::path> search_dirs =
                                         default_search_dirs());  // $TLA_LIBRARY_PATH, ':'-separated
```

Direct port of MirrorECMA `spec.ts`:

- Parse `EXTENDS`/`INSTANCE` clauses (first token per comma-separated part, so
  `INSTANCE X WITH …` yields `X`).
- Skip builtins: `Naturals Integers Reals Sequences FiniteSets TLC Bags Apalache`.
- Resolve `<Name>.tla` in the importing file's directory, then `search_dirs`.
- **Ambiguity is an error** (same module name found in two directories), as is a **missing** module.
- Diamonds deduplicated by canonical path; result is `{sources: [root, …deps]}`, root first.

### 5.6 `mirrorcpp/client.hpp` — flows and sessions

```cpp
using StateComputer = std::function<State(std::string_view action,
                                          const State& params_or_state,
                                          const State& prev_state)>;

// Stepping flows (block until all_steps_done / mismatch / error):
Result<void> run_client(Transport&, const ApalacheConfig&, const TraceGenerationConfig&,
                        StateComputer, std::optional<ApalacheSpec> inline_spec = std::nullopt);
Result<void> run_client_with_traces(Transport&, const ApalacheConfig&,
                                    const std::vector<std::string>& itf_trace_paths, StateComputer);
Result<void> run_client_explore(Transport&, const ApalacheSpec&,
                                std::vector<std::string> invariants,
                                std::vector<std::string> exports, long long max_steps,
                                StateComputer);

// One-shot flows:
struct GenTracesResult { std::vector<std::string> itf_trace_paths;
                         std::vector<nlohmann::json> itf_traces; };  // inline contents
Result<GenTracesResult> run_client_gen_traces(Transport&, const ApalacheConfig&,
                                              const TraceGenerationConfig&,
                                              std::optional<std::string> dest_path = std::nullopt,
                                              std::optional<ApalacheSpec> = std::nullopt);
struct ValidateVerdict { bool valid; std::string detail; };  // detail = apalache output when invalid
Result<ValidateVerdict> run_client_validate(Transport&, const ApalacheConfig&, long long bound,
                                            std::optional<ApalacheSpec> = std::nullopt);

// Asynchronous job interface (guide §6; server-mode transports only — checked via
// Transport::async_capable(), stdio submits fail fast with Error{protocol}).
// jobIds are plain strings: cross-connection visible (C17). await_job is long-polling:
// timeout → JobStatus (non-terminal, never an error); terminal JobResults are idempotent
// until eviction (C18). JobPhase::unknown = never submitted/evicted — never retry-loop
// on it (C21). A full job queue rejects at submit with register_error →
// Error{registration} (C22). The validate outcome reuses SpecValidated — identical to
// the sync verdict for the same config (C20). The borrowed transport is NEVER closed
// by these calls.
Result<JobAccepted> submit_validate_async(Transport&, const ApalacheConfig&, long long bound,
                                          std::optional<ApalacheSpec> = std::nullopt);
Result<JobAccepted> submit_trace_gen_async(Transport&, const ApalacheConfig&,
                                           const TraceGenerationConfig&,
                                           std::optional<std::string> dest_path = std::nullopt,
                                           std::optional<ApalacheSpec> = std::nullopt);
Result<JobStatus>   query_job(Transport&, std::string_view job_id);
using AwaitResult = std::variant<JobStatus, JobResult>;
Result<AwaitResult> await_job(Transport&, std::string_view job_id,
                              std::optional<long long> timeout_secs = std::nullopt);
Result<AwaitResult> cancel_job(Transport&, std::string_view job_id);

// Client-driven symbolic sessions:
class ExploreSession {
public:
  struct Ready { long long init_transitions, next_transitions, state_invariants; };
  static Result<ExploreSession> open(std::unique_ptr<Transport>, const ApalacheSpec&,
                                     std::vector<std::string> invariants,
                                     std::vector<std::string> exports);
  const Ready& ready() const;
  Result<TransitionStatus> assume_transition(long long transition_id);
  Result<long long>        next_step();
  Result<State>            query_state();
  Result<InvariantStatus>  check_invariant(long long invariant_id);
  Result<TransitionStatus> assume_state(const State&);
  Result<long long>        rollback(long long snapshot_id);
  Result<void>             done();     // explore_done → explore_session_done, then close
  ~ExploreSession();                   // best-effort done() if still open
private:
  Result<MirrorMessage> command(const ClientMessage&);  // send + recv; protocol_error → close
};

// Test helper: report a fixed sequence of states, then fail ("preset_client exhausted").
StateComputer preset_client(std::vector<State> states);
```

Behavioral contract of the stepping loop (identical to MirrorECMA `mainLoop`):

1. Await `spec_validated`; `invalid` → `spec_invalid` error; `register_error`/`protocol_error`
   propagate; anything else → `protocol` error naming the offending tag.
2. `initial_state`: call `compute(action, state, {})`; `next_step`: `compute(action, parameters,
   prev)` — remembering that in `register_explore` `parameters` is the full expected state.
3. Reply `report_state` with the returned state (client code is responsible for the
   include/omit rules of §3.5; the library documents but cannot enforce them).
4. `step_mismatch` → `step_mismatch` error with expected/actual/hints and the last action name in
   the message; `all_steps_done` → success, then `close()`.
5. Every terminal path closes the transport (and thus reaps the stdio child).

### 5.7 `mirrorcpp-validate` CLI (optional component)

A CI-friendly port of `ModelMirrors validate`:

```
mirrorcpp-validate --host H --port P [--registry URL] --spec S.tla [--dep D.tla]...
                   [--bound N] [--inv I] [--init P] [--next P] [--cinit C]
                   [--tls --cert C --key K --ca CA] [--pin SHA256]
```

Exit codes: `0` valid (`VALID` on stdout), `1` invalid (`INVALID` + apalache output),
`2` infrastructure error (stderr). `--registry` implies mTLS discovery and is mutually exclusive
with `--host`/`--port`; spec sources travel inline via `spec_from_files`; bounds outside
`[1, 100]` are rejected mirror-side (`register_error` → exit 2).

## 6. Dependencies and build

### Dependencies (all widely packaged)

| Dependency | Use | Notes |
|---|---|---|
| **nlohmann/json ≥ 3.11** | JSON DOM + serializer | `FetchContent` fallback to `find_package` |
| **Boost.Multiprecision** | `cpp_int` (arbitrary-precision ints) | Header-only; the only Boost component used |
| **OpenSSL ≥ 3.0** | TLS 1.3 transport, SHA-256 pins | Optional: `MIRRORCPP_WITH_TLS=OFF` builds without `TlsTransport`/registry-connect (registry *parsing* stays) |
| **Catch2 v3** (tests only) | Unit/integration tests | `FetchContent` |

### CMake

- CMake ≥ 3.24. Targets: `mirrorcpp` (static by default; `BUILD_SHARED_LIBS` respected),
  alias `mirrorcpp::mirrorcpp`; install rules + `mirrorcppConfig.cmake` package config so
  downstreams use `find_package(mirrorcpp CONFIG)`.
- Options: `MIRRORCPP_WITH_TLS=ON`, `MIRRORCPP_BUILD_TESTS=ON`, `MIRRORCPP_BUILD_EXAMPLES=ON`,
  `MIRRORCPP_BUILD_CLI=ON`.
- `-Wall -Wextra`; optional `MIRRORCPP_WERROR`. Sanitizer toggle for tests
  (`MIRRORCPP_SANITIZE=address,undefined`).
- Platforms: Linux (primary), macOS, Windows (Winsock2 + `CreateProcess`; POSIX-only key-mode
  check compiled out).

## 7. Project layout

```
MirrorCPP/
├── CMakeLists.txt
├── cmake/mirrorcppConfig.cmake.in
├── include/mirrorcpp/
│   ├── mirrorcpp.hpp        # umbrella
│   ├── value.hpp  protocol.hpp  transport.hpp  registry.hpp
│   ├── spec.hpp   client.hpp    error.hpp
├── src/
│   ├── value.cpp  protocol.cpp  client.cpp  registry.cpp  spec.cpp
│   └── detail/{net,process,tls,http}.{hpp,cpp}  (+ _posix/_win32 variants)
├── cli/validate.cpp
├── examples/hourclock.cpp
├── specs/                     # vendored from ModelMirrors/MirrorECMA
│   ├── HourClock.tla  Counter.tla  ExtMain.tla  ExtDep.tla
│   └── traces/*.itf.json
├── test/
│   ├── unit/{value,protocol,spec,registry}_test.cpp
│   └── integration/…          # driven by $MIRROR_BIN
├── docs/design.md             # this document
└── README.md
```

## 8. Testing strategy

### Unit tests (no mirror needed)

- **Value codec**: round-trip every constructor; `#bigint` edge cases (`""`→null, `"-5"`,
  huge > 2⁶⁴ values, malformed → parse error); `#map` with string and `#bigint` key forms;
  variant vs record disambiguation; bare number → int.
- **Protocol codec**: every message both directions; optional fields (`hints`, `itfTraces`,
  `spec`); unknown `proto_step` → `ProtocolError`; `spec_validated` both result shapes;
  hint rendering.
- **spec_from_files**: fixtures for diamond imports, `INSTANCE … WITH`, missing module,
  ambiguity across two search dirs, builtin filtering, `TLA_LIBRARY_PATH`.
- **Registry parsing**: the full fail-closed matrix from §5.4 (ported cases from MirrorECMA's
  `registry.test.ts`).
- **TLS helpers**: pin normalization/validation; SHA-256 hex; key-mode acceptance/rejection
  (POSIX).
- **Framing**: send rejects embedded newline; recv_line buffering across partial reads.

### Integration tests (require `MIRROR_BIN`)

Skipped with a message when `MIRROR_BIN` is unset; a standalone script (like MirrorECMA's smoke
test) hard-fails instead. Scenarios, each over **stdio and TCP** (and **mTLS + registry stub** where
applicable):

1. HourClock happy path: `run_client_with_traces` over vendored `specs/traces/*.itf.json` →
   `all_steps_done`.
2. Deliberate mismatch: report a wrong state; assert `step_mismatch` error carries hints.
3. `register` end-to-end (mirror generates traces itself) with the vendored `HourClock.tla`.
4. `register_trace_gen` → `gen_traces_done` with inline `itfTraces`.
5. `register_validate` valid + invalid specs; bound `101` → `register_error`.
6. `register_explore` and a scripted `ExploreSession` (assume/next/query/checkInvariant/
   rollback/done), including a deliberate bad command → fatal `protocol_error` → session closed.
7. mTLS: certs generated at test time with OpenSSL (or ModelMirrors `scripts/gen-certs.sh`
   equivalent); pin match and pin mismatch; wrong-CA client cert → handshake failure.
8. Registry: tiny stub HTTP server in the test harness serving the Consul health response;
   candidate iteration with a dead first candidate; fail-closed on malformed JSON.
9. Inline multi-module spec via `spec_from_files(ExtMain.tla)`.

### Conformance north star (later milestone)

ModelMirrors self-verifies by model-based testing: `specs/MirrorProtocol.tla` generates protocol
traces that are replayed against the real mirror. MirrorCPP can adopt the same idea — drive
MirrorCPP through spec-generated message sequences (vendored ITF fixtures under
`specs/traces/` already cover `all_steps_done`, `step_mismatch`, `register_error`,
`explore_session`, `explore_cmd`, `fault_close`) and assert the emitted client messages match
the trace — giving TLA+-level confidence in the client implementation itself.

### Golden wire corpus (implemented)

`test/fixtures/golden/` vendors the frozen Haskell wire corpus (`client_messages.jsonl`,
`mirror_messages.jsonl`, `decode_only.jsonl`, `consul_payloads.jsonl`, `diff_cases.jsonl` +
`manifest.json`/`metadata.json`, Haskell `ModelMirrors@3496251`; provenance and refresh
procedure in `test/fixtures/golden/README.md`). `mirrorcpp_golden_corpus_test` replays it
direction-aware: mirror messages decode-side against the manifest constructors, client
messages encode-side with absent≡null normalization for optional keys (C14), `decode_only`
as value-codec vectors (incl. bare integral number → `#bigint` acceptance), `consul_payloads`
`health` lines through the registry parser, and all 500 `diff_cases` lines as decode/equality
vectors under `filterMeta` semantics. Always on — no `MIRROR_BIN` needed.

## 9. Security considerations

- TLS 1.3 only; SAN hostname/IP verification always on; no "insecure" toggle in the public API.
- Fingerprint pinning is enforced whenever a pin is available (explicit override wins over
  registry-advertised pins).
- POSIX client key must be `0600`; refuse otherwise (same rule as the Haskell and TS clients).
- Registry is plain HTTP and **fail-closed**; it provides location only — authentication always
  happens in the mTLS handshake, so a compromised registry cannot impersonate a mirror.
- No certificate/key material is ever logged; error messages quote mirror text but not local paths
  of key files beyond what the caller supplied.
- The stdio child is always reaped; a destructed session never orphans an Apalache JVM.

## 10. Usage example (target API)

```cpp
#include <mirrorcpp/mirrorcpp.hpp>
using namespace mirrorcpp;

int main() {
  // stdio: spawn the mirror; TCP: connect_tcp("127.0.0.1", 8823);
  // mTLS+registry: connect_from_registry("http://127.0.0.1:8500", tls_opts);
  auto transport = spawn_mirror("/usr/local/bin/ModelMirrors");

  auto spec = spec_from_files("specs/HourClock.tla");
  if (!spec) { /* report spec.error() */ }

  ApalacheConfig cfg{ .spec_path = "ignored-when-spec-present",
                      .invariant = "TraceComplete", .length_bound = 13 };

  State hr_state;  // the C++ implementation under test
  auto result = run_client(*transport, cfg, {.num_traces = 1},
    [&](std::string_view action, const State& params, const State&) -> State {
      if (action == "init") hr_state = params;          // adopt the oracle's init
      else                  hr_state = hc_tick(hr_state); // real C++ transition
      return hr_state;                                   // every var, no paramVars
    },
    *spec);

  if (!result && result.error().kind == ErrorKind::step_mismatch) {
    // result.error().hints → render_diff_hints(...)
  }
}
```

## 11. Implementation milestones

1. **M1 — Core codec + stdio replay**: `error`, `value`, `protocol`, `detail/process`,
   stdio `Transport`, `run_client_with_traces`, HourClock integration test.
2. **M2 — TCP + one-shot flows**: `connect_tcp`, `register`, `register_trace_gen`,
   `register_validate`.
3. **M3 — mTLS + registry + inline specs**: `connect_tls` with pinning, `discover_mirrors`,
   `connect_from_registry`, `spec_from_files`, registry stub tests.
4. **M4 — Symbolic flows**: `run_client_explore`, `ExploreSession`.
5. **M5 — Polish**: `mirrorcpp-validate` CLI, packaging (install/config), sanitizer CI matrix,
   Windows port, MBT conformance fixtures.

Each milestone keeps the full test suite green against a real mirror binary.

## 12. Alternatives considered

| Decision | Chosen | Alternative (why not) |
|---|---|---|
| Async model | Blocking calls | Coroutines/io_uring/Asio async: the protocol strictly alternates; async adds machinery with zero throughput benefit. Callers thread externally. |
| Socket layer | Small internal wrapper (~250 LoC) | standalone Asio + `asio::ssl`: heavyweight dependency for three blocking use-cases; wrapper is reused by TCP/TLS/HTTP. Revisit if non-blocking transports are ever needed. |
| JSON | nlohmann/json | Boost.JSON (drags in more Boost), simdjson (no DOM mutation, overkill), glaze (younger ecosystem). |
| BigInt | Boost.Multiprecision `cpp_int` | Hand-rolled bigint (needless risk), GMP (non-header-only, licensing friction on Windows). |
| Errors | `std::expected` returns | Exceptions across the API (hostile to embedded/no-exceptions consumers), error-code out-params (clunky). `tl::expected` shim if C++20 support is ever required. |
| Build | CMake | Bazel (MirrorECMA uses it; can be added later — nothing in this design precludes it). |
| TLS library | OpenSSL 3 | Schannel/Secure Transport per-platform (triples the TLS test matrix; OpenSSL is ubiquitous and matches the mirror's own stack semantics). |
| Distribution | Compiled library | Header-only (forces OpenSSL/process code into every consumer's compile). |

## 13. Conformance checklist

| Requirement (ModelMirrors source) | MirrorCPP component | Test |
|---|---|---|
| Single-line JSON framing, no embedded newlines | `Transport::send_line`, `encode_client_message` | framing unit tests |
| First message must be `Register*` | client flows always register first | integration: all flows |
| `apalacheConfig` field set incl. always-sent `specPath` | `ApalacheConfig` + encoder | protocol unit tests |
| `spec.sources[0]` = root; inline materialization | `spec_from_files`, `ApalacheSpec` | ExtMain/ExtDep integration |
| ITF value encoding incl. `#bigint` arbitrary precision | `value.hpp` codec | value round-trip tests |
| `report_state`: all vars, omit paramVars, include `action_taken` | documented on `StateComputer`; helpers | HourClock + paramVars integration |
| `step_mismatch` hints surfaced | `Error::hints`, `render_diff_hints` | mismatch integration test |
| Validate bound cap 100 → `register_error` | `run_client_validate` passthrough | bound=101 integration test |
| Async jobs: submit/query/await/cancel, C17–C22 semantics | `submit_*_async`, `query_job`, `await_job`, `cancel_job` | `mirrorcpp_async_jobs_test` (fake TCP mirror) + real-mirror `--serve` scenario |
| Golden wire corpus replay (guide §10.1) | codecs vs `test/fixtures/golden/` | `mirrorcpp_golden_corpus_test` |
| `register_explore.maxSteps` always sent (Lean mirror `reqNat`) | `run_client_explore` | real-mirror explore scenario |
| `gen_traces_done.itfTraces` optional | `GenTracesResult` | trace-gen integration test |
| Explore strict alternation; fatal `protocol_error` | `ExploreSession::command` | explore integration tests |
| TLS 1.3 only, SAN verify, client cert | `detail/tls.hpp` | TLS integration tests |
| Pin = SHA-256(leaf DER) lowercase hex; override wins | `connect_tls`, `connect_from_registry` | pin match/mismatch tests |
| Client key mode 0600 (POSIX) | `detail/tls.hpp` | key-mode unit test |
| Registry endpoint, fail-closed, entry validation | `registry.hpp` + `detail/http` | registry unit + stub tests |
| No greeting; mirror silent until `Register*` | transport attach order | TLS integration test |
| stdio child stderr inherited, always reaped | `detail/process.hpp` | spawn/exit-code tests |

---

## Appendix A — Reference sources

- ModelMirrors README — flows, transports, process model, secure server mode, remote validation.
- `docs/protocol-spec.md` — framing, discovery + mTLS client guide, value encoding, state machine.
- `src/Protocol/Format/Json.hs` — **authoritative wire format** (all message fields).
- `src/Protocol/Core.hs` — canonical message/type inventory.
- `src/Apalache/Types.hs` — `ApalacheConfig`, ITF `Value` semantics and equality.
- `src/Engine/Core.hs` — `diffState`, meta-key exclusion, hint caps (50, depth 8).
- `src/Protocol/Client.hs` — reference client loop (`stepLoop`/`handleStep`).
- `src-tls/Protocol/Transport/Tls.hs` — key-mode checks, TLS setup.
- `src/Protocol/Registry.hs` — Consul endpoint and metadata (`cert-sha256`).
- MirrorECMA `src/{protocol,transport,client,spec,registry}.ts` — sibling client architecture and
  battle-tested edge-case handling (pin normalization, fail-closed parsing, double-wrap pitfall).
