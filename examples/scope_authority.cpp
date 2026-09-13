// Which generation is authoritative where, and why.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

int main() {
  std::printf("== scope_authority ==\n");
  LifecycleEngine engine;
  ScopeId global;
  ScopeId tenant_a;
  ScopeId tenant_b;
  mlfexample::require(engine.register_scope("global", global), "global");
  mlfexample::require(engine.register_scope("global/cluster:c1/tenant:a", tenant_a), "tenant a");
  mlfexample::require(engine.register_scope("global/cluster:c1/tenant:b", tenant_b), "tenant b");

  ModelId model;
  mlfexample::require(engine.register_model("scoped-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("scoped-llm.bin", 31, 1024);
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
  mlfexample::require(promote(v2, tenant_a), "v2 current for tenant a");

  const auto report = [&](const char* label, ScopeId scope) {
    const AuthorityQueryResult result = engine.query_authority(scope);
    const std::uint64_t version =
        result.binding != nullptr ? result.binding->version.raw() : 0;
    std::printf("%-22s -> version %llu inherited=%s outcome=%s\n", label,
                static_cast<unsigned long long>(version), result.inherited ? "yes" : "no",
                std::string(to_string(result.outcome)).c_str());
  };
  report("global", global);
  report("tenant a", tenant_a);
  report("tenant b", tenant_b);

  std::printf("tenant a explanation:\n%s",
              engine.query_authority(tenant_a).explanation.render().c_str());
  return 0;
}