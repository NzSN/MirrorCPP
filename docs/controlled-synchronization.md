# Controlled synchronization prototype

`<mirrorcpp/schedule_sync.hpp>` supplies the separate
`mirrorcpp.controlled-sync-experimental/v1` profile. It implements a controlled
nonrecursive mutex and a bounded FIFO of signed 64-bit integers on real worker
threads. It preserves actual application stacks and data. The coordinator's
mutex/condition-variable synchronization makes completed worker intervals
visible to the observer; this is a participating-actor prototype, not an
unmodified-binary or weak-memory-complete scheduler.

## Interface and ownership

An `Adapter` declares exact actor IDs, execution-local resources, identity labels
and a deferred `Program` factory. `Program` provides workers, an actual-state
observer and teardown. Static bounds/identity/declaration admission happens
before factory acquisition. Hooks belong to their worker thread and execution.
Identity labels are caller declarations; an acceptance manifest must separately
verify source, executable and dependency hashes.

Call `start(adapter, policy)`, then query `enabled()` and choose `advance(actor)`.
Each advance grants that actor's pending operation and waits for its next
resource boundary or completion. Other actors remain parked. A worker calls
`lock`, `unlock`, `send`, `receive` or `checkpoint`; these operations keep real
stack/locals intact. `$done` and initial startup are explicit decisions.

`MutexGuard` acquires/relinquishes controlled ownership in the owning worker.
A holder can park at a controlled checkpoint; another actor's acquire becomes
blocked, and the holder stays enabled to continue/release. Ownership is never
silently dropped to repair a schedule. Wrong-owner release, recursive acquire,
kind mismatch, unknown resources and foreign hook callers fail the execution.

The FIFO enables send when capacity remains and receive when data exists. Each
granted operation changes the actual queue exactly once. A newly eligible
waiter still needs its own permit; waking does not transfer data. Internal
locks are released while parking. The observer must return actual quiescent
state and must not wait on parked actors or their application locks.

## Findings and cleanup

Fixed replay refuses a disabled actor choice. Exploration may choose only from
the observed enabled set. When every unfinished actor waits on declared
resources and none is enabled, the result is `controlled_deadlock`. The receipt
retains the original ownership/wait graph before cancellation. The coordinator's
timed boundary wait reports `timed_out` if a worker does not reach its next
scheduler boundary; it never infers a controlled deadlock from that timeout.

A `$done` interval joins the selected worker before recording its completion
observation. Thread-local destructors are part of actual exit and their state
changes must be visible to the observer, even when other actors have not yet
finished. This join consumes the execution budget. A late join records timeout
and no completion observation; cleanup still owns any remaining joins and
teardown. Cancellation joins use the separate cleanup budget.

`cleanup(timeout)` cancels parked workers, joins actual threads and invokes
teardown once. An incomplete attempt retains the execution for explicit retry;
the destructor never detaches and can wait indefinitely for uncooperative
application code. Hard process termination belongs to an external process
owner. Teardown failure remains separate from the primary result. Early
cancellation cannot be a successful completed execution.

One monotonic execution deadline covers factory acquisition, the initial barrier,
all worker intervals and every observer, including the final observer. Checks
before and after returning callbacks prevent late work from establishing a pass.
A factory that returns late starts no workers; its acquired program remains
owned for teardown. Exceptions keep their primary attribution even if that
callback also exceeded its budget. Execution timing ends after the final timely
observation; cleanup then has its own budget.

Each `cleanup(timeout)` attempt has one absolute deadline across waiting, actual
joins (including thread-local destruction), and teardown. If time expires before
the next cleanup step, `cleanup: "incomplete"` retains ownership for explicit
retry. A join or teardown already in progress is not interrupted. When teardown
returns late, `cleanup: "confirmed"` still describes physical completion, while
sticky `budgetExceeded.execution` / `budgetExceeded.cleanup` facts prevent a
pass. A prior primary finding and its failure graph are preserved; otherwise a
budget overrun yields `timed_out`. Retries never erase budget excess or repeat
an attempted teardown, including teardown that threw or returned late.

These are returning-callback deadline checks, not hard in-process interruption.
A non-returning factory, observer, join or teardown can block its caller. No
callback thread is detached; an external process owner supplies hard termination.

Receipts use `mirrors.controlled-sync-replay/v1` and explicitly state
`modelComparison: false`. `passed` means complete controlled execution with
confirmed cleanup; it does not establish model conformance. Actual comparison
requires an explicit reviewed application/model adapter. The ordinary
`BindingSession` and generated one-transition/one-interval mapping remain
separate from this session's extra resource boundaries.

## Exploration and bounds

`explore(factory, max_runs, policy)` replays each decision prefix into a fresh
execution and branches over the next canonical enabled set. It retains actual
receipts, complete/incomplete coverage, terminals and controlled deadlocks. It
uses no POR. Run, decision, total-time and retained-evidence limits are explicit;
lost cleanup halts the search and returns its retained execution.

The total exploration deadline is checked around adapter acquisition, each run,
every decision/observer and final cleanup. Execution uses the earlier of its
own deadline and the exploration deadline; cleanup retains a separate drain
budget. A late final prefix cannot establish complete coverage. Receipts retain
`stopReason`, `budgetExceeded.total` and `evidenceComplete` separately, preserving
the first stop reason if a later budget check also fails.

Every acquired attempt has an ordered row, including adapter-factory exceptions
and incomplete cleanup. Incomplete executions remain available for cleanup retry.
The serialized exploration receipt is bounded by `max_evidence_bytes`; capacity
is reserved for failure metadata. If a full run receipt cannot fit, its row
explicitly records `evidenceError: "receipt_byte_bound_exceeded"`, coverage is
incomplete and `evidenceComplete` is false. Too small a budget to hold the header
and reserved metadata refuses before acquisition. `runs` includes failed
adapter-factory attempts; it does not imply completed executions.

Limits: 16 actors, 32 resources, queue capacity 64, 4,096 decisions/run and
4,096 exploration runs. Defaults are 2 seconds/execution, 1 second/cleanup,
30 seconds/exploration and 8 MiB of retained event/observation evidence. Admission
charges the complete serialized row, including an observation's `afterDecisions`
and `state` wrapper and the comma appended to a nonempty array. The existing
array brackets count toward the retained size. Over-limit rows are refused
before retention. Typed declaration metadata is separately bounded. Actor/resource
identifiers are bounded ASCII. Recursive/shared/timed locks, condition variables, dynamic
actors, uncontrolled native locks, queue close/reopen and arbitrary instruction
preemption are outside the prototype.

## Example and verification

`examples/controlled_sync.cpp` is installed under
`share/mirrorcpp/examples/controlled_sync.cpp`. It drives a producer/consumer
queue with mutex-protected actual counters and checks FIFO values independently
of the scheduler receipt. Its identity strings are fixture labels; the installed
acceptance manifest records the actual binary/source hashes.

`mirrorcpp_schedule_sync_test` covers contention, disabled-choice refusal,
queue backpressure/FIFO, deadlock graph retention, resource misuse, fresh prefix
exploration, timeout/cleanup retry, teardown/evidence failure, early-release and
queue-order application mutations, and cancellation before application entry.
The [Mirrors CLI guide](../../Mirrors/Docs/dpm-cli-guide.md) describes installed
consumer acceptance and receipt rendering. Static capability declarations are
in `share/mirrorcpp/controlled-sync-capabilities.json`; runtime acceptance is a
separate dated record.
