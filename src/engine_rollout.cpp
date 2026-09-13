#include "engine_impl.hpp"

#include <algorithm>

namespace mlf {
namespace {

/// Deterministic, de-duplicated scope set a stage moves.
std::vector<ScopeId> stage_scopes(const RolloutPlan& plan, const StageDefinition& stage) {
  std::vector<ScopeId> out;
  for (CohortId cohort_id : stage.cohorts) {
    const CohortDefinition* cohort = plan.cohort(cohort_id);
    if (cohort == nullptr) continue;
    for (ScopeId scope : cohort->scopes) out.push_back(scope);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

/// Total percentage of the root scope a stage moves, used for the blast-radius
/// check.
std::uint32_t stage_blast_radius(const RolloutPlan& plan, const StageDefinition& stage) {
  std::uint32_t total = 0;
  for (CohortId cohort_id : stage.cohorts) {
    const CohortDefinition* cohort = plan.cohort(cohort_id);
    if (cohort == nullptr) continue;
    const std::uint32_t share =
        cohort->traffic_percent != 0 ? cohort->traffic_percent : 100u;
    total += share;
  }
  return total > 100u ? 100u : total;
}

void raise(Decision& decision, OutcomeCode code) {
  // Stage decisions use a small fixed severity ordering.
  const auto rank = [](OutcomeCode value) {
    switch (value) {
      case OutcomeCode::STAGE_ADVANCE_ALLOWED:
      case OutcomeCode::CANARY_ALLOWED:            return 0;
      case OutcomeCode::Rejected:                  return 1;
      case OutcomeCode::STAGE_ADVANCE_BLOCKED:
      case OutcomeCode::CANARY_BLOCKED:            return 2;
      case OutcomeCode::CONFLICT:                  return 3;
      case OutcomeCode::BOUNDS_EXCEEDED:           return 4;
      case OutcomeCode::STALE_PLAN:                return 5;
      case OutcomeCode::STALE_EVIDENCE:            return 6;
      case OutcomeCode::REVALIDATION_REQUIRED:     return 7;
      case OutcomeCode::ROLLBACK_DECISION_REQUIRED:return 8;
      case OutcomeCode::AMBIGUOUS:                 return 9;
      case OutcomeCode::INCOMPATIBLE:              return 10;
      default:                                     return 2;
    }
  };
  if (rank(code) > rank(decision.outcome())) decision.set_outcome(code);
}

}  // namespace

Decision LifecycleEngine::create_rollout(const RolloutPlanRequest& request, RolloutId& out,
                                         RolloutGeneration& generation) {
  std::lock_guard<std::mutex> guard(mutex_);
  out = RolloutId{};
  generation = RolloutGeneration{};
  Decision decision(OutcomeCode::Accepted);

  ModelVersionRecord* record = impl_->version_mut(request.model, request.candidate);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout candidate is not registered");
  }
  if (record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PromotionCandidateRetired,
                        "cannot plan a rollout for a retired generation");
  }
  if (request.promotion.valid() && record->last_promotion != request.promotion) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::PromotionGenerationMismatch,
                        "plan cites a promotion that is not the candidate's current promotion");
  }
  if (request.promotion_generation.valid() &&
      record->promotion_generation != request.promotion_generation) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::PromotionGenerationMismatch,
                        "plan cites a superseded promotion generation");
  }
  if (!record->last_promotion.valid() && !request.promotion.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PromotionNotAuthorized,
                        "a rollout plan requires a committed promotion");
  }
  if (record->state != LifecycleState::CANARY_PENDING &&
      record->state != LifecycleState::ROLLOUT_ACTIVE &&
      record->state != LifecycleState::PARTIALLY_PROMOTED) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::VersionNotPromotionEligible,
                        "candidate is not awaiting a canary");
  }
  if (request.candidate_generation.valid() &&
      request.candidate_generation != record->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::ModelGenerationMismatch, "plan cites an old model generation");
  }
  if (request.artifact_generation.valid() &&
      request.artifact_generation != record->artifact.generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::ArtifactGenerationMismatch,
                        "plan cites an old artifact generation");
  }
  if (request.compatibility_generation.valid() &&
      request.compatibility_generation != record->compatibility_generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::CompatibilityGenerationMismatch,
                        "plan cites an old compatibility generation");
  }
  const Decision policy_decision = impl_->check_policy_generation(request.policy_generation);
  if (!policy_decision.allowed()) return policy_decision;
  const Decision epoch_decision = impl_->check_epoch(request.epoch);
  if (!epoch_decision.allowed()) return epoch_decision;
  const Decision worker_decision = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker_decision.allowed()) return worker_decision;

  const ScopeRecord* root = impl_->state.scopes.find(request.root_scope);
  if (root == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownScope, "plan root scope is not registered");
  }
  if (request.root_scope_generation.valid() &&
      request.root_scope_generation != root->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::ScopeGenerationMismatch, "plan root scope generation advanced");
  }
  const LifecyclePolicy effective =
      impl_->state.policy.effective(impl_->state.scopes, request.root_scope);
  if (request.max_blast_radius_percent == 0 ||
      request.max_blast_radius_percent > effective.max_blast_radius_percent) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::BlastRadiusExceeded,
                        "plan blast radius exceeds the policy ceiling");
  }
  if (request.cohorts.empty() || request.stages.empty()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::StageOrderViolation, "plan requires cohorts and stages");
  }
  if (request.stages.size() > config_.bounds.max_stages_per_rollout) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsStageCount, "stage bound reached");
  }
  if (request.cohorts.size() > config_.bounds.max_cohorts_per_rollout) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsCohortCount, "cohort bound reached");
  }
  if (impl_->state.rollouts.size() >= config_.bounds.max_rollouts) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsRolloutCount, "rollout bound reached");
  }
  for (const auto& entry : impl_->state.rollouts) {
    const RolloutPlan& existing = entry.second;
    if (!existing.live()) continue;
    if (existing.candidate != record->version) continue;
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::PromotionCommitDuplicate,
                        "the candidate already has a live rollout plan");
  }

  RolloutPlan plan;
  plan.id = RolloutId(impl_->state.next_rollout_id);
  plan.generation = next_generation(record->rollout_generation);
  plan.model = record->model;
  plan.candidate = record->version;
  plan.candidate_generation = record->generation;
  plan.candidate_artifact_set = record->artifact.set_id;
  plan.candidate_artifact_generation = record->artifact.generation;
  plan.compatibility_generation = record->compatibility_generation;
  plan.policy_generation = effective.generation;
  plan.evidence_generation = EvidenceGeneration(impl_->state.next_evidence_generation > 0
                                                   ? impl_->state.next_evidence_generation - 1
                                                   : 0);
  plan.root_scope = request.root_scope;
  plan.root_scope_generation = root->generation;
  plan.strategy = request.strategy;
  plan.promotion = record->last_promotion;
  plan.promotion_generation = record->promotion_generation;
  plan.rollback_target = request.rollback_target;
  plan.rollback_target_generation = request.rollback_target_generation;
  plan.max_blast_radius_percent = request.max_blast_radius_percent;
  plan.manual_progression = request.manual_progression;
  plan.provenance = config_.default_provenance;
  plan.created_sequence = impl_->bump_sequence();

  if (request.previous.valid()) {
    plan.previous = request.previous;
    const ModelVersionRecord* previous = impl_->version_of(request.model, request.previous);
    plan.previous_generation = previous != nullptr ? previous->generation : ModelGeneration{};
  } else {
    for (const AuthorityBinding* binding :
         impl_->state.authority.subtree_bindings(impl_->state.scopes, request.root_scope)) {
      if (binding->kind != AuthorityKind::Exclusive) continue;
      if (binding->model != record->model) continue;
      if (binding->version == record->version) continue;
      plan.previous = binding->version;
      plan.previous_generation = binding->model_generation;
      break;
    }
  }

  std::vector<std::string> cohort_names;
  for (const CohortSpec& spec : request.cohorts) {
    if (!detail::valid_token(spec.name, config_.bounds.max_name_length)) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::InvalidName, "cohort name must be a bounded token");
    }
    if (std::find(cohort_names.begin(), cohort_names.end(), spec.name) != cohort_names.end()) {
      decision.set_outcome(OutcomeCode::CONFLICT);
      return decision.add(ReasonCode::InvalidName, "cohort names must be unique within a plan");
    }
    cohort_names.push_back(spec.name);
    if (spec.scopes.empty()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::CohortScopeInvalid, "cohort has no scopes");
    }
    CohortDefinition cohort;
    cohort.id = CohortId(impl_->state.next_cohort_id++);
    cohort.name = spec.name;
    cohort.traffic_percent = spec.traffic_percent;
    cohort.max_blast_radius_percent = spec.max_blast_radius_percent;
    for (ScopeId scope : spec.scopes) {
      const ScopeRecord* scope_record = impl_->state.scopes.find(scope);
      if (scope_record == nullptr) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::UnknownScope, "cohort references an unregistered scope");
      }
      if (!impl_->state.scopes.is_ancestor_or_self(request.root_scope, scope)) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::CohortScopeInvalid,
                            "cohort scope lies outside the plan root scope");
      }
      cohort.scopes.push_back(scope);
    }
    std::sort(cohort.scopes.begin(), cohort.scopes.end());
    cohort.scopes.erase(std::unique(cohort.scopes.begin(), cohort.scopes.end()),
                        cohort.scopes.end());
    if (cohort.traffic_percent > 100) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::BlastRadiusExceeded, "cohort traffic percent exceeds 100");
    }
    plan.cohorts.push_back(std::move(cohort));
  }

  std::set<std::uint64_t> used_cohorts;
  for (const StageSpec& spec : request.stages) {
    if (!detail::valid_token(spec.name, config_.bounds.max_name_length)) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::InvalidName, "stage name must be a bounded token");
    }
    if (spec.cohort_indices.empty()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::StageOrderViolation, "stage references no cohort");
    }
    StageDefinition stage;
    stage.id = RolloutStageId(impl_->state.next_stage_id++);
    stage.name = spec.name;
    stage.require_residency_before_entry = spec.require_residency_before_entry;
    stage.require_drain_after_commit = spec.require_drain_after_commit;
    for (std::size_t index : spec.cohort_indices) {
      if (index >= plan.cohorts.size()) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::StageOrderViolation, "stage references an unknown cohort");
      }
      const CohortId id = plan.cohorts[index].id;
      if (!used_cohorts.insert(id.raw()).second) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::StageOrderViolation,
                            "a cohort may belong to exactly one stage");
      }
      stage.cohorts.push_back(id);
    }
    stage.required_evidence = spec.required_evidence;
    stage.acceptance = spec.acceptance;
    stage.failure = spec.failure;
    for (const EvidenceRequirement& requirement : stage.required_evidence) {
      if (static_cast<std::size_t>(requirement.kind) >= kEvidenceKindCount) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::ProtocolUnknownType, "stage requires an unknown evidence "
                                                             "kind");
      }
    }
    plan.stages.push_back(std::move(stage));
  }

  if (impl_->state.rollouts.size() >= config_.bounds.max_rollouts) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsRolloutCount, "rollout bound reached");
  }

  out = plan.id;
  generation = plan.generation;
  record->rollout = plan.id;
  record->rollout_generation = plan.generation;
  record->updated_sequence = impl_->bump_sequence();
  impl_->state.next_rollout_id = plan.id.raw() + 1;
  impl_->state.rollouts.emplace(plan.id.raw(), std::move(plan));
  return decision;
}

Decision LifecycleEngine::begin_rollout(const RolloutProgressRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::CANARY_ALLOWED);
  RolloutPlan* plan = impl_->rollout_mut(request.rollout);
  if (plan == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout is not registered");
  }
  if (request.rollout_generation.valid() && request.rollout_generation != plan->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutGenerationMismatch,
                        "request cites a superseded rollout generation");
  }
  const Decision epoch = impl_->check_epoch(request.epoch);
  if (!epoch.allowed()) return epoch;
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) return worker;
  if (plan->superseded) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutSuperseded, "rollout was superseded");
  }
  if (plan->completed || plan->failed) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::RolloutAlreadyComplete, "rollout is no longer live");
  }
  if (plan->stage_entered) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::StageOrderViolation, "a stage is already entered");
  }
  if (plan->current_stage_index != 0) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::StageOrderViolation, "rollout did not begin at the first stage");
  }
  const detail::PlanStaleness staleness = impl_->check_plan_bindings(*plan);
  if (staleness.stale) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutPlanStale, staleness.detail);
  }
  ModelVersionRecord* record = impl_->version_mut(plan->model, plan->candidate);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout candidate is not registered");
  }
  const StageDefinition& stage = plan->stages[0];
  const std::vector<ScopeId> scopes = stage_scopes(*plan, stage);
  const LifecyclePolicy effective =
      impl_->state.policy.effective(impl_->state.scopes, plan->root_scope);

  const bool residency_required =
      stage.require_residency_before_entry || effective.require_residency_before_stage_entry;
  for (ScopeId scope : scopes) {
    const EvidenceSubject subject = impl_->version_subject(*record, scope);
    Decision evidence = impl_->check_evidence_list(stage.required_evidence, subject);
    if (!evidence.allowed()) {
      decision.set_outcome(OutcomeCode::CANARY_BLOCKED);
      for (const Reason& reason : evidence.reasons()) decision.add(reason);
      decision.normalize();
      return decision;
    }
    if (residency_required) {
      EvidenceRequirement requirement;
      requirement.kind = EvidenceKind::ResidencyReady;
      requirement.required_verdict = EvidenceVerdict::Satisfied;
      requirement.require_exact_subject = true;
      Decision residency = impl_->check_evidence_list({requirement}, subject);
      if (!residency.allowed()) {
        decision.set_outcome(OutcomeCode::CANARY_BLOCKED);
        for (const Reason& reason : residency.reasons()) {
          decision.add(ReasonCode::ResidencyNotResident, reason.detail);
        }
        decision.normalize();
        return decision;
      }
    }
  }

  // --- commit -------------------------------------------------------------
  plan->current_stage_generation = next_generation(plan->current_stage_generation);
  plan->stage_entered = true;
  plan->updated_sequence = impl_->bump_sequence();
  StageRecord entered;
  entered.stage = stage.id;
  entered.generation = plan->current_stage_generation;
  entered.entered_sequence = plan->updated_sequence;
  entered.decision = StageDecision::Entered;
  plan->history.push_back(std::move(entered));

  Decision advanced = impl_->advance_lifecycle_to(*record, LifecycleState::CANARY_ACTIVE);
  if (!advanced.allowed()) return advanced;
  record->current_stage = stage.id;
  record->current_stage_generation = plan->current_stage_generation;
  record->updated_sequence = impl_->bump_sequence();

  for (ScopeId scope : scopes) {
    Decision granted = impl_->grant_authority(*record, scope, AuthorityKind::Canary, plan, &stage,
                                              plan->promotion, plan->promotion_generation,
                                              RollbackId{}, RollbackGeneration{}, false,
                                              plan->rollback_target,
                                              plan->rollback_target_generation);
    if (!granted.allowed()) {
      decision.set_outcome(granted.outcome());
      for (const Reason& reason : granted.reasons()) decision.add(reason);
      return decision;
    }
  }
  // Entering a stage is itself an authority change in the scopes it moves.
  // Refresh the root binding the plan carries so that the plan's own authorized
  // action does not invalidate it; any change originating elsewhere still does.
  if (const ScopeRecord* root = impl_->state.scopes.find(plan->root_scope)) {
    plan->root_scope_generation = root->generation;
  }
  return decision;
}

Decision LifecycleEngine::evaluate_stage_advance_locked(const RolloutProgressRequest& request,
                                                        Explanation* explanation) const {
  Decision decision(OutcomeCode::STAGE_ADVANCE_ALLOWED);
  Explanation local(OutcomeCode::STAGE_ADVANCE_ALLOWED);
  const auto fault = [&decision, &local](OutcomeCode code, ReasonCode reason, std::string text) {
    raise(decision, code);
    detail::clamp_detail(text);
    decision.add(reason, text);
    local.add(reason, text);
  };

  const RolloutPlan* plan = impl_->rollout_of(request.rollout);
  if (plan == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion, "rollout is not registered");
    if (explanation != nullptr) *explanation = local;
    return decision;
  }
  if (request.rollout_generation.valid() && request.rollout_generation != plan->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::RolloutGenerationMismatch,
          "request cites a superseded rollout generation");
    if (explanation != nullptr) *explanation = local;
    return decision;
  }
  if (plan->superseded) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::RolloutSuperseded, "rollout was superseded");
  }
  if (plan->completed || plan->failed) {
    fault(OutcomeCode::Rejected, ReasonCode::RolloutAlreadyComplete, "rollout is no longer live");
  }
  if (!plan->stage_entered) {
    fault(OutcomeCode::Rejected, ReasonCode::StageOrderViolation, "no stage is currently entered");
  }
  const StageDefinition* stage = plan->stage_at(plan->current_stage_index);
  if (stage == nullptr) {
    fault(OutcomeCode::Rejected, ReasonCode::StageOrderViolation, "current stage index is invalid");
  }
  if (stage != nullptr && request.stage.valid() && request.stage != stage->id) {
    fault(OutcomeCode::Rejected, ReasonCode::StageOrderViolation,
          "request names a stage that is not the current stage");
  }
  if (request.stage_generation.valid() &&
      request.stage_generation != plan->current_stage_generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::RolloutStageGenerationMismatch,
          "request cites a superseded stage generation");
  }
  for (const StageRecord& entry : plan->history) {
    if (entry.generation != plan->current_stage_generation) continue;
    if (entry.decision == StageDecision::Entered) continue;
    fault(OutcomeCode::CONFLICT, ReasonCode::StageCompletionDuplicate,
          "this stage generation already has a committed decision");
  }

  const Decision epoch = impl_->check_epoch(request.epoch);
  if (!epoch.allowed()) {
    for (const Reason& reason : epoch.reasons()) fault(epoch.outcome(), reason.code, reason.detail);
  }
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) {
    for (const Reason& reason : worker.reasons()) fault(worker.outcome(), reason.code, reason.detail);
  }

  const detail::PlanStaleness staleness = impl_->check_plan_bindings(*plan);
  if (staleness.stale) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::RolloutPlanStale, staleness.detail);
  }
  const ScopeRecord* root = impl_->state.scopes.find(plan->root_scope);
  if (root == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownScope, "plan root scope no longer exists");
  } else if (request.scope_generation.valid() &&
             request.scope_generation != root->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ScopeGenerationMismatch,
          "scope generation advanced since the request was formed");
  }

  const LifecyclePolicy effective =
      impl_->state.policy.effective(impl_->state.scopes, plan->root_scope);
  if (plan->manual_progression && !request.administrative_approval) {
    fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::PolicyRequiresApproval,
          "plan requires explicit administrative progression");
  }
  if (stage != nullptr) {
    const std::uint32_t radius = stage_blast_radius(*plan, *stage);
    if (radius > plan->max_blast_radius_percent || radius > effective.max_blast_radius_percent) {
      fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::BlastRadiusExceeded,
            "stage blast radius exceeds the authorized ceiling");
    }
  }

  const ModelVersionRecord* record = impl_->version_of(plan->model, plan->candidate);
  if (record == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion, "rollout candidate is not registered");
  }
  if (record != nullptr && stage != nullptr) {
    const std::vector<ScopeId> scopes = stage_scopes(*plan, *stage);
    for (ScopeId scope : scopes) {
      const EvidenceSubject subject = impl_->stage_subject(*record, *plan, *stage, scope);
      if (effective.require_evidence_for_stage_advance) {
        Decision evidence = impl_->check_evidence_list(stage->required_evidence, subject);
        if (!evidence.allowed()) {
          for (const Reason& reason : evidence.reasons()) {
            fault(evidence.outcome(), reason.code, reason.detail);
          }
        }
      }
      for (const Criterion& criterion : stage->failure) {
        const Impl::CriterionResult result = impl_->evaluate_criterion(criterion, subject);
        if (!result.present) {
          fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::StageFeedbackIncomplete,
                std::string("failure criterion ") + std::string(to_string(criterion.kind)) +
                    " has no current evidence");
          continue;
        }
        const bool tripped = criterion.comparison == CriterionComparison::AtLeast
                                 ? result.value >= criterion.threshold
                                 : result.value <= criterion.threshold;
        if (tripped) {
          fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::StageFailureThresholdExceeded,
                std::string("failure criterion ") + std::string(to_string(criterion.kind)) +
                    " was met");
        }
      }
      for (const Criterion& criterion : stage->acceptance) {
        const Impl::CriterionResult result = impl_->evaluate_criterion(criterion, subject);
        if (!result.present) {
          fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::StageFeedbackIncomplete,
                std::string("acceptance criterion ") + std::string(to_string(criterion.kind)) +
                    " has no current evidence");
          continue;
        }
        if (!result.satisfied) {
          fault(OutcomeCode::STAGE_ADVANCE_BLOCKED, ReasonCode::StageAcceptanceNotMet,
                std::string("acceptance criterion ") + std::string(to_string(criterion.kind)) +
                    " was not met");
        }
      }
    }
  }

  local.set("rollout_id", plan->id.raw());
  local.set("rollout_generation", plan->generation.raw());
  local.set("strategy", std::string(to_string(plan->strategy)));
  local.set("stage_index", plan->current_stage_index);
  if (stage != nullptr) {
    local.set("stage_id", stage->id.raw());
    local.set("stage_name", stage->name);
    local.set("stage_generation", plan->current_stage_generation.raw());
    local.set("stage_blast_radius_percent", stage_blast_radius(*plan, *stage));
  }
  local.set("candidate_model_generation", plan->candidate_generation.raw());
  local.set("candidate_artifact_generation", plan->candidate_artifact_generation.raw());
  local.set("plan_compatibility_generation", plan->compatibility_generation.raw());
  local.set("policy_generation", effective.generation.raw());
  local.set("coordinator_epoch", impl_->state.epoch.raw());
  local.set_outcome(decision.outcome());
  local.normalize();
  decision.normalize();
  if (explanation != nullptr) *explanation = local;
  return decision;
}

Decision LifecycleEngine::evaluate_stage_advance(const RolloutProgressRequest& request,
                                                 Explanation* explanation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluate_stage_advance_locked(request, explanation);
}

Decision LifecycleEngine::advance_stage(const RolloutProgressRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision gate = evaluate_stage_advance_locked(request, nullptr);
  RolloutPlan* plan = impl_->rollout_mut(request.rollout);
  if (plan == nullptr) return gate;

  const bool stage_failed = gate.has(ReasonCode::StageFailureThresholdExceeded);
  if (!gate.allowed() && !stage_failed) return gate;

  ModelVersionRecord* record = impl_->version_mut(plan->model, plan->candidate);
  if (record == nullptr) {
    Decision decision(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout candidate is not registered");
  }
  const StageDefinition& stage = plan->stages[plan->current_stage_index];
  const std::vector<ScopeId> scopes = stage_scopes(*plan, stage);

  // --- failure path -------------------------------------------------------
  if (stage_failed) {
    StageRecord failed;
    failed.stage = stage.id;
    failed.generation = plan->current_stage_generation;
    failed.entered_sequence = plan->updated_sequence;
    failed.decided_sequence = impl_->bump_sequence();
    failed.decision = StageDecision::Failed;
    failed.reasons = gate.reasons();
    plan->history.push_back(std::move(failed));
    plan->failed = true;
    plan->updated_sequence = impl_->bump_sequence();
    impl_->state.authority.supersede_rollout(plan->id, plan->generation);
    impl_->advance_lifecycle_to(*record, LifecycleState::CANARY_FAILED);
    record->updated_sequence = impl_->bump_sequence();

    const LifecyclePolicy effective =
        impl_->state.policy.effective(impl_->state.scopes, plan->root_scope);
    Decision decision(OutcomeCode::STAGE_ADVANCE_BLOCKED);
    for (const Reason& reason : gate.reasons()) decision.add(reason);
    if (effective.allow_automatic_rollback) {
      RollbackRequest rollback;
      rollback.model = plan->model;
      rollback.candidate = plan->candidate;
      rollback.candidate_generation = record->generation;
      rollback.rollout = plan->id;
      rollback.rollout_generation = plan->generation;
      rollback.target = plan->rollback_target;
      rollback.target_generation = plan->rollback_target_generation;
      rollback.scope = plan->root_scope;
      rollback.scope_generation = plan->root_scope_generation;
      rollback.policy_generation = effective.generation;
      rollback.epoch = request.epoch;
      rollback.worker = request.worker;
      rollback.boot = request.boot;
      rollback.automatic = true;
      rollback.detail = "policy-authorized automatic rollback after stage failure";
      {
        // The engine mutex is already held here, so the rollback runs through
        // the unlocked evaluation core and commits in the same transaction as
        // the stage failure it responds to.
        Decision allowed = evaluate_rollback_locked(rollback, nullptr);
        if (allowed.allowed()) {
          const ModelVersionRecord* target = impl_->version_of(plan->model, rollback.target);
          if (target != nullptr) {
            const RollbackId id(impl_->state.next_rollback_id);
            const RollbackGeneration generation = next_generation(record->rollback_generation);
            impl_->state.authority.supersede_scope(plan->root_scope);
            impl_->grant_authority(*record, plan->root_scope, AuthorityKind::Exclusive, nullptr,
                                   nullptr, PromotionId{}, PromotionGeneration{}, id, generation,
                                   true, rollback.target, rollback.target_generation);
            ModelVersionRecord* target_mut = impl_->version_mut(plan->model, rollback.target);
            if (target_mut != nullptr) {
              impl_->advance_lifecycle_to(*target_mut, LifecycleState::CURRENT);
              target_mut->updated_sequence = impl_->bump_sequence();
            }
            record->last_rollback = id;
            record->rollback_generation = generation;
            plan->last_rollback = id;
            plan->last_rollback_generation = generation;
            impl_->state.next_rollback_id = id.raw() + 1;
            impl_->state.committed_rollbacks.emplace(plan->id.raw(), generation.raw());
            impl_->advance_lifecycle_to(*record, LifecycleState::ROLLED_BACK);
            impl_->state.scopes.touch(plan->root_scope);
            decision.set_outcome(OutcomeCode::ROLLBACK_ALLOWED);
          }
        } else {
          for (const Reason& reason : allowed.reasons()) decision.add(reason);
        }
      }
    } else {
      decision.set_outcome(OutcomeCode::ROLLBACK_DECISION_REQUIRED);
      decision.add(ReasonCode::PolicyDeniesAutoRollback,
                   "stage failure requires an explicit rollback decision");
    }
    decision.normalize();
    return decision;
  }

  // --- success path -------------------------------------------------------
  StageRecord advanced;
  advanced.stage = stage.id;
  advanced.generation = plan->current_stage_generation;
  advanced.entered_sequence = plan->updated_sequence;
  advanced.decided_sequence = impl_->bump_sequence();
  advanced.decision = StageDecision::Advanced;
  plan->history.push_back(std::move(advanced));
  plan->updated_sequence = impl_->bump_sequence();

  Decision decision(OutcomeCode::STAGE_ADVANCE_ALLOWED);
  for (ScopeId scope : scopes) {
    Decision granted = impl_->grant_authority(
        *record, scope, AuthorityKind::Exclusive, plan, &stage, plan->promotion,
        plan->promotion_generation, RollbackId{}, RollbackGeneration{}, true, plan->rollback_target,
        plan->rollback_target_generation);
    if (!granted.allowed()) {
      decision.set_outcome(granted.outcome());
      for (const Reason& reason : granted.reasons()) decision.add(reason);
      return decision;
    }
    // The displaced generation may now be drained.
    if (stage.require_drain_after_commit ||
        impl_->state.policy.effective(impl_->state.scopes, plan->root_scope)
            .require_drain_after_stage_commit) {
      impl_->state.authority.mark_draining(plan->model, plan->previous, scope);
      ModelVersionRecord* previous = impl_->version_mut(plan->model, plan->previous);
      if (previous != nullptr && !previous->retired()) {
        impl_->advance_lifecycle_to(*previous, LifecycleState::DRAINING);
      }
    }
  }

  const bool last_stage = plan->current_stage_index + 1 >= plan->stages.size();
  if (last_stage) {
    plan->completed = true;
    plan->stage_entered = false;
    plan->updated_sequence = impl_->bump_sequence();
    impl_->advance_lifecycle_to(*record, LifecycleState::CURRENT);
  } else {
    plan->current_stage_index += 1;
    const StageDefinition& next_stage = plan->stages[plan->current_stage_index];
    plan->stage_entered = true;
    plan->current_stage_generation = next_generation(plan->current_stage_generation);
    plan->updated_sequence = impl_->bump_sequence();
    StageRecord entered;
    entered.stage = next_stage.id;
    entered.generation = plan->current_stage_generation;
    entered.entered_sequence = plan->updated_sequence;
    entered.decision = StageDecision::Entered;
    plan->history.push_back(std::move(entered));
    impl_->advance_lifecycle_to(*record, LifecycleState::PARTIALLY_PROMOTED);
    record->current_stage = next_stage.id;
    record->current_stage_generation = plan->current_stage_generation;
    // Entering the next stage grants canary authority over that stage's cohorts,
    // exactly as beginning the rollout does for the first stage.
    for (ScopeId scope : stage_scopes(*plan, next_stage)) {
      Decision granted = impl_->grant_authority(
          *record, scope, AuthorityKind::Canary, plan, &next_stage, plan->promotion,
          plan->promotion_generation, RollbackId{}, RollbackGeneration{}, false,
          plan->rollback_target, plan->rollback_target_generation);
      if (!granted.allowed()) {
        decision.set_outcome(granted.outcome());
        for (const Reason& reason : granted.reasons()) decision.add(reason);
        return decision;
      }
    }
  }
  if (const ScopeRecord* root = impl_->state.scopes.find(plan->root_scope)) {
    plan->root_scope_generation = root->generation;
  }
  record->updated_sequence = impl_->bump_sequence();
  return decision;
}

Decision LifecycleEngine::fail_stage(const RolloutProgressRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  RolloutPlan* plan = impl_->rollout_mut(request.rollout);
  Decision decision(OutcomeCode::Accepted);
  if (plan == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout is not registered");
  }
  if (request.rollout_generation.valid() && request.rollout_generation != plan->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutGenerationMismatch,
                        "request cites a superseded rollout generation");
  }
  if (request.stage_generation.valid() &&
      request.stage_generation != plan->current_stage_generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutStageGenerationMismatch,
                        "request cites a superseded stage generation");
  }
  const Decision epoch = impl_->check_epoch(request.epoch);
  if (!epoch.allowed()) return epoch;
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) return worker;
  if (plan->superseded) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutSuperseded, "rollout was superseded");
  }
  if (plan->completed || plan->failed) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::RolloutAlreadyComplete, "rollout is no longer live");
  }
  if (!plan->stage_entered) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::StageOrderViolation, "no stage is currently entered");
  }
  for (const StageRecord& entry : plan->history) {
    if (entry.generation != plan->current_stage_generation) continue;
    if (entry.decision == StageDecision::Entered) continue;
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::StageCompletionDuplicate,
                        "this stage generation already has a committed decision");
  }
  ModelVersionRecord* record = impl_->version_mut(plan->model, plan->candidate);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout candidate is not registered");
  }
  const StageDefinition& stage = plan->stages[plan->current_stage_index];
  StageRecord failed;
  failed.stage = stage.id;
  failed.generation = plan->current_stage_generation;
  failed.entered_sequence = plan->updated_sequence;
  failed.decided_sequence = impl_->bump_sequence();
  failed.decision = StageDecision::Failed;
  if (!request.detail.empty()) {
    std::string detail_text = request.detail;
    detail::clamp_detail(detail_text);
    failed.reasons.emplace_back(ReasonCode::StageFailureThresholdExceeded, detail_text);
  }
  plan->history.push_back(std::move(failed));
  plan->failed = true;
  plan->stage_entered = false;
  plan->updated_sequence = impl_->bump_sequence();
  impl_->state.authority.supersede_rollout(plan->id, plan->generation);
  impl_->advance_lifecycle_to(*record, LifecycleState::CANARY_FAILED);
  record->updated_sequence = impl_->bump_sequence();
  return decision;
}

Decision LifecycleEngine::supersede_rollout(RolloutId rollout_id, RolloutGeneration generation,
                                            CoordinatorEpoch epoch, std::string_view reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  RolloutPlan* plan = impl_->rollout_mut(rollout_id);
  if (plan == nullptr) {
    decision.set_outcome(OutcomeCode::NOT_FOUND);
    return decision.add(ReasonCode::UnknownVersion, "rollout is not registered");
  }
  if (generation.valid() && generation != plan->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    return decision.add(ReasonCode::RolloutGenerationMismatch,
                        "a superseded generation cannot supersede a newer plan");
  }
  if (epoch.valid() && epoch < impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "supersession request originates from a superseded epoch");
  }
  if (plan->superseded) return decision;

  plan->superseded = true;
  plan->stage_entered = false;
  plan->updated_sequence = impl_->bump_sequence();
  StageRecord record;
  record.stage = plan->stages[plan->current_stage_index].id;
  record.generation = plan->current_stage_generation;
  record.entered_sequence = plan->created_sequence;
  record.decided_sequence = plan->updated_sequence;
  record.decision = StageDecision::Superseded;
  if (!reason.empty()) {
    std::string detail_text(reason);
    detail::clamp_detail(detail_text);
    record.reasons.emplace_back(ReasonCode::RolloutSuperseded, detail_text);
  }
  plan->history.push_back(std::move(record));
  impl_->state.authority.supersede_rollout(plan->id, plan->generation);
  ModelVersionRecord* candidate = impl_->version_mut(plan->model, plan->candidate);
  if (candidate != nullptr && !candidate->retired()) {
    impl_->advance_lifecycle_to(*candidate, LifecycleState::DRAINING);
    candidate->updated_sequence = impl_->bump_sequence();
  }
  return decision;
}

}  // namespace mlf
