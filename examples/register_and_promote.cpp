// Register two model generations and promote the newer one.
#include "example_support.hpp"

int main() {
  using namespace mlf;
using namespace mlfexample;
  std::printf("== register_and_promote ==\n");

  LifecycleEngine engine;
  ScopeId global;
  mlfexample::require(engine.register_scope("global", global), "register global scope");

  ModelId model;
  mlfexample::require(engine.register_model("demo-llm", "demo", Provenance::Synthetic, model),
          "register model");
  const std::string artifact = mlfexample::write_artifact("demo-llm-v1.bin", 11, 2048);

  ModelRequirements requirements;
  requirements.runtime = "cuda-driver";
  requirements.backend = "cuda";
  requirements.architecture = "sm_120";
  requirements.precision = "fp8";

  RegisterVersionRequest first;
  first.model = model;
  first.label = "1.0.0";
  first.artifact = ArtifactBinding{ArtifactSetId(1), ArtifactGeneration(1), ""};
  std::string digest;
  if (compute_file_digest(artifact, digest)) first.artifact.digest = digest;
  first.requirements = requirements;
  first.provenance = Provenance::Real;
  ModelVersionId v1;
  mlfexample::require(engine.register_version(first, v1), "register version 1.0.0");

  RegisterVersionRequest second = first;
  second.label = "2.0.0";
  second.artifact.set_id = ArtifactSetId(2);
  second.predecessor = v1;
  ModelVersionId v2;
  mlfexample::require(engine.register_version(second, v2), "register version 2.0.0");

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
            "publish compatibility");
  }

  const ModelVersionRecord record = mlfexample::version_of(engine, v1);
  PromotionRequest request;
  request.model = model;
  request.candidate = v1;
  request.candidate_generation = record.generation;
  request.artifact_generation = record.artifact.generation;
  request.compatibility_generation = record.compatibility_generation;
  request.scope = global;
  request.epoch = engine.epoch();
  request.environment_key = environment.key;
  request.strategy = RolloutStrategy::ImmediateCutover;
  request.request_immediate_cutover = true;

  const PromotionOutcome outcome = engine.promote(request);
  std::printf("promotion %s id=%llu\n", outcome.decision.render().c_str(),
              static_cast<unsigned long long>(outcome.promotion.raw()));

  const AuthorityQueryResult authority = engine.query_authority(global);
  std::printf("authoritative version for global = %llu (state %s)\n",
              static_cast<unsigned long long>(authority.binding != nullptr
                                                  ? authority.binding->version.raw()
                                                  : 0),
              std::string(to_string(engine.version(model, v1)->state)).c_str());
  return outcome.decision.allowed() ? 0 : 1;
}