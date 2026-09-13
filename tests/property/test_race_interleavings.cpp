// Deterministic race tests: forced interleavings, not random scheduling.
#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

#include "mlf_test.hpp"
#include "scenario.hpp"

using namespace mlf;
using mlftest::require_ok;

namespace {

/// A forced two-step interleaving: the first action is applied, then the racing
/// action, then the original action is retried. The outcome is asserted, not
/// observed.
struct Interleaving {
  mlftest::Scenario scenario;
  PromotionOutcome promotion{};
  RolloutId rollout{};
  RolloutGeneration generation{};

  explicit Interleaving(std::uint64_t seed) : scenario(mlftest::make_scenario(seed, 2048)) {}

  bool stage_canary() {
    const ModelVersionRecord candidate = mlftest::version_of(*scenario.engine, scenario.v2);
    PromotionRequest promotion_request;
    promotion_request.model = candidate.model;
    promotion_request.candidate = candidate.version;
    promotion_request.scope = scenario.global;
    promotion_request.epoch = scenario.engine->epoch();
    promotion_request.environment_key = scenario.environment.key;
    promotion_request.strategy = RolloutStrategy::Canary;
    promotion_request.rollback_target = scenario.v1;
    promotion = scenario.engine->promote(promotion_request);
    if (!promotion.decision.allowed()) return false;

    RolloutPlanRequest plan;
    plan.model = candidate.model;
    plan.candidate = candidate.version;
    plan.root_scope = scenario.global;
    plan.promotion = promotion.promotion;
    plan.rollback_target = scenario.v1;
    CohortSpec cohort;
    cohort.name = "canary";
    cohort.scopes = {scenario.cluster};
    plan.cohorts.push_back(cohort);
    StageSpec stage;
    stage.name = "canary";
    stage.cohort_indices = {0};
    Criterion health;
    health.kind = EvidenceKind::HealthCheck;
    health.comparison = CriterionComparison::AtLeast;
    health.threshold = 0.9;
    stage.acceptance.push_back(health);
    plan.stages.push_back(stage);
    return scenario.engine->create_rollout(plan, rollout, generation).allowed();
  }

  RolloutProgressRequest progress() const {
    const RolloutPlan plan = *scenario.engine->rollout(rollout);
    RolloutProgressRequest request;
    request.rollout = rollout;
    request.rollout_generation = generation;
    request.stage = plan.stages[plan.current_stage_index].id;
    request.stage_generation = plan.current_stage_generation;
    request.epoch = scenario.engine->epoch();
    return request;
  }
};

}  // namespace

MLF_TEST(race, promotion_versus_compatibility_change) {
  for (std::uint64_t seed = 500; seed <= 510; ++seed) {
    Interleaving interleaving(seed);
    const ModelVersionRecord record = mlftest::version_of(*interleaving.scenario.engine,
                                                          interleaving.scenario.v2);
    PromotionRequest request;
    request.model = record.model;
    request.candidate = record.version;
    request.compatibility_generation = record.compatibility_generation;
    request.scope = interleaving.scenario.global;
    request.epoch = interleaving.scenario.engine->epoch();
    request.environment_key = interleaving.scenario.environment.key;
    request.rollback_target = interleaving.scenario.v1;
    MLF_CHECK(interleaving.scenario.engine->evaluate_promotion(request, nullptr).allowed());

    // The racing action changes compatibility before the commit lands.
    EnvironmentProfile changed = interleaving.scenario.environment;
    changed.key = "second";
    require_ok(interleaving.scenario.engine->publish_compatibility(
                   record.version, changed, CompatibilityOutcome::COMPATIBLE, "second",
                   Provenance::Real),
               "second environment");

    // The stale request now cites a superseded compatibility generation.
    const Decision decision = interleaving.scenario.engine->promote(request).decision;
    MLF_CHECK(!decision.allowed());
    MLF_CHECK(decision.has(ReasonCode::CompatibilityGenerationMismatch));

    // The refreshed request succeeds.
    request.compatibility_generation =
        mlftest::version_of(*interleaving.scenario.engine, record.version).compatibility_generation;
    MLF_CHECK(interleaving.scenario.engine->promote(request).decision.allowed());
  }
}

MLF_TEST(race, stage_advance_versus_evidence_change) {
  for (std::uint64_t seed = 520; seed <= 530; ++seed) {
    Interleaving interleaving(seed);
    MLF_CHECK(interleaving.stage_canary());
    RolloutProgressRequest begin = interleaving.progress();
    begin.stage = RolloutStageId{};
    begin.stage_generation = RolloutStageGeneration{};
    require_ok(interleaving.scenario.engine->begin_rollout(begin), "begin");

    const RolloutPlan plan = *interleaving.scenario.engine->rollout(interleaving.rollout);
    const ModelVersionRecord candidate =
        mlftest::version_of(*interleaving.scenario.engine, interleaving.scenario.v2);
    require_ok(mlftest::publish_stage_evidence(*interleaving.scenario.engine, candidate, plan,
                                               interleaving.scenario.cluster,
                                               EvidenceKind::HealthCheck,
                                               EvidenceVerdict::Satisfied, 0.99),
               "satisfying evidence");
    // The racing action replaces the evidence with a violation.
    require_ok(mlftest::publish_stage_evidence(*interleaving.scenario.engine, candidate, plan,
                                               interleaving.scenario.cluster,
                                               EvidenceKind::HealthCheck,
                                               EvidenceVerdict::Unsatisfied, 0.10),
               "violating evidence");

    RolloutProgressRequest advance = interleaving.progress();
    const Decision decision = interleaving.scenario.engine->advance_stage(advance);
    MLF_CHECK(!decision.allowed());
    MLF_CHECK(decision.has(ReasonCode::StageAcceptanceNotMet));
  }
}

MLF_TEST(race, rollback_versus_late_candidate_success) {
  for (std::uint64_t seed = 540; seed <= 550; ++seed) {
    Interleaving interleaving(seed);
    MLF_CHECK(interleaving.stage_canary());
    RolloutProgressRequest begin = interleaving.progress();
    begin.stage = RolloutStageId{};
    begin.stage_generation = RolloutStageGeneration{};
    require_ok(interleaving.scenario.engine->begin_rollout(begin), "begin");

    const RolloutPlan plan = *interleaving.scenario.engine->rollout(interleaving.rollout);
    const ModelVersionRecord candidate =
        mlftest::version_of(*interleaving.scenario.engine, interleaving.scenario.v2);
    require_ok(mlftest::publish_stage_evidence(*interleaving.scenario.engine, candidate, plan,
                                               interleaving.scenario.cluster,
                                               EvidenceKind::HealthCheck,
                                               EvidenceVerdict::Satisfied, 0.99),
               "evidence");

    // Rollback first.
    RollbackRequest rollback;
    rollback.model = interleaving.scenario.model;
    rollback.candidate = interleaving.scenario.v2;
    rollback.target = interleaving.scenario.v1;
    rollback.rollout = interleaving.rollout;
    rollback.rollout_generation = interleaving.generation;
    // The canary holds authority over the cohort scope, so that is the scope a
    // rollback takes back.
    rollback.scope = interleaving.scenario.cluster;
    rollback.epoch = interleaving.scenario.engine->epoch();
    require_ok(interleaving.scenario.engine->rollback(rollback).decision, "rollback");

    // The late canary success must not re-promote.
    RolloutProgressRequest late = interleaving.progress();
    const Decision decision = interleaving.scenario.engine->advance_stage(late);
    MLF_CHECK(!decision.allowed());
    MLF_CHECK_EQ(interleaving.scenario.engine->query_authority(interleaving.scenario.cluster)
                     .binding->version.raw(),
                 interleaving.scenario.v1.raw());
  }
}

MLF_TEST(race, retirement_versus_scope_query) {
  for (std::uint64_t seed = 560; seed <= 570; ++seed) {
    Interleaving interleaving(seed);
    require_ok(mlftest::promote_immediately(*interleaving.scenario.engine, interleaving.scenario,
                                            interleaving.scenario.v2, interleaving.scenario.global,
                                            ModelVersionId{})
                   .decision,
               "v2 current");
    const AuthorityQueryResult before =
        interleaving.scenario.engine->query_authority(interleaving.scenario.global);
    MLF_CHECK(before.binding != nullptr);

    RetirementRequest retirement;
    retirement.model = interleaving.scenario.model;
    retirement.version = interleaving.scenario.v1;
    retirement.epoch = interleaving.scenario.engine->epoch();
    require_ok(interleaving.scenario.engine->retire(retirement), "retire v1");

    const AuthorityQueryResult after =
        interleaving.scenario.engine->query_authority(interleaving.scenario.global);
    MLF_CHECK(after.binding != nullptr);
    MLF_CHECK_EQ(after.binding->version.raw(), interleaving.scenario.v2.raw());
    MLF_CHECK_EQ(std::string(to_string(after.outcome)), std::string("OK"));
  }
}

MLF_TEST(race, worker_fence_versus_activation_completion) {
  for (std::uint64_t seed = 580; seed <= 590; ++seed) {
    Interleaving interleaving(seed);
    WorkerRegistration registration;
    registration.id = WorkerId(1);
    registration.boot = WorkerBootId(1);
    registration.epoch = interleaving.scenario.engine->epoch();
    require_ok(interleaving.scenario.engine->register_worker(registration), "worker");

    AttemptRequest attempt;
    attempt.kind = AttemptKind::Activate;
    attempt.worker = registration.id;
    attempt.boot = registration.boot;
    attempt.epoch = interleaving.scenario.engine->epoch();
    attempt.model = interleaving.scenario.model;
    attempt.version = interleaving.scenario.v1;
    attempt.scope = interleaving.scenario.global;
    const AttemptOutcome started = interleaving.scenario.engine->begin_attempt(attempt);
    MLF_CHECK(started.decision.allowed());

    require_ok(interleaving.scenario.engine->fence_worker(
                   registration.id, registration.boot, interleaving.scenario.engine->epoch(),
                   "fence wins the race"),
               "fence");

    CompletionRecord completion;
    completion.attempt = started.attempt;
    completion.worker = registration.id;
    completion.boot = registration.boot;
    completion.epoch = interleaving.scenario.engine->epoch();
    completion.success = true;
    completion.confirmed = true;
    const Decision decision = interleaving.scenario.engine->complete_attempt(completion);
    MLF_CHECK(!decision.allowed());
    MLF_CHECK(decision.has(ReasonCode::WorkerBootStale));

    const std::optional<Attempt> settled = interleaving.scenario.engine->attempt(started.attempt);
    MLF_CHECK(settled.has_value());
    MLF_CHECK(settled->state == AttemptState::OutcomeUnknown);
  }
}

MLF_TEST(race, coordinator_epoch_change_versus_stale_frame) {
  for (std::uint64_t seed = 600; seed <= 610; ++seed) {
    Interleaving interleaving(seed);
    const CoordinatorEpoch observed = interleaving.scenario.engine->epoch();
    CoordinatorEpoch advanced;
    require_ok(interleaving.scenario.engine->advance_epoch(observed, advanced), "advance");
    MLF_CHECK(advanced > observed);

    PromotionRequest request;
    const ModelVersionRecord record =
        mlftest::version_of(*interleaving.scenario.engine, interleaving.scenario.v2);
    request.model = record.model;
    request.candidate = record.version;
    request.scope = interleaving.scenario.global;
    request.epoch = observed;
    request.environment_key = interleaving.scenario.environment.key;
    request.rollback_target = interleaving.scenario.v1;
    const Decision decision = interleaving.scenario.engine->promote(request).decision;
    MLF_CHECK(!decision.allowed());
    MLF_CHECK(decision.has(ReasonCode::CoordinatorEpochStale));
  }
}

MLF_TEST(race, snapshot_versus_lifecycle_mutation) {
  for (std::uint64_t seed = 620; seed <= 630; ++seed) {
    Interleaving interleaving(seed);
    const auto snapshot = interleaving.scenario.engine->export_durable_state();
    require_ok(mlftest::promote_immediately(*interleaving.scenario.engine, interleaving.scenario,
                                            interleaving.scenario.v1, interleaving.scenario.global,
                                            ModelVersionId{})
                   .decision,
               "mutate after snapshot");

    // The snapshot still describes the pre-mutation world.
    MLF_CHECK_EQ(snapshot->authority.size(), 0u);
    LifecycleEngine reloaded;
    require_ok(reloaded.import_durable_state(*snapshot, next_generation(snapshot->epoch)),
               "import snapshot");
    MLF_CHECK_EQ(std::string(to_string(
                     reloaded.query_authority(interleaving.scenario.global).outcome)),
                 std::string("NO_AUTHORITATIVE_MODEL"));
    // The live engine is unaffected.
    MLF_CHECK_EQ(std::string(to_string(interleaving.scenario.engine
                                           ->query_authority(interleaving.scenario.global)
                                           .outcome)),
                 std::string("OK"));
  }
}

MLF_TEST(race, persistence_versus_promotion) {
  for (std::uint64_t seed = 640; seed <= 650; ++seed) {
    Interleaving interleaving(seed);
    require_ok(mlftest::promote_immediately(*interleaving.scenario.engine, interleaving.scenario,
                                            interleaving.scenario.v1, interleaving.scenario.global,
                                            ModelVersionId{})
                   .decision,
               "first authority");
    const auto between = interleaving.scenario.engine->export_durable_state();
    require_ok(mlftest::promote_immediately(*interleaving.scenario.engine, interleaving.scenario,
                                            interleaving.scenario.v2, interleaving.scenario.cluster,
                                            interleaving.scenario.v1)
                   .decision,
               "second authority");

    // Persisting the earlier view is legal and must load the earlier world.
    std::vector<std::uint8_t> bytes;
    const DecodeLimits limits =
        DecodeLimits::from_bounds(interleaving.scenario.engine->bounds());
    require_ok(Decision(serialize_durable_state(*between, limits, bytes).ok() ? OutcomeCode::Ok
                                                                              : OutcomeCode::Rejected),
               "serialize");
    DurableState decoded;
    require_ok(Decision(deserialize_durable_state(bytes.data(), bytes.size(), limits, decoded).ok()
                            ? OutcomeCode::Ok
                            : OutcomeCode::Rejected),
               "deserialize");
    LifecycleEngine reloaded;
    require_ok(reloaded.import_durable_state(decoded, next_generation(decoded.epoch)), "import");
    MLF_CHECK_EQ(reloaded.all_authority_bindings().size(), 1u);
    MLF_CHECK_EQ(interleaving.scenario.engine->all_authority_bindings().size(), 2u);
  }
}

MLF_TEST(race, shutdown_versus_rollout_command) {
  for (std::uint64_t seed = 660; seed <= 670; ++seed) {
    Interleaving interleaving(seed);
    MLF_CHECK(interleaving.stage_canary());
    RolloutProgressRequest begin = interleaving.progress();
    begin.stage = RolloutStageId{};
    begin.stage_generation = RolloutStageGeneration{};
    require_ok(interleaving.scenario.engine->begin_rollout(begin), "begin");

    // A shutdown equivalent: the engine is destroyed and another engine recovers
    // from the durable view. The persisted stage must not imply live support.
    const auto state = interleaving.scenario.engine->export_durable_state();
    LifecycleEngine recovered;
    require_ok(recovered.import_durable_state(*state, next_generation(state->epoch)), "import");
    recovered.invalidate_volatile_state("shutdown");

    const std::optional<RolloutPlan> plan = recovered.rollout(interleaving.rollout);
    MLF_CHECK(plan.has_value());
    MLF_CHECK(plan->stage_entered);

    const std::vector<Finding> findings = recovered.reconcile(ReconciliationInput{});
    bool unsupported = false;
    for (const Finding& finding : findings) {
      if (finding.kind == FindingKind::RolloutStageWithoutWorkers ||
          finding.kind == FindingKind::AuthorityWithoutReplicas) {
        unsupported = true;
      }
    }
    MLF_CHECK(unsupported);

    RolloutProgressRequest advance;
    advance.rollout = interleaving.rollout;
    advance.rollout_generation = interleaving.generation;
    advance.stage = plan->stages[plan->current_stage_index].id;
    advance.stage_generation = plan->current_stage_generation;
    advance.epoch = recovered.epoch();
    const Decision decision = recovered.advance_stage(advance);
    MLF_CHECK(!decision.allowed());
  }
}
