// Model Lifecycle Fabric — shared scenario construction for tests.
//
// Everything here goes through the public API. There is no privileged path.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mlf/artifact.hpp"
#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "test_process_util.hpp"

namespace mlftest {

/// Fail the running test when a setup step is refused. Setup failures are test
/// defects, not product behaviour, so they surface immediately.
inline void require_ok(const mlf::Decision& decision, const char* what) {
  if (!decision.allowed()) {
    throw ::mlftest::Failure(std::string("setup step failed: ") + what + " -> " +
                             decision.render());
  }
}

/// Publish a Satisfied readiness or residency record for a version generation.
inline mlf::Decision publish_ready(mlf::LifecycleEngine& engine,
                                   const mlf::ModelVersionRecord& record, mlf::ScopeId scope,
                                   mlf::EvidenceKind kind) {
  mlf::EvidenceRecord evidence;
  evidence.kind = kind;
  evidence.verdict = mlf::EvidenceVerdict::Satisfied;
  evidence.subject.model = record.model;
  evidence.subject.version = record.version;
  evidence.subject.model_generation = record.generation;
  evidence.subject.artifact_set = record.artifact.set_id;
  evidence.subject.artifact_generation = record.artifact.generation;
  evidence.subject.scope = scope;
  evidence.subject.compatibility_generation = record.compatibility_generation;
  evidence.provenance = mlf::Provenance::Synthetic;
  evidence.value = 1.0;
  evidence.unit = "boolean";
  evidence.detail = "test readiness";
  return engine.publish_evidence(evidence);
}

/// Publish a record for an arbitrary evidence kind and verdict.
inline mlf::Decision publish_evidence(mlf::LifecycleEngine& engine,
                                      const mlf::ModelVersionRecord& record, mlf::ScopeId scope,
                                      mlf::EvidenceKind kind, mlf::EvidenceVerdict verdict,
                                      double value) {
  mlf::EvidenceRecord evidence;
  evidence.kind = kind;
  evidence.verdict = verdict;
  evidence.subject.model = record.model;
  evidence.subject.version = record.version;
  evidence.subject.model_generation = record.generation;
  evidence.subject.artifact_set = record.artifact.set_id;
  evidence.subject.artifact_generation = record.artifact.generation;
  evidence.subject.scope = scope;
  evidence.subject.compatibility_generation = record.compatibility_generation;
  evidence.provenance = mlf::Provenance::Synthetic;
  evidence.value = value;
  evidence.unit = "ratio";
  evidence.detail = "test evidence";
  return engine.publish_evidence(evidence);
}

/// Publish a stage-scoped record for the rollout stage currently entered.
inline mlf::Decision publish_stage_evidence(mlf::LifecycleEngine& engine,
                                            const mlf::ModelVersionRecord& record,
                                            const mlf::RolloutPlan& plan, mlf::ScopeId scope,
                                            mlf::EvidenceKind kind,
                                            mlf::EvidenceVerdict verdict, double value) {
  mlf::EvidenceRecord evidence;
  evidence.kind = kind;
  evidence.verdict = verdict;
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
  evidence.provenance = mlf::Provenance::Synthetic;
  evidence.value = value;
  evidence.unit = "ratio";
  evidence.detail = "stage evidence";
  return engine.publish_evidence(evidence);
}

/// A scenario with one model, two versions, a scope tree, and published
/// compatibility. Every generation is a real generation produced by the engine.
struct Scenario {
  mlf::EngineConfig config{};
  std::unique_ptr<mlf::LifecycleEngine> engine{};
  mlf::ModelId model{};
  mlf::ModelVersionId v1{};
  mlf::ModelVersionId v2{};
  mlf::ScopeId global{};
  mlf::ScopeId cluster{};
  mlf::ScopeId tenant{};
  mlf::EnvironmentProfile environment{};
  std::string artifact_path{};
};

inline mlf::ModelVersionRecord version_of(const mlf::LifecycleEngine& engine,
                                          mlf::ModelVersionId id) {
  for (const mlf::ModelVersionRecord& record : engine.all_versions()) {
    if (record.version == id) return record;
  }
  return {};
}

inline mlf::ScopeGeneration generation_of(const mlf::LifecycleEngine& engine, mlf::ScopeId scope) {
  for (const mlf::ScopeRecord& record : engine.scopes()) {
    if (record.id == scope) return record.generation;
  }
  return mlf::ScopeGeneration{};
}

/// Build a scenario. Two artifact generations are written to disk so the
/// artifact bindings are real content digests.
inline Scenario make_scenario(std::uint64_t seed = 1, std::size_t artifact_size = 4096,
                              mlf::EngineConfig config = {}) {
  Scenario scenario;
  scenario.config = config;
  scenario.engine = std::make_unique<mlf::LifecycleEngine>(config);

  mlf::ScopeId global;
  require_ok(scenario.engine->register_scope("global", global), "register global scope");
  mlf::ScopeId cluster;
  require_ok(scenario.engine->register_scope("global/cluster:c1", cluster), "register cluster");
  mlf::ScopeId tenant;
  require_ok(scenario.engine->register_scope("global/cluster:c1/tenant:t1", tenant),
             "register tenant");
  scenario.global = global;
  scenario.cluster = cluster;
  scenario.tenant = tenant;

  mlf::ModelId model;
  require_ok(scenario.engine->register_model("demo-model", "demo-family",
                                             mlf::Provenance::Synthetic, model),
             "register model");
  scenario.model = model;

  scenario.artifact_path = write_artifact("scenario-artifact", seed, artifact_size);

  mlf::ModelRequirements requirements;
  requirements.runtime = "cuda-driver";
  requirements.backend = "cuda";
  requirements.architecture = "sm_120";
  requirements.precision = "fp8";

  mlf::RegisterVersionRequest first;
  first.model = model;
  first.label = "1.0.0";
  first.artifact = binding_from_file(scenario.artifact_path, 1, 1);
  first.requirements = requirements;
  first.provenance = mlf::Provenance::Real;
  mlf::ModelVersionId v1;
  require_ok(scenario.engine->register_version(first, v1), "register version 1");
  scenario.v1 = v1;

  mlf::RegisterVersionRequest second;
  second.model = model;
  second.label = "2.0.0";
  second.artifact = binding_from_file(scenario.artifact_path, 2, 1);
  second.requirements = requirements;
  second.predecessor = v1;
  second.provenance = mlf::Provenance::Real;
  mlf::ModelVersionId v2;
  require_ok(scenario.engine->register_version(second, v2), "register version 2");
  scenario.v2 = v2;

  scenario.environment.key = "local-synthetic";
  scenario.environment.runtime = "cuda-driver";
  scenario.environment.backend = "cuda";
  scenario.environment.architecture = "sm_120";
  scenario.environment.compute_capability = 1200;
  scenario.environment.precisions.insert("fp8");
  scenario.environment.precisions.insert("bf16");
  scenario.environment.provenance = mlf::Provenance::Synthetic;

  for (mlf::ModelVersionId id : {v1, v2}) {
    require_ok(scenario.engine->publish_compatibility(id, scenario.environment,
                                                      mlf::CompatibilityOutcome::COMPATIBLE,
                                                      "scenario", mlf::Provenance::Synthetic),
               "publish compatibility");
  }
  return scenario;
}

/// Promote a candidate immediately into a scope, committing exclusive authority.
inline mlf::PromotionOutcome promote_immediately(mlf::LifecycleEngine& engine,
                                                 const Scenario& scenario,
                                                 mlf::ModelVersionId candidate, mlf::ScopeId scope,
                                                 mlf::ModelVersionId rollback_target,
                                                 bool administrative_approval = false) {
  const mlf::ModelVersionRecord record = version_of(engine, candidate);
  mlf::PromotionRequest request;
  request.model = record.model;
  request.candidate = record.version;
  request.candidate_generation = record.generation;
  request.artifact_generation = record.artifact.generation;
  request.compatibility_generation = record.compatibility_generation;
  request.scope = scope;
  request.scope_generation = generation_of(engine, scope);
  request.epoch = engine.epoch();
  request.strategy = mlf::RolloutStrategy::ImmediateCutover;
  request.request_immediate_cutover = true;
  request.rollback_target = rollback_target;
  request.environment_key = scenario.environment.key;
  request.administrative_approval = administrative_approval;
  return engine.promote(request);
}

}  // namespace mlftest
