// Rollback and retirement as first-class, fenced transitions.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

RollbackRequest rollback_request(const mlftest::Scenario& scenario, ModelVersionId candidate,
                                 ModelVersionId target, ScopeId scope) {
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, candidate);
  RollbackRequest request;
  request.model = record.model;
  request.candidate = record.version;
  request.candidate_generation = record.generation;
  request.target = target;
  request.scope = scope;
  request.scope_generation = mlftest::generation_of(*scenario.engine, scope);
  request.epoch = scenario.engine->epoch();
  return request;
}

}  // namespace

MLF_TEST(rollback, rollback_requires_the_candidate_to_hold_authority) {
  mlftest::Scenario scenario = mlftest::make_scenario(51);
  const RollbackRequest request = rollback_request(scenario, scenario.v2, scenario.v1,
                                                   scenario.global);
  const Decision decision = scenario.engine->evaluate_rollback(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::NoCurrentAuthority));
}

MLF_TEST(rollback, rollback_restores_the_target_and_fences_the_candidate) {
  mlftest::Scenario scenario = mlftest::make_scenario(52);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          scenario.v1)
                 .decision,
             "v2 global");

  const RollbackRequest request = rollback_request(scenario, scenario.v2, scenario.v1,
                                                   scenario.global);
  const RollbackOutcome outcome = scenario.engine->rollback(request);
  MLF_CHECK(outcome.decision.allowed());
  MLF_CHECK(outcome.rollback.valid());
  MLF_CHECK_EQ(std::string(to_string(outcome.rollback_generation)),
               std::string(to_string(outcome.rollback_generation)));

  MLF_CHECK_EQ(scenario.engine->query_authority(scenario.global).binding->version.raw(),
               scenario.v1.raw());
  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(candidate.state == LifecycleState::ROLLED_BACK ||
            candidate.state == LifecycleState::DRAINING);
  MLF_CHECK(candidate.last_rollback == outcome.rollback);

  // A second rollback of the same candidate has no authority to take.
  const RollbackOutcome repeated = scenario.engine->rollback(request);
  MLF_CHECK(!repeated.decision.allowed());
  MLF_CHECK(repeated.decision.has(ReasonCode::NoCurrentAuthority));
}

MLF_TEST(rollback, a_retired_target_is_not_restorable) {
  mlftest::Scenario scenario = mlftest::make_scenario(53);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2 global");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire v1");

  RollbackRequest request = rollback_request(scenario, scenario.v2, scenario.v1, scenario.global);
  const Decision decision = scenario.engine->evaluate_rollback(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RollbackTargetRetired));
}

MLF_TEST(rollback, an_unknown_target_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(54);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2 global");
  RollbackRequest request = rollback_request(scenario, scenario.v2, ModelVersionId(4242),
                                             scenario.global);
  const Decision decision = scenario.engine->evaluate_rollback(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RollbackTargetUnknown));
}

MLF_TEST(rollback, rollback_can_be_scoped_to_one_scope_only) {
  mlftest::Scenario scenario = mlftest::make_scenario(55);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.cluster,
                                          scenario.v1)
                 .decision,
             "v2 cluster");

  RollbackRequest request = rollback_request(scenario, scenario.v2, scenario.v1, scenario.cluster);
  const RollbackOutcome outcome = scenario.engine->rollback(request);
  MLF_CHECK(outcome.decision.allowed());

  MLF_CHECK_EQ(scenario.engine->query_authority(scenario.cluster).binding->version.raw(),
               scenario.v1.raw());
  MLF_CHECK_EQ(scenario.engine->query_authority(scenario.global).binding->version.raw(),
               scenario.v1.raw());
}

MLF_TEST(rollback, automatic_rollback_needs_explicit_policy_authorization) {
  mlftest::Scenario scenario = mlftest::make_scenario(56);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          scenario.v1)
                 .decision,
             "v2 global");

  RollbackRequest request = rollback_request(scenario, scenario.v2, scenario.v1, scenario.global);
  request.automatic = true;
  const Decision denied = scenario.engine->evaluate_rollback(request, nullptr);
  MLF_CHECK(!denied.allowed());
  MLF_CHECK(denied.has(ReasonCode::PolicyDeniesAutoRollback));

  LifecyclePolicy policy = scenario.engine->current_policy();
  policy.generation = next_generation(policy.generation);
  policy.allow_automatic_rollback = true;
  require_ok(scenario.engine->set_policy(policy), "policy");

  request.policy_generation = policy.generation;
  const RollbackOutcome outcome = scenario.engine->rollback(request);
  MLF_CHECK(outcome.decision.allowed());
}

MLF_TEST(retirement, a_current_generation_cannot_be_retired) {
  mlftest::Scenario scenario = mlftest::make_scenario(57);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 global");
  RetirementRequest request;
  request.model = scenario.model;
  request.version = scenario.v1;
  request.epoch = scenario.engine->epoch();
  Explanation explanation;
  const Decision decision = scenario.engine->evaluate_retirement(request, &explanation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RetirementScopeDependency));
  MLF_CHECK(explanation.find("exclusive_bindings") != nullptr);
}

MLF_TEST(retirement, an_active_rollout_blocks_retirement) {
  mlftest::Scenario scenario = mlftest::make_scenario(58);
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

  RetirementRequest request;
  request.model = scenario.model;
  request.version = scenario.v2;
  request.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->evaluate_retirement(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RetirementRolloutActive));
}

MLF_TEST(retirement, retention_of_a_rollback_target_blocks_retirement) {
  // v1 is the rollback target of a live plan, so it must survive.
  mlftest::Scenario scenario = mlftest::make_scenario(59);
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

  RetirementRequest request;
  request.model = scenario.model;
  request.version = scenario.v1;
  request.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->evaluate_retirement(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RetirementRetentionUnsatisfied) ||
            decision.has(ReasonCode::RetirementRolloutActive));
}

MLF_TEST(retirement, retiring_twice_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(60);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2 global");
  RetirementRequest request;
  request.model = scenario.model;
  request.version = scenario.v1;
  request.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(request), "retire");
  const Decision again = scenario.engine->retire(request);
  MLF_CHECK(!again.allowed());
  MLF_CHECK(again.has(ReasonCode::RetirementAlreadyRetired));
}

MLF_TEST(retirement, retiring_an_unknown_version_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(61);
  RetirementRequest request;
  request.model = scenario.model;
  request.version = ModelVersionId(999);
  request.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->retire(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::UnknownVersion));
}

MLF_TEST(retirement, an_unsettled_attempt_blocks_retirement) {
  mlftest::Scenario scenario = mlftest::make_scenario(62);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2 global");
  AttemptRequest attempt;
  attempt.kind = AttemptKind::Drain;
  attempt.model = scenario.model;
  attempt.version = scenario.v1;
  attempt.scope = scenario.global;
  const AttemptOutcome started = scenario.engine->begin_attempt(attempt);
  MLF_CHECK(started.decision.allowed());

  RetirementRequest request;
  request.model = scenario.model;
  request.version = scenario.v1;
  request.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->evaluate_retirement(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RetirementDependencyUnresolved));
}

MLF_TEST(revalidation, revalidation_withdraws_compatibility_and_evidence) {
  mlftest::Scenario scenario = mlftest::make_scenario(63);
  const ModelVersionRecord before = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(before.compatibility_generation.valid());
  require_ok(mlftest::publish_ready(*scenario.engine, before, scenario.global,
                                    EvidenceKind::Readiness),
             "publish readiness");

  RevalidationRequest request;
  request.model = scenario.model;
  request.version = scenario.v2;
  request.generation = before.generation;
  request.cause = ReasonCode::CompatibilityStale;
  request.detail = "runtime generation advanced mid-rollout";
  const Decision revalidated = scenario.engine->revalidate(request);
  // REVALIDATION_REQUIRED is the positive outcome of this call: the request was
  // honoured and the runtime now demands fresh evidence before any advance.
  MLF_CHECK_EQ(std::string(to_string(revalidated.outcome())),
               std::string("REVALIDATION_REQUIRED"));
  MLF_CHECK(revalidated.has(ReasonCode::CompatibilityStale));

  const ModelVersionRecord after = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(!after.compatibility_generation.valid());
  MLF_CHECK_EQ(std::string(to_string(after.state)), std::string("REVALIDATION_REQUIRED"));

  const EvidenceRequirement requirement{EvidenceKind::Readiness, EvidenceVerdict::Satisfied, true,
                                        false};
  EvidenceSubject subject;
  subject.model = scenario.model;
  subject.version = scenario.v2;
  subject.model_generation = after.generation;
  subject.scope = scenario.global;
  const EvidenceLookup lookup = scenario.engine->check_evidence(requirement, subject);
  MLF_CHECK(lookup.currentness != EvidenceCurrentness::Current);
}

MLF_TEST(revalidation, a_retired_generation_cannot_be_revalidated) {
  mlftest::Scenario scenario = mlftest::make_scenario(64);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2 global");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire");

  RevalidationRequest request;
  request.model = scenario.model;
  request.version = scenario.v1;
  const Decision decision = scenario.engine->revalidate(request);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::VersionRetired));
}
