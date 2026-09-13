// Rollout plans, canary stages, acceptance and failure criteria.
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

/// A two-stage canary plan: stage 0 moves the canary scope, stage 1 the rest.
bool build(const char* label, Staged& out) {
  out.scenario = mlftest::make_scenario(41);
  const ModelVersionRecord candidate = mlftest::version_of(*out.scenario.engine, out.scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.candidate_generation = candidate.generation;
  promotion.artifact_generation = candidate.artifact.generation;
  promotion.compatibility_generation = candidate.compatibility_generation;
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
  plan.candidate_generation = candidate.generation;
  plan.artifact_generation = candidate.artifact.generation;
  plan.compatibility_generation = candidate.compatibility_generation;
  plan.root_scope = out.scenario.global;
  plan.strategy = RolloutStrategy::Canary;
  plan.promotion = out.promotion.promotion;
  plan.promotion_generation = out.promotion.promotion_generation;
  plan.rollback_target = out.scenario.v1;
  plan.manual_progression = false;

  CohortSpec canary;
  canary.name = "canary";
  canary.scopes = {out.scenario.cluster};
  canary.traffic_percent = 5;
  plan.cohorts.push_back(canary);

  CohortSpec fleet;
  fleet.name = "fleet";
  fleet.scopes = {out.scenario.global};
  fleet.traffic_percent = 100;
  plan.cohorts.push_back(fleet);

  Criterion health;
  health.kind = EvidenceKind::HealthCheck;
  health.comparison = CriterionComparison::AtLeast;
  health.threshold = 0.99;

  StageSpec first;
  first.name = label;
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
  request.stage = plan.stage_entered ? plan.stages[plan.current_stage_index].id
                                     : RolloutStageId{};
  request.stage_generation = plan.current_stage_generation;
  request.scope_generation = plan.root_scope_generation;
  request.epoch = staged.scenario.engine->epoch();
  return request;
}

}  // namespace

MLF_TEST(canary, a_plan_requires_a_committed_promotion) {
  mlftest::Scenario scenario = mlftest::make_scenario(42);
  RolloutPlanRequest plan;
  plan.model = scenario.model;
  plan.candidate = scenario.v2;
  plan.root_scope = scenario.global;
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
  const Decision decision = scenario.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::PromotionNotAuthorized));
}

MLF_TEST(canary, cohort_scopes_must_lie_inside_the_root_scope) {
  mlftest::Scenario scenario = mlftest::make_scenario(43);
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.candidate_generation = candidate.generation;
  promotion.artifact_generation = candidate.artifact.generation;
  promotion.compatibility_generation = candidate.compatibility_generation;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  ScopeId unrelated;
  require_ok(scenario.engine->register_scope("global/cluster:other", unrelated), "unrelated");
  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.cluster;
  plan.promotion = outcome.promotion;
  CohortSpec cohort;
  cohort.name = "c";
  cohort.scopes = {unrelated};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "s";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  RolloutId rollout;
  RolloutGeneration generation;
  const Decision decision = scenario.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::CohortScopeInvalid));
}

MLF_TEST(canary, a_cohort_may_belong_to_exactly_one_stage) {
  Staged staged;
  MLF_CHECK(build("canary", staged));
  // Re-plan with a duplicated cohort.
  mlftest::Scenario scenario = mlftest::make_scenario(44);
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
  CohortSpec cohort;
  cohort.name = "c";
  cohort.scopes = {scenario.global};
  plan.cohorts.push_back(cohort);
  StageSpec first;
  first.name = "s1";
  first.cohort_indices = {0};
  plan.stages.push_back(first);
  StageSpec second;
  second.name = "s2";
  second.cohort_indices = {0};
  plan.stages.push_back(second);
  RolloutId rollout;
  RolloutGeneration generation;
  const Decision decision = scenario.engine->create_rollout(plan, rollout, generation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::StageOrderViolation));
}

MLF_TEST(canary, stage_advance_requires_current_acceptance_evidence) {
  Staged staged;
  MLF_CHECK(build("canary", staged));

  RolloutProgressRequest request = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(request), "begin");

  request = progress_for(staged);
  const Decision premature = staged.scenario.engine->advance_stage(request);
  MLF_CHECK(!premature.allowed());
  MLF_CHECK(premature.has(ReasonCode::StageFeedbackIncomplete));

  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, plan,
                                             staged.scenario.cluster, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.995),
             "publish stage evidence");

  request = progress_for(staged);
  const Decision advanced = staged.scenario.engine->advance_stage(request);
  MLF_CHECK(advanced.allowed());

  const RolloutPlan after = *staged.scenario.engine->rollout(staged.rollout);
  MLF_CHECK_EQ(after.current_stage_index, 1u);
  MLF_CHECK(!after.completed);
  MLF_CHECK_EQ(after.history.size(), 3u);

  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, after,
                                             staged.scenario.global, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.999),
             "publish fleet evidence");
  RolloutProgressRequest final_request = progress_for(staged);
  const Decision completed = staged.scenario.engine->advance_stage(final_request);
  MLF_CHECK(completed.allowed());
  const RolloutPlan finished = *staged.scenario.engine->rollout(staged.rollout);
  MLF_CHECK(finished.completed);
  MLF_CHECK_EQ(std::string(to_string(mlftest::version_of(*staged.scenario.engine, staged.scenario.v2).state)),
               std::string("CURRENT"));
  MLF_CHECK_EQ(staged.scenario.engine->query_authority(staged.scenario.global).binding->version.raw(),
               staged.scenario.v2.raw());
}

MLF_TEST(canary, a_tripped_failure_criterion_blocks_advance_and_demands_a_decision) {
  mlftest::Scenario scenario = mlftest::make_scenario(45);
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
  cohort.name = "canary";
  cohort.scopes = {scenario.cluster};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "canary";
  stage.cohort_indices = {0};
  Criterion failure;
  failure.kind = EvidenceKind::ErrorRate;
  failure.comparison = CriterionComparison::AtLeast;
  failure.threshold = 0.10;
  stage.failure.push_back(failure);
  plan.stages.push_back(stage);

  RolloutId rollout;
  RolloutGeneration generation;
  require_ok(scenario.engine->create_rollout(plan, rollout, generation), "plan");

  RolloutProgressRequest request;
  request.rollout = rollout;
  request.rollout_generation = generation;
  request.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->begin_rollout(request), "begin");

  const RolloutPlan entered = *scenario.engine->rollout(rollout);
  require_ok(mlftest::publish_stage_evidence(*scenario.engine, candidate, entered,
                                             scenario.cluster, EvidenceKind::ErrorRate,
                                             EvidenceVerdict::Satisfied, 0.42),
             "publish error rate");

  request.stage = entered.stages[0].id;
  request.stage_generation = entered.current_stage_generation;
  const Decision decision = scenario.engine->advance_stage(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::StageFailureThresholdExceeded));
  MLF_CHECK_EQ(std::string(to_string(decision.outcome())),
               std::string("ROLLBACK_DECISION_REQUIRED"));

  const RolloutPlan failed = *scenario.engine->rollout(rollout);
  MLF_CHECK(failed.failed);
  MLF_CHECK_EQ(std::string(to_string(mlftest::version_of(*scenario.engine, scenario.v2).state)),
               std::string("CANARY_FAILED"));
}

MLF_TEST(canary, duplicated_stage_completion_is_refused) {
  Staged staged;
  MLF_CHECK(build("canary", staged));
  RolloutProgressRequest request = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(request), "begin");

  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  const ModelVersionRecord candidate = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, plan,
                                             staged.scenario.cluster, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.999),
             "evidence");

  RolloutProgressRequest first = progress_for(staged);
  require_ok(staged.scenario.engine->advance_stage(first), "first advance");

  // Replaying the same stage generation must be refused.
  RolloutProgressRequest replay = first;
  const Decision repeated = staged.scenario.engine->advance_stage(replay);
  MLF_CHECK(!repeated.allowed());
  MLF_CHECK(repeated.has(ReasonCode::RolloutStageGenerationMismatch) ||
            repeated.has(ReasonCode::StageCompletionDuplicate));
}

MLF_TEST(canary, begin_is_refused_twice) {
  Staged staged;
  MLF_CHECK(build("canary", staged));
  RolloutProgressRequest request = progress_for(staged);
  require_ok(staged.scenario.engine->begin_rollout(request), "begin");
  const Decision again = staged.scenario.engine->begin_rollout(request);
  MLF_CHECK(!again.allowed());
  MLF_CHECK(again.has(ReasonCode::StageOrderViolation));
}

MLF_TEST(canary, manual_progression_requires_explicit_approval) {
  mlftest::Scenario scenario = mlftest::make_scenario(46);
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::ManualStages;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.global;
  plan.promotion = outcome.promotion;
  plan.manual_progression = true;
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

  RolloutProgressRequest request;
  request.rollout = rollout;
  request.rollout_generation = generation;
  request.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->begin_rollout(request), "begin");

  const RolloutPlan entered = *scenario.engine->rollout(rollout);
  request.stage = entered.stages[0].id;
  request.stage_generation = entered.current_stage_generation;
  const Decision denied = scenario.engine->advance_stage(request);
  MLF_CHECK(!denied.allowed());
  MLF_CHECK(denied.has(ReasonCode::PolicyRequiresApproval));

  request.administrative_approval = true;
  const Decision approved = scenario.engine->advance_stage(request);
  MLF_CHECK(approved.allowed());
}
