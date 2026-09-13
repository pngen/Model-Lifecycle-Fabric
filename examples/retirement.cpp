// Retirement is blocked while any scope still depends on the generation.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

int main() {
  std::printf("== retirement ==\n");
  LifecycleEngine engine;
  ScopeId global;
  ScopeId cluster;
  mlfexample::require(engine.register_scope("global", global), "global");
  mlfexample::require(engine.register_scope("global/cluster:c1", cluster), "cluster");

  ModelId model;
  mlfexample::require(engine.register_model("rt-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("rt-llm.bin", 51, 1024);
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

  const auto promote = [&](ModelVersionId candidate, ScopeId scope,
                          ModelVersionId rollback_target) {
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
    // Replacing a serving generation requires a rollback target; the first
    // deployment of a scope does not.
    promotion.rollback_target = rollback_target;
    return engine.promote(promotion).decision;
  };
  mlfexample::require(promote(v1, global, ModelVersionId{}), "v1 globally current");

  RetirementRequest retirement;
  retirement.model = model;
  retirement.version = v1;
  retirement.epoch = engine.epoch();

  Explanation explanation;
  const Decision blocked = engine.evaluate_retirement(retirement, &explanation);
  std::printf("retirement while current: %s\n", blocked.render().c_str());
  std::printf("%s", explanation.render().c_str());

  // Replace v1 with v2 everywhere. v1 remains retained as v2's rollback target,
  // which is exactly what keeps retirement blocked.
  mlfexample::require(promote(v2, global, v1), "v2 globally current");
  const Decision retained = engine.evaluate_retirement(retirement, &explanation);
  std::printf("retirement while retained as a rollback target: %s\n", retained.render().c_str());
  std::printf("%s", explanation.render().c_str());

  // Releasing the retention obligation is an explicit operator decision, taken
  // through policy: it is never implied by a promotion.
  LifecyclePolicy policy = engine.current_policy();
  policy.generation = next_generation(policy.generation);
  policy.allow_retirement_without_drain = true;
  mlfexample::require(engine.set_policy(policy), "release rollback retention");

  const Decision allowed = engine.evaluate_retirement(retirement, &explanation);
  std::printf("retirement after release: %s\n", allowed.render().c_str());
  if (allowed.allowed()) {
    mlfexample::require(engine.retire(retirement), "retire v1");
  }

  const ModelVersionRecord retired = mlfexample::version_of(engine, v1);
  std::printf("v1 state = %s\n", std::string(to_string(retired.state)).c_str());

  // A retired generation can never be promoted again.
  const Decision revive = promote(v1, cluster, ModelVersionId{});
  std::printf("revive attempt: %s\n", revive.render().c_str());
  return !blocked.allowed() && !retained.allowed() && allowed.allowed() && !revive.allowed()
             ? 0
             : 1;
}