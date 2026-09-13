#include "engine_impl.hpp"

#include <deque>
#include <tuple>

namespace mlf {
namespace {

/// Severity ordering used to pick the single reported outcome when several
/// independent hard checks fail. Deterministic and total over the codes used.
int outcome_rank(OutcomeCode code) noexcept {
  switch (code) {
    case OutcomeCode::PROMOTION_ALLOWED: return 0;
    case OutcomeCode::Rejected:          return 1;
    case OutcomeCode::PROMOTION_BLOCKED: return 2;
    case OutcomeCode::NOT_FOUND:         return 3;
    case OutcomeCode::CONFLICT:          return 4;
    case OutcomeCode::BOUNDS_EXCEEDED:   return 5;
    case OutcomeCode::STALE_PLAN:        return 6;
    case OutcomeCode::STALE_EVIDENCE:    return 7;
    case OutcomeCode::REVALIDATION_REQUIRED: return 8;
    case OutcomeCode::AMBIGUOUS:         return 9;
    case OutcomeCode::INCOMPATIBLE:      return 10;
    default:                             return 2;
  }
}

void raise(Decision& decision, OutcomeCode code) {
  if (outcome_rank(code) > outcome_rank(decision.outcome())) decision.set_outcome(code);
}

}  // namespace

Decision LifecycleEngine::Impl::advance_lifecycle_to(ModelVersionRecord& record,
                                                     LifecycleState target) {
  Decision decision(OutcomeCode::Accepted);
  if (record.state == target) return decision;
  if (is_terminal(record.state)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired,
                        "a retired generation cannot change lifecycle state");
  }

  // Breadth-first search over the successor table: the shortest legal path.
  std::map<std::uint8_t, std::uint8_t> previous;
  std::deque<LifecycleState> frontier;
  std::set<std::uint8_t> seen;
  frontier.push_back(record.state);
  seen.insert(static_cast<std::uint8_t>(record.state));
  bool found = false;
  while (!frontier.empty() && !found) {
    const LifecycleState current = frontier.front();
    frontier.pop_front();
    std::size_t count = 0;
    const LifecycleState* list = successors(current, count);
    for (std::size_t i = 0; i < count; ++i) {
      const std::uint8_t next = static_cast<std::uint8_t>(list[i]);
      if (seen.count(next) != 0) continue;
      seen.insert(next);
      previous[next] = static_cast<std::uint8_t>(current);
      if (list[i] == target) {
        found = true;
        break;
      }
      frontier.push_back(list[i]);
    }
  }
  if (!found) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidLifecycleTransition,
                        std::string(to_string(record.state)) + " -> " +
                            std::string(to_string(target)) + " has no legal path");
  }

  std::vector<LifecycleState> path;
  std::uint8_t cursor = static_cast<std::uint8_t>(target);
  const std::uint8_t start = static_cast<std::uint8_t>(record.state);
  while (cursor != start) {
    path.push_back(static_cast<LifecycleState>(cursor));
    const auto parent = previous.find(cursor);
    if (parent == previous.end()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::InvalidLifecycleTransition, "lifecycle path reconstruction "
                                                                  "failed");
    }
    cursor = parent->second;
  }
  std::reverse(path.begin(), path.end());
  for (LifecycleState step : path) {
    Decision stepped = transition(record, step);
    if (!stepped.allowed()) return stepped;
  }
  return decision;
}

std::optional<EnvironmentProfile> LifecycleEngine::promotion_environment(
    const PromotionRequest& request) const {
  if (request.environment.has_value()) return request.environment;
  return std::nullopt;
}

Decision LifecycleEngine::evaluate_promotion_locked(const PromotionRequest& request,
                                                    Explanation* explanation) const {
  Decision decision(OutcomeCode::PROMOTION_ALLOWED);
  Explanation local(OutcomeCode::PROMOTION_ALLOWED);
  const RegistryBounds& bounds = config_.bounds;

  const auto fault = [&decision, &local](OutcomeCode code, ReasonCode reason, std::string text) {
    raise(decision, code);
    detail::clamp_detail(text);
    decision.add(reason, text);
    local.add(reason, text);
  };

  // --- authority of the caller -------------------------------------------
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
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) {
    for (const Reason& reason : worker.reasons()) fault(worker.outcome(), reason.code, reason.detail);
  }

  const ModelVersionRecord* record = impl_->version_of(request.model, request.candidate);
  if (record == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion,
          "candidate version is not registered for this model");
    if (explanation != nullptr) {
      local.normalize();
      *explanation = local;
    }
    return decision;
  }

  // --- hard eligibility ---------------------------------------------------
  if (record->retired()) {
    fault(OutcomeCode::INCOMPATIBLE, ReasonCode::PromotionCandidateRetired,
          "candidate generation is retired and can never regain authority");
  }
  if (!is_promotable_from(record->state)) {
    fault(OutcomeCode::CONFLICT, ReasonCode::VersionNotPromotionEligible,
          std::string("candidate is in state ") + std::string(to_string(record->state)));
  }
  if (request.candidate_generation.valid() &&
      request.candidate_generation != record->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ModelGenerationMismatch,
          "request cites a superseded model generation");
  }
  if (!record->artifact.valid()) {
    fault(OutcomeCode::INCOMPATIBLE, ReasonCode::ArtifactSetMissing,
          "candidate has no artifact-set binding");
  }
  if (request.artifact_generation.valid() &&
      request.artifact_generation != record->artifact.generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ArtifactGenerationMismatch,
          "request cites a superseded artifact generation");
  }

  const ScopeRecord* scope_record = impl_->state.scopes.find(request.scope);
  if (scope_record == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownScope, "target scope is not registered");
  } else if (request.scope_generation.valid() &&
             request.scope_generation != scope_record->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ScopeGenerationMismatch,
          "target scope generation advanced since the request was formed");
  }

  const LifecyclePolicy effective =
      scope_record != nullptr ? impl_->state.policy.effective(impl_->state.scopes, request.scope)
                              : impl_->state.policy.base();
  if (scope_record != nullptr && !effective.promotable_scope_kinds.empty() &&
      std::find(effective.promotable_scope_kinds.begin(), effective.promotable_scope_kinds.end(),
                scope_record->kind) == effective.promotable_scope_kinds.end()) {
    fault(OutcomeCode::INCOMPATIBLE, ReasonCode::PolicyScopeNotTargetable,
          "policy does not permit promotion into this scope kind");
  }

  // --- compatibility ------------------------------------------------------
  const CompatibilityResult compatibility = impl_->resolve_compatibility_for(
      *record, request.environment_key, promotion_environment(request));
  if (!is_serving_compatible(compatibility.outcome)) {
    if (compatibility.outcome == CompatibilityOutcome::UNKNOWN) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::CompatibilityUnknown,
            "compatibility could not be established for the candidate");
    } else if (compatibility.outcome == CompatibilityOutcome::STALE_EVIDENCE) {
      fault(OutcomeCode::STALE_EVIDENCE, ReasonCode::CompatibilityStale,
            "published compatibility no longer matches the candidate generation");
    } else {
      fault(OutcomeCode::INCOMPATIBLE, reason_for(compatibility.outcome),
            "candidate is not compatible with the target environment");
    }
    for (const Reason& reason : compatibility.decision.reasons()) {
      if (reason.detail.empty()) continue;
      local.add(reason.code, reason.detail);
    }
  }
  if (request.compatibility_generation.valid() &&
      request.compatibility_generation != record->compatibility_generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::CompatibilityGenerationMismatch,
          "request cites a superseded compatibility generation");
  }

  // --- rollback target ----------------------------------------------------
  const ModelVersionRecord* rollback = nullptr;
  if (request.rollback_target.valid()) {
    rollback = impl_->version_of(request.model, request.rollback_target);
    if (rollback == nullptr) {
      fault(OutcomeCode::NOT_FOUND, ReasonCode::RollbackTargetUnknown,
            "declared rollback target is not registered");
    } else if (rollback->retired()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetRetired,
            "declared rollback target is retired");
    } else if (!rollback->artifact.valid()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetArtifactMissing,
            "declared rollback target has no artifact binding");
    } else if (!rollback->has_compatibility()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetIncompatible,
            "declared rollback target has no current compatibility");
    } else if (request.rollback_target_generation.valid() &&
               request.rollback_target_generation != rollback->generation) {
      fault(OutcomeCode::STALE_PLAN, ReasonCode::ModelGenerationMismatch,
            "rollback target generation advanced");
    }
  } else {
    // A rollback target is required exactly when the promotion displaces an
    // existing authoritative generation in the target scope. Demanding one for a
    // first deployment would make the first promotion impossible; accepting none
    // while replacing a serving generation would make rollback unplannable.
    bool displaces = false;
    for (const AuthorityBinding* binding :
         impl_->state.authority.live_bindings_for(request.scope)) {
      if (binding->kind != AuthorityKind::Exclusive) continue;
      if (binding->model == record->model && binding->version == record->version &&
          binding->model_generation == record->generation) {
        continue;
      }
      displaces = true;
    }
    if (displaces && effective.require_rollback_target) {
      fault(OutcomeCode::PROMOTION_BLOCKED, ReasonCode::RollbackTargetRequired,
            "policy requires a valid rollback target when replacing a serving generation");
    }
    local.set("displaces_authority", displaces ? "yes" : "no");
  }

  // --- evidence -----------------------------------------------------------
  if (!effective.globally_required_evidence.empty()) {
    std::vector<EvidenceRequirement> requirements;
    for (EvidenceKind kind : effective.globally_required_evidence) {
      EvidenceRequirement requirement;
      requirement.kind = kind;
      requirement.required_verdict = EvidenceVerdict::Satisfied;
      requirement.require_exact_subject = true;
      requirements.push_back(requirement);
    }
    const EvidenceSubject subject = impl_->version_subject(*record, request.scope);
    const Decision evidence = impl_->check_evidence_list(requirements, subject);
    if (!evidence.allowed()) {
      for (const Reason& reason : evidence.reasons()) {
        fault(evidence.outcome(), reason.code, reason.detail);
      }
    }
  }
  if (effective.require_manual_approval_for_promotion && !request.administrative_approval) {
    fault(OutcomeCode::PROMOTION_BLOCKED, ReasonCode::PolicyRequiresApproval,
          "policy requires an explicit administrative approval for promotion");
  }

  // --- coexistence and blast radius --------------------------------------
  if (scope_record != nullptr) {
    const std::vector<const AuthorityBinding*> live =
        impl_->state.authority.live_bindings_for(request.scope);
    std::vector<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>> generations;
    bool candidate_already_present = false;
    for (const AuthorityBinding* binding : live) {
      if (binding->kind == AuthorityKind::RollbackRetained) continue;
      if (binding->model == record->model && binding->version == record->version &&
          binding->model_generation == record->generation) {
        candidate_already_present = true;
        continue;
      }
      generations.emplace_back(binding->model.raw(), binding->version.raw(),
                               binding->model_generation.raw());
    }
    std::sort(generations.begin(), generations.end());
    generations.erase(std::unique(generations.begin(), generations.end()), generations.end());
    const std::size_t projected = generations.size() + (candidate_already_present ? 0u : 1u);
    if (projected > effective.max_active_generations_per_scope) {
      fault(OutcomeCode::CONFLICT, ReasonCode::CoexistenceLimitExceeded,
            "scope would hold more authoritative generations than policy permits");
    }
    if (projected > 1 && !effective.allow_split_traffic &&
        !(request.request_immediate_cutover && effective.max_active_generations_per_scope >= 1)) {
      fault(OutcomeCode::CONFLICT, ReasonCode::PolicyDeniesCoexistence,
            "policy forbids split traffic in this scope");
    }
  }

  if (request.request_immediate_cutover && !effective.allow_immediate_cutover) {
    fault(OutcomeCode::PROMOTION_BLOCKED, ReasonCode::PolicyDeniesPromotion,
          "policy forbids immediate cutover");
  }
  if (request.strategy == RolloutStrategy::ImmediateCutover && !effective.allow_immediate_cutover) {
    fault(OutcomeCode::PROMOTION_BLOCKED, ReasonCode::PolicyDeniesPromotion,
          "policy forbids the immediate-cutover strategy");
  }
  if (request.model.valid() && record->model != request.model) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownModel,
          "candidate version does not belong to the requested model");
  }
  if (bounds.max_models == 0 || bounds.max_versions_per_model == 0) {
    fault(OutcomeCode::BOUNDS_EXCEEDED, ReasonCode::BoundsModelCount,
          "registry is configured with zero capacity");
  }

  // --- explanation --------------------------------------------------------
  local.set("candidate_model_id", record->model.raw());
  local.set("candidate_version_id", record->version.raw());
  local.set("candidate_label", record->label);
  local.set("candidate_model_generation", record->generation.raw());
  local.set("candidate_artifact_set_id", record->artifact.set_id.raw());
  local.set("candidate_artifact_generation", record->artifact.generation.raw());
  local.set("candidate_lifecycle_state", std::string(to_string(record->state)));
  local.set("candidate_lifecycle_generation", record->lifecycle_generation.raw());
  local.set("compatibility_generation", record->compatibility_generation.raw());
  local.set("policy_generation", effective.generation.raw());
  local.set("coordinator_epoch", impl_->state.epoch.raw());
  local.set("requested_strategy", std::string(to_string(request.strategy)));
  if (scope_record != nullptr) {
    local.set("scope", scope_record->path);
    local.set("scope_generation", scope_record->generation.raw());
  }
  if (request.rollback_target.valid()) {
    local.set("rollback_target_version_id", request.rollback_target.raw());
    if (rollback != nullptr) {
      local.set("rollback_target_generation", rollback->generation.raw());
      local.set("rollback_target_state", std::string(to_string(rollback->state)));
    }
  }
  local.set_outcome(decision.outcome());
  local.normalize();
  decision.normalize();
  if (explanation != nullptr) *explanation = local;
  return decision;
}

Decision LifecycleEngine::evaluate_promotion(const PromotionRequest& request,
                                             Explanation* explanation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluate_promotion_locked(request, explanation);
}

PromotionOutcome LifecycleEngine::promote(const PromotionRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  PromotionOutcome outcome;
  Explanation explanation;
  Decision gate = evaluate_promotion_locked(request, &explanation);
  outcome.decision = gate;
  if (!gate.allowed()) return outcome;

  ModelVersionRecord* record = impl_->version_mut(request.model, request.candidate);
  if (record == nullptr) {
    outcome.decision = Decision(OutcomeCode::NOT_FOUND);
    outcome.decision.add(ReasonCode::UnknownVersion, "candidate vanished during promotion");
    return outcome;
  }

  const LifecyclePolicy effective = impl_->state.policy.effective(impl_->state.scopes, request.scope);

  // A candidate may hold at most one live promotion identity at a time.
  for (const auto& entry : impl_->state.rollouts) {
    const RolloutPlan& plan = entry.second;
    if (!plan.live()) continue;
    if (plan.candidate != record->version) continue;
    outcome.decision = Decision(OutcomeCode::CONFLICT);
    outcome.decision.add(ReasonCode::PromotionCommitDuplicate,
                         "candidate already has a live rollout under another promotion");
    return outcome;
  }

  // Supersede any in-flight rollout of a different candidate for this model.
  if (effective.supersede_inflight_rollout_on_new_promotion) {
    for (auto& entry : impl_->state.rollouts) {
      RolloutPlan& plan = entry.second;
      if (!plan.live()) continue;
      if (plan.model != record->model) continue;
      if (plan.candidate == record->version) continue;
      plan.superseded = true;
      plan.stage_entered = false;
      plan.updated_sequence = impl_->bump_sequence();
      StageRecord record_of_supersession;
      record_of_supersession.stage = plan.stages[plan.current_stage_index].id;
      record_of_supersession.generation = plan.current_stage_generation;
      record_of_supersession.entered_sequence = plan.created_sequence;
      record_of_supersession.decided_sequence = plan.updated_sequence;
      record_of_supersession.decision = StageDecision::Superseded;
      record_of_supersession.reasons.emplace_back(
          ReasonCode::VersionSuperseded,
          "a newer promotion superseded this rollout");
      plan.history.push_back(std::move(record_of_supersession));
      impl_->state.authority.supersede_rollout(plan.id, plan.generation);
      ModelVersionRecord* superseded_version = impl_->version_mut(plan.model, plan.candidate);
      if (superseded_version != nullptr && !superseded_version->retired()) {
        impl_->advance_lifecycle_to(*superseded_version, LifecycleState::DRAINING);
      }
    }
  }

  const bool immediate = request.request_immediate_cutover ||
                         request.strategy == RolloutStrategy::ImmediateCutover;
  const PromotionId promotion(impl_->state.next_promotion_id);
  const PromotionGeneration promotion_generation = next_generation(record->promotion_generation);

  if (immediate) {
    Decision advanced = impl_->advance_lifecycle_to(*record, LifecycleState::CURRENT);
    if (!advanced.allowed()) {
      outcome.decision = advanced;
      return outcome;
    }
    Decision granted = impl_->grant_authority(
        *record, request.scope, AuthorityKind::Exclusive, nullptr, nullptr, promotion,
        promotion_generation, RollbackId{}, RollbackGeneration{}, true, request.rollback_target,
        request.rollback_target_generation);
    if (!granted.allowed()) {
      // Authority was refused: roll the lifecycle change back so a failed
      // promotion never leaves the candidate looking current.
      impl_->advance_lifecycle_to(*record, LifecycleState::PROMOTION_BLOCKED);
      outcome.decision = granted;
      return outcome;
    }
    outcome.immediate = true;
  } else {
    Decision advanced = impl_->advance_lifecycle_to(*record, LifecycleState::CANARY_PENDING);
    if (!advanced.allowed()) {
      outcome.decision = advanced;
      return outcome;
    }
  }

  record->last_promotion = promotion;
  record->promotion_generation = promotion_generation;
  record->updated_sequence = impl_->bump_sequence();
  impl_->state.next_promotion_id = promotion.raw() + 1;
  impl_->state.committed_promotions.insert(promotion.raw());

  outcome.promotion = promotion;
  outcome.promotion_generation = promotion_generation;
  outcome.decision = Decision(OutcomeCode::PROMOTION_ALLOWED);
  return outcome;
}

}  // namespace mlf
