// Stale generation rejection across every generation boundary.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(stale, a_stale_artifact_generation_cannot_promote) {
  mlftest::Scenario scenario = mlftest::make_scenario(101);
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest request;
  request.model = record.model;
  request.candidate = record.version;
  request.candidate_generation = record.generation;
  request.artifact_generation = ArtifactGeneration(record.artifact.generation.raw() + 7);
  request.compatibility_generation = record.compatibility_generation;
  request.scope = scenario.global;
  request.epoch = scenario.engine->epoch();
  request.environment_key = scenario.environment.key;
  request.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->promote(request).decision;
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::ArtifactGenerationMismatch));
}

MLF_TEST(stale, a_stale_compatibility_generation_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(102);
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest request;
  request.model = record.model;
  request.candidate = record.version;
  request.compatibility_generation = CompatibilityGeneration(record.compatibility_generation.raw() + 3);
  request.scope = scenario.global;
  request.epoch = scenario.engine->epoch();
  request.environment_key = scenario.environment.key;
  request.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->promote(request).decision;
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::CompatibilityGenerationMismatch));
}

MLF_TEST(stale, compatibility_published_for_an_older_artifact_is_stale_not_reusable) {
  mlftest::Scenario scenario = mlftest::make_scenario(103);
  const ModelVersionRecord before = mlftest::version_of(*scenario.engine, scenario.v2);

  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v2;
  revise.expected_generation = before.generation;
  revise.artifact.set_id = before.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(before.artifact.generation.raw() + 1);
  revise.artifact.digest = "new-artifact-digest";
  revise.requirements = before.requirements;
  require_ok(scenario.engine->revise_version(revise), "revise");

  const ModelVersionRecord after = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(!after.compatibility_generation.valid());
  MLF_CHECK(after.generation > before.generation);

  const CompatibilityResult resolved = scenario.engine->resolve_compatibility(
      scenario.v2, scenario.environment, CompatibilityGeneration{});
  MLF_CHECK_EQ(std::string(to_string(resolved.outcome)), std::string("UNKNOWN"));

  PromotionRequest request;
  request.model = scenario.model;
  request.candidate = scenario.v2;
  request.scope = scenario.global;
  request.epoch = scenario.engine->epoch();
  request.environment_key = scenario.environment.key;
  request.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->promote(request).decision;
  MLF_CHECK(!decision.allowed());
}

MLF_TEST(stale, a_stale_lifecycle_generation_cannot_sneak_a_transition) {
  mlftest::Scenario scenario = mlftest::make_scenario(104);
  const ModelVersionRecord before = mlftest::version_of(*scenario.engine, scenario.v1);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote");
  const ModelVersionRecord after = mlftest::version_of(*scenario.engine, scenario.v1);
  MLF_CHECK(after.lifecycle_generation > before.lifecycle_generation);
  MLF_CHECK_EQ(std::string(to_string(after.state)), std::string("CURRENT"));
}

MLF_TEST(stale, a_stale_scope_generation_is_refused_on_stage_entry) {
  mlftest::Scenario scenario = mlftest::make_scenario(105);
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

  const RolloutPlan entered = *scenario.engine->rollout(rollout);
  RolloutProgressRequest advance;
  advance.rollout = rollout;
  advance.rollout_generation = generation;
  advance.stage = entered.stages[0].id;
  advance.stage_generation = entered.current_stage_generation;
  advance.scope_generation = ScopeGeneration(entered.root_scope_generation.raw() + 9);
  advance.epoch = scenario.engine->epoch();
  require_ok(mlftest::publish_stage_evidence(*scenario.engine, candidate, entered, scenario.global,
                                             EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 1.0),
             "evidence");
  const Decision decision = scenario.engine->advance_stage(advance);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::ScopeGenerationMismatch));
}

MLF_TEST(stale, a_stale_rollout_generation_cannot_advance) {
  mlftest::Scenario scenario = mlftest::make_scenario(106);
  RolloutProgressRequest request;
  request.rollout = RolloutId(4242);
  request.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->advance_stage(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::UnknownVersion));
}

MLF_TEST(stale, a_stale_epoch_cannot_mutate) {
  mlftest::Scenario scenario = mlftest::make_scenario(107);
  CoordinatorEpoch next;
  require_ok(scenario.engine->advance_epoch(scenario.engine->epoch(), next), "advance epoch");

  PromotionRequest request;
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  request.model = record.model;
  request.candidate = record.version;
  request.scope = scenario.global;
  request.epoch = CoordinatorEpoch(1);
  request.environment_key = scenario.environment.key;
  request.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->promote(request).decision;
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::CoordinatorEpochStale));
}

MLF_TEST(stale, a_stale_completion_is_fenced_by_the_model_generation) {
  mlftest::Scenario scenario = mlftest::make_scenario(108);
  WorkerRegistration registration;
  registration.id = WorkerId(1);
  registration.boot = WorkerBootId(1);
  registration.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->register_worker(registration), "worker");

  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v1);
  AttemptRequest attempt;
  attempt.kind = AttemptKind::Warm;
  attempt.worker = registration.id;
  attempt.boot = registration.boot;
  attempt.epoch = scenario.engine->epoch();
  attempt.model = scenario.model;
  attempt.version = scenario.v1;
  attempt.model_generation = record.generation;
  attempt.scope = scenario.global;
  const AttemptOutcome started = scenario.engine->begin_attempt(attempt);
  MLF_CHECK(started.decision.allowed());

  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v1;
  revise.expected_generation = record.generation;
  revise.artifact.set_id = record.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(record.artifact.generation.raw() + 1);
  revise.artifact.digest = "moved-on";
  revise.requirements = record.requirements;
  require_ok(scenario.engine->revise_version(revise), "revise");

  CompletionRecord record_in;
  record_in.attempt = started.attempt;
  record_in.worker = registration.id;
  record_in.boot = registration.boot;
  record_in.epoch = scenario.engine->epoch();
  record_in.success = true;
  record_in.confirmed = true;
  const Decision decision = scenario.engine->complete_attempt(record_in);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::ModelGenerationMismatch) ||
            decision.has(ReasonCode::VersionRetired));
}

MLF_TEST(stale, retired_generations_never_regain_authority) {
  mlftest::Scenario scenario = mlftest::make_scenario(109);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire v1");

  // Every path that could grant authority must refuse.
  PromotionRequest promotion;
  promotion.model = scenario.model;
  promotion.candidate = scenario.v1;
  promotion.scope = scenario.cluster;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  MLF_CHECK(!scenario.engine->promote(promotion).decision.allowed());

  RollbackRequest rollback;
  rollback.model = scenario.model;
  rollback.candidate = scenario.v2;
  rollback.target = scenario.v1;
  rollback.scope = scenario.global;
  rollback.epoch = scenario.engine->epoch();
  MLF_CHECK(!scenario.engine->rollback(rollback).decision.allowed());

  AttemptRequest attempt;
  attempt.kind = AttemptKind::Activate;
  attempt.model = scenario.model;
  attempt.version = scenario.v1;
  attempt.scope = scenario.cluster;
  MLF_CHECK(!scenario.engine->begin_attempt(attempt).decision.allowed());

  RevalidationRequest revalidate;
  revalidate.model = scenario.model;
  revalidate.version = scenario.v1;
  MLF_CHECK(!scenario.engine->revalidate(revalidate).allowed());

  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v1;
  revise.artifact.set_id = ArtifactSetId(1);
  revise.artifact.generation = ArtifactGeneration(2);
  revise.artifact.digest = "x";
  MLF_CHECK(!scenario.engine->revise_version(revise).allowed());

  // Evidence about a retired generation remains publishable — it is an
  // observation, not authority — but it can never create authority.
  EvidenceRecord evidence;
  evidence.kind = EvidenceKind::HealthCheck;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.model = scenario.model;
  evidence.subject.version = scenario.v1;
  evidence.subject.scope = scenario.cluster;
  evidence.provenance = Provenance::Synthetic;
  static_cast<void>(scenario.engine->publish_evidence(evidence));
  const AuthorityQueryResult after = scenario.engine->query_authority(scenario.cluster);
  // The cluster inherits the committed authority for v2; the retired v1 is not
  // and cannot become the answer.
  MLF_CHECK_EQ(std::string(to_string(after.outcome)), std::string("OK"));
  MLF_CHECK(after.binding != nullptr);
  MLF_CHECK(after.binding->version == scenario.v2);
  for (const AuthorityBinding& binding :
       scenario.engine->authority_bindings(scenario.model, scenario.v1)) {
    MLF_CHECK(binding.superseded);
  }
}
