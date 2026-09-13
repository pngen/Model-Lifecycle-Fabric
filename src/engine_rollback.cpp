#include "engine_impl.hpp"

#include <algorithm>

namespace mlf {
namespace {

void raise(Decision& decision, OutcomeCode code) {
  const auto rank = [](OutcomeCode value) {
    switch (value) {
      case OutcomeCode::ROLLBACK_ALLOWED:   return 0;
      case OutcomeCode::Rejected:           return 1;
      case OutcomeCode::ROLLBACK_BLOCKED:   return 2;
      case OutcomeCode::NOT_FOUND:          return 3;
      case OutcomeCode::CONFLICT:           return 4;
      case OutcomeCode::STALE_PLAN:         return 5;
      case OutcomeCode::STALE_EVIDENCE:     return 6;
      case OutcomeCode::REVALIDATION_REQUIRED: return 7;
      case OutcomeCode::AMBIGUOUS:          return 8;
      case OutcomeCode::INCOMPATIBLE:       return 9;
      default:                              return 2;
    }
  };
  if (rank(code) > rank(decision.outcome())) decision.set_outcome(code);
}

}  // namespace

Decision LifecycleEngine::evaluate_rollback_locked(const RollbackRequest& request,
                                                   Explanation* explanation) const {
  Decision decision(OutcomeCode::ROLLBACK_ALLOWED);
  Explanation local(OutcomeCode::ROLLBACK_ALLOWED);
  const auto fault = [&decision, &local](OutcomeCode code, ReasonCode reason, std::string text) {
    raise(decision, code);
    detail::clamp_detail(text);
    decision.add(reason, text);
    local.add(reason, text);
  };

  const ModelVersionRecord* candidate = impl_->version_of(request.model, request.candidate);
  if (candidate == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion, "rollback candidate is not "
                                                              "registered");
    if (explanation != nullptr) {
      local.normalize();
      *explanation = local;
    }
    return decision;
  }
  if (request.candidate_generation.valid() &&
      request.candidate_generation != candidate->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ModelGenerationMismatch,
          "rollback cites a superseded candidate generation");
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
  const Decision worker = impl_->check_worker(request.worker, request.boot, request.epoch);
  if (!worker.allowed()) {
    for (const Reason& reason : worker.reasons()) fault(worker.outcome(), reason.code, reason.detail);
  }

  const ScopeRecord* scope_record = impl_->state.scopes.find(request.scope);
  if (scope_record == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownScope, "rollback scope is not registered");
  } else if (request.scope_generation.valid() &&
             request.scope_generation != scope_record->generation) {
    fault(OutcomeCode::STALE_PLAN, ReasonCode::ScopeGenerationMismatch,
          "rollback cites a superseded scope generation");
  }

  // The candidate must actually hold authority that can be taken away.
  bool candidate_holds = false;
  if (scope_record != nullptr) {
    for (const AuthorityBinding* binding : impl_->state.authority.live_bindings_for(request.scope)) {
      if (binding->model != candidate->model || binding->version != candidate->version) continue;
      if (binding->kind == AuthorityKind::RollbackRetained) continue;
      candidate_holds = true;
    }
  }
  if (!candidate_holds) {
    fault(OutcomeCode::ROLLBACK_BLOCKED, ReasonCode::NoCurrentAuthority,
          "the candidate holds no live authority in the rollback scope");
  }

  const RolloutPlan* plan = nullptr;
  if (request.rollout.valid()) {
    plan = impl_->rollout_of(request.rollout);
    if (plan == nullptr) {
      fault(OutcomeCode::NOT_FOUND, ReasonCode::UnknownVersion, "rollback cites an unknown "
                                                                "rollout");
    } else {
      if (request.rollout_generation.valid() && request.rollout_generation != plan->generation) {
        fault(OutcomeCode::STALE_PLAN, ReasonCode::RolloutGenerationMismatch,
              "rollback cites a superseded rollout generation");
      }
      if (plan->candidate != candidate->version) {
        fault(OutcomeCode::CONFLICT, ReasonCode::RollbackSupersededByNewerGeneration,
              "rollback cites a rollout for a different candidate generation");
      }
      if (plan->last_rollback.valid()) {
        fault(OutcomeCode::CONFLICT, ReasonCode::RollbackCommitDuplicate,
              "this rollout already committed a rollback");
      }
    }
  }

  const ModelVersionRecord* target = impl_->version_of(request.model, request.target);
  if (target == nullptr) {
    fault(OutcomeCode::NOT_FOUND, ReasonCode::RollbackTargetUnknown,
          "rollback target is not registered for this model");
  } else {
    if (target->retired()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetRetired,
            "rollback target is retired and cannot be restored");
    }
    if (!target->artifact.valid()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetArtifactMissing,
            "rollback target has no artifact binding");
    }
    if (request.target_artifact_generation.valid() &&
        request.target_artifact_generation != target->artifact.generation) {
      fault(OutcomeCode::STALE_PLAN, ReasonCode::ArtifactGenerationMismatch,
            "rollback cites a superseded target artifact generation");
    }
    if (request.target_generation.valid() && request.target_generation != target->generation) {
      fault(OutcomeCode::STALE_PLAN, ReasonCode::ModelGenerationMismatch,
            "rollback cites a superseded target model generation");
    }
    if (!target->has_compatibility()) {
      fault(OutcomeCode::INCOMPATIBLE, ReasonCode::RollbackTargetIncompatible,
            "rollback target has no current compatibility binding");
    }
    if (target->version == candidate->version) {
      fault(OutcomeCode::Rejected, ReasonCode::RollbackTargetUnknown,
            "rollback target is the candidate itself");
    }
  }

  const LifecyclePolicy effective =
      scope_record != nullptr
          ? impl_->state.policy.effective(impl_->state.scopes, request.scope)
          : impl_->state.policy.base();
  if (request.automatic && !effective.allow_automatic_rollback) {
    fault(OutcomeCode::ROLLBACK_BLOCKED, ReasonCode::PolicyDeniesAutoRollback,
          "policy does not authorize automatic rollback");
  }
  if (!effective.require_rollback_target && !request.target.valid()) {
    fault(OutcomeCode::ROLLBACK_BLOCKED, ReasonCode::RollbackTargetRequired,
          "a rollback requires an explicit target generation");
  }

  local.set("candidate_model_generation", candidate->generation.raw());
  local.set("candidate_lifecycle_state", std::string(to_string(candidate->state)));
  if (target != nullptr) {
    local.set("target_version_id", target->version.raw());
    local.set("target_label", target->label);
    local.set("target_model_generation", target->generation.raw());
    local.set("target_lifecycle_state", std::string(to_string(target->state)));
    local.set("target_artifact_generation", target->artifact.generation.raw());
    local.set("target_compatibility_generation", target->compatibility_generation.raw());
  }
  if (scope_record != nullptr) local.set("scope", scope_record->path);
  if (plan != nullptr) {
    local.set("rollout_id", plan->id.raw());
    local.set("rollout_generation", plan->generation.raw());
    local.set("rollout_failed", plan->failed ? "yes" : "no");
  }
  local.set("automatic", request.automatic ? "yes" : "no");
  local.set("policy_generation", effective.generation.raw());
  local.set("coordinator_epoch", impl_->state.epoch.raw());
  local.set_outcome(decision.outcome());
  local.normalize();
  decision.normalize();
  if (explanation != nullptr) *explanation = local;
  return decision;
}

Decision LifecycleEngine::evaluate_rollback(const RollbackRequest& request,
                                            Explanation* explanation) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return evaluate_rollback_locked(request, explanation);
}

RollbackOutcome LifecycleEngine::rollback(const RollbackRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  RollbackOutcome outcome;
  Explanation explanation;
  Decision gate = evaluate_rollback_locked(request, &explanation);
  outcome.decision = gate;
  if (!gate.allowed()) return outcome;

  ModelVersionRecord* candidate = impl_->version_mut(request.model, request.candidate);
  ModelVersionRecord* target = impl_->version_mut(request.model, request.target);
  if (candidate == nullptr || target == nullptr) {
    outcome.decision = Decision(OutcomeCode::NOT_FOUND);
    outcome.decision.add(ReasonCode::UnknownVersion, "rollback participants vanished");
    return outcome;
  }

  const RollbackId rollback_id(impl_->state.next_rollback_id);
  const RollbackGeneration rollback_generation = next_generation(candidate->rollback_generation);

  // 1. Fence the candidate's promotion authority in the rollback scope. A
  //    rollback never lets the failed generation keep serving that scope.
  impl_->state.authority.supersede_scope(request.scope);

  // 2. Restore the previous generation's authority.
  Decision granted = impl_->grant_authority(*target, request.scope, AuthorityKind::Exclusive,
                                            nullptr, nullptr, PromotionId{},
                                            PromotionGeneration{}, rollback_id,
                                            rollback_generation, true, ModelVersionId{},
                                            ModelGeneration{});
  if (!granted.allowed()) {
    outcome.decision = granted;
    return outcome;
  }
  impl_->advance_lifecycle_to(*target, LifecycleState::CURRENT);
  target->updated_sequence = impl_->bump_sequence();
  outcome.restored_scopes.push_back(request.scope);

  // 3. Drain the candidate where it lost authority, and record the rollback.
  impl_->state.authority.mark_draining(candidate->model, candidate->version, request.scope);
  if (!impl_->holds_live_authority(candidate->model, candidate->version, true)) {
    impl_->advance_lifecycle_to(*candidate, LifecycleState::ROLLED_BACK);
  } else {
    impl_->advance_lifecycle_to(*candidate, LifecycleState::PARTIALLY_PROMOTED);
  }
  candidate->last_rollback = rollback_id;
  candidate->rollback_generation = rollback_generation;
  candidate->updated_sequence = impl_->bump_sequence();

  // 4. Stop further rollout of the candidate. A late stage completion can no
  //    longer commit because the plan is no longer live and its authority is
  //    gone.
  if (request.rollout.valid()) {
    RolloutPlan* plan = impl_->rollout_mut(request.rollout);
    if (plan != nullptr) {
      if (plan->live()) {
        plan->failed = true;
        plan->stage_entered = false;
        StageRecord record;
        record.stage = plan->stages[plan->current_stage_index].id;
        record.generation = plan->current_stage_generation;
        record.entered_sequence = plan->created_sequence;
        record.decided_sequence = impl_->bump_sequence();
        record.decision = StageDecision::RolledBack;
        record.reasons.emplace_back(ReasonCode::RolloutSuperseded,
                                    "rollback committed against this rollout");
        plan->history.push_back(std::move(record));
      }
      plan->last_rollback = rollback_id;
      plan->last_rollback_generation = rollback_generation;
      plan->updated_sequence = impl_->bump_sequence();
    }
  }

  impl_->state.next_rollback_id = rollback_id.raw() + 1;
  impl_->state.committed_rollbacks.emplace(request.rollout.valid() ? request.rollout.raw() : 0,
                                           rollback_generation.raw());
  impl_->state.scopes.touch(request.scope);

  outcome.rollback = rollback_id;
  outcome.rollback_generation = rollback_generation;
  outcome.decision = Decision(OutcomeCode::ROLLBACK_ALLOWED);
  return outcome;
}

}  // namespace mlf
