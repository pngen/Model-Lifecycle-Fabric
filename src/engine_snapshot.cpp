#include "engine_impl.hpp"

#include <algorithm>

namespace mlf {
namespace {

/// Evidence kinds whose value is a durable lifecycle fact rather than a live
/// telemetry reading. Only these survive a restart.
bool is_durable_evidence(EvidenceKind kind) noexcept {
  switch (kind) {
    case EvidenceKind::ArtifactIntegrity:
    case EvidenceKind::RuntimeCompatibility:
    case EvidenceKind::BackendCompatibility:
    case EvidenceKind::HardwareCapability:
    case EvidenceKind::TokenizerMatch:
    case EvidenceKind::AdapterCompatibility:
    case EvidenceKind::TestResult:
    case EvidenceKind::PolicyApproval:
    case EvidenceKind::ManualApproval:
    case EvidenceKind::Signature:
      return true;
    default:
      return false;
  }
}

bool is_settled(AttemptState state) noexcept {
  return state == AttemptState::Completed || state == AttemptState::Failed ||
         state == AttemptState::Abandoned;
}

}  // namespace

std::shared_ptr<const DurableState> LifecycleEngine::export_durable_state() const {
  std::lock_guard<std::mutex> guard(mutex_);
  auto state = std::make_shared<DurableState>();
  state->format_version = kStateFormatVersion;
  state->generation = next_generation(impl_->state.snapshot_generation);
  state->epoch = impl_->state.epoch;
  state->sequence = impl_->state.sequence;

  state->models = impl_->state.models;
  state->versions = impl_->state.versions;
  state->scopes = impl_->state.scopes.records();
  state->root_scope = impl_->state.scopes.root();
  state->policy_overrides = impl_->state.policy.overrides();
  state->base_policy = impl_->state.policy.base();
  state->compatibility = impl_->state.compatibility.raw();

  state->rollouts = impl_->state.rollouts;
  state->authority.clear();
  for (const auto& entry : impl_->state.authority.raw()) {
    std::vector<AuthorityBinding> live;
    for (const AuthorityBinding& binding : entry.second) {
      // Superseded bindings are not authoritative and are not durable; they are
      // reconstructed from the sequence history of the structures they came
      // from. Persisting them would let a restart resurrect a dead binding.
      if (binding.superseded) continue;
      live.push_back(binding);
    }
    if (!live.empty()) state->authority.emplace(entry.first, std::move(live));
  }

  state->worker_leases.clear();
  for (const auto& entry : impl_->state.workers) {
    WorkerLease lease = entry.second;
    // A worker's liveness is volatile. On load the lease is persisted as fenced
    // so that recovery cannot treat a dead boot as current.
    lease.fenced = true;
    lease.detail = "fenced by coordinator restart";
    state->worker_leases.emplace(entry.first, std::move(lease));
  }

  state->evidence.clear();
  for (const auto& bucket : impl_->state.evidence.raw_buckets()) {
    for (const EvidenceRecord& record : bucket.second) {
      if (!is_durable_evidence(record.kind)) continue;
      state->evidence.push_back(record);
    }
  }
  std::sort(state->evidence.begin(), state->evidence.end(),
            [](const EvidenceRecord& a, const EvidenceRecord& b) {
              return a.generation < b.generation;
            });

  state->unsettled_attempts.clear();
  for (const auto& entry : impl_->state.attempts) {
    if (is_settled(entry.second.state)) continue;
    state->unsettled_attempts.emplace(entry.first, entry.second);
  }

  state->committed_promotions = impl_->state.committed_promotions;
  state->committed_rollbacks = impl_->state.committed_rollbacks;

  state->next_model_id = impl_->state.next_model_id;
  state->next_version_id = impl_->state.next_version_id;
  state->next_rollout_id = impl_->state.next_rollout_id;
  state->next_stage_id = impl_->state.next_stage_id;
  state->next_cohort_id = impl_->state.next_cohort_id;
  state->next_promotion_id = impl_->state.next_promotion_id;
  state->next_rollback_id = impl_->state.next_rollback_id;
  state->next_attempt_id = impl_->state.next_attempt_id;
  state->next_worker_id = impl_->state.next_worker_id;
  state->next_evidence_id = impl_->state.next_evidence_id;
  state->next_evidence_generation = impl_->state.next_evidence_generation;
  state->next_compatibility_generation = impl_->state.next_compatibility_generation;
  return state;
}

Decision LifecycleEngine::import_durable_state(const DurableState& state, CoordinatorEpoch new_epoch) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);

  if (state.format_version != kStateFormatVersion) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::SnapshotGenerationStale,
                        "durable state format version is not supported");
  }
  if (!new_epoch.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::CoordinatorEpochStale, "a recovery epoch is required");
  }
  if (new_epoch <= state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "recovery must advance the coordinator epoch");
  }
  if (state.sequence <= impl_->state.sequence && impl_->state.sequence.valid() &&
      !impl_->state.models.empty()) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::SequenceRegression,
                        "durable state is older than the live engine state");
  }

  // --- validate everything before applying anything -----------------------
  ScopeRegistry scopes;
  scopes.set_max_scopes(config_.bounds.max_scopes);
  if (!scopes.restore(state.scopes, state.root_scope)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope,
                        "durable scope tree is structurally invalid");
  }

  if (state.models.size() > config_.bounds.max_models) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsModelCount, "durable model count exceeds bounds");
  }
  std::map<std::string, ModelId> model_names;
  for (const auto& entry : state.models) {
    if (!entry.second.id.valid() || entry.first != entry.second.id.raw()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::DuplicateModel, "durable model identity is inconsistent");
    }
    if (!model_names.emplace(entry.second.name, entry.second.id).second) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::DuplicateModel, "durable model names are not unique");
    }
  }

  std::size_t per_model_limit = 0;
  for (const auto& entry : state.models) {
    std::size_t count = 0;
    for (const auto& version : state.versions) {
      if (version.first.first == entry.first) ++count;
    }
    per_model_limit = std::max(per_model_limit, count);
  }
  if (per_model_limit > config_.bounds.max_versions_per_model) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsVersionCount,
                        "durable per-model version count exceeds bounds");
  }
  std::map<std::pair<std::uint64_t, std::string>, ModelVersionId> version_labels;
  for (const auto& entry : state.versions) {
    const ModelVersionRecord& record = entry.second;
    if (entry.first.first != record.model.raw() || entry.first.second != record.version.raw()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::DuplicateVersion, "durable version key is inconsistent");
    }
    if (state.models.find(record.model.raw()) == state.models.end()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::UnknownModel, "durable version references an unknown model");
    }
    if (static_cast<std::size_t>(record.state) >= kLifecycleStateCount) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::InvalidLifecycleTransition,
                          "durable version carries an impossible lifecycle state");
    }
    if (!record.generation.valid() || !record.lifecycle_generation.valid()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::ModelGenerationMismatch,
                          "durable version carries a zero generation");
    }
    if (!version_labels.emplace(std::make_pair(record.model.raw(), record.label), record.version)
             .second) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::DuplicateVersion,
                          "durable version labels are not unique per model");
    }
  }

  CompatibilityRegistry compatibility(impl_->state.compatibility.max_facts());
  if (!compatibility.restore(state.compatibility)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::CompatibilityGenerationMismatch,
                        "durable compatibility facts are inconsistent");
  }
  CompatibilityGeneration latest_compatibility;
  for (const auto& entry : state.compatibility) {
    const auto key = std::make_pair(entry.second.model.raw(), entry.second.version.raw());
    if (state.versions.find(key) == state.versions.end()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::UnknownVersion,
                          "durable compatibility fact references an unknown version");
    }
    latest_compatibility = std::max(latest_compatibility, entry.second.generation);
  }
  if (state.next_compatibility_generation <= latest_compatibility.raw()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::CompatibilityGenerationMismatch,
                        "durable compatibility allocator would regress");
  }

  AuthorityTable authority(impl_->state.authority.max_bindings());
  if (!authority.restore(state.authority)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PolicyDeniesCoexistence,
                        "durable authority table violates the single-exclusive invariant");
  }
  std::map<std::uint64_t, bool> worker_liveness;
  for (const auto& entry : state.authority) {
    for (const AuthorityBinding& binding : entry.second) {
      const auto key = std::make_pair(binding.model.raw(), binding.version.raw());
      const auto version = state.versions.find(key);
      if (version == state.versions.end()) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::UnknownVersion,
                            "durable authority references an unknown version");
      }
      if (version->second.retired()) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::VersionRetired,
                            "durable authority would resurrect a retired generation");
      }
      if (binding.model_generation > version->second.generation) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::ModelGenerationMismatch,
                            "durable authority cites a future model generation");
      }
      if (state.scopes.empty() || binding.scope.raw() > state.scopes.size()) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::UnknownScope,
                            "durable authority references an unknown scope");
      }
      if (binding.rollout.valid()) {
        const auto rollout = state.rollouts.find(binding.rollout.raw());
        if (rollout == state.rollouts.end()) {
          decision.set_outcome(OutcomeCode::Rejected);
          return decision.add(ReasonCode::UnknownVersion,
                              "durable authority references an unknown rollout");
        }
        if (rollout->second.generation != binding.rollout_generation) {
          decision.set_outcome(OutcomeCode::Rejected);
          return decision.add(ReasonCode::RolloutGenerationMismatch,
                              "durable authority rollout generation does not match the plan");
        }
      }
      if (!binding.granted_sequence.valid()) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::SequenceRegression,
                            "durable authority carries no granting sequence");
      }
    }
  }

  EvidenceStore evidence(impl_->state.evidence.max_records());
  if (!evidence.restore(state.evidence, 0)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::EvidenceGenerationMismatch,
                        "durable evidence is inconsistent");
  }
  for (const EvidenceRecord& record : state.evidence) {
    if (!is_durable_evidence(record.kind)) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::EvidenceStale,
                          "durable state carries a volatile evidence kind");
    }
  }

  std::uint64_t highest_evaluation = 0;
  for (const auto& entry : state.rollouts) {
    if (entry.first != entry.second.id.raw()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::RolloutGenerationMismatch,
                          "durable rollout key is inconsistent");
    }
    if (state.versions.find(std::make_pair(entry.second.model.raw(), entry.second.candidate.raw())) ==
        state.versions.end()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::UnknownVersion,
                          "durable rollout references an unknown candidate");
    }
    if (entry.second.stages.empty()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::StageOrderViolation, "durable rollout has no stages");
    }
    if (entry.second.live() && entry.second.current_stage_index >= entry.second.stages.size()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::StageOrderViolation,
                          "durable rollout stage index is out of range");
    }
    highest_evaluation = std::max(highest_evaluation, entry.second.generation.raw());
  }
  if (state.rollouts.size() > config_.bounds.max_rollouts) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsRolloutCount, "durable rollout count exceeds bounds");
  }
  if (state.next_rollout_id <= highest_evaluation) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::RolloutGenerationMismatch,
                        "durable rollout allocator would regress");
  }

  PolicySet policy;
  if (!policy.restore(state.policy_overrides, state.base_policy)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PolicyGenerationMismatch,
                        "durable policy structure is invalid");
  }
  for (const auto& entry : state.policy_overrides) {
    if (entry.first == 0 || entry.first > state.scopes.size()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::UnknownScope,
                          "durable policy override references an unknown scope");
    }
  }

  std::map<std::uint64_t, WorkerLease> workers;
  for (const auto& entry : state.worker_leases) {
    WorkerLease lease = entry.second;
    if (entry.first != lease.id.raw()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::WorkerUnknown, "durable worker lease key is inconsistent");
    }
    // Recovery never resurrects worker authority.
    lease.fenced = true;
    if (lease.fenced_sequence.raw() == 0) lease.fenced_sequence = state.sequence;
    workers.emplace(entry.first, std::move(lease));
    worker_liveness[entry.first] = false;
  }
  if (workers.size() > config_.bounds.max_workers) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsWorkerCount, "durable worker count exceeds bounds");
  }

  std::map<std::uint64_t, Attempt> attempts;
  for (const auto& entry : state.unsettled_attempts) {
    Attempt attempt = entry.second;
    if (entry.first != attempt.id.raw()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::AttemptUnknown, "durable attempt key is inconsistent");
    }
    if (attempt.state == AttemptState::Dispatched) {
      // The performer's fate is unknown after a restart. Keep the attempt
      // conservative instead of pretending it either happened or did not.
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.detail = "coordinator restarted while the attempt was in flight";
    }
    attempts.emplace(entry.first, std::move(attempt));
  }
  if (attempts.size() > config_.bounds.max_attempts) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsAttemptCount, "durable attempt count exceeds bounds");
  }

  // --- apply --------------------------------------------------------------
  impl_->state.models = state.models;
  impl_->state.model_names = std::move(model_names);
  impl_->state.versions = state.versions;
  impl_->state.version_labels = std::move(version_labels);
  impl_->state.version_index.clear();
  for (const auto& entry : impl_->state.versions) {
    impl_->state.version_index.emplace(entry.second.version.raw(), entry.first);
  }
  impl_->state.scopes = std::move(scopes);
  impl_->state.policy = std::move(policy);
  impl_->state.compatibility = std::move(compatibility);
  impl_->state.authority = std::move(authority);
  impl_->state.evidence = std::move(evidence);
  impl_->state.rollouts = state.rollouts;
  impl_->state.workers = std::move(workers);
  impl_->state.worker_liveness = std::move(worker_liveness);
  impl_->state.attempts = std::move(attempts);
  impl_->state.rollout_driver_boot.clear();
  impl_->state.committed_promotions = state.committed_promotions;
  impl_->state.committed_rollbacks = state.committed_rollbacks;
  impl_->state.next_model_id = state.next_model_id;
  impl_->state.next_version_id = state.next_version_id;
  impl_->state.next_rollout_id = state.next_rollout_id;
  impl_->state.next_stage_id = state.next_stage_id;
  impl_->state.next_cohort_id = state.next_cohort_id;
  impl_->state.next_promotion_id = state.next_promotion_id;
  impl_->state.next_rollback_id = state.next_rollback_id;
  impl_->state.next_attempt_id = state.next_attempt_id;
  impl_->state.next_worker_id = state.next_worker_id;
  impl_->state.next_evidence_id = state.next_evidence_id;
  impl_->state.next_evidence_generation = std::max(state.next_evidence_generation, state.sequence.raw() + 1);
  impl_->state.next_compatibility_generation = state.next_compatibility_generation;
  impl_->state.sequence = std::max(state.sequence, impl_->state.sequence);
  impl_->state.epoch = new_epoch;
  impl_->state.snapshot_generation = state.generation;
  impl_->bump_sequence();
  return decision;
}

void LifecycleEngine::invalidate_volatile_state(std::string_view cause) {
  std::lock_guard<std::mutex> guard(mutex_);
  std::string detail_text(cause);
  detail::clamp_detail(detail_text);

  for (auto& entry : impl_->state.workers) {
    entry.second.fenced = true;
    if (entry.second.detail.empty()) entry.second.detail = detail_text;
  }
  for (auto& entry : impl_->state.worker_liveness) entry.second = false;
  impl_->state.rollout_driver_boot.clear();

  // Dynamic evidence is not durable truth: it is withdrawn so that no later
  // decision can consume a reading taken before the restart. Durable lifecycle
  // evidence (artifact identity, compatibility, approvals, test provenance) is
  // preserved.
  std::vector<EvidenceKind> volatile_kinds;
  for (std::size_t i = 0; i < kEvidenceKindCount; ++i) {
    const auto kind = static_cast<EvidenceKind>(i);
    if (!is_durable_evidence(kind)) volatile_kinds.push_back(kind);
  }
  static_cast<void>(impl_->state.evidence.remove_kinds(volatile_kinds));
  for (auto& entry : impl_->state.rollouts) {
    RolloutPlan& plan = entry.second;
    if (!plan.live()) continue;
    if (!plan.stage_entered) continue;
    // A persisted stage does not imply its workers are still current.
    plan.updated_sequence = impl_->bump_sequence();
  }
  impl_->bump_sequence();
}

}  // namespace mlf
