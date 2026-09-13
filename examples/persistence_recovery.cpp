// Persist lifecycle truth, restart, and prove volatile authority did not return.
#include "example_support.hpp"

#include <filesystem>

#include "mlf/state_store.hpp"

using namespace mlf;
using namespace mlfexample;
using namespace mlfexample;

int main() {
  std::printf("== persistence_recovery ==\n");
  const std::string state_path =
      (std::filesystem::path(mlfexample::scratch_directory()) / "recovery.mlfs").string();
  std::error_code remove_error;
  std::filesystem::remove(state_path, remove_error);

  ModelId model;
  ModelVersionId v1;
  CoordinatorEpoch restored_epoch;

  {
    LifecycleEngine engine;
    ScopeId global;
    mlfexample::require(engine.register_scope("global", global), "global");
    mlfexample::require(engine.register_model("pr-llm", "demo", Provenance::Synthetic, model), "model");
    const std::string artifact = mlfexample::write_artifact("pr-llm.bin", 81, 1024);
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

    const ModelVersionRecord record = mlfexample::version_of(engine, v1);
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
    mlfexample::require(engine.promote(promotion).decision, "promote");

    // A worker publishes readiness, then the coordinator is restarted.
    WorkerRegistration registration;
    registration.id = WorkerId(1);
    registration.boot = WorkerBootId(2001);
    registration.epoch = engine.epoch();
    mlfexample::require(engine.register_worker(registration), "register worker");
    EvidenceRecord evidence;
    evidence.kind = EvidenceKind::Readiness;
    evidence.verdict = EvidenceVerdict::Satisfied;
    evidence.subject.model = model;
    evidence.subject.version = v1;
    evidence.subject.model_generation = record.generation;
    evidence.subject.scope = global;
    evidence.provenance = Provenance::Synthetic;
    evidence.worker = registration.id;
    evidence.boot = registration.boot;
    mlfexample::require(engine.publish_evidence(evidence), "publish readiness");

    const auto state = engine.export_durable_state();
    const StateStoreResult saved =
        save_durable_state(*state, DecodeLimits::from_bounds(engine.bounds()), state_path);
    std::printf("save: %s\n", std::string(to_string(saved.status)).c_str());
  }

  {
    LifecycleEngine engine;
    DurableState loaded;
    const StateStoreResult result =
        load_durable_state(state_path, DecodeLimits::from_bounds(engine.bounds()), loaded);
    mlfexample::require(Decision(result.ok() ? OutcomeCode::Ok : OutcomeCode::Rejected), "load state");
    const CoordinatorEpoch next = next_generation(loaded.epoch);
    mlfexample::require(engine.import_durable_state(loaded, next), "import durable state");
    engine.invalidate_volatile_state("coordinator restart");
    restored_epoch = engine.epoch();

    ScopeId global;
    mlfexample::require(engine.register_scope("global", global), "global after restart");
    const AuthorityQueryResult authority = engine.query_authority(global);
    std::printf("authority survived restart: version %llu\n",
                static_cast<unsigned long long>(authority.binding != nullptr
                                                    ? authority.binding->version.raw()
                                                    : 0));
    const ModelVersionRecord record = mlfexample::version_of(engine, v1);
    std::printf("lifecycle state survived: %s\n", std::string(to_string(record.state)).c_str());

    const EvidenceRequirement requirement{EvidenceKind::Readiness, EvidenceVerdict::Satisfied, true,
                                          true};
    EvidenceSubject subject;
    subject.model = model;
    subject.version = v1;
    subject.model_generation = record.generation;
    subject.scope = global;
    const EvidenceLookup lookup = engine.check_evidence(requirement, subject);
    std::printf("readiness after restart: %s\n",
                std::string(to_string(lookup.currentness)).c_str());
    std::printf("restored epoch = %llu\n",
                static_cast<unsigned long long>(restored_epoch.raw()));
  }

  std::filesystem::remove(state_path, remove_error);
  return restored_epoch.raw() > 1 ? 0 : 1;
}