// Independent downstream consumer of the installed Model Lifecycle Fabric
// package. It answers the runtime's core question end to end using only the
// public API obtained through find_package(ModelLifecycleFabric CONFIG REQUIRED).
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <mlf/engine.hpp>
#include <mlf/host_env.hpp>
#include <mlf/state_store.hpp>
#include <mlf/version.hpp>

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
  if (condition) return;
  std::printf("FAILED: %s\n", what);
  ++failures;
}

}  // namespace

int main() {
  using namespace mlf;
  std::printf("Model Lifecycle Fabric %s consumed through find_package\n",
              std::string(kVersionString).c_str());
  std::printf("protocol version %u, durable state format %u\n",
              static_cast<unsigned>(kProtocolVersion),
              static_cast<unsigned>(kStateFormatVersion));

  LifecycleEngine engine;
  ScopeId global;
  ScopeId canary;
  expect(engine.register_scope("global", global).allowed(), "register global scope");
  expect(engine.register_scope("global/cluster:canary", canary).allowed(), "register canary scope");

  ModelId model;
  expect(engine.register_model("consumer-llm", "consumer", Provenance::Synthetic, model).allowed(),
         "register model");

  ModelRequirements requirements;
  requirements.runtime = "cuda-driver";
  requirements.backend = "cuda";
  requirements.architecture = "sm_120";
  requirements.precision = "fp8";

  const auto add_version = [&](const char* label, std::uint64_t set_id,
                               ModelVersionId predecessor) {
    RegisterVersionRequest request;
    request.model = model;
    request.label = label;
    request.artifact.set_id = ArtifactSetId(set_id);
    request.artifact.generation = ArtifactGeneration(1);
    request.artifact.digest = std::string("consumer-artifact-") + label;
    request.requirements = requirements;
    request.predecessor = predecessor;
    request.provenance = Provenance::Synthetic;
    ModelVersionId version;
    expect(engine.register_version(request, version).allowed(), "register version");
    return version;
  };
  const ModelVersionId v1 = add_version("1.0.0", 1, ModelVersionId{});
  const ModelVersionId v2 = add_version("2.0.0", 2, v1);

  EnvironmentProfile environment;
  environment.key = "consumer";
  environment.runtime = "cuda-driver";
  environment.backend = "cuda";
  environment.architecture = "sm_120";
  environment.compute_capability = 1200;
  environment.precisions.insert("fp8");
  environment.provenance = Provenance::Synthetic;
  for (ModelVersionId id : {v1, v2}) {
    expect(engine.publish_compatibility(id, environment, CompatibilityOutcome::COMPATIBLE,
                                        "consumer", Provenance::Synthetic)
               .allowed(),
           "publish compatibility");
  }

  const auto record_of = [&](ModelVersionId id) {
    for (const ModelVersionRecord& candidate : engine.all_versions()) {
      if (candidate.version == id) return candidate;
    }
    return ModelVersionRecord{};
  };

  // First generation becomes current globally.
  {
    const ModelVersionRecord record = record_of(v1);
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
    expect(engine.promote(request).decision.allowed(), "promote v1 globally");
  }

  // Second generation becomes current only in the canary scope.
  {
    const ModelVersionRecord record = record_of(v2);
    PromotionRequest request;
    request.model = model;
    request.candidate = v2;
    request.candidate_generation = record.generation;
    request.artifact_generation = record.artifact.generation;
    request.compatibility_generation = record.compatibility_generation;
    request.scope = canary;
    request.epoch = engine.epoch();
    request.environment_key = environment.key;
    request.rollback_target = v1;
    request.strategy = RolloutStrategy::ImmediateCutover;
    request.request_immediate_cutover = true;
    expect(engine.promote(request).decision.allowed(), "promote v2 into the canary scope");
  }

  const AuthorityQueryResult globally = engine.query_authority(global);
  const AuthorityQueryResult in_canary = engine.query_authority(canary);
  expect(globally.binding != nullptr && globally.binding->version == v1,
         "global authority is v1");
  expect(in_canary.binding != nullptr && in_canary.binding->version == v2,
         "canary authority is v2");
  std::printf("global -> version %llu, canary -> version %llu\n",
              static_cast<unsigned long long>(globally.binding->version.raw()),
              static_cast<unsigned long long>(in_canary.binding->version.raw()));

  // Durable truth round-trips through this consumer's own filesystem.
  const auto state = engine.export_durable_state();
  std::vector<std::uint8_t> bytes;
  const DecodeLimits limits = DecodeLimits::from_bounds(engine.bounds());
  expect(serialize_durable_state(*state, limits, bytes).ok(), "serialize durable state");

  LifecycleEngine reloaded;
  DurableState decoded;
  expect(deserialize_durable_state(bytes.data(), bytes.size(), limits, decoded).ok(),
         "deserialize durable state");
  expect(reloaded.import_durable_state(decoded, next_generation(decoded.epoch)).allowed(),
         "import durable state");
  const AuthorityQueryResult reloaded_canary = reloaded.query_authority(canary);
  expect(reloaded_canary.binding != nullptr && reloaded_canary.binding->version == v2,
         "canary authority survived the reload");
  expect(reloaded.epoch() > state->epoch, "recovery advanced the coordinator epoch");

  // Capability discovery is exposed to consumers too.
  const HostEnvironment host = discover_host_environment();
  std::printf("host %s, %u logical cores, cuda=%s\n", host.host.c_str(),
              host.cpu.logical_cores, std::string(to_string(host.cuda.provenance)).c_str());

  std::printf(failures == 0 ? "downstream consumer: PASS\n" : "downstream consumer: FAIL\n");
  return failures;
}
