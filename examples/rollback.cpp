// Roll back one scope without disturbing the rest of the fleet.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

int main() {
  std::printf("== rollback ==\n");
  LifecycleEngine engine;
  ScopeId global;
  ScopeId cluster;
  mlfexample::require(engine.register_scope("global", global), "global");
  mlfexample::require(engine.register_scope("global/cluster:c1", cluster), "cluster");

  ModelId model;
  mlfexample::require(engine.register_model("rb-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("rb-llm.bin", 41, 1024);
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

  const auto promote = [&](ModelVersionId candidate, ScopeId scope) {
    const ModelVersionRecord record = mlfexample::version_of(engine, candidate);
    PromotionRequest promotion;
    promotion.model = model;
    promotion.candidate = candidate;
    promotion.candidate_generation = record.generation;
    promotion.artifact_generation = record.artifact.generation;
    promotion.compatibility_generation = record.compatibility_generation;
    promotion.scope = scope;
    promotion.epoch = engine.epoch();
    promotion.environment_key = environment.key;
    promotion.strategy = RolloutStrategy::ImmediateCutover;
    promotion.request_immediate_cutover = true;
    return engine.promote(promotion).decision;
  };
  mlfexample::require(promote(v1, global), "v1 globally current");
  mlfexample::require(promote(v2, cluster), "v2 current in cluster c1");

  RollbackRequest rollback;
  rollback.model = model;
  rollback.candidate = v2;
  rollback.target = v1;
  rollback.scope = cluster;
  rollback.epoch = engine.epoch();
  rollback.detail = "operator decision after canary regression";

  const RollbackOutcome outcome = engine.rollback(rollback);
  std::printf("rollback: %s id=%llu\n", outcome.decision.render().c_str(),
              static_cast<unsigned long long>(outcome.rollback.raw()));

  const auto report = [&](const char* label, ScopeId scope) {
    const AuthorityQueryResult result = engine.query_authority(scope);
    std::printf("%-16s -> version %llu\n", label,
                static_cast<unsigned long long>(result.binding != nullptr
                                                    ? result.binding->version.raw()
                                                    : 0));
  };
  report("global", global);
  report("cluster c1", cluster);

  // A second rollback of the same pair is refused: the plan already committed one.
  const RollbackOutcome repeated = engine.rollback(rollback);
  std::printf("repeat rollback: %s\n", repeated.decision.render().c_str());
  return outcome.decision.allowed() && !repeated.decision.allowed() ? 0 : 1;
}