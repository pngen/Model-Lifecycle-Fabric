// Compatibility hardening: mid-rollout changes force revalidation.
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

bool build_staged(Staged& out, std::uint64_t seed, bool two_stages = false) {
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
  if (two_stages) {
    CohortSpec fleet;
    fleet.name = "fleet";
    fleet.scopes = {out.scenario.global};
    plan.cohorts.push_back(fleet);
  }
  StageSpec first;
  first.name = "canary";
  first.cohort_indices = {0};
  plan.stages.push_back(first);
  if (two_stages) {
    StageSpec second;
    second.name = "fleet";
    second.cohort_indices = {1};
    plan.stages.push_back(second);
  }
  return out.scenario.engine->create_rollout(plan, out.rollout, out.generation).allowed();
}

}  // namespace

MLF_TEST(compatibility_adversarial, an_unknown_runtime_is_never_compatible) {
  ModelRequirements requirements;
  requirements.runtime = "not-installed";
  EnvironmentProfile environment;
  environment.key = "e";
  environment.runtime = "cuda-driver";
  environment.provenance = Provenance::Real;
  const CompatibilityResult result = evaluate_requirements(requirements, environment);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_RUNTIME"));
}

MLF_TEST(compatibility_adversarial, a_runtime_generation_change_mid_rollout_forces_revalidation) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 221));
  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  // The runtime family changes under the running canary.
  EnvironmentProfile changed = staged.scenario.environment;
  changed.runtime = "different-runtime";
  require_ok(staged.scenario.engine->publish_compatibility(
                 staged.scenario.v2, changed, CompatibilityOutcome::INCOMPATIBLE_RUNTIME,
                 "runtime family changed", Provenance::Real),
             "publish changed compatibility");

  const RolloutPlan entered = *staged.scenario.engine->rollout(staged.rollout);
  RolloutProgressRequest advance;
  advance.rollout = staged.rollout;
  advance.rollout_generation = staged.generation;
  advance.stage = entered.stages[0].id;
  advance.stage_generation = entered.current_stage_generation;
  advance.epoch = staged.scenario.engine->epoch();

  const Decision decision = staged.scenario.engine->advance_stage(advance);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RolloutPlanStale));
  MLF_CHECK_EQ(std::string(to_string(decision.outcome())), std::string("STALE_PLAN"));
}

MLF_TEST(compatibility_adversarial, a_capability_removed_after_the_canary_started_is_detected) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 222));

  // Artifact replacement: the bound generation moves on.
  const ModelVersionRecord before = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  ReviseVersionRequest revise;
  revise.model = staged.scenario.model;
  revise.version = staged.scenario.v2;
  revise.expected_generation = before.generation;
  revise.artifact.set_id = before.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(before.artifact.generation.raw() + 1);
  revise.artifact.digest = "replaced";
  revise.requirements = before.requirements;
  require_ok(staged.scenario.engine->revise_version(revise), "revise");

  const RolloutPlan plan = *staged.scenario.engine->rollout(staged.rollout);
  MLF_CHECK(!plan.live());
  MLF_CHECK(plan.superseded);

  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();
  const Decision decision = staged.scenario.engine->begin_rollout(begin);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::RolloutSuperseded) ||
            decision.has(ReasonCode::RolloutPlanStale));
}

MLF_TEST(compatibility_adversarial, an_incompatible_architecture_cannot_promote) {
  mlftest::Scenario scenario = mlftest::make_scenario(223);
  EnvironmentProfile hostile = scenario.environment;
  hostile.architecture = "sm_90";
  hostile.compute_capability = 900;
  require_ok(scenario.engine->publish_compatibility(scenario.v2, hostile,
                                                    CompatibilityOutcome::INCOMPATIBLE_ARCHITECTURE,
                                                    "probe", Provenance::Real),
             "publish");

  ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  MLF_CHECK(record.compatibility_generation.valid());
  const CompatibilityResult resolved = scenario.engine->resolve_compatibility(
      scenario.v2, hostile, record.compatibility_generation);
  MLF_CHECK_EQ(std::string(to_string(resolved.outcome)),
               std::string("INCOMPATIBLE_ARCHITECTURE"));

  PromotionRequest request;
  request.model = scenario.model;
  request.candidate = scenario.v2;
  request.compatibility_generation = record.compatibility_generation;
  request.scope = scenario.global;
  request.epoch = scenario.engine->epoch();
  request.environment_key = hostile.key;
  request.rollback_target = scenario.v1;
  const Decision decision = scenario.engine->promote(request).decision;
  MLF_CHECK(!decision.allowed());
}

MLF_TEST(compatibility_adversarial, a_missing_rollback_artifact_blocks_rollback) {
  mlftest::Scenario scenario = mlftest::make_scenario(224);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v1, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v1");
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          scenario.v1)
                 .decision,
             "v2");

  // Replace the v1 artifact binding so it no longer describes a usable artifact.
  const ModelVersionRecord target = mlftest::version_of(*scenario.engine, scenario.v1);
  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v1;
  revise.expected_generation = target.generation;
  revise.artifact.set_id = target.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(target.artifact.generation.raw() + 1);
  revise.artifact.digest = "moved";
  revise.requirements = target.requirements;
  require_ok(scenario.engine->revise_version(revise), "revise v1");

  RollbackRequest rollback;
  rollback.model = scenario.model;
  rollback.candidate = scenario.v2;
  rollback.target = scenario.v1;
  rollback.scope = scenario.global;
  rollback.epoch = scenario.engine->epoch();
  const Decision decision = scenario.engine->evaluate_rollback(rollback, nullptr);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::ArtifactGenerationMismatch) ||
            decision.has(ReasonCode::RollbackTargetIncompatible));
}

MLF_TEST(compatibility_adversarial, a_precision_mismatch_is_named) {
  ModelRequirements requirements;
  requirements.precision = "int4";
  EnvironmentProfile environment;
  environment.key = "e";
  environment.precisions.insert("fp32");
  environment.provenance = Provenance::Real;
  const CompatibilityResult result = evaluate_requirements(requirements, environment);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_PRECISION"));
  MLF_CHECK(result.decision.has(ReasonCode::IncompatiblePrecision));
}

MLF_TEST(compatibility_adversarial, an_adapter_mismatch_is_named) {
  ModelRequirements requirements;
  requirements.adapter_set = "lora-x";
  EnvironmentProfile environment;
  environment.key = "e";
  environment.adapter_set = "lora-y";
  environment.provenance = Provenance::Real;
  const CompatibilityResult result = evaluate_requirements(requirements, environment);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_ADAPTER"));
}

MLF_TEST(compatibility_adversarial, a_tokenizer_mismatch_is_named) {
  ModelRequirements requirements;
  requirements.tokenizer_generation = "tok-1";
  EnvironmentProfile environment;
  environment.key = "e";
  environment.tokenizer_generation = "tok-2";
  environment.provenance = Provenance::Real;
  const CompatibilityResult result = evaluate_requirements(requirements, environment);
  MLF_CHECK_EQ(std::string(to_string(result.outcome)), std::string("INCOMPATIBLE_TOKENIZER"));
}

MLF_TEST(compatibility_adversarial, publishing_for_a_retired_generation_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(225);
  require_ok(mlftest::promote_immediately(*scenario.engine, scenario, scenario.v2, scenario.global,
                                          ModelVersionId{})
                 .decision,
             "v2");
  RetirementRequest retirement;
  retirement.model = scenario.model;
  retirement.version = scenario.v1;
  retirement.epoch = scenario.engine->epoch();
  require_ok(scenario.engine->retire(retirement), "retire");

  const Decision decision = scenario.engine->publish_compatibility(
      scenario.v1, scenario.environment, CompatibilityOutcome::COMPATIBLE, "late", Provenance::Real);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::VersionRetired));
}
