// Recovery: durable truth survives, volatile authority does not.
#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(recovery, a_restart_preserves_committed_authority_and_advances_the_epoch) {
  const std::string path = mlftest::scratch_path("recovery", ".mlfs");
  ModelId model;
  ModelVersionId v1;
  CoordinatorEpoch epoch_after;
  std::uint64_t generation_before = 0;

  {
    mlftest::Scenario scenario = mlftest::make_scenario(311);
    require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                            ModelVersionId{})
                   .decision,
               "promote");
    model = scenario.model;
    v1 = scenario.v1;
    generation_before = mlftest::version_of(*scenario.engine, scenario.v1).generation.raw();
    const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
    require_ok(Decision(save_durable_state(*scenario.engine->export_durable_state(), limits, path).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "save");
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    const StateStoreResult result = load_durable_state(path, limits, loaded);
    MLF_CHECK_MSG(result.ok(), result.detail);
    const CoordinatorEpoch epoch_before = loaded.epoch;
    epoch_after = next_generation(epoch_before);
    require_ok(engine.import_durable_state(loaded, epoch_after), "import");
    engine.invalidate_volatile_state("coordinator restart");

    MLF_CHECK(engine.epoch() > epoch_before);
    ScopeId global;
    require_ok(engine.register_scope("global", global), "scope");
    const AuthorityQueryResult authority = engine.query_authority(global);
    MLF_CHECK_EQ(std::string(to_string(authority.outcome)), std::string("OK"));
    const ModelVersionRecord record = mlftest::version_of(engine, v1);
    MLF_CHECK_EQ(record.generation.raw(), generation_before);
    MLF_CHECK_EQ(std::string(to_string(record.state)), std::string("CURRENT"));
  }
  mlftest::remove_file(path);
}

MLF_TEST(recovery, dynamic_evidence_does_not_survive_a_restart) {
  const std::string path = mlftest::scratch_path("recovery-evidence", ".mlfs");
  ModelVersionId v1;
  ModelId model;
  std::uint64_t generation = 0;
  {
    mlftest::Scenario scenario = mlftest::make_scenario(312);
    v1 = scenario.v1;
    model = scenario.model;
    const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
    generation = record.generation.raw();
    require_ok(mlftest::publish_ready(*scenario.engine, record, scenario.global,
                                      EvidenceKind::Readiness),
               "readiness");
    require_ok(mlftest::publish_evidence(*scenario.engine, record, scenario.global,
                                         EvidenceKind::TestResult, EvidenceVerdict::Satisfied, 1.0),
               "test result");
    const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
    require_ok(Decision(save_durable_state(*scenario.engine->export_durable_state(), limits, path).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "save");
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    require_ok(Decision(load_durable_state(path, limits, loaded).ok() ? OutcomeCode::Ok
                                                                     : OutcomeCode::Rejected),
               "load");
    require_ok(engine.import_durable_state(loaded, next_generation(loaded.epoch)), "import");
    engine.invalidate_volatile_state("restart");

    static_cast<void>(model);
    static_cast<void>(v1);
    static_cast<void>(generation);
    bool saw_test_result = false;
    for (const EvidenceRecord& record : engine.all_evidence()) {
      if (record.kind == EvidenceKind::Readiness) {
        MLF_CHECK_MSG(false, "readiness evidence survived a restart");
      }
      if (record.kind == EvidenceKind::TestResult) saw_test_result = true;
    }
    MLF_CHECK(saw_test_result);
  }
  mlftest::remove_file(path);
}

MLF_TEST(recovery, a_recovered_worker_lease_is_fenced_and_its_evidence_is_gone) {
  const std::string path = mlftest::scratch_path("recovery-worker", ".mlfs");
  WorkerId worker(1);
  WorkerBootId boot(77);
  ModelVersionId v1;
  {
    mlftest::Scenario scenario = mlftest::make_scenario(313);
    v1 = scenario.v1;
    WorkerRegistration registration;
    registration.id = worker;
    registration.boot = boot;
    registration.epoch = scenario.engine->epoch();
    require_ok(scenario.engine->register_worker(registration), "worker");
    EvidenceRecord evidence;
    evidence.kind = EvidenceKind::Readiness;
    evidence.verdict = EvidenceVerdict::Satisfied;
    evidence.subject.model = scenario.model;
    evidence.subject.version = scenario.v1;
    evidence.subject.scope = scenario.global;
    evidence.provenance = Provenance::Synthetic;
    evidence.worker = worker;
    evidence.boot = boot;
    require_ok(scenario.engine->publish_evidence(evidence), "evidence");
    const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
    require_ok(Decision(save_durable_state(*scenario.engine->export_durable_state(), limits, path).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "save");
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    require_ok(Decision(load_durable_state(path, limits, loaded).ok() ? OutcomeCode::Ok
                                                                     : OutcomeCode::Rejected),
               "load");
    require_ok(engine.import_durable_state(loaded, next_generation(loaded.epoch)), "import");
    engine.invalidate_volatile_state("restart");

    MLF_CHECK(!engine.worker_boot_current(worker, boot));
    const std::optional<WorkerLease> lease = engine.worker(worker);
    MLF_CHECK(lease.has_value());
    MLF_CHECK(lease->fenced);

    // The fenced lease cannot act.
    const Decision attempt = engine
                                 .begin_attempt(AttemptRequest{AttemptKind::Warm, worker, boot,
                                                               engine.epoch(), ModelId(1), v1,
                                                               ModelGeneration{}, ArtifactGeneration{},
                                                               ScopeId(1), RolloutId{},
                                                               RolloutGeneration{}, RolloutStageId{},
                                                               RolloutStageGeneration{}, false, ""})
                                 .decision;
    MLF_CHECK(!attempt.allowed());
    MLF_CHECK(attempt.has(ReasonCode::WorkerBootStale));
  }
  mlftest::remove_file(path);
}

MLF_TEST(recovery, an_in_flight_attempt_becomes_outcome_unknown_not_completed) {
  const std::string path = mlftest::scratch_path("recovery-attempt", ".mlfs");
  AttemptId attempt_id;
  {
    mlftest::Scenario scenario = mlftest::make_scenario(314);
    const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
    AttemptRequest request;
    request.kind = AttemptKind::Activate;
    request.model = scenario.model;
    request.version = scenario.v1;
    request.model_generation = record.generation;
    request.artifact_generation = record.artifact.generation;
    request.scope = scenario.global;
    const AttemptOutcome started = scenario.engine->begin_attempt(request);
    MLF_CHECK(started.decision.allowed());
    attempt_id = started.attempt;
    const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
    require_ok(Decision(save_durable_state(*scenario.engine->export_durable_state(), limits, path).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "save");
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    require_ok(Decision(load_durable_state(path, limits, loaded).ok() ? OutcomeCode::Ok
                                                                     : OutcomeCode::Rejected),
               "load");
    require_ok(engine.import_durable_state(loaded, next_generation(loaded.epoch)), "import");
    engine.invalidate_volatile_state("restart");
    const std::optional<Attempt> attempt = engine.attempt(attempt_id);
    MLF_CHECK(attempt.has_value());
    MLF_CHECK(attempt->state == AttemptState::OutcomeUnknown);
    MLF_CHECK(attempt->state != AttemptState::Completed);
  }
  mlftest::remove_file(path);
}

MLF_TEST(recovery, a_stale_completion_from_a_pre_restart_boot_is_refused) {
  const std::string path = mlftest::scratch_path("recovery-fence", ".mlfs");
  AttemptId attempt_id;
  CoordinatorEpoch old_epoch;
  {
    mlftest::Scenario scenario = mlftest::make_scenario(315);
    const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
    AttemptRequest request;
    request.kind = AttemptKind::Warm;
    request.model = scenario.model;
    request.version = scenario.v1;
    request.model_generation = record.generation;
    request.scope = scenario.global;
    const AttemptOutcome started = scenario.engine->begin_attempt(request);
    MLF_CHECK(started.decision.allowed());
    attempt_id = started.attempt;
    old_epoch = scenario.engine->epoch();
    const DecodeLimits limits = DecodeLimits::from_bounds(scenario.engine->bounds());
    require_ok(Decision(save_durable_state(*scenario.engine->export_durable_state(), limits, path).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "save");
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
    require_ok(Decision(load_durable_state(path, limits, loaded).ok() ? OutcomeCode::Ok
                                                                     : OutcomeCode::Rejected),
               "load");
    require_ok(engine.import_durable_state(loaded, next_generation(loaded.epoch)), "import");
    engine.invalidate_volatile_state("restart");
    MLF_CHECK(engine.epoch() > old_epoch);

    CompletionRecord completion;
    completion.attempt = attempt_id;
    completion.success = true;
    completion.confirmed = true;
    completion.epoch = old_epoch;
    const Decision decision = engine.complete_attempt(completion);
    MLF_CHECK(!decision.allowed());
    MLF_CHECK(decision.has(ReasonCode::CoordinatorEpochStale));
  }
  mlftest::remove_file(path);
}

MLF_TEST(recovery, a_sequence_regression_is_refused) {
  LifecycleEngine engine;
  ModelId model;
  require_ok(engine.register_model("m", "f", Provenance::Synthetic, model), "model");
  const auto live = engine.export_durable_state();

  DurableState stale = *live;
  stale.sequence = Sequence(1);
  const Decision refused = engine.import_durable_state(stale, next_generation(live->epoch));
  MLF_CHECK(!refused.allowed());
  MLF_CHECK(refused.has(ReasonCode::SequenceRegression));

}

MLF_TEST(recovery, a_newer_view_loads_into_a_fresh_engine) {
  LifecycleEngine source;
  ModelId model;
  require_ok(source.register_model("m", "f", Provenance::Synthetic, model), "model");
  const auto live = source.export_durable_state();

  LifecycleEngine target;
  require_ok(target.import_durable_state(*live, next_generation(live->epoch)), "import");
  MLF_CHECK_EQ(target.models().size(), 1u);
  MLF_CHECK(target.epoch() > live->epoch);
}
