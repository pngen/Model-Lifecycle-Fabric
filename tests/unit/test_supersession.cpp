// Supersession: a newer generation deterministically displaces an older rollout.
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
  CohortSpec cohort;
  cohort.name = "canary";
  cohort.scopes = {out.scenario.cluster};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "canary";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  return out.scenario.engine->create_rollout(plan, out.rollout, out.generation).allowed();
}

}  // namespace

MLF_TEST(supersession, a_new_promotion_supersedes_an_in_flight_rollout) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 71));
  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  // A third generation arrives and is promoted for the same model.
  RegisterVersionRequest request;
  request.model = staged.scenario.model;
  request.label = "3.0.0";
  request.artifact.set_id = ArtifactSetId(9);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "superseding-artifact";
  request.requirements = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2).requirements;
  request.predecessor = staged.scenario.v2;
  request.provenance = Provenance::Synthetic;
  ModelVersionId v3;
  require_ok(staged.scenario.engine->register_version(request, v3), "register v3");
  require_ok(staged.scenario.engine->publish_compatibility(v3, staged.scenario.environment,
                                                           CompatibilityOutcome::COMPATIBLE, "x",
                                                           Provenance::Synthetic),
             "compat v3");

  const ModelVersionRecord record = mlftest::version_of(*staged.scenario.engine, v3);
  PromotionRequest promotion;
  promotion.model = record.model;
  promotion.candidate = v3;
  promotion.scope = staged.scenario.global;
  promotion.epoch = staged.scenario.engine->epoch();
  promotion.environment_key = staged.scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = staged.scenario.v1;
  const PromotionOutcome outcome = staged.scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  const RolloutPlan superseded = *staged.scenario.engine->rollout(staged.rollout);
  MLF_CHECK(superseded.superseded);
  MLF_CHECK(!superseded.live());
  MLF_CHECK(superseded.history.back().decision == StageDecision::Superseded);
  MLF_CHECK_EQ(std::string(to_string(mlftest::version_of(*staged.scenario.engine, staged.scenario.v2).state)),
               std::string("DRAINING"));
}

MLF_TEST(supersession, a_late_stage_completion_after_supersession_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 72));
  RolloutProgressRequest begin;
  begin.rollout = staged.rollout;
  begin.rollout_generation = staged.generation;
  begin.epoch = staged.scenario.engine->epoch();
  require_ok(staged.scenario.engine->begin_rollout(begin), "begin");

  const RolloutPlan entered = *staged.scenario.engine->rollout(staged.rollout);
  const ModelVersionRecord candidate =
      mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  require_ok(mlftest::publish_stage_evidence(*staged.scenario.engine, candidate, entered,
                                             staged.scenario.cluster, EvidenceKind::HealthCheck,
                                             EvidenceVerdict::Satisfied, 0.999),
             "stage evidence");

  RolloutProgressRequest advance;
  advance.rollout = staged.rollout;
  advance.rollout_generation = staged.generation;
  advance.stage = entered.stages[0].id;
  advance.stage_generation = entered.current_stage_generation;
  advance.epoch = staged.scenario.engine->epoch();

  require_ok(staged.scenario.engine->supersede_rollout(staged.rollout, staged.generation,
                                                       staged.scenario.engine->epoch(),
                                                       "replaced by a newer generation"),
             "supersede");

  const Decision late = staged.scenario.engine->advance_stage(advance);
  MLF_CHECK(!late.allowed());
  MLF_CHECK(late.has(ReasonCode::RolloutSuperseded) || late.has(ReasonCode::RolloutPlanStale));

  const AuthorityQueryResult authority = staged.scenario.engine->query_authority(staged.scenario.cluster);
  MLF_CHECK_EQ(std::string(to_string(authority.outcome)), std::string("NO_AUTHORITATIVE_MODEL"));
}

MLF_TEST(supersession, superseding_with_a_stale_generation_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 73));
  const Decision stale = staged.scenario.engine->supersede_rollout(
      staged.rollout, RolloutGeneration(staged.generation.raw() + 4),
      staged.scenario.engine->epoch(), "stale");
  MLF_CHECK(!stale.allowed());
  MLF_CHECK(stale.has(ReasonCode::RolloutGenerationMismatch));
  MLF_CHECK(staged.scenario.engine->rollout(staged.rollout)->live());
}

MLF_TEST(supersession, superseding_from_an_old_epoch_is_refused) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 74));
  CoordinatorEpoch next;
  require_ok(staged.scenario.engine->advance_epoch(staged.scenario.engine->epoch(), next), "epoch");
  const Decision stale = staged.scenario.engine->supersede_rollout(
      staged.rollout, staged.generation, staged.scenario.engine->epoch()  /* current */, "current");
  MLF_CHECK(stale.allowed());

  Staged second;
  MLF_CHECK(build_staged(second, 75));
  CoordinatorEpoch latest;
  require_ok(second.scenario.engine->advance_epoch(second.scenario.engine->epoch(), latest), "epoch");
  require_ok(second.scenario.engine->advance_epoch(second.scenario.engine->epoch(), latest), "epoch");
  const Decision old_epoch = second.scenario.engine->supersede_rollout(
      second.rollout, second.generation, CoordinatorEpoch(1), "old epoch");
  MLF_CHECK(!old_epoch.allowed());
  MLF_CHECK(old_epoch.has(ReasonCode::CoordinatorEpochStale));
}

MLF_TEST(supersession, a_candidate_cannot_hold_two_live_promotions) {
  Staged staged;
  MLF_CHECK(build_staged(staged, 76));
  const ModelVersionRecord record = mlftest::version_of(*staged.scenario.engine, staged.scenario.v2);
  PromotionRequest promotion;
  promotion.model = record.model;
  promotion.candidate = staged.scenario.v2;
  promotion.scope = staged.scenario.global;
  promotion.epoch = staged.scenario.engine->epoch();
  promotion.environment_key = staged.scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = staged.scenario.v1;
  const PromotionOutcome second = staged.scenario.engine->promote(promotion);
  MLF_CHECK(!second.decision.allowed());
  MLF_CHECK(second.decision.has(ReasonCode::PromotionCommitDuplicate) ||
            second.decision.has(ReasonCode::VersionNotPromotionEligible));
}
