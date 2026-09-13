// Rollout hardening: supersession, duplicates, skips and rollback races.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

struct Staged {
  mlftest::Scenario scenario;
  PromotionOutcome promotion{};
  RolloutId rollout{};
  RolloutGeneration generation{};
};

bool build_staged(Staged& out, std::uint64_t seed) {
  out.scenario = mlftest::make_scenario(seed);
  const ModelVersionRecord candidate = mlftest::version_of(*out.scenario.engine, out.scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = out.scenario.global;
  promotion.epoch = out.scenario.engine->epoch();
  promotion.environment_key = out.scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = out.scenario.v1;
  out.promotion = out.scenario.engine->promote(promotion);
  if (!out.promotion.decision.allowed()) return false;

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = out.scenario.global;
  plan.promotion = out.promotion.promotion;
  plan.rollback_target = out.scenario.v1;
  CohortSpec canary;
  canary.name = "canary";
  canary.scopes = {out.scenario.cluster};
  plan.cohorts.push_back(canary);
  CohortSpec fleet;
  fleet.name = "fleet";
  fleet.scopes = {out.scenario.global};
  plan.cohorts.push_back(fleet);
  Criterion health;
  health.kind = EvidenceKind::HealthCheck;
  health.comparison = CriterionComparison::AtLeast;
  health.threshold = 0.9;
  StageSpec first;
  first.name = "canary";
  first.cohort_indices = {0};
  first.acceptance.push_back(health);
  plan.stages.push_back(first);
  StageSpec second;
  second.name = "fleet";
  second.cohort_indices = {1};
  second.acceptance.push_back(health);
  plan.stages.push_back(second);
  return out.scenario.engine->create_rollout(plan, out.rollout, out.generation).allowed();
}

RolloutProgressRequest progress_for(const Staged& staged) {
  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  RolloutProgressRequest request;
  request.rollout = staged.rollout;
  request.rollout_generation = staged.generation;
  request.stage = plan.stage_entered ? plan.stages[plan.current_stage_index].id : RolloutStageId{};
  request.stage_generation = plan.current_stage_generation;
  request.epoch = staged.scenario.engine->epoch();
  return request;
}

}  // namespace

MLF_TEST(rollout_adversarial, advancing_a_stage_that_was_never_entered_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 231));
  RolloutProgressRequest request = progress_for(staged);
  const Decision decision = staged.scenario.engine->advance_stage(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::StageOrderViolation));
}

MLF_TEST(rollout_adversarial, failing_a_stage_that_was_never_entered_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 232));
  RolloutProgressRequest request = progress_for(staged);
  const Decision decision = staged.scenario.engine->fail_stage(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::StageOrderViolation));
}

MLF_TEST(rollout_adversarial, a_duplicate_stage_completion_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 233));
  RolloutProgressRequest begin = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  const RolloutPlan entered = *staged.scenario.engine->rollout(staged.rollout);
  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, entered,
                                             staged.scenario.cluster, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.99),
             "evidence");

  RolloutProgressRequest first = progress_for(staged);
  require_ok(staged.scenario.engine->advance_stage(first), "advance");
  const Decision replay = staged.scenario.engine->advance_stage(first);
  MLF_CHECK(!replay.allowed());
}

MLF_TEST(rollout_adversarial, a_skipped_stage_is_detected) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 234));
  RolloutProgressRequest begin = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  RolloutProgressRequest future;
  future.rollout = staged.rollout;
  future.rollout_generation = staged.generation;
  future.stage = plan.stages[1].id;  // the stage that has not been entered
  future.stage_generation = plan.current_stage_generation;
  future.epoch = staged.scenario.engine->epoch();
  const Decision decision = staged.scenario.engine->advance_stage(future);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::StageOrderViolation));
}

MLF_TEST(rollout_adversarial, a_rollback_after_a_failed_canary_stops_further_progress) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 235));
  RolloutProgressRequest begin = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  const RolloutPlan entered = *staged.scenario.engine->rollout(staged.rollout);
  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, entered,
                                             staged.scenario.cluster, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.99),
             "evidence");
  RolloutProgressRequest advance = progress_for(staged);
  require_ok(staged.scenario.engine->advance_stage(advance), "advance to fleet stage");

  RollbackRequest rollback;
  rollback.model = staged.scenario.model;
  rollback.candidate = staged.scenario.v2;
  rollback.target = staged.scenario.v1;
  rollback.rollout = staged.rollout;
  rollback.rollout_generation = staged.generation;
  rollback.scope = staged.scenario.global;
  rollback.epoch = staged.scenario.engine->epoch();
  const RollbackOutcome outcome = staged.scenario.engine->rollback(rollback);
  MLF_CHECK(outcome.decision.allowed());

  const RolloutPlan after = *staged.scenario.engine->rollout(staged.rollout);
  MLF_CHECK(!after.live());
  MLF_CHECK(!after.last_rollback.valid() == false);

  // A late canary success cannot re-promote the candidate.
  RolloutProgressRequest late = progress_for(staged);
  late.stage = after.stages[0].id;
  const Decision refused = staged.scenario.engine->advance_stage(late);
  MLF_CHECK(!refused.allowed());

  MLF_CHECK_EQ(staged.scenario.engine->query_authority(staged.scenario.global).binding->version.raw(),
               staged.scenario.v1.raw());
}

MLF_TEST(rollout_adversarial, a_second_live_plan_for_the_same_candidate_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 236));

  RolloutPlanRequest plan;
  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = staged.scenario.global;
  plan.promotion = staged.promotion.promotion;
  plan.rollback_target = staged.scenario.v1;
  plan.max_blast_radius_percent = 100;
  CohortSpec cohort;
  cohort.name = "everything";
  cohort.scopes = {staged.scenario.global};
  cohort.traffic_percent = 100;
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "s";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  RolloutId rollout;
  RolloutGeneration generation;
  const Decision decision = staged.scenario.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::PromotionCommitDuplicate));
}

MLF_TEST(rollout_adversarial, two_stale_epochs_cannot_promote) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 237));
  CoordinatorEpoch advanced;
  require_ok(staged.scenario.engine->advance_epoch(staged.scenario.engine->epoch(), advanced),
             "advance epoch");

  RegisterVersionRequest request;
  request.model = staged.scenario.model;
  request.label = "9.0.0";
  request.artifact.set_id = ArtifactSetId(90);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "late";
  request.requirements = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2).requirements;
  request.provenance = Provenance::Synthetic;
  ModelVersionId v9;
  require_ok(staged.scenario.engine->register_version(request, v9), "register");
  require_ok(staged.scenario.engine->publish_compatibility(v9, staged.scenario.environment,
                                                           CompatibilityOutcome::COMPATIBLE, "x",
                                                           Provenance::Synthetic),
             "compat");

  PromotionRequest promotion;
  promotion.model = staged.scenario.model;
  promotion.candidate = v9;
  promotion.scope = staged.scenario.global;
  promotion.epoch = CoordinatorEpoch(1);
  promotion.environment_key = staged.scenario.environment.key;
  promotion.rollback_target = staged.scenario.v1;
  const Decision decision = staged.scenario.engine->promote(promotion).decision;
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::CoordinatorEpochStale));
}

MLF_TEST(rollout_adversarial, a_candidate_that_dies_before_the_canary_cannot_begin) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 238));
  // Withdraw the evidence the stage needs before it can start.
  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();

  // Revise the candidate: the plan bindings are now stale, so entry fails.
  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  ReviseVersionRequest revise;
  revise.model = staged.scenario.model;
  revise.version = staged.scenario.v2;
  revise.expected_generation = candidate.generation;
  revise.artifact.set_id = candidate.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(candidate.artifact.generation.raw() + 1);
  revise.artifact.digest = "post-plan";
  revise.requirements = candidate.requirements;
  require_ok(staged.scenario.engine->revise_version(revise), "revise");

  const Decision decision = staged.scenario.engine->begin_rollout(begin);
  MLF_CHECK(!decision.allowed());
  static_cast<void>(plan);
}

MLF_TEST(rollout_adversarial, stage_entry_with_a_stale_plan_binding_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 239));
  LifecyclePolicy policy = staged.scenario.engine->current_policy();
  policy.generation = next_generation(policy.generation);
  require_ok(staged.scenario.engine->set_policy(policy), "policy change");

  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();
  const Decision decision = staged.scenario.engine->begin_rollout(begin);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RolloutPlanStale));
  static_cast<void>(policy);
}
