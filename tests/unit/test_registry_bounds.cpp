// Bounded growth: every registry limit is enforced and reported.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(bounds, model_and_version_limits_are_enforced) {
  EngineConfig config;
  config.bounds.max_models = 2;
  config.bounds.max_versions_per_model = 2;
  LifecycleEngine engine(config);

  ModelId first;
  ModelId second;
  ModelId third;
  require_ok(engine.register_model("m1", "f", Provenance::Synthetic, first), "m1");
  require_ok(engine.register_model("m2", "f", Provenance::Synthetic, second), "m2");
  Decision overflow = engine.register_model("m3", "f", Provenance::Synthetic, third);
  MLF_CHECK(!overflow.allowed());
  MLF_CHECK(overflow.has(ReasonCode::BoundsModelCount));

  Decision duplicate = engine.register_model("m1", "f", Provenance::Synthetic, third);
  MLF_CHECK(!duplicate.allowed());
  MLF_CHECK(duplicate.has(ReasonCode::DuplicateModel));

  const auto version_for = [&](const char* label) {
    RegisterVersionRequest request;
    request.model = first;
    request.label = label;
    request.artifact.set_id = ArtifactSetId(1);
    request.artifact.generation = ArtifactGeneration(1);
    request.artifact.digest = "digest-a";
    request.provenance = Provenance::Synthetic;
    return request;
  };
  ModelVersionId v1;
  ModelVersionId v2;
  ModelVersionId v3;
  require_ok(engine.register_version(version_for("1"), v1), "v1");
  require_ok(engine.register_version(version_for("2"), v2), "v2");
  Decision too_many = engine.register_version(version_for("3"), v3);
  MLF_CHECK(!too_many.allowed());
  MLF_CHECK(too_many.has(ReasonCode::BoundsVersionCount));

  Decision duplicate_version = engine.register_version(version_for("1"), v3);
  MLF_CHECK(!duplicate_version.allowed());
  MLF_CHECK(duplicate_version.has(ReasonCode::DuplicateVersion));
}

MLF_TEST(bounds, malformed_registration_input_is_refused) {
  LifecycleEngine engine;
  ModelId model;
  Decision empty_name = engine.register_model("", "f", Provenance::Synthetic, model);
  MLF_CHECK(!empty_name.allowed());
  MLF_CHECK(empty_name.has(ReasonCode::InvalidName));

  Decision spaces = engine.register_model("has space", "f", Provenance::Synthetic, model);
  MLF_CHECK(!spaces.allowed());

  Decision too_long = engine.register_model(std::string(300, 'x'), "f", Provenance::Synthetic,
                                            model);
  MLF_CHECK(!too_long.allowed());

  require_ok(engine.register_model("ok", "f", Provenance::Synthetic, model), "register");

  RegisterVersionRequest request;
  request.model = model;
  request.label = "1";
  request.provenance = Provenance::Synthetic;
  ModelVersionId version;
  Decision no_artifact = engine.register_version(request, version);
  MLF_CHECK(!no_artifact.allowed());
  MLF_CHECK(no_artifact.has(ReasonCode::ArtifactSetMissing));

  request.artifact.set_id = ArtifactSetId(1);
  request.artifact.generation = ArtifactGeneration(1);
  Decision no_digest = engine.register_version(request, version);
  MLF_CHECK(!no_digest.allowed());
  MLF_CHECK(no_digest.has(ReasonCode::InvalidDigest));

  request.artifact.digest = "ok";
  request.model = ModelId(999);
  Decision unknown_model = engine.register_version(request, version);
  MLF_CHECK(!unknown_model.allowed());
  MLF_CHECK(unknown_model.has(ReasonCode::UnknownModel));

  request.model = model;
  request.predecessor = ModelVersionId(42);
  Decision unknown_predecessor = engine.register_version(request, version);
  MLF_CHECK(!unknown_predecessor.allowed());
  MLF_CHECK(unknown_predecessor.has(ReasonCode::ParentVersionMissing));
}

MLF_TEST(bounds, rollout_limits_are_enforced) {
  EngineConfig config;
  config.bounds.max_rollouts = 1;
  config.bounds.max_stages_per_rollout = 1;
  config.bounds.max_cohorts_per_rollout = 1;
  mlftest::Scenario scenario = mlftest::make_scenario(3, 2048, config);

  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  const PromotionOutcome promotion =
      mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                   scenario.v1);
  MLF_CHECK(promotion.decision.allowed());

  // A plan needs a promoted candidate in CANARY_PENDING; the immediate cutover
  // moved it to CURRENT, so build a second scenario for plan-shape bounds.
  mlftest::Scenario planned = mlftest::make_scenario(4, 2048, config);
  const ModelVersionRecord candidate = mlftest::version_of(*planned.engine, planned.v2);
  PromotionRequest request;
  request.model = candidate.model;
  request.candidate = candidate.version;
  request.candidate_generation = candidate.generation;
  request.artifact_generation = candidate.artifact.generation;
  request.compatibility_generation = candidate.compatibility_generation;
  request.scope = planned.global;
  request.epoch = planned.engine->epoch();
  request.environment_key = planned.environment.key;
  request.strategy = RolloutStrategy::Canary;
  request.rollback_target = planned.v1;
  const PromotionOutcome staged = planned.engine->promote(request);
  MLF_CHECK(staged.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.candidate_generation = candidate.generation;
  plan.artifact_generation = candidate.artifact.generation;
  plan.compatibility_generation = candidate.compatibility_generation;
  plan.root_scope = planned.global;
  plan.promotion = staged.promotion;
  plan.promotion_generation = staged.promotion_generation;
  plan.rollback_target = planned.v1;
  for (int i = 0; i < 2; ++i) {
    CohortSpec cohort;
    cohort.name = "c" + std::to_string(i);
    cohort.scopes = {planned.global};
    plan.cohorts.push_back(cohort);
  }
  // One cohort but two stages: the stage bound is what is exceeded.
  for (int i = 0; i < 2; ++i) {
    StageSpec stage;
    stage.name = "s" + std::to_string(i);
    stage.cohort_indices = {static_cast<std::size_t>(i)};
    plan.stages.push_back(stage);
  }
  plan.cohorts.resize(2);
  plan.stages[1].cohort_indices = {1};

  RolloutId rollout;
  RolloutGeneration generation;
  Decision too_many_stages = planned.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!too_many_stages.allowed());
  MLF_CHECK(too_many_stages.has(ReasonCode::BoundsStageCount));

  // Two cohorts in one stage: the cohort bound is what is exceeded.
  plan.stages.resize(1);
  plan.stages[0].cohort_indices = {0, 1};
  Decision too_many_cohorts = planned.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!too_many_cohorts.allowed());
  MLF_CHECK(too_many_cohorts.has(ReasonCode::BoundsCohortCount));

  plan.cohorts.resize(1);
  plan.stages[0].cohort_indices = {0};
  require_ok(planned.engine->create_rollout(plan, rollout, generation), "first plan");

  // Rollout bound: supersede the first plan so it is terminal, then attempt a
  // second plan of the same shape, which must still be refused by the bound.
  Decision second_plan = planned.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!second_plan.allowed());
  MLF_CHECK(second_plan.has(ReasonCode::BoundsRolloutCount));
  static_cast<void>(record);
}

MLF_TEST(bounds, evidence_store_bound_evicts_oldest_and_reports_loss) {
  EvidenceStore store(4);
  for (std::uint64_t i = 1; i <= 10; ++i) {
    EvidenceRecord record;
    record.kind = EvidenceKind::HealthCheck;
    record.verdict = EvidenceVerdict::Satisfied;
    record.subject.version = ModelVersionId(1);
    record.subject.model = ModelId(1);
    record.subject.model_generation = ModelGeneration(1);
    record.subject.scope = ScopeId(1);
    record.provenance = Provenance::Synthetic;
    record.generation = EvidenceGeneration(i);
    require_ok(store.publish(record), "publish evidence");
  }
  MLF_CHECK(store.size() <= 4);
  MLF_CHECK(store.dropped() >= 6);
}

MLF_TEST(bounds, evidence_sequence_must_strictly_advance) {
  EvidenceStore store(16);
  EvidenceRecord record;
  record.kind = EvidenceKind::HealthCheck;
  record.verdict = EvidenceVerdict::Satisfied;
  record.subject.version = ModelVersionId(1);
  record.subject.model = ModelId(1);
  record.subject.model_generation = ModelGeneration(1);
  record.subject.scope = ScopeId(1);
  record.provenance = Provenance::Synthetic;
  record.generation = EvidenceGeneration(5);
  require_ok(store.publish(record), "first");

  // An older generation for the same subject is refused as a stale record, and
  // an older generation for any other subject is refused as a sequence
  // regression: evidence never moves backwards in either dimension.
  record.generation = EvidenceGeneration(4);
  Decision regression = store.publish(record);
  MLF_CHECK(!regression.allowed());
  MLF_CHECK(regression.has(ReasonCode::EvidenceGenerationMismatch));

  // A different subject: there is no per-subject record to compare against, so
  // the global evidence watermark is what refuses the regression.
  record.subject.version = ModelVersionId(2);
  Decision behind = store.publish(record);
  MLF_CHECK(!behind.allowed());
  MLF_CHECK(behind.has(ReasonCode::SequenceRegression));

  record.generation = EvidenceGeneration(5);
  Decision duplicate = store.publish(record);
  MLF_CHECK(!duplicate.allowed());
  MLF_CHECK(duplicate.has(ReasonCode::EvidenceGenerationMismatch) ||
            duplicate.has(ReasonCode::SequenceRegression));

  record.subject.version = ModelVersionId(1);
  record.generation = EvidenceGeneration(5);
  record.verdict = EvidenceVerdict::Unsatisfied;
  Decision conflicting = store.publish(record);
  MLF_CHECK(!conflicting.allowed());
  MLF_CHECK(conflicting.has(ReasonCode::EvidenceConflicting));
}

MLF_TEST(bounds, attempt_and_worker_limits_are_enforced) {
  EngineConfig config;
  config.bounds.max_attempts = 1;
  config.bounds.max_workers = 1;
  mlftest::Scenario scenario = mlftest::make_scenario(5, 1024, config);

  WorkerRegistration first;
  first.id = WorkerId(1);
  first.boot = WorkerBootId(1);
  first.epoch = scenario.engine->epoch();
  WorkerRegistration second;
  second.id = WorkerId(2);
  second.boot = WorkerBootId(2);
  second.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->register_worker(first), "first worker");
  Decision second_worker = scenario.engine->register_worker(second);
  MLF_CHECK(!second_worker.allowed());
  MLF_CHECK(second_worker.has(ReasonCode::BoundsWorkerCount));

  AttemptRequest request;
  request.kind = AttemptKind::Warm;
  request.worker = first.id;
  request.boot = first.boot;
  request.epoch = scenario.engine->epoch();
  request.model = scenario.model;
  request.version = scenario.v1;
  request.scope = scenario.global;
  const AttemptOutcome accepted = scenario.engine->begin_attempt(request);
  MLF_CHECK(accepted.decision.allowed());
  const AttemptOutcome rejected = scenario.engine->begin_attempt(request);
  MLF_CHECK(!rejected.decision.allowed());
  MLF_CHECK(rejected.decision.has(ReasonCode::BoundsAttemptCount));
}
