// Residency transition authority and readiness gating.
#include "mlf/engine.hpp"
#include "mlf/synthetic_backend.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

MLF_TEST(residency, a_cold_replica_is_not_resident_and_not_ready) {
  SyntheticLifecycleBackend backend;
  ReplicaId replica;
  require_ok(backend.add_replica(ScopeId(1), ModelId(1), ModelVersionId(1), ModelGeneration(1),
                                 ArtifactGeneration(1), replica),
             "add replica");
  const std::optional<SyntheticReplica> cold = backend.replica(replica);
  MLF_CHECK(cold.has_value());
  MLF_CHECK(!cold->resident);
  MLF_CHECK(!cold->ready);
  MLF_CHECK(!cold->warm);
}

MLF_TEST(residency, activation_without_warmup_is_refused) {
  SyntheticLifecycleBackend backend;
  ReplicaId replica;
  require_ok(backend.add_replica(ScopeId(1), ModelId(1), ModelVersionId(1), ModelGeneration(1),
                                 ArtifactGeneration(1), replica),
             "add replica");
  const Decision cold = backend.activate(replica);
  MLF_CHECK(!cold.allowed());
  MLF_CHECK(cold.has(ReasonCode::ReadinessMissing));
  require_ok(backend.warm(replica), "warm");
  require_ok(backend.activate(replica), "activate");
  const std::optional<SyntheticReplica> ready = backend.replica(replica);
  MLF_CHECK(ready->resident);
  MLF_CHECK(ready->ready);
}

MLF_TEST(residency, an_unhealthy_replica_cannot_warm) {
  SyntheticLifecycleBackend backend;
  ReplicaId replica;
  require_ok(backend.add_replica(ScopeId(1), ModelId(1), ModelVersionId(1), ModelGeneration(1),
                                 ArtifactGeneration(1), replica),
             "add replica");
  require_ok(backend.set_healthy(replica, false), "unhealthy");
  const Decision warm = backend.warm(replica);
  MLF_CHECK(!warm.allowed());
  MLF_CHECK(warm.has(ReasonCode::WarmupIncomplete));
}

MLF_TEST(residency, losing_a_replica_makes_residency_evidence_vanish) {
  SyntheticLifecycleBackend backend;
  ReplicaId replica;
  require_ok(backend.add_replica(ScopeId(3), ModelId(1), ModelVersionId(1), ModelGeneration(1),
                                 ArtifactGeneration(1), replica),
             "add replica");
  require_ok(backend.warm(replica), "warm");
  require_ok(backend.activate(replica), "activate");
  MLF_CHECK_EQ(backend.residency().size(), 1u);
  require_ok(backend.lose_replica(replica), "lose replica");
  MLF_CHECK_EQ(backend.residency().size(), 0u);
  MLF_CHECK_EQ(backend.replica_count(), 0u);
}

MLF_TEST(residency, residency_observations_carry_generations_and_provenance) {
  SyntheticLifecycleBackend backend;
  ReplicaId replica;
  require_ok(backend.add_replica(ScopeId(2), ModelId(4), ModelVersionId(4), ModelGeneration(7),
                                 ArtifactGeneration(3), replica),
             "add replica");
  require_ok(backend.warm(replica), "warm");
  require_ok(backend.activate(replica), "activate");
  const std::vector<ResidencyObservation> observations = backend.residency();
  MLF_CHECK_EQ(observations.size(), 1u);
  MLF_CHECK_EQ(observations.front().model_generation.raw(), 7u);
  MLF_CHECK_EQ(observations.front().artifact_generation.raw(), 3u);
  MLF_CHECK(observations.front().resident);
  MLF_CHECK(observations.front().ready);
  MLF_CHECK(observations.front().generation.valid());
  MLF_CHECK_EQ(std::string(to_string(observations.front().provenance)),
               std::string("SYNTHETIC"));
}

MLF_TEST(residency, backend_never_claims_more_than_it_was_told) {
  SyntheticLifecycleBackend backend;
  backend.set_max_replicas(1);
  ReplicaId first;
  ReplicaId second;
  require_ok(backend.add_replica(ScopeId(1), ModelId(1), ModelVersionId(1), ModelGeneration(1),
                                 ArtifactGeneration(1), first),
             "first");
  const Decision overflow = backend.add_replica(ScopeId(1), ModelId(1), ModelVersionId(1),
                                                ModelGeneration(1), ArtifactGeneration(1), second);
  MLF_CHECK(!overflow.allowed());
  MLF_CHECK(overflow.has(ReasonCode::BoundsModelCount));
}

MLF_TEST(readiness, a_stage_requiring_residency_cannot_start_without_it) {
  EngineConfig config;
  LifecyclePolicy policy = config.policy;
  policy.require_residency_before_stage_entry = true;
  config.policy = policy;
  mlftest::Scenario scenario = mlftest::make_scenario(81, 2048, config);

  const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
  PromotionRequest promotion;
  promotion.model = candidate.model;
  promotion.candidate = candidate.version;
  promotion.scope = scenario.global;
  promotion.epoch = scenario.engine->epoch();
  promotion.environment_key = scenario.environment.key;
  promotion.strategy = RolloutStrategy::Canary;
  promotion.rollback_target = scenario.v1;
  const PromotionOutcome outcome = scenario.engine->promote(promotion);
  MLF_CHECK(outcome.decision.allowed());

  RolloutPlanRequest plan;
  plan.model = candidate.model;
  plan.candidate = candidate.version;
  plan.root_scope = scenario.global;
  plan.promotion = outcome.promotion;
  plan.rollback_target = scenario.v1;
  CohortSpec cohort;
  cohort.name = "canary";
  cohort.scopes = {scenario.cluster};
  plan.cohorts.push_back(cohort);
  StageSpec stage;
  stage.name = "canary";
  stage.cohort_indices = {0};
  plan.stages.push_back(stage);
  RolloutId rollout;
  RolloutGeneration generation;
  require_ok(scenario.engine->create_rollout(plan, rollout, generation), "plan");

  RolloutProgressRequest begin;
  begin.rollout = rollout;
  begin.rollout_generation = generation;
  begin.epoch = scenario.engine->epoch();
  const Decision blocked = scenario.engine->begin_rollout(begin);
  MLF_CHECK(!blocked.allowed());
  MLF_CHECK(blocked.has(ReasonCode::ResidencyNotResident));

  require_ok(mlftest::publish_ready(*scenario.engine, candidate, scenario.cluster,
                                    EvidenceKind::ResidencyReady),
             "publish residency");
  const Decision allowed = scenario.engine->begin_rollout(begin);
  MLF_CHECK(allowed.allowed());
}

MLF_TEST(readiness, readiness_evidence_for_an_older_generation_is_stale) {
  mlftest::Scenario scenario = mlftest::make_scenario(82);
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  require_ok(mlftest::publish_ready(*scenario.engine, record, scenario.global,
                                    EvidenceKind::Readiness),
             "publish readiness");

  // Revise the version: the artifact generation moves on and the old readiness
  // record no longer describes the current generation.
  ReviseVersionRequest revise;
  revise.model = scenario.model;
  revise.version = scenario.v2;
  revise.expected_generation = record.generation;
  revise.artifact.set_id = record.artifact.set_id;
  revise.artifact.generation = ArtifactGeneration(record.artifact.generation.raw() + 1);
  revise.artifact.digest = "revised-digest";
  revise.requirements = record.requirements;
  require_ok(scenario.engine->revise_version(revise), "revise");

  const ModelVersionRecord revised = mlftest::version_of(*scenario.engine, scenario.v2);
  EvidenceRequirement requirement{EvidenceKind::Readiness, EvidenceVerdict::Satisfied, true, false};
  EvidenceSubject subject;
  subject.model = scenario.model;
  subject.version = scenario.v2;
  subject.model_generation = revised.generation;
  subject.artifact_set = revised.artifact.set_id;
  subject.artifact_generation = revised.artifact.generation;
  subject.scope = scenario.global;
  const EvidenceLookup lookup = scenario.engine->check_evidence(requirement, subject);
  MLF_CHECK(lookup.currentness != EvidenceCurrentness::Current);
}

MLF_TEST(readiness, evidence_without_provenance_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(83);
  EvidenceRecord evidence;
  evidence.kind = EvidenceKind::Readiness;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.model = scenario.model;
  evidence.subject.version = scenario.v1;
  evidence.subject.scope = scenario.global;
  evidence.provenance = Provenance::Unknown;
  const Decision decision = scenario.engine->publish_evidence(evidence);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::EvidenceUnknownProvenance));
}

MLF_TEST(readiness, evidence_for_an_unknown_version_is_refused) {
  mlftest::Scenario scenario = mlftest::make_scenario(84);
  EvidenceRecord evidence;
  evidence.kind = EvidenceKind::Readiness;
  evidence.verdict = EvidenceVerdict::Satisfied;
  evidence.subject.model = scenario.model;
  evidence.subject.version = ModelVersionId(9999);
  evidence.provenance = Provenance::Synthetic;
  const Decision decision = scenario.engine->publish_evidence(evidence);
  MLF_CHECK(!decision.allowed());
  MLF_CHECK(decision.has(ReasonCode::UnknownVersion));
}

MLF_TEST(readiness, evidence_resolves_by_exact_subject_and_reports_why_it_cannot) {
  mlftest::Scenario scenario = mlftest::make_scenario(85);
  const ModelVersionRecord record = mlftest::version_of(*scenario.engine, scenario.v2);
  require_ok(mlftest::publish_ready(*scenario.engine, record, scenario.cluster,
                                    EvidenceKind::Readiness),
             "readiness in cluster");

  EvidenceRequirement requirement{EvidenceKind::Readiness, EvidenceVerdict::Satisfied, true, false};
  EvidenceSubject elsewhere;
  elsewhere.model = scenario.model;
  elsewhere.version = scenario.v2;
  elsewhere.model_generation = record.generation;
  elsewhere.artifact_set = record.artifact.set_id;
  elsewhere.artifact_generation = record.artifact.generation;
  elsewhere.scope = scenario.global;
  const EvidenceLookup lookup = scenario.engine->check_evidence(requirement, elsewhere);
  MLF_CHECK_EQ(std::string(to_string(lookup.currentness)),
               std::string("SUBJECT_MISMATCH"));

  EvidenceSubject other_generation = elsewhere;
  other_generation.scope = scenario.cluster;
  other_generation.model_generation = ModelGeneration(record.generation.raw() + 1);
  const EvidenceLookup stale = scenario.engine->check_evidence(requirement, other_generation);
  MLF_CHECK_EQ(std::string(to_string(stale.currentness)), std::string("GENERATION_STALE"));
}
