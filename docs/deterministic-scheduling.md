# Cooperative checkpoint scheduling (experimental)

The `mirrorcpp::schedule` module and its binding/exploration layers implement
DPM-0–DPM-5 for `mirrorcpp.cooperative-checkpoints/v1`. Real worker-stack
checkpoint control, generated model comparison, bounded finite exploration and
installed consumers are accepted; the application-specific four-case native
WriteSentry pilot retains its own exact scope. See
[framework scheduling](../../Mirrors/Docs/deterministic-scheduling.md) and
[retained acceptance](../../Mirrors/Plans/dpm2-dpm5-qualified-20261004/README.md).
A successful local schedule alone is not model conformance or unrestricted
concurrent correctness; the later sections describe comparison and cleanup.

## Interface

Include `<mirrorcpp/schedule.hpp>` (also available in the umbrella header).
Construct a `Schedule`, an `Adapter`, and optionally a `Policy` and stop token;
call `run_schedule`. Keep the returned `Execution` alive until its actors are
joined. Use `report()` for typed outcomes and `receipt()` for JSON evidence.
`parse_schedule` / `encode_schedule` preserve the versioned replay artifact.

The adapter declares unique logical actor names, one operation instance per
actor, and checkpoint names. Its factory receives the recorded JSON inputs and
returns `Program {workers, observe, teardown}`. An explicit teardown callback is
required, even when it has no application resources to release. Each worker receives its own
`Checkpoint&` and calls `arrive("name")` at safe cooperative pause points.
The hook belongs to that worker thread; calling from another thread fails the
execution. Background actors that never call a hook cannot be detected by this
interface and are outside the profile. Actors must capture owning state rather
than references to expired stack objects.

All declarations, schedule steps, completion order, profile, identity labels and
bounds are checked before the factory runs. The factory is exception-safe and
must not start its own threads. The coordinator starts exactly the declared
workers, parks them before application code, and waits for the initial barrier.
A step grants one actor a permit and names its expected next checkpoint. `$done`
means the worker function must return. Every actor requires exactly one terminal
step, with no later steps for that actor. Repeated checkpoint names are allowed
within an operation; their order is determined by the recorded steps.

The worker's real stack stays alive across checkpoints. A selected actor runs
until its next hook or completion; other actors remain parked. The controller
checks the actual arrival before invoking `observe`. The observer runs while all
declared actors are parked or finished; mutex synchronization makes preceding
worker writes visible. It must read the actual SUT and must not wait on workers
or locks held by parked workers. Direct observer hook reentry is rejected.

The serialized schedule contains `schema`, `profile`, `identity`, `inputs` and
`steps`. The reader rejects duplicate/unknown fields, nesting beyond 32,
artifacts over 1 MiB and malformed identifiers. Binary/discarded JSON values,
non-finite numbers and excessive input/observation nesting are rejected rather
than silently changed by serialization. Model semantic, mapping and
instrumented implementation identities are lowercase 64-hex strings and must
match the trusted adapter's labels. The coordinator does not inspect model or
executable files to attest those labels. An acceptance harness must retain and
verify actual artifacts separately. No expected model state is supplied to the
worker or observer by this module.

## Outcomes, cancellation and ownership

`Report::passed()` requires a completed schedule, no primary failure, all started
actors joined and successful teardown. Outcomes distinguish invalid schedules,
identity mismatch, unexpected checkpoint, uncontrolled hook caller, cancellation,
timeout, application/observer failure and thread creation failure. The receipt
preserves permits, actual arrivals, observations and cleanup attempts. A timed-out
wait is not fabricated as a checkpoint arrival or described as deadlock.

A stop token interrupts coordinator waits. On failure/cancellation, parked actors
unwind from their hooks; do not catch and suppress the hook's stop exception.
Teardown is called once after all actors are joined. Its failure remains separate
from the primary outcome. The initial factory must clean up its own partial
construction if it throws; `not_started` means no program was returned to the
coordinator, not a global resource guarantee.

The execution timeout is one steady-clock deadline for the run. Cleanup has a
separate wait budget. If an actor ignores cancellation, `cleanup == incomplete`
and `remaining_actors` name the still-owned threads. Retain the handle, unblock
application-owned waits, then call `cleanup(timeout)` to retry. Earlier attempts
and the primary failure remain visible. There is **no detach or forced thread
termination**. The handle destructor cancels and joins; it can block forever on
uncooperative code. Factory, observer, teardown and thread/TLS destructors are
trusted prompt-returning callbacks; budgets do not preempt them. Use a separate
process owner for hard termination. No new process supervisor is supplied here.

Only the owning controller calls `cleanup`. Callbacks cannot recursively start
another schedule. Each execution gets a random execution identifier and a
process-local generation; both are excluded from deterministic replay comparisons.
Inputs, schedule decisions, accepted events and observations must remain equal.

Policy bounds: at most 64 actors and 65,536 steps; configurable lower limits,
positive execution/cleanup budgets up to 24 hours, 128-character identifiers,
65,535-byte inputs/individual observations and 8 MiB aggregate observation data.
No implicit random, clock, OS-fault, external-thread or weak-memory control is
provided. The fixed-schedule API reports no exploration denominator; the separate finite exploration API below does.

## Portable fixture and checks

`examples/scheduled_counter.hpp` supplies two split increment operations. A local
read value remains on each real thread's stack between `read` and `write` hooks.
Serial execution produces two increments; overlapping reads produce one lost
update. The test oracle checks these actual observations; it does not replace
them with expected values. Identity strings in this fixture are explicit test
labels, with real source/executable hashes recorded by the acceptance runner.

```sh
cmake --build build --target mirrorcpp_schedule_test scheduled_counter
build/test/unit/mirrorcpp_schedule_test '[schedule]'
build/examples/scheduled_counter > receipt.json
# Extract receipt.schedule into schedule.json, then replay in a fresh process:
build/examples/scheduled_counter --replay schedule.json
python3 test/schedule/replay_check.py --binary build/examples/scheduled_counter \
  --out /tmp/mirrorcpp-dpm1-fresh-evidence
```

The test suite covers repeated schedules, actual fixture mutation observations,
strict decoding/admission, foreign hook callers, unexpected/early completion,
actor/observer failure, cancellation, bounded waits, retained incomplete cleanup,
retry and teardown failure. CTest imposes a separate outer test timeout. The
installed package smoke checks header/linking/thread-dependency use; it does not
advertise DPM-5 qualification.

## WriteSentry integration limit

WriteSentry's existing native phase worker uses preallocated POD and atomics on
trap paths. This module's mutex/condition-variable hook cannot run in VEH or
suspended-target regions. A native adapter must preserve that contract,
its current model correspondence and its own cancellation/resource ownership.
No WriteSentry source or existing native evidence is changed by DPM-1.

## Incremental generated bindings (DPM-2)

`start_schedule` admits the complete schedule and acquires a fresh application
instance. The controller then calls `Execution::advance(step)` for each exact
next interval and `finish()` for terminal cleanup. The execution deadline spans
callback gaps. Completion joins the selected worker before observing its final
state, including thread-local destructors.

`BindingSession` owns these handles across generated callbacks. Construction does
not acquire application state. `initialize()` acquires a fresh generation;
`advance()` grants one declared interval; `observation()` returns actual state;
`dispose()` retains and cleans up the final execution. A generated port maps typed
actions to these calls. The connection-local `LocalBinding` owns disposal and
keeps its generated port alive. Never derive implementation state from the oracle
state or the callback's previous model state.

`replay_with_traces` uses the existing negotiated client loop and retains the real
peer verdict separately from the client error and scheduler cleanup. A matched
comparison with failed teardown is not a successful replay. An empty peer verdict
without a completed execution cannot earn credit. Reusing a session rejects before
replay and closes the supplied transport. Session evidence is bounded to 64
executions and 16 MiB.

The framework fixture and acceptance runner are in Mirrors
`tools/deterministic-scheduling/`. They use generated `ScheduledCounter` codecs,
real implementation threads and the actual Mirrors trace-comparison process.
The checked-in trace is explicitly authored compiler/test evidence; local replay
does not establish a fresh Apalache capture.

## Finite exploration (DPM-4)

`schedule_exploration.hpp` exposes `FiniteSpace`, `ExplorationLimits`,
`local_exploration_runner`, `comparison_sample` and `explore_finite`. Each actor
has a fixed checkpoint chain ending in `$done`; all order-preserving merges are
enumerated canonically over an explicit finite input list. A preemption switches
away from an unfinished actor. Switching after `$done` does not count.

Enumeration, execution, time and evidence limits are explicit. Unknown totals
remain unknown, truncated campaigns remain incomplete, and unknown cleanup stops
further acquisition. Every execution must bind the exact candidate identity and
schedule and have a fresh execution identifier. Callback runners are trusted
prompt-returning code; the library cannot forcibly stop one that hangs.

Coverage projects only the declared base variables, with any omitted
instrumentation variables listed explicitly. Canonical typed states normalize
sets and map order while preserving sequence/tuple order and ordinary record
keys. Duplicate map keys and unsupported mixed/compound keys reject coverage.
This canonicalization does not change the existing protocol Value equality.

Local execution coverage and model comparison have separate counters. Requiring
model comparison rejects local-only samples. The current 20-schedule/two-input
fixture exercises 40 actual local executions; it does not prove all 40 conform
to the model or establish universal concurrent correctness. No partial-order
reduction is enabled.

## Installed scope and capability declaration (DPM-5)

The package installs `share/mirrorcpp/scheduling-capabilities.json`, a versioned
experimental API declaration. It is not a qualification certificate. Mirrors'
`tools/deterministic-scheduling/check_installed.py` records exact artifact hashes
and builds a fresh consumer with repositories hidden and networking disabled.
It runs real replay/mutation controls, finite exploration, incompatible
profile/mapping controls and missing/tampered-artifact verifier controls.
System compiler, C/C++ runtime and OpenSSL dependencies are admitted separately
from the installed SDK/header dependency prefix.

The WriteSentry bridge prepared in Mirrors preserves its native POD/atomic phase
handshake and schedules IPC proxy actors outside trap paths. It is application
specific. Synthetic transport controls do not qualify native execution. The
DPM-3 pilot and dependent DPM-5 installed claim are now accepted for the four
named stable/publication-overlap schedules. Fresh independent model captures,
eight repeated native comparisons, two production mutation comparisons and four
failure controls passed. Three incompatible mappings rejected before acquisition.
The installed Linux controller ran with source checkouts hidden and networking
isolated, using actual Windows workers through WSL's existing local carrier.
Each scheduled `MBTSafety` check returned VALID; this proves only those fixed
bounded model schedules. Complete evidence and exact exclusions are retained in
Mirrors `Plans/dpm2-dpm5-qualified-20261004/README.md`. This does not qualify a
Windows build of the entire MirrorCPP SDK, the whole WriteSentry product, a
Windows Gate backend or a new M5 release candidate.
