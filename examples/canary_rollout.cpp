// Drive a bounded two-stage canary rollout with real acceptance evidence.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

namespace {

ModelVersionRecord refresh(const LifecycleEngine& engine, ModelVersionId id) {
  return mlfexample::version_of(engine, id);
}

/// Publish acceptance evidence bound to the stage currently entered. Evidence
/// published without the rollout and stage identity cannot satisfy a stage
/// criterion: the runtime will not reuse a version-level reading for a stage
/// decision.
Decision publish(LifecycleEngine& engine, const ModelVersionRecord& record, const RolloutPlan& plan,
                 ScopeId scope, EvidenceKind kind, double value) {
  EvidenceRecord evidence;
  evidence.kind = kind;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.model = record.model;
  evidence.subject.version = record.version;
  evidence.subject.model_generation = record.generation;
  evidence.subject.artifact_set = record.artifact.set_id;
  evidence.subject.artifact_generation = record.artifact.generation;
  evidence.subject.scope = scope;
  evidence.subject.rollout = plan.id;
  evidence.subject.rollout_generation = plan.generation;
  evidence.subject.stage = plan.stages[plan.current_stage_index].id;
  evidence.subject.stage_generation = plan.current_stage_generation;
  evidence.subject.compatibility_generation = record.compatibility_generation;
  evidence.provenance = Provenance::Synthetic;
  evidence.value = value;
  evidence.unit = "ratio";
  return engine.publish_evidence(evidence);
}

}  // namespace

int main() {
  std::printf("== canary_rollout ==\n");
  LifecycleEngine engine;
  ScopeId global;
  ScopeId cluster;
  mlfexample::require(engine.register_scope("global", global), "global");
  mlfexample::require(engine.register_scope("global/cluster:c1", cluster), "cluster:c1");

  ModelId model;
  mlfexample::require(engine.register_model("canary-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("canary-llm.bin", 21, 2048);
  ModelRequirements requirements;
  requirements.runtime = "cuda-driver";
  requirements.backend = "cuda";
  requirements.architecture = "sm_120";
  requirements.precision = "fp8";

  RegisterVersionRequest request;
  request.model = model;
  request.label = "1.0.0";
  request.artifact.set_id = ArtifactSetId(1);
  request.artifact.generation = ArtifactGeneration(1);
  request.requirements = requirements;
  std::string digest;
  if (compute_file_digest(artifact, digest)) request.artifact.digest = digest;
  request.provenance = Provenance::Real;
  ModelVersionId v1;
  mlfexample::require(engine.register_version(request, v1), "v1");

  request.label = "2.0.0";
  request.artifact.set_id = ArtifactSetId(2);
  request.predecessor = v1;
  ModelVersionId v2;
  mlfexample::require(engine.register_version(request, v2), "v2");

  EnvironmentProfile environment;
  environment.key = "local";
  environment.runtime = "cuda-driver";
  environment.backend = "cuda";
  environment.architecture = "sm_120";
  environment.compute_capability = 1200;
  environment.precisions.insert("fp8");
  environment.provenance = Provenance::Synthetic;
  for (ModelVersionId id : {v1, v2}) {
    mlfexample::require(engine.publish_compatibility(id, environment, CompatibilityOutcome::COMPATIBLE,
                                         "smoke", Provenance::Synthetic),
            "compatibility");
  }

  // v1 becomes current at global first.
  ModelVersionRecord record = refresh(engine, v1);
  PromotionRequest promotion;
  promotion.model = model;
  promotion.candidate = v1;
  promotion.candidate_generation = record.generation;
  promotion.artifact_generation = record.artifact.generation;
  promotion.compatibility_generation = record.compatibility_generation;
  promotion.scope = global;
  promotion.epoch = engine.epoch();
  promotion.environment_key = environment.key;
  promotion.strategy = RolloutStrategy::ImmediateCutover;
  promotion.request_immediate_cutover = true;
  mlfexample::require(engine.promote(promotion).decision, "promote v1 at global");

  // v2 is promoted as a staged canary.
  record = refresh(engine, v2);
  promotion.candidate = v2;
  promotion.candidate_generation = record.generation;
  promotion.artifact_generation = record.artifact.generation;
  promotion.compatibility_generation = record.compatibility_generation;
  promotion.scope = global;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.request_immediate_cutover = false;
  promotion.rollback_target = v1;
  const PromotionOutcome staged = engine.promote(promotion);
  std::printf("staged promotion: %s\n", staged.decision.render().c_str());
  if (!staged.decision.allowed()) return 1;

  RolloutPlanRequest plan;
  plan.model = model;
  plan.candidate = v2;
  plan.candidate_generation = record.generation;
  plan.artifact_generation = record.artifact.generation;
  plan.compatibility_generation = record.compatibility_generation;
  plan.root_scope = global;
  plan.strategy = RolloutStrategy::Canary;
  plan.promotion = staged.promotion;
  plan.promotion_generation = staged.promotion_generation;
  plan.rollback_target = v1;
  // The plan ceiling bounds what a single stage may move; the canary cohort
  // itself moves 5%, and the final fleet stage is allowed to reach the whole
  // scope only because the ceiling permits it.
  plan.max_blast_radius_percent = 100;

  CohortSpec canary;
  canary.name = "canary";
  canary.scopes = {cluster};
  canary.traffic_percent = 5;
  plan.cohorts.push_back(canary);

  CohortSpec fleet;
  fleet.name = "fleet";
  fleet.scopes = {global};
  fleet.traffic_percent = 100;
  plan.cohorts.push_back(fleet);

  StageSpec first;
  first.name = "canary";
  first.cohort_indices = {0};
  Criterion health;
  health.kind = EvidenceKind::HealthCheck;
  health.comparison = CriterionComparison::AtLeast;
  health.threshold = 0.99;
  first.acceptance.push_back(health);
  plan.stages.push_back(first);

  StageSpec second;
  second.name = "fleet";
  second.cohort_indices = {1};
  second.acceptance.push_back(health);
  plan.stages.push_back(second);

  RolloutId rollout;
  RolloutGeneration rollout_generation;
  mlfexample::require(engine.create_rollout(plan, rollout, rollout_generation), "create plan");

  RolloutProgressRequest progress;
  progress.rollout = rollout;
  progress.rollout_generation = rollout_generation;
  progress.epoch = engine.epoch();
  mlfexample::require(engine.begin_rollout(progress), "begin canary stage");

  // Advance attempt before evidence: refused.
  const Decision premature = engine.advance_stage(progress);
  std::printf("advance without evidence: %s\n", premature.render().c_str());

  RolloutPlan current = *engine.rollout(rollout);
  progress.stage = current.stages[current.current_stage_index].id;
  progress.stage_generation = current.current_stage_generation;
  publish(engine, refresh(engine, v2), current, cluster, EvidenceKind::HealthCheck, 0.999);
  const Decision advanced = engine.advance_stage(progress);
  std::printf("advance after evidence: %s\n", advanced.render().c_str());

  current = *engine.rollout(rollout);
  progress.stage = current.stages[current.current_stage_index].id;
  progress.stage_generation = current.current_stage_generation;
  publish(engine, refresh(engine, v2), current, global, EvidenceKind::HealthCheck, 0.999);
  const Decision completed = engine.advance_stage(progress);
  std::printf("final stage: %s\n", completed.render().c_str());

  const AuthorityQueryResult authority = engine.query_authority(global);
  std::printf("global now authoritative for version %llu\n",
              static_cast<unsigned long long>(authority.binding->version.raw()));
  return advanced.allowed() && completed.allowed() ? 0 : 1;
}