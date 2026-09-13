// Fence a worker boot and prove a replacement must start clean.
#include "example_support.hpp"

using namespace mlf;
using namespace mlfexample;

int main() {
  std::printf("== worker_fencing ==\n");
  LifecycleEngine engine;
  ScopeId global;
  mlfexample::require(engine.register_scope("global", global), "global");

  ModelId model;
  mlfexample::require(engine.register_model("wf-llm", "demo", Provenance::Synthetic, model), "model");
  const std::string artifact = mlfexample::write_artifact("wf-llm.bin", 71, 1024);
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
  mlfexample::require(engine.publish_compatibility(v1, EnvironmentProfile{"local"}, CompatibilityOutcome::COMPATIBLE,
                                       "smoke", Provenance::Synthetic),
          "compatibility");

  WorkerRegistration registration;
  registration.id = WorkerId(1);
  registration.boot = WorkerBootId(1001);
  registration.epoch = engine.epoch();
  mlfexample::require(engine.register_worker(registration), "register worker boot 1001");

  // Evidence published by the live boot is accepted.
  ModelVersionRecord record = mlfexample::version_of(engine, v1);
  EvidenceRecord evidence;
  evidence.kind = EvidenceKind::Readiness;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.version = v1;
  evidence.subject.model = model;
  evidence.subject.model_generation = record.generation;
  evidence.subject.scope = global;
  evidence.provenance = Provenance::Synthetic;
  evidence.worker = registration.id;
  evidence.boot = registration.boot;
  mlfexample::require(engine.publish_evidence(evidence), "publish readiness from live boot");

  // Fence the boot: authority is withdrawn with it.
  mlfexample::require(engine.fence_worker(registration.id, registration.boot, engine.epoch(), "simulated death"),
          "fence worker");

  const EvidenceRequirement requirement{EvidenceKind::Readiness, EvidenceVerdict::Satisfied, true,
                                        true};
  EvidenceSubject subject;
  subject.model = model;
  subject.version = v1;
  subject.model_generation = record.generation;
  subject.scope = global;
  const EvidenceLookup after_fence = engine.check_evidence(requirement, subject);
  std::printf("readiness after fence: %s\n",
              std::string(to_string(after_fence.currentness)).c_str());

  // The fenced boot cannot re-register silently; a replacement needs a new boot id.
  WorkerRegistration replacement = registration;
  replacement.boot = WorkerBootId(1002);
  const Decision rejected = engine.register_worker(replacement);
  std::printf("replacement while fenced lease present: %s\n", rejected.render().c_str());

  WorkerRegistration replacement_after_fence = registration;
  replacement_after_fence.boot = WorkerBootId(1002);
  const Decision accepted = engine.register_worker(replacement_after_fence);
  std::printf("replacement boot: %s\n", accepted.render().c_str());
  std::printf("boot 1001 current: %s\n",
              engine.worker_boot_current(registration.id, registration.boot) ? "yes" : "no");
  std::printf("boot 1002 current: %s\n",
              engine.worker_boot_current(registration.id, WorkerBootId(1002)) ? "yes" : "no");
  return accepted.allowed() && !engine.worker_boot_current(registration.id, registration.boot) ? 0
                                                                                              : 1;
}