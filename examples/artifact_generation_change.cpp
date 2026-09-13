// Replacing artifact bytes invalidates the generation everything was bound to.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

int main() {
  std::printf("== artifact_generation_change ==\n");
  LifecycleEngine engine;
  ScopeId global;
  mlfexample::require(engine.register_scope("global", global), "global");

  ModelId model;
  mlfexample::require(engine.register_model("af-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("af-llm.bin", 61, 4096);

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

  EnvironmentProfile environment;
  environment.key = "local";
  environment.runtime = "cuda-driver";
  environment.backend = "cuda";
  environment.architecture = "sm_120";
  environment.compute_capability = 1200;
  environment.precisions.insert("fp8");
  environment.provenance = Provenance::Synthetic;
  mlfexample::require(engine.publish_compatibility(v1, environment, CompatibilityOutcome::COMPATIBLE,
                                       "smoke", Provenance::Synthetic),
          "compatibility");

  ModelVersionRecord record = mlfexample::version_of(engine, v1);
  std::printf("before: model_gen=%llu artifact_gen=%llu compat_gen=%llu digest=%s\n",
              static_cast<unsigned long long>(record.generation.raw()),
              static_cast<unsigned long long>(record.artifact.generation.raw()),
              static_cast<unsigned long long>(record.compatibility_generation.raw()),
              record.artifact.digest.c_str());

  // Replace the bytes on disk: a new artifact generation with a new digest.
  const std::string replaced = mlfexample::write_artifact("af-llm.bin", 62, 4096);
  ReviseVersionRequest revise;
  revise.model = model;
  revise.version = v1;
  revise.expected_generation = record.generation;
  revise.artifact.set_id = ArtifactSetId(1);
  revise.artifact.generation = ArtifactGeneration(2);
  std::string new_digest;
  if (compute_file_digest(replaced, new_digest)) revise.artifact.digest = new_digest;
  revise.requirements = requirements;
  mlfexample::require(engine.revise_version(revise), "revise version");

  record = mlfexample::version_of(engine, v1);
  std::printf("after:  model_gen=%llu artifact_gen=%llu compat_gen=%llu digest=%s\n",
              static_cast<unsigned long long>(record.generation.raw()),
              static_cast<unsigned long long>(record.artifact.generation.raw()),
              static_cast<unsigned long long>(record.compatibility_generation.raw()),
              record.artifact.digest.c_str());

  // Promotion with the old artifact generation is now a stale plan.
  PromotionRequest promotion;
  promotion.model = model;
  promotion.candidate = v1;
  promotion.candidate_generation = ModelGeneration(record.generation.raw() - 1);
  promotion.artifact_generation = ArtifactGeneration(1);
  promotion.scope = global;
  promotion.epoch = engine.epoch();
  promotion.environment_key = environment.key;
  const PromotionOutcome stale = engine.promote(promotion);
  std::printf("promotion with old binding: %s\n", stale.decision.render().c_str());
  return stale.decision.allowed() ? 1 : 0;
}