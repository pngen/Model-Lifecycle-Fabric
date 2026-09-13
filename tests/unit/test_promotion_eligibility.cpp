// Promotion eligibility is a hard gate that favourable evidence cannot bypass.
#include "mlf/engine.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

PromotionRequest base_request(const mlftest::Scenario& scenario, ModelVersionId candidate,
                              ScopeId scope, bool immediate = true) {
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, candidate);
  PromotionRequest request;
  request.model = record.model;
  request.candidate = record.version;
  request.candidate_generation = record.generation;
  request.artifact_generation = record.artifact.generation;
  request.compatibility_generation = record.compatibility_generation;
  request.scope = scope;
  request.scope_generation = mlftest::generation_of(*scenario.engine, scope);
  request.epoch = scenario.engine->epoch();
  request.environment_key = scenario.environment.key;
  request.strategy = RolloutStrategy::ImmediateCutover;
  request.request_immediate_cutover = immediate;
  return request;
}

}  // namespace

MLF_TEST(promotion, an_eligible_candidate_is_allowed_with_a_rollback_target) {
  mlftest::Scenario scenario = mlftest::make_scenario(11);
  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  Explanation explanation;
  const Decision decision = scenario.engine->evaluate_promotion(request, &explanation);
  MLF_CHECK(decision.allowed());
  MLF_CHECK_EQ(std::string(to_string(decision.outcome())), std::string("PROMOTION_ALLOWED"));
  MLF_CHECK(explanation.find("candidate_version_id") != nullptr);
  MLF_CHECK(explanation.find("candidate_model_generation") != nullptr);
  MLF_CHECK_EQ(std::string(to_string(explanation.outcome())),
               std::string("PROMOTION_ALLOWED"));
}

MLF_TEST(promotion, stability_of_state_produces_stability_of_explanation) {
  mlftest::Scenario scenario = mlftest::make_scenario(12);
  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  Explanation first;
  Explanation second;
  static_cast<void>(scenario.engine->evaluate_promotion(request, &first));
  static_cast<void>(scenario.engine->evaluate_promotion(request, &second));
  first.normalize();
  second.normalize();
  MLF_CHECK_EQ(first.render(), second.render());
}

MLF_TEST(promotion, retired_candidates_can_never_promote) {
  mlftest::Scenario scenario = mlftest::make_scenario(13);
  // v1 never holds authority, so it can be retired directly; afterwards no
  // promotion of that generation may be authorized again.
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire v1");

  PromotionRequest request = base_request(scenario, scenario.v1, scenario.global);
  const Decision decision = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::PromotionCandidateRetired));
  MLF_CHECK_EQ(std::string(to_string(decision.outcome())), std::string("INCOMPATIBLE"));
}

MLF_TEST(promotion, stale_generations_and_scope_generations_block_promotion) {
  mlftest::Scenario scenario = mlftest::make_scenario(14);
  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  request.candidate_generation = ModelGeneration(request.candidate_generation.raw() + 5);
  const Decision generation = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!generation.allowed());
  MLF_CHECK(generation.has(ReasonCode::ModelGenerationMismatch));
  MLF_CHECK_EQ(std::string(to_string(generation.outcome())), std::string("STALE_PLAN"));

  PromotionRequest scope_request = base_request(scenario, scenario.v2, scenario.global);
  scope_request.rollback_target = scenario.v1;
  scope_request.scope_generation = ScopeGeneration(scope_request.scope_generation.raw() + 5);
  const Decision scope = scenario.engine->evaluate_promotion(scope_request, nullptr);
  MLF_CHECK(!scope.allowed());
  MLF_CHECK(scope.has(ReasonCode::ScopeGenerationMismatch));

  PromotionRequest policy_request = base_request(scenario, scenario.v2, scenario.global);
  policy_request.rollback_target = scenario.v1;
  policy_request.policy_generation = PolicyGeneration(99);
  const Decision policy = scenario.engine->evaluate_promotion(policy_request, nullptr);
  MLF_CHECK(!policy.allowed());
  MLF_CHECK(policy.has(ReasonCode::PolicyGenerationMismatch));

  PromotionRequest epoch_request = base_request(scenario, scenario.v2, scenario.global);
  epoch_request.rollback_target = scenario.v1;
  epoch_request.epoch = CoordinatorEpoch(9999);
  const Decision epoch = scenario.engine->evaluate_promotion(epoch_request, nullptr);
  MLF_CHECK(!epoch.allowed());
  MLF_CHECK(epoch.has(ReasonCode::CoordinatorEpochStale));
}

MLF_TEST(promotion, incompatible_candidates_are_refused_even_with_good_health) {
  mlftest::Scenario scenario = mlftest::make_scenario(15);
  EnvironmentProfile hostile = scenario.environment;
  hostile.backend = "rocm";
  require_ok(scenario.engine->publish_compatibility(scenario.v2, hostile,
                                                    CompatibilityOutcome::INCOMPATIBLE_BACKEND,
                                                    "probe", Provenance::Real),
             "publish incompatible");

  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  request.environment_key = hostile.key;
  const Decision decision = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK_EQ(std::string(to_string(decision.outcome())), std::string("INCOMPATIBLE"));

  // Strong health evidence must not change the answer.
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  require_ok(mlftest::publish_evidence(*scenario.engine, record, scenario.global,
                                       EvidenceKind::HealthCheck, EvidenceVerdict::Satisfied, 1.0),
             "publish health");
  const Decision after = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!after.allowed());
}

MLF_TEST(promotion, missing_compatibility_blocks_and_names_the_gap) {
  mlftest::Scenario scenario = mlftest::make_scenario(16);
  ModelVersionId fresh;
  RegisterVersionRequest request;
  request.model = scenario.model;
  request.label = "3.0.0";
  request.artifact.set_id = ArtifactSetId(3);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "fresh-artifact";
  request.requirements.runtime = "cuda-driver";
  request.requirements.backend = "cuda";
  request.requirements.architecture = "sm_120";
  request.requirements.precision = "fp8";
  request.provenance = Provenance::Synthetic;
  require_ok(scenario.engine->register_version(request, fresh), "register fresh version");

  PromotionRequest promotion = base_request(scenario, fresh, scenario.global);
  promotion.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->evaluate_promotion(promotion, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::CompatibilityMissing) ||
            decision.has(ReasonCode::CompatibilityUnknown));
}

MLF_TEST(promotion, a_first_deployment_needs_no_rollback_target) {
  mlftest::Scenario scenario = mlftest::make_scenario(17);
  // Nothing is serving the scope yet, so there is nothing to roll back to.
  PromotionRequest request = base_request(scenario, scenario.v1, scenario.global);
  const Decision decision = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(decision.allowed());
}

MLF_TEST(promotion, replacing_a_serving_generation_requires_a_rollback_target) {
  mlftest::Scenario scenario = mlftest::make_scenario(17);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1 becomes current");
  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  Explanation explanation;
  const Decision decision = scenario.engine->evaluate_promotion(request, &explanation);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RollbackTargetRequired));
  MLF_CHECK(explanation.find("displaces_authority") != nullptr);
  MLF_CHECK_EQ(*explanation.find("displaces_authority"), std::string("yes"));
}

MLF_TEST(promotion, a_retired_rollback_target_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(18);
  // Promote v2 first so v1 has no authority, then retire v1.
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "promote v2");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire v1");

  ModelVersionId fresh;
  RegisterVersionRequest request;
  request.model = scenario.model;
  request.label = "4.0.0";
  request.artifact.set_id = ArtifactSetId(4);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "fresh-artifact-4";
  request.requirements.runtime = "cuda-driver";
  request.requirements.backend = "cuda";
  request.requirements.architecture = "sm_120";
  request.requirements.precision = "fp8";
  request.provenance = Provenance::Synthetic;
  require_ok(scenario.engine->register_version(request, fresh), "register");
  require_ok(scenario.engine->publish_compatibility(fresh, scenario.environment,
                                                    CompatibilityOutcome::COMPATIBLE, "x",
                                                    Provenance::Synthetic),
             "compat");

  PromotionRequest promotion = base_request(scenario, fresh, scenario.global);
  promotion.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->evaluate_promotion(promotion, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RollbackTargetRetired));
}

MLF_TEST(promotion, policy_evidence_requirements_are_enforced) {
  mlftest::Scenario scenario = mlftest::make_scenario(19);
  LifecyclePolicy policy = scenario.engine->current_policy();
  policy.generation = next_generation(policy.generation);
  policy.globally_required_evidence = {EvidenceKind::TestResult};
  require_ok(scenario.engine->set_policy(policy), "set policy");

  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  const Decision missing = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!missing.allowed());
  MLF_CHECK(missing.has(ReasonCode::EvidenceMissing));
  MLF_CHECK_EQ(std::string(to_string(missing.outcome())), std::string("STALE_EVIDENCE"));

  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  require_ok(mlftest::publish_evidence(*scenario.engine, record, scenario.global,
                                       EvidenceKind::TestResult, EvidenceVerdict::Satisfied, 1.0),
             "publish test result");
  const Decision satisfied = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(satisfied.allowed());
}

MLF_TEST(promotion, manual_approval_requirement_is_enforced) {
  mlftest::Scenario scenario = mlftest::make_scenario(20);
  LifecyclePolicy policy = scenario.engine->current_policy();
  policy.generation = next_generation(policy.generation);
  policy.require_manual_approval_for_promotion = true;
  require_ok(scenario.engine->set_policy(policy), "set policy");

  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  request.rollback_target = scenario.v1;
  const Decision denied = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(!denied.allowed());
  MLF_CHECK(denied.has(ReasonCode::PolicyRequiresApproval));

  request.administrative_approval = true;
  const Decision approved = scenario.engine->evaluate_promotion(request, nullptr);
  MLF_CHECK(approved.allowed());
}

MLF_TEST(promotion, a_failed_promotion_leaves_no_authority_behind) {
  mlftest::Scenario scenario = mlftest::make_scenario(21);
  PromotionRequest request = base_request(scenario, scenario.v2, scenario.global);
  // A superseded compatibility generation: the gate refuses before anything
  // commits, and nothing about the candidate may change.
  request.compatibility_generation =
      CompatibilityGeneration(request.compatibility_generation.raw() + 9);
  const Sequence sequence_before = scenario.engine->sequence();
  const PromotionOutcome outcome = scenario.engine->promote(request);
  MLF_CHECK(!outcome.decision.allowed());
  MLF_CHECK(scenario.engine->sequence() == sequence_before);
  MLF_CHECK(!outcome.promotion.valid());
  const AuthorityQueryResult authority = scenario.engine->query_authority(scenario.global);
  MLF_CHECK_EQ(std::string(to_string(authority.outcome)),
               std::string("NO_AUTHORITATIVE_MODEL"));
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(!is_promotable_from(LifecycleState::CURRENT));
  MLF_CHECK(record.state != LifecycleState::CURRENT);
}
