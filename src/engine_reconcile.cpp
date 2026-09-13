#include "engine_impl.hpp"

#include <algorithm>
#include <tuple>

namespace mlf {
namespace {

void raise(Decision& decision, OutcomeCode code) {
  const auto rank = [](OutcomeCode value) {
    switch (value) {
      case OutcomeCode::RETIREMENT_ALLOWED: return 0;
      case OutcomeCode::Rejected:           return 1;
      case OutcomeCode::RETIREMENT_BLOCKED: return 2;
      case OutcomeCode::NOT_FOUND:          return 3;
      case OutcomeCode::CONFLICT:           return 4;
      case OutcomeCode::STALE_PLAN:         return 5;
      case OutcomeCode::STALE_EVIDENCE:     return 6;
      default:                              return 2;
    }
  };
  if (rank(code) > rank(decision.outcome())) decision.set_outcome(code);
}

/// Counts of the five non-evidence lifecycle states a version may accumulate.
struct AuthoritySummary {
  std::size_t exclusive{0};
  std::size_t canary{0};
  std::size_t retained{0};
  std::size_t draining{0};
};

}  // namespace

Decision LifecycleEngine::evaluate_retirement_locked(const RetirementRequest& request,
                                                     Explanation* explanation) const {
  Decision decision(OutcomeCode::RETIREMENT_ALLOWED);
  Explanation local(OutcomeCode::RETIREMENT_ALLOWED);
  const auto fault = [&decision, &local](OutcomeCode code, ReasonCode reason, std::string text) {
    raise(decision, code);
    detail::clamp_detail(text);
    decision.add(reason, text);
    local.add(reason, text);
  };

  const ModelVersionRecord* record = impl_->version_of(request.model, request.version);
  if (record == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion, "version is not registered");
    if (explanation != nullptr) {
      local.normalize();
      *explanation = local;
    }
    return decision;
  }
  if (record->retired()) {
    fault(OutcomeCode::CONFLICT, ReasonCode::RetirementAlreadyRetired,
          "generation is already retired");
  }
  if (request.generation.valid() && request.generation != record->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ModelGenerationMismatch,
          "retirement cites a superseded model generation");
  }
  const Decision epoch = impl_->check_epoch(request.epoch);
  if (!epoch.allowed()) {
    for (const Reason& reason : epoch.reasons()) fault(epoch.outcome(), reason.code, reason.detail);
  }
  const Decision policy_decision = impl_->check_policy_generation(request.policy_generation);
  if (!policy_decision.allowed()) {
    for (const Reason& reason : policy_decision.reasons()) {
      fault(policy_decision.outcome(), reason.code, reason.detail);
    }
  }

  // --- active rollout -----------------------------------------------------
  if (record->rollout.valid()) {
    const RolloutPlan* plan = impl_->rollout_of(record->rollout);
    if (plan != nullptr && plan->live()) {
      fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementRolloutActive,
            "the generation still has a live rollout");
    }
  }
  for (const auto& entry : impl_->state.rollouts) {
    const RolloutPlan& plan = entry.second;
    if (!plan.live()) continue;
    if (plan.candidate == record->version) {
      fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementRolloutActive,
            "a live rollout still targets this generation");
    }
    if (plan.rollback_target == record->version) {
      fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementRetentionUnsatisfied,
            "a live rollout still names this generation as its rollback target");
    }
  }

  // --- scope dependency ---------------------------------------------------
  AuthoritySummary summary;
  for (const AuthorityBinding* binding :
       impl_->state.authority.bindings_for_version(record->model, record->version)) {
    if (binding->superseded) continue;
    switch (binding->kind) {
      case AuthorityKind::Exclusive:         ++summary.exclusive; break;
      case AuthorityKind::Canary:
      case AuthorityKind::SplitTraffic:      ++summary.canary; break;
      case AuthorityKind::RollbackRetained:  ++summary.retained; break;
      case AuthorityKind::Draining:          ++summary.draining; break;
    }
  }
  if (summary.exclusive > 0 || summary.canary > 0) {
    fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementScopeDependency,
          "the generation still holds live authoritative scope bindings");
  }
  const LifecyclePolicy effective = impl_->state.policy.base();
  if (summary.draining > 0) {
    fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementDrainIncomplete,
          "the generation is still draining in at least one scope");
  }
  if (summary.retained > 0 && !effective.allow_retirement_without_drain) {
    fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementDrainIncomplete,
          "rollback retention is still outstanding in at least one scope");
  }

  // --- unresolved external work -------------------------------------------
  for (const auto& entry : impl_->state.attempts) {
    const Attempt& attempt = entry.second;
    if (attempt.version != record->version) continue;
    if (attempt.state == AttemptState::Dispatched || attempt.state == AttemptState::OutcomeUnknown ||
        attempt.state == AttemptState::ReconcileRequired) {
      fault(OutcomeCode::RETIREMENT_BLOCKED, ReasonCode::RetirementDependencyUnresolved,
            "an external attempt against this generation has not settled");
    }
  }

  local.set("version_id", record->version.raw());
  local.set("label", record->label);
  local.set("model_generation", record->generation.raw());
  local.set("lifecycle_state", std::string(to_string(record->state)));
  local.set("exclusive_bindings", summary.exclusive);
  local.set("canary_bindings", summary.canary);
  local.set("retained_bindings", summary.retained);
  local.set("draining_bindings", summary.draining);
  local.set("rollback_retention_generations", effective.rollback_retention_generations);
  local.set("allow_retirement_without_drain", effective.allow_retirement_without_drain ? "yes"
                                                                                       : "no");
  local.set_outcome(decision.outcome());
  local.normalize();
  decision.normalize();
  if (explanation != nullptr) *explanation = local;
  return decision;
}

Decision LifecycleEngine::evaluate_retirement(const RetirementRequest& request,
                                              Explanation* explanation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluate_retirement_locked(request, explanation);
}

Decision LifecycleEngine::retire(const RetirementRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision gate = evaluate_retirement_locked(request, nullptr);
  if (!gate.allowed()) return gate;
  ModelVersionRecord* record = impl_->version_mut(request.model, request.version);
  if (record == nullptr) {
    Decision decision(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "version vanished during retirement");
  }

  // Fence every remaining binding so no stale frame can find authority here.
  for (auto& entry : impl_->state.authority.raw_mutable()) {
    for (AuthorityBinding& binding : entry.second) {
      if (binding.superseded) continue;
      if (binding.model != record->model || binding.version != record->version) continue;
      binding.superseded = true;
    }
  }
  // Force the scope generations forward: any plan or frame that cited the old
  // binding structure is now stale by construction.
  for (const AuthorityBinding* binding :
       impl_->state.authority.bindings_for_version(record->model, record->version)) {
    impl_->state.scopes.touch(binding->scope);
  }
  for (auto& entry : impl_->state.rollouts) {
    RolloutPlan& plan = entry.second;
    if (plan.candidate != record->version) continue;
    plan.superseded = true;
    plan.stage_entered = false;
    plan.updated_sequence = impl_->bump_sequence();
  }

  Decision advanced = impl_->advance_lifecycle_to(*record, LifecycleState::RETIRED);
  if (!advanced.allowed()) return advanced;
  record->updated_sequence = impl_->bump_sequence();

  Decision decision(OutcomeCode::RETIREMENT_ALLOWED);
  return decision;
}

Decision LifecycleEngine::revalidate(const RevalidationRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::REVALIDATION_REQUIRED);
  ModelVersionRecord* record = impl_->version_mut(request.model, request.version);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "version is not registered");
  }
  if (record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired, "a retired generation cannot be revalidated");
  }
  if (request.generation.valid() && request.generation != record->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::ModelGenerationMismatch,
                        "revalidation cites a superseded model generation");
  }

  // Compatibility is withdrawn: the next promotion or stage advance must
  // re-establish it rather than inherit a stale verdict.
  impl_->state.compatibility.invalidate_version(record->version);
  record->compatibility_generation = CompatibilityGeneration{};
  // Evidence bound to the current generation is withdrawn as well. Passing the
  // successor generation as the floor removes every record at or below it.
  impl_->state.evidence.invalidate_model_generation(record->version,
                                                    next_generation(record->generation));
  // Canary authority is withdrawn; only committed exclusive authority survives.
  for (const AuthorityBinding* binding :
       impl_->state.authority.bindings_for_version(record->model, record->version)) {
    if (binding->superseded) continue;
    if (binding->kind == AuthorityKind::Exclusive) continue;
    impl_->state.authority.supersede_scope(binding->scope);
  }
  impl_->advance_lifecycle_to(*record, LifecycleState::REVALIDATION_REQUIRED);
  record->updated_sequence = impl_->bump_sequence();

  std::string detail_text(request.detail);
  detail::clamp_detail(detail_text);
  decision.add(request.cause, detail_text);
  return decision;
}

// ---------------------------------------------------------------------------
// External attempts
// ---------------------------------------------------------------------------

AttemptOutcome LifecycleEngine::begin_attempt(const AttemptRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  AttemptOutcome outcome;
  Decision decision(OutcomeCode::Accepted);

  if (static_cast<std::size_t>(request.kind) >= kAttemptKindCount) {
    decision.set_outcome(OutcomeCode::Rejected);
    decision.add(ReasonCode::ProtocolUnknownType, "unknown attempt kind");
    outcome.decision = decision;
    return outcome;
  }
  if (impl_->state.attempts.size() >= config_.bounds.max_attempts) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    decision.add(ReasonCode::BoundsAttemptCount, "pending attempt bound reached");
    outcome.decision = decision;
    return outcome;
  }
  const Decision epoch = impl_->check_epoch(request.epoch);
  if (!epoch.allowed()) {
    outcome.decision = epoch;
    return outcome;
  }
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) {
    outcome.decision = worker;
    return outcome;
  }
  const ModelVersionRecord* record = impl_->version_of(request.model, request.version);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    decision.add(ReasonCode::UnknownVersion, "attempt targets an unregistered version");
    outcome.decision = decision;
    return outcome;
  }
  if (record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    decision.add(ReasonCode::VersionRetired, "attempt targets a retired generation");
    outcome.decision = decision;
    return outcome;
  }
  if (request.model_generation.valid() && request.model_generation != record->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    decision.add(ReasonCode::ModelGenerationMismatch,
                 "attempt targets a superseded model generation");
    outcome.decision = decision;
    return outcome;
  }
  if (request.scope.valid() && impl_->state.scopes.find(request.scope) == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    decision.add(ReasonCode::UnknownScope, "attempt targets an unknown scope");
    outcome.decision = decision;
    return outcome;
  }
  if (request.rollout.valid()) {
    const RolloutPlan* plan = impl_->rollout_of(request.rollout);
    if (plan == nullptr) {
      decision.set_outcome(OutcomeCode::NOT_FOUND);
      decision.add(ReasonCode::UnknownVersion, "attempt targets an unknown rollout");
      outcome.decision = decision;
      return outcome;
    }
    if (request.rollout_generation.valid() && request.rollout_generation != plan->generation) {
      decision.set_outcome(OutcomeCode::STALE_PLAN);
      decision.add(ReasonCode::RolloutGenerationMismatch,
                   "attempt targets a superseded rollout generation");
      outcome.decision = decision;
      return outcome;
    }
  }

  Attempt attempt;
  attempt.id = AttemptId(impl_->state.next_attempt_id);
  attempt.kind = request.kind;
  attempt.worker = request.worker;
  attempt.boot = request.boot;
  attempt.epoch = impl_->state.epoch;
  attempt.model = record->model;
  attempt.version = record->version;
  attempt.model_generation = record->generation;
  attempt.artifact_generation = record->artifact.generation;
  attempt.scope = request.scope;
  attempt.scope_generation = request.scope.valid()
                                 ? impl_->state.scopes.find(request.scope)->generation
                                 : ScopeGeneration{};
  attempt.rollout = request.rollout;
  attempt.rollout_generation = request.rollout_generation;
  attempt.stage = request.stage;
  attempt.stage_generation = request.stage_generation;
  attempt.residency_generation = ResidencyGeneration(impl_->state.next_attempt_id);
  attempt.state = AttemptState::Dispatched;
  attempt.idempotent = request.idempotent;
  attempt.issued_sequence = impl_->bump_sequence();
  attempt.detail = request.detail;
  detail::clamp_detail(attempt.detail);

  outcome.attempt = attempt.id;
  impl_->state.next_attempt_id = attempt.id.raw() + 1;
  impl_->state.attempts.emplace(attempt.id.raw(), std::move(attempt));
  outcome.decision = decision;
  return outcome;
}

Decision LifecycleEngine::complete_attempt(const CompletionRecord& completion) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  const auto found = impl_->state.attempts.find(completion.attempt.raw());
  if (found == impl_->state.attempts.end()) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::AttemptUnknown, "attempt is not registered");
  }
  Attempt& attempt = found->second;

  if (attempt.state == AttemptState::Completed || attempt.state == AttemptState::Failed) {
    const bool conflicting = (attempt.state == AttemptState::Completed) != completion.success;
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(conflicting ? ReasonCode::AttemptConflictingCompletion
                                    : ReasonCode::AttemptAlreadyCompleted,
                        "attempt already settled");
  }
  if (attempt.state == AttemptState::Abandoned) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::AttemptAlreadyCompleted, "attempt was abandoned");
  }
  if (completion.epoch.valid() && completion.epoch != impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "completion arrives from a superseded coordinator epoch");
  }
  if (completion.worker.valid() &&
      (completion.worker != attempt.worker || completion.boot != attempt.boot)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerBootStale,
                        "completion originates from a boot that did not receive the attempt");
  }
  {
    const Decision worker =
        impl_->check_worker(attempt.worker, attempt.boot, CoordinatorEpoch{});
    if (!worker.allowed()) {
      decision.set_outcome(worker.outcome());
      for (const Reason& reason : worker.reasons()) decision.add(reason);
      return decision;
    }
  }
  const ModelVersionRecord* record = impl_->version_of(attempt.model, attempt.version);
  if (record == nullptr || record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired,
                        "attempt subject is gone or retired; the completion is fenced");
  }
  if (record->generation != attempt.model_generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::ModelGenerationMismatch,
                        "attempt targeted a superseded model generation");
  }
  if (attempt.rollout.valid()) {
    const RolloutPlan* plan = impl_->rollout_of(attempt.rollout);
    if (plan == nullptr || plan->generation != attempt.rollout_generation) {
      decision.set_outcome(OutcomeCode::STALE_PLAN);
      return decision.add(ReasonCode::RolloutGenerationMismatch,
                          "attempt targeted a superseded rollout generation");
    }
  }

  if (!completion.confirmed) {
    // The effect may or may not have happened. Never assume either way, and
    // never repeat a non-idempotent action on this basis.
    attempt.state = AttemptState::OutcomeUnknown;
    attempt.settled_sequence = impl_->bump_sequence();
    attempt.detail = completion.detail.empty() ? "acknowledgement lost or unconfirmed"
                                               : completion.detail;
    detail::clamp_detail(attempt.detail);
    decision.set_outcome(OutcomeCode::OUTCOME_UNKNOWN);
    return decision.add(ReasonCode::AttemptOutcomeUnknown,
                        "external effect completion is unconfirmed; reconciliation is required");
  }

  attempt.state = completion.success ? AttemptState::Completed : AttemptState::Failed;
  attempt.settled_sequence = impl_->bump_sequence();
  if (!completion.detail.empty()) attempt.detail = completion.detail;
  detail::clamp_detail(attempt.detail);
  return decision;
}

AttemptOutcome LifecycleEngine::reconcile_attempt(AttemptId attempt_id, bool side_effect_observed,
                                                  CoordinatorEpoch epoch,
                                                  std::string_view detail_text) {
  std::lock_guard<std::mutex> guard(mutex_);
  AttemptOutcome outcome;
  Decision decision(OutcomeCode::Accepted);
  const auto found = impl_->state.attempts.find(attempt_id.raw());
  if (found == impl_->state.attempts.end()) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    decision.add(ReasonCode::AttemptUnknown, "attempt is not registered");
    outcome.decision = decision;
    return outcome;
  }
  Attempt& attempt = found->second;
  if (attempt.state == AttemptState::Completed || attempt.state == AttemptState::Failed ||
      attempt.state == AttemptState::Abandoned) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    decision.add(ReasonCode::AttemptAlreadyCompleted, "attempt is already settled");
    outcome.decision = decision;
    return outcome;
  }
  if (attempt.state == AttemptState::Dispatched) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    decision.add(ReasonCode::UnreconciledAttempt,
                 "attempt is still dispatched; it must be settled or fenced first");
    outcome.decision = decision;
    return outcome;
  }
  if (epoch.valid() && epoch < impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    decision.add(ReasonCode::CoordinatorEpochStale, "reconciliation from a superseded epoch");
    outcome.decision = decision;
    return outcome;
  }

  outcome.attempt = attempt.id;
  if (side_effect_observed) {
    attempt.state = AttemptState::Completed;
    attempt.settled_sequence = impl_->bump_sequence();
    attempt.detail.assign(detail_text);
    detail::clamp_detail(attempt.detail);
    decision.set_outcome(OutcomeCode::Accepted);
    outcome.decision = decision;
    return outcome;
  }
  if (!attempt.idempotent) {
    // Not observed and not safe to repeat: the conservative outcome is to keep
    // demanding reconciliation rather than inventing certainty.
    attempt.state = AttemptState::ReconcileRequired;
    attempt.settled_sequence = impl_->bump_sequence();
    attempt.detail.assign(detail_text);
    detail::clamp_detail(attempt.detail);
    decision.set_outcome(OutcomeCode::RECONCILIATION_REQUIRED);
    decision.add(ReasonCode::UnreconciledAttempt,
                 "side effect was not observed and the action is not idempotent");
    outcome.decision = decision;
    outcome.reconciliation_required = true;
    return outcome;
  }
  attempt.state = AttemptState::Failed;
  attempt.settled_sequence = impl_->bump_sequence();
  attempt.detail.assign(detail_text);
  detail::clamp_detail(attempt.detail);
  decision.set_outcome(OutcomeCode::Accepted);
  outcome.decision = decision;
  return outcome;
}

// ---------------------------------------------------------------------------
// Reconciliation
// ---------------------------------------------------------------------------

std::vector<Finding> LifecycleEngine::reconcile(const ReconciliationInput& input) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<Finding> findings;

  const auto add = [&findings](FindingKind kind, ReasonCode reason, ScopeId scope, ModelId model,
                               ModelVersionId version, ModelGeneration generation,
                               std::string detail_text) {
    Finding finding;
    finding.kind = kind;
    finding.reason = reason;
    finding.scope = scope;
    finding.model = model;
    finding.version = version;
    finding.model_generation = generation;
    finding.detail = std::move(detail_text);
    detail::clamp_detail(finding.detail, detail::kMaxRecordDetail);
    findings.push_back(std::move(finding));
  };

  // A residency lookup that never fabricates presence: an absent observation is
  // reported as absent, not as healthy.
  const auto resident = [&input](ModelId model, ModelVersionId version,
                                 ModelGeneration generation, ScopeId scope) {
    for (const ResidencyObservation& observation : input.residency) {
      if (observation.model != model || observation.version != version) continue;
      if (observation.model_generation != generation) continue;
      if (observation.scope != scope) continue;
      if (observation.resident && observation.ready) return true;
    }
    for (const ReplicaObservation& observation : input.replicas) {
      if (observation.model != model || observation.version != version) continue;
      if (observation.model_generation != generation) continue;
      if (observation.scope != scope) continue;
      if (observation.resident && observation.ready) return true;
    }
    return false;
  };

  // 1. Every live exclusive binding expects a resident, ready generation.
  for (const auto& entry : impl_->state.authority.raw()) {
    for (const AuthorityBinding& binding : entry.second) {
      if (binding.superseded) continue;
      if (binding.kind != AuthorityKind::Exclusive && binding.kind != AuthorityKind::Canary) continue;
      if (resident(binding.model, binding.version, binding.model_generation, binding.scope)) {
        continue;
      }
      add(FindingKind::AuthorityWithoutReplicas, ReasonCode::ExpectedResidentMissing,
          binding.scope, binding.model, binding.version, binding.model_generation,
          "no resident ready replica or residency observation supports the committed authority");
    }
  }

  // 2. A retired generation must not still be resident.
  for (const ResidencyObservation& observation : input.residency) {
    const ModelVersionRecord* record = impl_->version_of(observation.model, observation.version);
    if (record == nullptr) continue;
    if (!record->retired()) continue;
    if (!observation.resident) continue;
    add(FindingKind::RetiredModelStillResident, ReasonCode::RetiredModelStillResident,
        observation.scope, observation.model, observation.version, observation.model_generation,
        "a retired generation is still reported resident");
  }
  for (const ReplicaObservation& observation : input.replicas) {
    const ModelVersionRecord* record = impl_->version_of(observation.model, observation.version);
    if (record == nullptr) continue;
    if (!record->retired()) continue;
    if (!observation.resident) continue;
    add(FindingKind::RetiredModelStillResident, ReasonCode::RetiredModelStillResident,
        observation.scope, observation.model, observation.version, observation.model_generation,
        "a retired generation is still reported resident by a replica");
  }

  // 3. A live rollout whose stage is entered must have live supporting workers.
  std::set<std::uint64_t> live_workers;
  for (WorkerId worker : input.live_workers) live_workers.insert(worker.raw());
  for (const auto& entry : impl_->state.rollouts) {
    const RolloutPlan& plan = entry.second;
    if (!plan.live() || !plan.stage_entered) continue;
    if (!input.worker_liveness_known) {
      add(FindingKind::RolloutStageWithoutWorkers, ReasonCode::ReplicaEvidenceMissing,
          plan.root_scope, plan.model, plan.candidate, plan.candidate_generation,
          "worker liveness is unknown: the runtime will not assume the stage is supported");
      continue;
    }
    bool supported = false;
    for (const auto& worker_entry : impl_->state.workers) {
      const WorkerLease& lease = worker_entry.second;
      if (lease.fenced) continue;
      if (live_workers.find(lease.id.raw()) == live_workers.end()) continue;
      supported = true;
    }
    if (!supported) {
      add(FindingKind::RolloutStageWithoutWorkers, ReasonCode::RolloutStageWithoutWorkers,
          plan.root_scope, plan.model, plan.candidate, plan.candidate_generation,
          "the persisted stage is entered but no live worker supports it");
    }
  }

  // 4. Replicas serving a generation that holds no authority where they run.
  for (const ReplicaObservation& observation : input.replicas) {
    if (!observation.resident && !observation.ready) continue;
    const AuthorityQueryResult query = [&]() {
      AuthorityQueryResult result;
      const std::vector<ScopeId> chain = impl_->state.scopes.chain(observation.scope);
      for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const AuthorityBinding* binding : impl_->state.authority.live_bindings_for(*it)) {
          if (binding->kind != AuthorityKind::Exclusive) continue;
          result.binding = binding;
          return result;
        }
      }
      return result;
    }();
    if (query.binding == nullptr) continue;
    if (query.binding->model == observation.model &&
        query.binding->version == observation.version &&
        query.binding->model_generation == observation.model_generation) {
      continue;
    }
    add(FindingKind::ReplicaForNonAuthoritativeGeneration, ReasonCode::ExternalStateDiverged,
        observation.scope, observation.model, observation.version, observation.model_generation,
        "a resident replica serves a generation that is not authoritative in its scope");
  }

  // 5. Unsettled attempts.
  for (const auto& entry : impl_->state.attempts) {
    const Attempt& attempt = entry.second;
    if (attempt.state == AttemptState::Dispatched || attempt.state == AttemptState::OutcomeUnknown ||
        attempt.state == AttemptState::ReconcileRequired) {
      add(FindingKind::UnreconciledAttempt, ReasonCode::UnreconciledAttempt, attempt.scope,
          attempt.model, attempt.version, attempt.model_generation,
          std::string("attempt ") + to_string(attempt.id) + " is " +
              std::string(to_string(attempt.state)));
    }
  }

  // 6. Live plans whose bindings no longer match registry truth.
  for (const auto& entry : impl_->state.rollouts) {
    const RolloutPlan& plan = entry.second;
    if (!plan.live()) continue;
    const detail::PlanStaleness staleness = impl_->check_plan_bindings(plan);
    if (!staleness.stale) continue;
    FindingKind kind = FindingKind::StalePlanBinding;
    if (staleness.reason == ReasonCode::ArtifactGenerationMismatch) {
      kind = FindingKind::ArtifactGenerationChanged;
    } else if (staleness.reason == ReasonCode::CompatibilityStale) {
      kind = FindingKind::CompatibilityChanged;
    }
    add(kind, staleness.reason, plan.root_scope, plan.model, plan.candidate,
        plan.candidate_generation, "live rollout plan binding went stale: " + staleness.detail);
  }

  // 7. A fenced worker boot must not have contributed evidence that is still
  //    considered current.
  for (const auto& entry : impl_->state.workers) {
    const WorkerLease& lease = entry.second;
    if (!lease.fenced) continue;
    for (const auto& bucket : impl_->state.evidence.raw_buckets()) {
      for (const EvidenceRecord& record : bucket.second) {
        if (record.boot != lease.boot) continue;
        add(FindingKind::FencedWorkerStillPublishing, ReasonCode::EvidencePublisherBootStale,
            record.subject.scope, record.subject.model, record.subject.version,
            record.subject.model_generation,
            "evidence published by a fenced boot is still retained");
      }
    }
  }

  std::sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
    return std::tie(a.kind, a.scope, a.model, a.version, a.model_generation, a.detail) <
           std::tie(b.kind, b.scope, b.model, b.version, b.model_generation, b.detail);
  });
  findings.erase(std::unique(findings.begin(), findings.end(),
                             [](const Finding& a, const Finding& b) {
                               return a.kind == b.kind && a.scope == b.scope &&
                                      a.model == b.model && a.version == b.version &&
                                      a.model_generation == b.model_generation &&
                                      a.detail == b.detail;
                             }),
                 findings.end());
  return findings;
}

}  // namespace mlf
