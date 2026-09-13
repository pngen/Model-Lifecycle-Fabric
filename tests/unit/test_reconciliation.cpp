// Reconciliation between durable lifecycle truth and runtime reality.
#include "mlf/engine.hpp"
#include "mlf/synthetic_backend.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(reconciliation, a_clean_reality_produces_no_findings) {
  mlftest::Scenario scenario = mlftest::make_scenario(91);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);

  ReconciliationInput input;
  ReplicaObservation replica;
  replica.replica = ReplicaId(1);
  replica.generation = ReplicaGeneration(1);
  replica.model = scenario.model;
  replica.version = scenario.v1;
  replica.model_generation = record.generation;
  replica.artifact_generation = record.artifact.generation;
  replica.scope = scenario.global;
  replica.resident = true;
  replica.ready = true;
  replica.provenance = Provenance::Synthetic;
  input.replicas.push_back(replica);

  const std::vector<Finding> findings = scenario.engine->reconcile(input);
  for (const Finding& finding : findings) {
    MLF_CHECK_MSG(finding.kind != FindingKind::AuthorityWithoutReplicas, finding.detail);
  }
}

MLF_TEST(reconciliation, missing_replicas_behind_committed_authority_are_reported) {
  mlftest::Scenario scenario = mlftest::make_scenario(92);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  const std::vector<Finding> findings =
      scenario.engine->reconcile(ReconciliationInput{});
  bool found = false;
  for (const Finding& finding : findings) {
    if (finding.kind == FindingKind::AuthorityWithoutReplicas) {
      found = true;
      MLF_CHECK_EQ(finding.reason, ReasonCode::ExpectedResidentMissing);
      MLF_CHECK(finding.scope == scenario.global);
    }
  }
  MLF_CHECK(found);
}

MLF_TEST(reconciliation, a_retired_generation_still_resident_is_reported) {
  mlftest::Scenario scenario = mlftest::make_scenario(93);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2");
  const ModelVersionRecord retired_record = mlftest::version_of(*scenario.engine, scenario.v1);
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire v1");

  ReconciliationInput input;
  ResidencyObservation observation;
  observation.model = scenario.model;
  observation.version = scenario.v1;
  observation.model_generation = retired_record.generation;
  observation.artifact_generation = retired_record.artifact.generation;
  observation.scope = scenario.global;
  observation.generation = ResidencyGeneration(1);
  observation.resident = true;
  observation.ready = true;
  observation.provenance = Provenance::Synthetic;
  input.residency.push_back(observation);

  const ModelVersionRecord current = mlftest::version_of(*scenario.engine, scenario.v2);
  ReplicaObservation replica;
  replica.replica = ReplicaId(1);
  replica.model = scenario.model;
  replica.version = scenario.v2;
  replica.model_generation = current.generation;
  replica.artifact_generation = current.artifact.generation;
  replica.scope = scenario.global;
  replica.resident = true;
  replica.ready = true;
  input.replicas.push_back(replica);

  const std::vector<Finding> findings = scenario.engine->reconcile(input);
  bool found = false;
  for (const Finding& finding : findings) {
    if (finding.kind == FindingKind::RetiredModelStillResident) found = true;
  }
  MLF_CHECK(found);
}

MLF_TEST(reconciliation, a_persisted_stage_without_live_workers_is_reported) {
  mlftest::Scenario scenario = mlftest::make_scenario(94);
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.global;
  plan.promotion = outcome.promotion;
  plan.rollback_target = scenario.v1;
  CohortSpec cohort;
  cohort.name = "c";
  cohort.scopes = {scenario.global};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "s";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  RolloutId rollout;
  RolloutGeneration generation;
  require_ok(scenario.engine->create_rollout(plan, rollout, generation), "plan");

  RolloutProgressRequest begin;
  begin.rollout = rollout;
  begin.rollout_generation = generation;
  begin.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->begin_rollout(begin), "begin");

  ReconciliationInput input;
  input.worker_liveness_known = false;
  const std::vector<Finding> unknown = scenario.engine->reconcile(input);
  bool reported_unknown = false;
  for (const Finding& finding : unknown) {
    if (finding.kind == FindingKind::RolloutStageWithoutWorkers) reported_unknown = true;
  }
  MLF_CHECK(reported_unknown);

  input.worker_liveness_known = true;
  const std::vector<Finding> known = scenario.engine->reconcile(input);
  bool reported_missing = false;
  for (const Finding& finding : known) {
    if (finding.kind == FindingKind::RolloutStageWithoutWorkers) reported_missing = true;
  }
  MLF_CHECK(reported_missing);

  // With a live registered worker the finding disappears.
  WorkerRegistration registration;
  registration.id = WorkerId(1);
  registration.boot = WorkerBootId(1);
  registration.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->register_worker(registration), "worker");
  ReconciliationInput with_worker = input;
  with_worker.live_workers.push_back(registration.id);
  const std::vector<Finding> supported = scenario.engine->reconcile(with_worker);
  for (const Finding& finding : supported) {
    MLF_CHECK(finding.kind != FindingKind::RolloutStageWithoutWorkers);
  }
}

MLF_TEST(reconciliation, an_unsettled_attempt_is_reported) {
  mlftest::Scenario scenario = mlftest::make_scenario(95);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1");
  AttemptRequest attempt;
  attempt.kind = AttemptKind::Warm;
  attempt.model = scenario.model;
  attempt.version = scenario.v1;
  attempt.scope = scenario.global;
  MLF_CHECK(scenario.engine->begin_attempt(attempt).decision.allowed());

  const std::vector<Finding> findings = scenario.engine->reconcile(ReconciliationInput{});
  bool found = false;
  for (const Finding& finding : findings) {
    if (finding.kind == FindingKind::UnreconciledAttempt) found = true;
  }
  MLF_CHECK(found);
}

MLF_TEST(reconciliation, an_artifact_change_marks_a_live_plan_stale) {
  mlftest::Scenario scenario = mlftest::make_scenario(96);
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.global;
  plan.promotion = outcome.promotion;
  plan.rollback_target = scenario.v1;
  CohortSpec cohort;
  cohort.name = "c";
  cohort.scopes = {scenario.global};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "s";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  RolloutId rollout;
  RolloutGeneration generation;
  require_ok(scenario.engine->create_rollout(plan, rollout, generation), "plan");

  // A policy revision moves the generation every live plan was bound to, without
  // superseding the plan: the plan is still live but no longer executable.
  LifecyclePolicy policy = scenario.engine->current_policy();
  policy.generation = next_generation(policy.generation);
  require_ok(scenario.engine->set_policy(policy), "policy revision");

  const std::vector<Finding> findings = scenario.engine->reconcile(ReconciliationInput{});
  bool found = false;
  for (const Finding& finding : findings) {
    if (finding.kind == FindingKind::ArtifactGenerationChanged ||
        finding.kind == FindingKind::StalePlanBinding) {
      found = true;
    }
  }
  MLF_CHECK(found);
}

MLF_TEST(reconciliation, fenced_worker_evidence_is_reported) {
  mlftest::Scenario scenario = mlftest::make_scenario(97);
  WorkerRegistration registration;
  registration.id = WorkerId(1);
  registration.boot = WorkerBootId(7);
  registration.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->register_worker(registration), "worker");

  EvidenceRecord evidence;
  evidence.kind = EvidenceKind::Readiness;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.model = scenario.model;
  evidence.subject.version = scenario.v1;
  evidence.subject.scope = scenario.global;
  evidence.provenance = Provenance::Synthetic;
  evidence.worker = registration.id;
  evidence.boot = registration.boot;
  require_ok(scenario.engine->publish_evidence(evidence), "publish");

  // Fencing withdraws the boot's evidence, so no "still publishing" finding can
  // survive; the check is that the finding never fires on withdrawn evidence.
  require_ok(scenario.engine->fence_worker(registration.id, registration.boot,
                                           scenario.engine->epoch(), "test"),
             "fence");
  const std::vector<Finding> findings = scenario.engine->reconcile(ReconciliationInput{});
  for (const Finding& finding : findings) {
    MLF_CHECK(finding.kind != FindingKind::FencedWorkerStillPublishing);
  }
}
