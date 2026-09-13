# Model Lifecycle Fabric

A vendor-neutral **C++20** runtime for governing model promotion, rollout, canarying,
rollback, retirement, compatibility, residency transition, and fleet-wide version
authority across heterogeneous AI infrastructure.

```
include/mlf/...      public headers
src/...              library implementation
tools/               mlf_cli, mlf_worker, mlf_coordinator
examples/            focused examples, plus an independent downstream consumer
benchmarks/          measured operations at 1 / 100 / 1000 scale
tests/               unit, adversarial, persistence, property, hardware, multiprocess
scripts/             MSVC environment import and build driver
docs/                lock ownership and deadlock audit
```

---

## The one systems question

> **Which model version is allowed to become authoritative where, under what
> compatibility, artifact, policy, health, rollout, residency, and generation
> evidence — and how does that authority evolve safely through promotion,
> canarying, rollout, rollback, drain, retirement, and replacement?**

The runtime exists to make three distinctions that a version store cannot make:

1. **A model artifact exists.** versus **This exact model generation is the
   authoritative serving or training model for this exact scope under current
   policy, compatibility, rollout, residency, and runtime evidence.**
2. **A new version was deployed somewhere.** versus **This exact candidate
   generation passed the current promotion gate, entered a bounded rollout stage,
   was admitted to these exact scopes, completed the required evidence, and either
   advanced or rolled back under current authority.**
3. **A rollout stage is persisted as active.** versus **The workers and replicas
   that made that stage executable are still current after a restart.**

## Exact boundary

Model Lifecycle Fabric **owns**: model identity; model version identity; model
generation; artifact-set identity; artifact generation; runtime compatibility
binding; backend compatibility binding; policy generation; lifecycle state;
promotion eligibility; promotion authority; rollout plan authority; canary
cohorts; rollout stages; fleet-wide rollout scope; scope targeting;
residency-transition authority at the lifecycle boundary; warmup and readiness
gates; health and evidence gates when supplied; compatibility gates; version
coexistence rules; rollback authority; drain and retirement semantics;
supersession; final retirement; stale-generation rejection; replica and model
authority binding; durable structural state; conservative recovery; distributed
coordinator and worker authority; deterministic explanations; immutable snapshots;
and REAL / SYNTHETIC / UNSUPPORTED provenance for hardware and runtime validation.

It **does not own**, and does not absorb: model artifact storage; Model Cache;
generic Model Residency policy; inference routing; Model Router decisions; request
scheduling; replica execution; inference serving; training; checkpoint creation;
adapter composition semantics; runtime package installation; compiler execution;
kernel compilation; fleet resource brokering; generic health monitoring; generic
SLO policy; generic cost policy; artifact promotion for autonomous research
artifacts; cluster management; or deployment orchestration outside the
model-lifecycle boundary.

It consumes evidence from those systems and never claims their responsibilities.

## Relationship to adjacent runtimes

| System | It owns | Model Lifecycle Fabric does |
| --- | --- | --- |
| **Model Cache** | reusable model assets | binds lifecycle authority to an exact artifact generation and digest; never stores or fetches artifacts |
| **Model Residency** | active weight and adapter presence and readiness on devices and hosts | authorizes *whether* a residency transition may happen, and consumes residency evidence as a gate |
| **Replica Fabric** | lifecycle and authority of serving replicas | binds model-version authority to replica generations and reconciles divergence |
| **Model Router** | request route selection | answers which model generation is authoritative for a scope; it never routes |
| **Artifact Fabric** | reusable machine-produced artifacts | consumes artifact identity and integrity; it is not an artifact store |
| **Compatibility Registry / Hardware Capability Registry** | canonical compatibility facts | consumes them, and owns only the compatibility verdict that gates a lifecycle transition |

---

## Model and version identity

A **version label** such as `2.4.1-rc2` is identity metadata. It is never
lifecycle authority. Authority is carried by generations, each of which
corresponds to state that can genuinely become stale, superseded, or semantically
different:

| Identity | Meaning |
| --- | --- |
| `ModelId` | stable identity of a model family |
| `ModelVersionId` | one registered version of a family |
| `ModelGeneration` | revisions of the version's declared identity content: artifact binding, requirements, predecessor |
| `ArtifactSetId`, `ArtifactGeneration` | which artifact set, at which content revision |
| `LifecycleGeneration` | every committed lifecycle transition of a version |
| `PromotionId`, `PromotionGeneration` | one promotion attempt |
| `RollbackId`, `RollbackGeneration` | one rollback attempt; at most one rollback outcome commits per generation |
| `RolloutId`, `RolloutGeneration` | one rollout plan revision |
| `RolloutStageId`, `RolloutStageGeneration` | one stage, and each entry into it |
| `CohortId` | a named set of scopes a stage moves |
| `ScopeId`, `ScopeGeneration` | a node of the scope tree, and each change to its lifecycle binding |
| `ReplicaId`, `ReplicaGeneration` | a replica incarnation |
| `ResidencyGeneration` | a residency intent or observation for a subject |
| `CompatibilityGeneration` | a published compatibility fact set |
| `PolicyGeneration` | a lifecycle policy revision |
| `HealthGeneration`, `ReadinessGeneration`, `EvidenceGeneration` | observation streams and the global evidence watermark |
| `WorkerId`, `WorkerBootId` | a worker, and one process incarnation of it |
| `CoordinatorEpoch` | one coordinator incarnation |
| `SnapshotGeneration` | one persisted snapshot |
| `Sequence` | the durable replay watermark |

Every generation above participates in a real staleness check. No generation is
decorative: a stale value is rejected with a named reason at the boundary that
depends on it.

## Lifecycle state machine

```
REGISTERED ─▶ VALIDATING ─▶ PROMOTION_ELIGIBLE ─▶ PROMOTION_BLOCKED
                   │                 │
                   │                 ├─▶ CANARY_PENDING ─▶ CANARY_ACTIVE ─┬─▶ CANARY_PASSED
                   │                 │                                    └─▶ CANARY_FAILED
                   │                 ├─▶ CURRENT                (immediate cutover)
                   │                 └─▶ PARTIALLY_PROMOTED     (immediate, narrow scope)
                   │
CANARY_PASSED ─▶ ROLLOUT_ACTIVE ─▶ PARTIALLY_PROMOTED ─▶ CURRENT
                       │                  │                │
                       └──────────────────┴────────────────┴─▶ ROLLBACK_PENDING ─▶ ROLLING_BACK ─▶ ROLLED_BACK
                                                                      │
                                            DRAINING ◀────────────────┘
                                                │
                                    RETIREMENT_PENDING ─▶ RETIRED   (terminal)
REVALIDATION_REQUIRED, FAILED                    (recovery and failure states)
```

* Every transition has explicit preconditions enforced by the engine.
* The state machine is a table, not an if-chain: `successors(state)` is
  enumerable, and every state except `RETIRED` is reachable from `REGISTERED`.
* **`RETIRED` is terminal.** No successor exists. A retired generation cannot be
  promoted, granted authority, revised, revalidated, rolled back to, or named as
  a rollback target. Reviving a retired lineage requires a distinct new model
  generation.
* Multi-step commits (for example `REGISTERED` to `CANARY_PENDING`) are executed
  by a breadth-first search over the transition table, so every intermediate edge
  is legal and no state is skipped or fabricated.

## Promotion

A promotion request is generation-bound and must carry: the candidate model
generation, the artifact generation, the compatibility generation, the policy
generation, the target scope and its generation, the coordinator epoch, the
strategy, the rollback target, and — when the caller is a worker — the worker boot
that asserts the request.

Promotion is never `set active = true`. It is a hard gate followed by a commit:

**Hard eligibility precedes every preference.** A candidate must have a bound
artifact, current compatibility for the target environment, a targetable scope, a
live promotion identity, no displacement of a serving generation without a valid
rollback target, current required evidence, matching generations, and a policy
that permits the transition. An ineligible candidate cannot become current through
favourable health, latency, or verifier evidence: the gate never consults those.

The rollback-target rule is precise: **a rollback target is required exactly when
the promotion displaces an existing authoritative generation in the target
scope.** A first deployment needs none, because there is nothing to roll back to;
replacing a serving generation requires one, because otherwise rollback would be
unplannable.

## Rollout and canary

A rollout is an explicit plan that binds the candidate generation, artifact
generation, compatibility generation, policy generation, root scope and root scope
generation, previous generation, cohorts with their scope sets and traffic shares,
ordered stages, per-stage required evidence, acceptance and failure criteria, the
rollback target, and the maximum blast radius.

**If any binding changes materially before execution, the plan is stale and is not
executed.** Staleness is checked at plan entry, at every stage advance, and during
reconciliation.

Canarying is first-class. A stage defines its cohort, target scopes, required
evidence, acceptance criteria, failure criteria, optional residency precondition,
optional drain requirement, and its own stage generation. Entering a stage grants
canary authority over that stage's cohort scopes; advancing a stage commits
exclusive authority over them and enters the next stage. **No stage success is ever
decided by a timer**: acceptance and failure criteria are numeric comparisons over
generation-bound evidence supplied by the caller.

When a failure criterion trips, the runtime commits the failure and returns
`ROLLBACK_DECISION_REQUIRED` unless policy explicitly authorizes automatic
rollback. It never invents certainty from ambiguous evidence.

## Scope authority

Scopes form a strict tree: `global` → site → region → cluster → environment →
service → tenant → replica group → workload class. Identity is assigned in creation
order, so identical call sequences produce identical identities.

Authority is resolved deterministically by walking the chain from the queried scope
towards the root and taking the nearest scope that holds a live exclusive binding.
The answer reports whether it was inherited, from where, and why. More than one
exclusive binding in one scope is reported as `AMBIGUOUS` rather than being
silently resolved. A scope with no exclusive binding anywhere in its chain returns
`NO_AUTHORITATIVE_MODEL`; the runtime never falls back to an older generation
unless a binding authorizes it.

Typical results:

```
global            -> version 1   (exclusive, not inherited)
cluster:c1        -> version 2   (exclusive, not inherited)
cluster:c1/tenant:a -> version 2 (exclusive)
cluster:c2        -> version 1   (inherited from global)
```

## Version coexistence

Coexistence is governed, not accidental. Policy sets the maximum number of
authoritative generations per scope and whether split traffic is permitted at all.
A scope holds at most one **exclusive** binding; additional generations coexist
only as `Canary` or `SplitTraffic` overlays, and a `RollbackRetained` binding
never counts as authority. A child scope may not silently override a parent policy:
a scope refinement applies only when every ancestor refinement that carries it
explicitly permits further refinement.

## Compatibility model

Compatibility is structured and returns named reasons:

`COMPATIBLE`, `COMPATIBLE_WITH_REBUILD`, `COMPATIBLE_WITH_RECOMPILE`,
`COMPATIBLE_WITH_CONVERSION`, `INCOMPATIBLE_ARTIFACT`,
`INCOMPATIBLE_RUNTIME`, `INCOMPATIBLE_BACKEND`, `INCOMPATIBLE_ARCHITECTURE`,
`INCOMPATIBLE_PRECISION`, `INCOMPATIBLE_TOKENIZER`, `INCOMPATIBLE_ADAPTER`,
`INCOMPATIBLE_POLICY`, `UNKNOWN`, `STALE_EVIDENCE`, `UNSUPPORTED`.

Dimensions evaluated: model artifact against runtime; runtime against backend;
artifact against accelerator architecture; quantization against hardware support;
tokenizer and config generation; adapter set against base model; required precision
against accelerator capabilities. Facts are published per environment key and are
bound to the model generation and artifact generation that produced them; a fact
for an older generation is reported as `STALE_EVIDENCE`, never reused.

**`UNKNOWN` never becomes `COMPATIBLE`.** When the environment reports nothing,
the verdict is `UNKNOWN` and the transition is refused.

## Residency-transition authority

Model Lifecycle Fabric authorizes residency transitions; it does not perform them.
It exposes intents: a candidate may warm; a candidate must be resident before a
stage may be entered; the previous generation must remain resident for rollback;
an old generation may drain after a stage commits; a retired generation may be
evicted.

Residency evidence is bound to the model generation, artifact generation, scope,
replica or node, residency generation, publisher boot, and evidence generation. A
residency observation for a different generation, scope, or publisher boot is
stale, and a stage that requires residency cannot be entered on stale evidence.

## Readiness

Loading a model is not readiness. The runtime recognizes distinct evidence:
warmup, readiness, residency readiness, replica readiness, health, and probes. A
synthetic replica that has been created but not warmed is present, not resident,
and not ready; activation without warmup is refused with `READINESS_MISSING`.
Readiness that was taken before an artifact revision no longer describes the
current generation and is reported as stale.

## Rollback

Rollback is a first-class fenced transition, not a pointer swap. A rollback
verifies the current rollout generation, the candidate generation, the target
generation, that the target is not retired, that its artifact is available, that
its compatibility is current, that the candidate actually holds authority in the
scope being taken back, the scope generation, the policy, and the coordinator
epoch. It then:

1. supersedes the candidate's authority in the rollback scope,
2. grants exclusive authority to the target and advances it to `CURRENT`,
3. marks the candidate `DRAINING` in that scope and `ROLLED_BACK` when it holds
   nothing else,
4. stops the rollout so a late stage completion cannot commit,
5. records the rollback identity and generation, so a second rollback of the same
   plan is refused.

Evidence is preserved, not deleted: the diagnostic record of why a rollback was
taken survives it.

## Retirement

Retirement is explicit and is blocked until the runtime can prove it is safe:

* no live authoritative scope binding,
* no live rollout targeting the generation,
* no live rollout naming it as its rollback target,
* no outstanding drain,
* no outstanding rollback retention, unless policy explicitly allows retirement
  without drain,
* no unsettled external attempt against the generation.

After retirement, new promotion from that generation is rejected, new scope
authority cannot be granted, stale frames cannot resurrect it, and every
historical record remains inspectable.

## Supersession

A new promotion supersedes an in-flight rollout of the same model
deterministically: the older plan is marked superseded, its authority is withdrawn,
a `Superseded` stage record is appended to its immutable history, and its
candidate is moved to `DRAINING`. A late stage completion from the superseded
generation is refused and cannot overwrite newer rollout state.

## Evidence and freshness

Every evidence record carries an identity, a generation, a publisher, the
publisher's boot, the subject generation, a provenance classification, and a
timestamp. The subject includes the model generation, artifact set and generation,
scope, rollout and stage generations, and compatibility generation.

Evidence is only usable when **all** of those match the subject under evaluation.
Anything else is reported as `MISSING`, `SUBJECT_MISMATCH`, `GENERATION_STALE`,
`PUBLISHER_STALE`, `EXPIRED`, `CONFLICTING`, or `UNKNOWN_PROVENANCE`. Old
evidence can never approve a newer model generation by accident.

The store is bounded. When it is full the oldest record is evicted and an explicit
drop counter is incremented, so incomplete evidence stays visible instead of
silently disappearing.

## Decisions and explanations

Every operation returns a stable enumerated outcome plus a deterministically
ordered list of named reasons. Callers never parse prose.

Outcomes: `PROMOTION_ALLOWED`, `PROMOTION_BLOCKED`, `CANARY_ALLOWED`,
`CANARY_BLOCKED`, `STAGE_ADVANCE_ALLOWED`, `STAGE_ADVANCE_BLOCKED`,
`ROLLBACK_ALLOWED`, `ROLLBACK_BLOCKED`, `RETIREMENT_ALLOWED`,
`RETIREMENT_BLOCKED`, `REVALIDATION_REQUIRED`, `ROLLBACK_DECISION_REQUIRED`,
`STALE_PLAN`, `STALE_EVIDENCE`, `INCOMPATIBLE`, `NO_AUTHORITATIVE_MODEL`,
`AMBIGUOUS`, `OUTCOME_UNKNOWN`, `RECONCILIATION_REQUIRED`, `CONFLICT`,
`BOUNDS_EXCEEDED`, `NOT_FOUND`, `UNSUPPORTED`.

Reasons are drawn from a fixed table of named codes such as
`ROLLBACK_TARGET_REQUIRED`, `COMPATIBILITY_GENERATION_MISMATCH`,
`SCOPE_GENERATION_MISMATCH`, `ROLLOUT_STAGE_GENERATION_MISMATCH`,
`EVIDENCE_PUBLISHER_BOOT_STALE`, `COEXISTENCE_LIMIT_EXCEEDED`, and
`COORDINATOR_EPOCH_STALE`. Reasons are sorted and de-duplicated, so stable state
produces byte-identical explanations.

## Persistence and recovery

Durable state contains model and version identities, artifact mappings, lifecycle
states, committed promotions, rollout definitions, committed stage history, scope
authority, rollback targets, retirement state, compatibility facts, sequence
watermarks, durable evidence summaries, fenced worker leases, committed promotion
and rollback identities, and the monotonic identity allocators.

It deliberately excludes volatile health, readiness, latency, error-rate and
residency readings, live worker connections, and in-flight attempt progress.

The on-disk format is versioned, integrity-checked (CRC-32 over a canonical
little-endian encoding), bounded in every count and string, and written by atomic
replacement through a temporary file. **A failed load cannot partially apply**:
decoding validates structure, cross-references, generation ordering, authority
invariants, and lifecycle-state legality before a single field is installed.

After a restart the coordinator advances its epoch, restores durable structure,
preserves committed authority that is still semantically valid, marks dynamic
execution and readiness evidence stale, fences every recovered worker lease,
requires fresh worker registration, keeps incomplete external attempts
conservative, and rejects every frame carrying the old epoch.

## Reconciliation

Reconciliation compares durable lifecycle truth against runtime reality and returns
deterministic findings: `AuthorityWithoutReplicas`,
`ExpectedResidentMissing`, `RetiredModelStillResident`,
`RolloutStageWithoutWorkers`, `ReplicaForNonAuthoritativeGeneration`,
`ArtifactGenerationChanged`, `CompatibilityChanged`, `UnreconciledAttempt`,
`FencedWorkerStillPublishing`, and `StalePlanBinding`.

An absent observation is reported as absent, never as healthy. When worker
liveness is unknown, the runtime says so instead of concluding that a worker died.
History is never silently rewritten.

## Distributed authority

`LifecycleCoordinator` owns durable state and the control plane.
`LifecycleWorker` is a real OS process that holds a worker lease bound to its
`(WorkerId, WorkerBootId, CoordinatorEpoch)`, executes lifecycle commands against
the synthetic backend, and publishes the evidence it observes. `LifecycleClient`
is the synchronous request/reply client used by the CLI, the examples, and the
tests.

The transport is a bounded, versioned, framed TCP protocol over real sockets. A
frame is a 16-byte header (magic, version, type, flags, payload length), a bounded
payload, and a trailing CRC-32 over everything before it. Decoding rejects a wrong
magic, an unsupported version, an unknown type, an oversized declared length,
truncation at any prefix, and any integrity failure **before** a message is
interpreted.

A dead worker permanently loses authority for that boot: its lease is fenced, its
evidence is withdrawn, and its in-flight attempts become `OUTCOME_UNKNOWN`. A
replacement requires a fresh boot identity and fresh evidence.

Acknowledgements are durable-first: a mutation is written to durable state before
its reply is sent, so a client that observes success and immediately loses the
coordinator still finds the change after recovery.

### Ambiguous completion

If a performer dies after applying a side effect but before acknowledging it, the
runtime returns `OUTCOME_UNKNOWN` and requires reconciliation. It never repeats a
non-idempotent external action on the strength of a guess, and it never reports an
unconfirmed effect as completed.

## REAL / SYNTHETIC / UNSUPPORTED

Every validation claim in this repository is classified:

* **REAL** — observed against the live host, real files for artifact identity, real
  OS processes, real TCP sockets, real process death and restart, and the CUDA
  driver API for device capability.
* **SYNTHETIC** — produced by the deterministic synthetic lifecycle backend:
  replicas, warmup, activation, drain, canary cohorts, fleet-scale rollout shapes,
  heterogeneous accelerator fleets, and cross-site lifecycle.
* **UNSUPPORTED** — not exercisable here; recorded instead of fabricated.

Synthetic rollout is never described as a physical deployment. No claim in this
README describes a production fleet.

## Hardware and runtime validation

The library discovers the host it actually runs on, through CPUID, the operating
system, and the CUDA driver loaded at run time (no build-time CUDA dependency — on
a host without the driver the probe reports `UNSUPPORTED`).

Measured on the machine used to validate this release:

```
os                = Windows 10.0 (build 26200)
machine           = x86_64
cpu_vendor        = AuthenticAMD
cpu_cores         = 8 physical, 16 logical
cpu_avx2          = yes
cpu_avx512        = yes
memory_bytes      = 66190020608
cuda_provenance   = REAL
cuda_driver       = present version=13.4
gpu[0]            = NVIDIA GeForce RTX 5090 sm_120 (12.0) vram=34162016256
```

The device-capability proof binds a model requirement to real device attributes:
a requirement for `sm_120` on the observed device is `COMPATIBLE`, a requirement
for `sm_999` is `INCOMPATIBLE_ARCHITECTURE`, a requirement for the `rocm`
backend is `INCOMPATIBLE_BACKEND`, and a requirement for an unadvertised precision
is `INCOMPATIBLE_PRECISION`. Capability discovery proves *compatibility
admissibility*; it is not evidence that model-serving lifecycle execution ran on
that device.

The artifact proof uses two real artifact generations on disk: SHA-256 digests are
computed over actual bytes (validated against published FIPS 180-4 test vectors),
registered as distinct generations, bound through promotion, and replaced to
demonstrate that a plan bound to the old artifact generation becomes stale.

Physical serving-runtime proof (load a candidate into a real inference runtime and
execute a bounded probe) is **UNSUPPORTED** in this environment: no inference
runtime with a loadable local model was present, and the repository does not
download one. Readiness is therefore driven by synthetic evidence labelled
`SYNTHETIC`.

## Build

Requirements: a C++20 compiler, CMake 3.20 or newer, and Ninja or another
generator. There are **no third-party dependencies**; the test harness, the
serialization, the SHA-256 and CRC-32 implementations, and the transport are all
first-party.

On Windows with MSVC:

```powershell
pwsh -File scripts/build.ps1 -Configuration Release
pwsh -File scripts/build.ps1 -Configuration Debug
pwsh -File scripts/build.ps1 -Configuration Release -Sanitizers
```

The script imports the MSVC developer environment (including `rc.exe` and
`mt.exe`) into the current process and then configures and builds with Ninja.
Directly with CMake:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Options: `MLF_BUILD_TESTS`, `MLF_BUILD_EXAMPLES`, `MLF_BUILD_BENCHMARKS`,
`MLF_BUILD_CLI`, `MLF_WARNINGS_AS_ERRORS`, `MLF_ENABLE_SANITIZERS`.

First-party code builds clean under MSVC `/W4 /WX /permissive-` with zero
warnings, in both Release and Debug.

### Sanitizers

```powershell
pwsh -File scripts/validate-sanitizers.ps1
```

The script probes for the x64 AddressSanitizer runtime and either builds and runs
the whole suite under it, or reports the gap and runs the strongest available
equivalent. When `MLF_ENABLE_SANITIZERS` is requested without that runtime
installed, the configure step fails with an actionable message instead of a link
error.

On the machine used to validate this release, **AddressSanitizer is UNSUPPORTED**:
the Visual Studio installation provides only the i386 `clang_rt.asan` runtime, so
an x64 instrumented build cannot be linked. The equivalent used instead is the
Debug configuration, which enables MSVC iterator debugging, the debug heap, and
`/RTC1` runtime checks; the complete suite passes under it.

## Tests

```sh
ctest --test-dir build-release --output-on-failure
```

25 suites covering:

* identity and scope model; lifecycle state machine; compatibility model;
  registry bounds
* promotion eligibility; rollout and canary; scope authority; rollback and
  retirement; supersession; residency and readiness; reconciliation
* stale generation; protocol adversarial; persistence adversarial; compatibility
  adversarial; rollout adversarial; backpressure
* durable state store; recovery
* randomized property tests; deterministic race interleavings; concurrency
* real host and CUDA validation; real artifact proof
* real multiprocess proofs: worker death, stale completion from a dead boot,
  coordinator restart, corrupt state at startup

**No test uses a timeout of any kind.** Every suite runs to natural completion; a
hang is treated as a defect rather than masked by a timer. Intentional process
termination is used only where process death is itself the subject.

## Examples

```sh
build-release/mlf_example_register_and_promote
build-release/mlf_example_canary_rollout
build-release/mlf_example_scope_authority
build-release/mlf_example_rollback
build-release/mlf_example_retirement
build-release/mlf_example_artifact_generation_change
build-release/mlf_example_worker_fencing
build-release/mlf_example_persistence_recovery
build-release/mlf_example_installed_consumer
```

Each example uses the public API only, and each returns a non-zero exit status when
the behaviour it demonstrates does not hold.

## CLI

`mlf_cli` separates read-only inspection from mutation.

Read-only, from a durable state file with no coordinator running:

```sh
mlf_cli summary     --state state.mlfs
mlf_cli models      --state state.mlfs
mlf_cli versions    --state state.mlfs
mlf_cli state       --state state.mlfs --version-id 2
mlf_cli authority   --state state.mlfs --scope global/cluster:c1
mlf_cli rollout     --state state.mlfs
mlf_cli compatibility --state state.mlfs
mlf_cli evidence    --state state.mlfs
mlf_cli workers     --state state.mlfs
```

Read-only against a live coordinator (bounded single-entity queries):
`mlf_cli summary --endpoint 127.0.0.1:9000`, `mlf_cli state`,
`mlf_cli authority`. Whole-registry listings require `--state`; the CLI says so
rather than fabricating a listing.

Mutation, always against a live coordinator:
`register-model`, `register-version`, `publish-compatibility`, `promote`,
`begin`, `advance`, `fail`, `rollback`, `retire`, `revalidate`, `fence`.

`mlf_cli host` reports the real host environment with its provenance
classification.

## Running the distributed runtime

```sh
# terminal 1
mlf_coordinator --bind 127.0.0.1 --port 9000 --state lifecycle.mlfs

# terminal 2
mlf_worker --host 127.0.0.1 --port 9000 --worker-id 1

# terminal 3
mlf_cli summary --endpoint 127.0.0.1:9000
```

## CMake installation

```sh
cmake --install build-release --prefix /path/to/prefix
```

This installs the library, the public headers, and a complete package:

* `ModelLifecycleFabricConfig.cmake`
* `ModelLifecycleFabricConfigVersion.cmake`
* `ModelLifecycleFabricTargets.cmake`, exporting
  `SummonSoftwareLabs::ModelLifecycleFabric`

## Downstream usage

```cmake
find_package(ModelLifecycleFabric CONFIG REQUIRED)
target_link_libraries(your_target PRIVATE SummonSoftwareLabs::ModelLifecycleFabric)
```

A complete independent consumer lives in `examples/downstream-consumer`. It is not
part of this build tree:

```sh
cmake -S examples/downstream-consumer -B build-consumer \
      -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build build-consumer
build-consumer/mlf_downstream_consumer
```

## Benchmarks

```sh
build-release/mlf_benchmarks
```

Measured operations at 1, 100, and 1000 models (2000 versions), over operations
that actually completed. Representative results from the validating machine:

```
operation                        scale   micros/op
model_registration                1000       0.828
promotion_eligibility             2000       0.304
authority_resolution              1000       0.412
scope_lookup_by_path               999       0.491
retirement_validation             2000       0.200
rollback_planning                 2000       0.236
snapshot_creation                 2000     839.480
persistence_save                  2000    3042.220
persistence_load                  2000    3001.370
artifact_digest_1MiB                 1    2656.505
```

Promotion, authority, scope, retirement and rollback planning are indexed
operations and stay sub-microsecond at fleet scale. Snapshot and persistence cost
is linear in durable state size, which is the expected and intended shape: they
copy and encode the whole durable view.

## Concurrency and lock discipline

The library contains exactly two mutexes: one per engine, one per synthetic
backend, and no code path holds both. The engine mutex is never held across socket
I/O, filesystem work, callbacks, thread joins, or process waits. Validation and
commit for an operation happen under one lock acquisition, which is what makes
"validate then apply" atomic. See `docs/LOCK_ORDER.md` for the full deadlock
audit.

## Genuine limitations

* **No production fleet integration.** No real inference fleet, cluster manager, or
  deployment orchestrator was available. Multi-cluster, multi-site, and
  heterogeneous-accelerator rollouts are exercised with the synthetic backend and
  are labelled `SYNTHETIC`.
* **No physical serving-runtime proof.** No local model was loaded into a real
  inference runtime. Physical load, probe, and serving readiness are
  `UNSUPPORTED` here.
* **The synthetic backend is not a simulator of a real fleet's failure modes.** It
  models replicas, residency, warmup, activation, drain, and loss, with explicit
  generations; it does not model network partitions, clock skew, or scheduler
  behaviour.
* **Fencing is graceful where the process is alive.** A worker that is unreachable
  is fenced by the coordinator when the connection closes; a coordinator that
  cannot reach a worker cannot force it to stop serving, which is why replacement
  always requires a fresh boot identity rather than a resumed one.
* **One coordinator per durable state file.** There is no quorum or leader
  election. Two coordinators sharing a state file would each advance the epoch and
  only the durable replay is protected.
* **The protocol is unauthenticated and unencrypted.** It binds to loopback by
  default and is intended for a trusted control plane; `--bind 0.0.0.0` is
  available but is a deliberate operator choice.
* **Policy is a single base policy plus scope refinements.** There is no policy
  language and no external policy engine.
* **Evidence retention is bounded and lossy by design.** The drop counter makes the
  loss visible; it does not make the loss disappear.
* **Version comparison is numeric-component based.** It is adequate for gating a
  minimum runtime version and is documented as such; version strings are never
  lifecycle authority.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
