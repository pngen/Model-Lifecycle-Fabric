# Lock ownership in Model Lifecycle Fabric

This document is the mandatory deadlock audit required by the runtime's design
rules. It records every lock in the library, the order in which locks may be
taken, and the operations that are deliberately performed outside every lock.

## Locks

There are exactly two kinds of lock in Model Lifecycle Fabric.

| Lock | Owner | Guards |
| --- | --- | --- |
| `LifecycleEngine::mutex_` | one per engine | every field of `LifecycleEngine::Impl::state` |
| `SyntheticLifecycleBackend::mutex_` | one per backend | the backend's replica table and tick |

No other mutex, condition variable, or blocking primitive exists in the library.
There is no reader/writer lock, no recursive mutex, and no lock hierarchy deeper
than two.

## Lock order

```
LifecycleEngine::mutex_   ->   SyntheticLifecycleBackend::mutex_
```

The two locks are never held at the same time by any code path. The engine never
calls into a backend, and the backend never calls into an engine:

* The engine operates on lifecycle state only.
* A `LifecycleWorker` owns a backend and calls the engine only through the
  network, over `FrameChannel`, which holds no engine lock.
* The coordinator calls the engine, releases the lock, and only then writes to a
  socket.

Because there is no path that acquires both, the ordering above is a statement
about intent rather than a constraint that can currently be violated. If a future
change makes a backend call reachable from engine code, that change must observe
this order.

## What is never done under a lock

The engine's mutex is never held across:

* **Socket I/O.** `FrameChannel::send`, `FrameChannel::receive`,
  `Socket::send_all` and `Socket::recv_some` are only ever called from
  `LifecycleCoordinator`, `LifecycleWorker` and `LifecycleClient` after the
  engine call that produced the reply has returned.
* **Filesystem work.** `save_durable_state` and `load_durable_state` are
  called with a `DurableState` that was produced by
  `export_durable_state()` (which takes the lock, copies, and releases) or that
  is about to be handed to `import_durable_state()`. No file is opened while
  the mutex is held.
* **Callbacks.** The library takes no user callback. Every notification is a
  returned value.
* **External calls.** No backend, runtime, or platform call is made from engine
  code.
* **Thread joins and process waits.** `ChildProcess::wait`, `terminate`,
  `Socket::accept` and `select` are only used by hosts and tests, never from a
  locked region.

## Reentrancy audit

The engine exposes four unlocked evaluation cores, called with the mutex held:

* `evaluate_promotion_locked`
* `evaluate_stage_advance_locked`
* `evaluate_rollback_locked`
* `evaluate_retirement_locked`

Each of these is a pure function of engine state: it reads fields, builds a
`Decision`, and returns. None of them acquires the mutex again, calls a public
`LifecycleEngine` method, or invokes any helper that locks. The public
`evaluate_*` entry points take the mutex and delegate to exactly these
functions, so there is no read-then-write reentrancy and no recursive locking.

The mutation paths (`promote`, `advance_stage`, `rollback`, `retire`) call
the same unlocked cores while already holding the mutex. That is what makes
"validate then apply" a single atomic step: no other thread can observe the state
between the gate and the commit.

## Specific hazards checked

| Hazard | Status |
| --- | --- |
| Read to write reentrancy | none: unlocked cores never call a locking entry point |
| Model/scope lock inversion | single lock; no inversion possible |
| Rollout/worker lock inversion | single lock; no inversion possible |
| Callback reentry under the lifecycle lock | no callbacks exist |
| Event publication while holding mutable registry state | no event system exists; all results are returned by value |
| Worker self-join | the coordinator never joins a session; sessions are polled from one thread |
| Coordinator stop under a session lock | there is no session lock; sessions are owned by the poll thread |
| Persistence under the mutation lock | export/import copy in and out; file I/O happens outside |
| Snapshot generation re-entering registry state | `export_durable_state` copies and returns; it never re-enters |
| Rollback waiting on state held by the rollback path | the rollback commit is a straight-line sequence under one lock |
| Drain/reconciliation cross-lock ordering | reconciliation takes the engine lock only, and reads no other lock |

## Concurrency model in one paragraph

`LifecycleEngine` is safe for concurrent use by any number of threads. Every
public method takes the single mutex, performs all validation and all mutation
for that operation, and releases it before returning. `LifecycleCoordinator`
services every session from one thread; because socket operations happen outside
the engine lock, a slow peer can delay the coordinator's own progress but can
never deadlock it. `LifecycleWorker` is single-threaded: it blocks on its own
socket and never shares its backend with another thread.
