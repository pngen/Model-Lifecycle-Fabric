// Consumer of the installed CMake package.
//
// This example is built both inside the source tree and as an independent
// downstream project via find_package(ModelLifecycleFabric CONFIG REQUIRED).
#include <cstdio>
#include <memory>

#include <mlf/authority.hpp>
#include <mlf/engine.hpp>
#include <mlf/version.hpp>

int main() {
  using namespace mlf;
  std::printf("Model Lifecycle Fabric %s (protocol %u, state format %u)\n",
              std::string(kVersionString).c_str(), static_cast<unsigned>(kProtocolVersion),
              static_cast<unsigned>(kStateFormatVersion));

  // std::unique_ptr is the only owned handle a consumer needs.
  auto engine = std::make_unique<LifecycleEngine>();
  ScopeId global;
  if (!engine->register_scope("global", global).allowed()) return 1;
  ModelId model;
  if (!engine->register_model("consumer-llm", "demo", Provenance::Synthetic, model).allowed()) {
    return 1;
  }

  RegisterVersionRequest request;
  request.model = model;
  request.label = "1.0.0";
  request.artifact.set_id = ArtifactSetId(1);
  request.artifact.generation = ArtifactGeneration(1);
  request.artifact.digest = "consumer-artifact-1";
  request.provenance = Provenance::Real;
  ModelVersionId version;
  if (!engine->register_version(request, version).allowed()) return 1;

  EnvironmentProfile environment;
  environment.key = "consumer";
  environment.provenance = Provenance::Synthetic;
  if (!engine->publish_compatibility(version, environment, CompatibilityOutcome::COMPATIBLE,
                                     "consumer", Provenance::Synthetic)
           .allowed()) {
    return 1;
  }

  const ModelVersionRecord record = [&]() {
    for (const ModelVersionRecord& candidate : engine->all_versions()) {
      if (candidate.version == version) return candidate;
    }
    return ModelVersionRecord{};
  }();

  PromotionRequest promotion;
  promotion.model = model;
  promotion.candidate = version;
  promotion.candidate_generation = record.generation;
  promotion.artifact_generation = record.artifact.generation;
  promotion.compatibility_generation = record.compatibility_generation;
  promotion.scope = global;
  promotion.epoch = engine->epoch();
  promotion.environment_key = environment.key;
  promotion.strategy = RolloutStrategy::ImmediateCutover;
  promotion.request_immediate_cutover = true;
  const PromotionOutcome outcome = engine->promote(promotion);
  std::printf("promotion: %s\n", outcome.decision.render().c_str());

  const AuthorityQueryResult query = engine->query_authority(global);
  std::printf("authoritative: %s\n",
              std::string(to_string(query.binding != nullptr ? AuthorityKind::Exclusive
                                                             : AuthorityKind::Draining))
                  .c_str());
  std::printf("authoritative version = %llu\n",
              static_cast<unsigned long long>(query.binding != nullptr ? query.binding->version.raw()
                                                                       : 0));
  return outcome.decision.allowed() ? 0 : 1;
}
