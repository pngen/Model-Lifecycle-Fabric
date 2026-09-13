#include "engine_impl.hpp"

#include <algorithm>

namespace mlf {

// ---------------------------------------------------------------------------
// Read-only queries
// ---------------------------------------------------------------------------

std::optional<ModelId> LifecycleEngine::find_model_by_name(std::string_view name) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = impl_->state.model_names.find(std::string(name));
  if (found == impl_->state.model_names.end()) return std::nullopt;
  return found->second;
}

std::optional<ModelId> LifecycleEngine::find_model_by_id(std::uint64_t raw) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = impl_->state.models.find(raw);
  if (found == impl_->state.models.end()) return std::nullopt;
  return found->second.id;
}

std::optional<ModelVersionId> LifecycleEngine::find_version_by_label(ModelId model,
                                                                    std::string_view label) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = impl_->state.version_labels.find(std::make_pair(model.raw(), std::string(label)));
  if (found == impl_->state.version_labels.end()) return std::nullopt;
  return found->second;
}

std::optional<ScopeId> LifecycleEngine::find_scope_by_path(std::string_view path) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.scopes.find_path(path);
}

std::vector<ModelRecord> LifecycleEngine::models() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ModelRecord> out;
  out.reserve(impl_->state.models.size());
  for (const auto& entry : impl_->state.models) out.push_back(entry.second);
  return out;
}

std::vector<ModelVersionRecord> LifecycleEngine::versions(ModelId model) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ModelVersionRecord> out;
  for (const auto& entry : impl_->state.versions) {
    if (entry.first.first == model.raw()) out.push_back(entry.second);
  }
  return out;
}

std::vector<ModelVersionRecord> LifecycleEngine::all_versions() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<ModelVersionRecord> out;
  out.reserve(impl_->state.versions.size());
  for (const auto& entry : impl_->state.versions) out.push_back(entry.second);
  return out;
}

std::optional<ModelVersionRecord> LifecycleEngine::version(ModelId model,
                                                           ModelVersionId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const ModelVersionRecord* record = impl_->version_of(model, id);
  if (record == nullptr) return std::nullopt;
  return *record;
}

std::optional<ModelRecord> LifecycleEngine::model(ModelId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = impl_->state.models.find(id.raw());
  if (found == impl_->state.models.end()) return std::nullopt;
  return found->second;
}

std::vector<ScopeRecord> LifecycleEngine::scopes() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.scopes.records();
}

std::vector<RolloutPlan> LifecycleEngine::rollouts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<RolloutPlan> out;
  out.reserve(impl_->state.rollouts.size());
  for (const auto& entry : impl_->state.rollouts) out.push_back(entry.second);
  return out;
}

std::optional<RolloutPlan> LifecycleEngine::rollout(RolloutId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const RolloutPlan* plan = impl_->rollout_of(id);
  if (plan == nullptr) return std::nullopt;
  return *plan;
}

std::vector<Attempt> LifecycleEngine::attempts() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<Attempt> out;
  out.reserve(impl_->state.attempts.size());
  for (const auto& entry : impl_->state.attempts) out.push_back(entry.second);
  return out;
}

std::optional<Attempt> LifecycleEngine::attempt(AttemptId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const auto found = impl_->state.attempts.find(id.raw());
  if (found == impl_->state.attempts.end()) return std::nullopt;
  return found->second;
}

std::vector<WorkerLease> LifecycleEngine::workers() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<WorkerLease> out;
  out.reserve(impl_->state.workers.size());
  for (const auto& entry : impl_->state.workers) out.push_back(entry.second);
  return out;
}

std::optional<WorkerLease> LifecycleEngine::worker(WorkerId id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const WorkerLease* lease = impl_->worker_of(id);
  if (lease == nullptr) return std::nullopt;
  return *lease;
}

std::vector<CompatibilityFact> LifecycleEngine::compatibility_facts(ModelVersionId version) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<CompatibilityFact> out;
  for (const auto& entry : impl_->state.compatibility.raw()) {
    if (entry.second.version == version) out.push_back(entry.second);
  }
  return out;
}

std::vector<EvidenceRecord> LifecycleEngine::evidence_for(ModelVersionId version) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<EvidenceRecord> out;
  for (const auto& entry : impl_->state.evidence.raw_buckets()) {
    for (const EvidenceRecord& record : entry.second) {
      if (record.subject.version == version) out.push_back(record);
    }
  }
  std::sort(out.begin(), out.end(), [](const EvidenceRecord& a, const EvidenceRecord& b) {
    return a.generation < b.generation;
  });
  return out;
}

std::vector<EvidenceRecord> LifecycleEngine::all_evidence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<EvidenceRecord> out;
  for (const auto& entry : impl_->state.evidence.raw_buckets()) {
    for (const EvidenceRecord& record : entry.second) out.push_back(record);
  }
  std::sort(out.begin(), out.end(), [](const EvidenceRecord& a, const EvidenceRecord& b) {
    return a.generation < b.generation;
  });
  return out;
}

LifecyclePolicy LifecycleEngine::current_policy() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.policy.base();
}

LifecyclePolicy LifecycleEngine::effective_policy(ScopeId scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.policy.effective(impl_->state.scopes, scope);
}

PolicyGeneration LifecycleEngine::policy_generation() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.policy.base().generation;
}

CoordinatorEpoch LifecycleEngine::epoch() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.epoch;
}

Sequence LifecycleEngine::sequence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.sequence;
}

SnapshotGeneration LifecycleEngine::snapshot_generation() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.snapshot_generation;
}

StoreStats LifecycleEngine::stats() const {
  std::lock_guard<std::mutex> guard(mutex_);
  StoreStats stats;
  stats.models = impl_->state.models.size();
  stats.versions = impl_->state.versions.size();
  stats.scopes = impl_->state.scopes.size();
  stats.rollouts = impl_->state.rollouts.size();
  stats.evidence = impl_->state.evidence.size();
  stats.evidence_dropped = impl_->state.evidence.dropped();
  stats.attempts = impl_->state.attempts.size();
  stats.workers = impl_->state.workers.size();
  stats.compatibility_facts = impl_->state.compatibility.raw().size();
  std::size_t bindings = 0;
  for (const auto& entry : impl_->state.authority.raw()) bindings += entry.second.size();
  stats.authority_bindings = bindings;
  stats.committed_promotions = impl_->state.committed_promotions.size();
  stats.committed_rollbacks = impl_->state.committed_rollbacks.size();
  stats.requeued_frames = impl_->state.requeued_frames;
  return stats;
}

Decision LifecycleEngine::advance_epoch(CoordinatorEpoch expected_current, CoordinatorEpoch& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  out = impl_->state.epoch;
  Decision decision(OutcomeCode::Accepted);
  if (expected_current.valid() && expected_current != impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "epoch precondition does not match the current epoch");
  }
  impl_->state.epoch = next_generation(impl_->state.epoch);
  impl_->bump_sequence();
  out = impl_->state.epoch;
  return decision;
}

// ---------------------------------------------------------------------------
// Compatibility
// ---------------------------------------------------------------------------

CompatibilityResult LifecycleEngine::evaluate_requirements(
    ModelVersionId version_id, const EnvironmentProfile& environment) const {
  std::lock_guard<std::mutex> guard(mutex_);
  CompatibilityResult result;
  result.outcome = CompatibilityOutcome::UNKNOWN;
  result.decision.set_outcome(OutcomeCode::UNSUPPORTED);
  result.decision.add(ReasonCode::UnknownVersion, "version is not registered");
  for (const auto& entry : impl_->state.versions) {
    if (entry.second.version == version_id) {
      return mlf::evaluate_requirements(entry.second.requirements, environment);
    }
  }
  return result;
}

Decision LifecycleEngine::publish_compatibility(ModelVersionId version_id,
                                                const EnvironmentProfile& environment,
                                                CompatibilityOutcome outcome,
                                                std::string_view detail_text,
                                                Provenance provenance) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  ModelVersionRecord* record = nullptr;
  for (auto& entry : impl_->state.versions) {
    if (entry.second.version == version_id) {
      record = &entry.second;
      break;
    }
  }
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "compatibility fact targets an unknown version");
  }
  if (record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired,
                        "compatibility cannot be published for a retired generation");
  }
  if (!environment.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName,
                        "environment key must be a bounded non-empty token");
  }
  if (static_cast<std::size_t>(outcome) >= kCompatibilityOutcomeCount) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::CompatibilityUnknown, "unknown compatibility outcome");
  }

  CompatibilityFact fact;
  fact.model = record->model;
  fact.version = record->version;
  fact.model_generation = record->generation;
  fact.artifact_set = record->artifact.set_id;
  fact.artifact_generation = record->artifact.generation;
  fact.environment_key = environment.key;
  fact.outcome = outcome;
  fact.detail.assign(detail_text);
  detail::clamp_detail(fact.detail, detail::kMaxRecordDetail);
  fact.provenance = provenance == Provenance::Unknown ? config_.default_provenance : provenance;
  fact.sequence = impl_->bump_sequence();

  const CompatibilityGeneration assigned(impl_->state.next_compatibility_generation);
  Decision published = impl_->state.compatibility.publish(fact, assigned);
  if (!published.allowed()) {
    decision.set_outcome(published.outcome());
    for (const Reason& reason : published.reasons()) decision.add(reason);
    return decision;
  }
  impl_->state.next_compatibility_generation = assigned.raw() + 1;
  record->compatibility_generation = assigned;
  record->updated_sequence = fact.sequence;
  if (record->state == LifecycleState::REGISTERED) {
    impl_->transition(*record, LifecycleState::VALIDATING);
  }
  return decision;
}

CompatibilityResult LifecycleEngine::resolve_compatibility(
    ModelVersionId version_id, const EnvironmentProfile& environment,
    CompatibilityGeneration required) const {
  std::lock_guard<std::mutex> guard(mutex_);
  CompatibilityResult result;
  result.outcome = CompatibilityOutcome::UNKNOWN;
  result.decision.set_outcome(OutcomeCode::UNSUPPORTED);
  const ModelVersionRecord* record = nullptr;
  for (const auto& entry : impl_->state.versions) {
    if (entry.second.version == version_id) {
      record = &entry.second;
      break;
    }
  }
  if (record == nullptr) {
    result.decision.add(ReasonCode::UnknownVersion, "version is not registered");
    return result;
  }
  const CompatibilityFact* fact = impl_->state.compatibility.lookup(version_id, environment.key);
  if (fact == nullptr) {
    result.decision.add(ReasonCode::CompatibilityMissing,
                        "no compatibility fact published for this environment");
    return result;
  }
  if (required.valid() && fact->generation != required) {
    result.outcome = CompatibilityOutcome::STALE_EVIDENCE;
    result.decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
    result.decision.add(ReasonCode::CompatibilityGenerationMismatch,
                        "published compatibility generation does not match the required one");
    return result;
  }
  if (fact->model_generation != record->generation ||
      fact->artifact_generation != record->artifact.generation) {
    result.outcome = CompatibilityOutcome::STALE_EVIDENCE;
    result.decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
    result.decision.add(ReasonCode::CompatibilityStale,
                        "published compatibility targets an older generation");
    return result;
  }
  result.outcome = fact->outcome;
  result.decision.set_outcome(is_serving_compatible(fact->outcome) ? OutcomeCode::Ok
                                                                  : OutcomeCode::INCOMPATIBLE);
  if (!is_serving_compatible(fact->outcome)) {
    result.decision.add(reason_for(fact->outcome), fact->detail);
  }
  return result;
}

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

Decision LifecycleEngine::publish_evidence(EvidenceRecord& record) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  if (!record.subject.model.valid() && record.subject.version.valid()) {
    if (const ModelVersionRecord* resolved = impl_->version_of_id(record.subject.version);
        resolved != nullptr) {
      record.subject.model = resolved->model;
    }
  }
  const ModelVersionRecord* owner = impl_->version_of(record.subject.model, record.subject.version);
  if (owner == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "evidence subject version is not registered");
  }
  if (static_cast<std::size_t>(record.kind) >= kEvidenceKindCount) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ProtocolUnknownType, "unknown evidence kind");
  }
  if (!record.subject.model_generation.valid()) {
    record.subject.model_generation = owner->generation;
  }
  if (!record.subject.artifact_set.valid()) record.subject.artifact_set = owner->artifact.set_id;
  if (!record.subject.artifact_generation.valid()) {
    record.subject.artifact_generation = owner->artifact.generation;
  }
  if (!record.subject.compatibility_generation.valid()) {
    record.subject.compatibility_generation = owner->compatibility_generation;
  }
  if (record.provenance == Provenance::Unknown) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::EvidenceUnknownProvenance,
                        "evidence must declare REAL, SYNTHETIC or UNSUPPORTED provenance");
  }
  detail::clamp_detail(record.detail, detail::kMaxRecordDetail);
  detail::clamp_detail(record.unit, 32);

  record.id = EvidenceId(impl_->state.next_evidence_id);
  record.generation = EvidenceGeneration(impl_->state.next_evidence_generation);
  record.epoch = impl_->state.epoch;
  if (!record.policy_generation.valid()) {
    record.policy_generation = impl_->state.policy.base().generation;
  }
  Decision published = impl_->state.evidence.publish(record);
  if (!published.allowed()) {
    record.id = EvidenceId{};
    record.generation = EvidenceGeneration{};
    decision.set_outcome(published.outcome());
    for (const Reason& reason : published.reasons()) decision.add(reason);
    return decision;
  }
  impl_->state.next_evidence_id = record.id.raw() + 1;
  impl_->state.next_evidence_generation = record.generation.raw() + 1;
  return decision;
}

EvidenceLookup LifecycleEngine::check_evidence(const EvidenceRequirement& requirement,
                                               const EvidenceSubject& subject) const {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.evidence.resolve(requirement, subject, WorkerBootId{},
                                       requirement.require_current_publisher, 0,
                                       impl_->state.policy.base().max_evidence_age_ticks);
}

std::size_t LifecycleEngine::expire_publisher_evidence(WorkerBootId boot) {
  std::lock_guard<std::mutex> guard(mutex_);
  const std::size_t removed = impl_->state.evidence.invalidate_publisher_boot(boot);
  if (removed > 0) impl_->bump_sequence();
  return removed;
}

// ---------------------------------------------------------------------------
// Workers
// ---------------------------------------------------------------------------

Decision LifecycleEngine::register_worker(const WorkerRegistration& registration) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  if (!registration.id.valid() || !registration.boot.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerUnknown, "worker identity and boot must be non-zero");
  }
  if (registration.epoch.valid() && registration.epoch != impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "worker registered against a superseded coordinator epoch");
  }
  for (ScopeId scope : registration.scopes) {
    if (impl_->state.scopes.find(scope) == nullptr) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::UnknownScope, "worker claims an unregistered scope");
    }
  }
  const WorkerLease* existing = impl_->worker_of(registration.id);
  if (existing == nullptr && impl_->state.workers.size() >= config_.bounds.max_workers) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsWorkerCount, "worker bound reached");
  }
  if (existing != nullptr && !existing->fenced && existing->boot != registration.boot) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::WorkerBootStale,
                        "an unfenced worker boot already holds this worker identity; fence it "
                        "first so replacement requires an explicit decision");
  }

  WorkerLease lease;
  lease.id = registration.id;
  lease.boot = registration.boot;
  lease.epoch = impl_->state.epoch;
  lease.scopes = registration.scopes;
  std::sort(lease.scopes.begin(), lease.scopes.end());
  lease.scopes.erase(std::unique(lease.scopes.begin(), lease.scopes.end()), lease.scopes.end());
  lease.fenced = false;
  lease.registered_sequence = impl_->bump_sequence();
  impl_->state.workers[lease.id.raw()] = lease;
  impl_->state.worker_liveness[lease.id.raw()] = true;
  return decision;
}

Decision LifecycleEngine::fence_worker(WorkerId id, WorkerBootId boot, CoordinatorEpoch epoch,
                                       std::string_view reason) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  WorkerLease* lease = nullptr;
  const auto found = impl_->state.workers.find(id.raw());
  if (found != impl_->state.workers.end()) lease = &found->second;
  if (lease == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerUnknown, "worker is not registered");
  }
  if (boot.valid() && lease->boot != boot) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::WorkerBootStale,
                        "fence targets a boot that is not the registered boot");
  }
  if (epoch.valid() && epoch < impl_->state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochStale,
                        "fence request originates from a superseded epoch");
  }
  if (lease->fenced) {
    return decision;
  }
  lease->fenced = true;
  lease->fenced_sequence = impl_->bump_sequence();
  lease->detail.assign(reason);
  detail::clamp_detail(lease->detail);
  impl_->state.worker_liveness[id.raw()] = false;

  // A fenced boot permanently loses authority: its evidence is withdrawn so that
  // no later decision can rely on it.
  impl_->state.evidence.invalidate_publisher_boot(lease->boot);
  for (auto& entry : impl_->state.attempts) {
    Attempt& attempt = entry.second;
    if (attempt.boot != lease->boot) continue;
    if (attempt.state == AttemptState::Dispatched) {
      attempt.state = AttemptState::OutcomeUnknown;
      attempt.detail = "performer boot was fenced before the attempt settled";
      attempt.settled_sequence = impl_->bump_sequence();
    }
  }
  return decision;
}

Decision LifecycleEngine::set_worker_liveness(WorkerId id, bool connected) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  if (impl_->state.workers.find(id.raw()) == impl_->state.workers.end()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerUnknown, "worker is not registered");
  }
  impl_->state.worker_liveness[id.raw()] = connected;
  return decision;
}

bool LifecycleEngine::worker_boot_current(WorkerId id, WorkerBootId boot) const {
  std::lock_guard<std::mutex> guard(mutex_);
  const WorkerLease* lease = impl_->worker_of(id);
  return lease != nullptr && !lease->fenced && lease->boot == boot;
}

// ---------------------------------------------------------------------------
// Authority
// ---------------------------------------------------------------------------

AuthorityQueryResult LifecycleEngine::query_authority(ScopeId scope) const {
  std::lock_guard<std::mutex> guard(mutex_);
  AuthorityQueryResult result;
  result.scope = scope;
  const ScopeRecord* scope_record = impl_->state.scopes.find(scope);
  if (scope_record == nullptr) {
    result.outcome = OutcomeCode::NO_AUTHORITATIVE_MODEL;
    result.explanation.set_outcome(OutcomeCode::NO_AUTHORITATIVE_MODEL);
    result.explanation.add(ReasonCode::UnknownScope, "scope is not registered");
    return result;
  }
  result.scope_generation = scope_record->generation;

  const std::vector<ScopeId> chain = impl_->state.scopes.chain(scope);
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const ScopeId node = *it;
    const std::vector<const AuthorityBinding*> live = impl_->state.authority.live_bindings_for(node);
    const AuthorityBinding* exclusive = nullptr;
    std::size_t exclusive_count = 0;
    for (const AuthorityBinding* binding : live) {
      if (binding->kind != AuthorityKind::Exclusive) continue;
      ++exclusive_count;
      if (exclusive == nullptr) exclusive = binding;
    }
    if (exclusive_count > 1) {
      result.outcome = OutcomeCode::AMBIGUOUS;
      result.scope = scope;
      result.explanation.set_outcome(OutcomeCode::AMBIGUOUS);
      result.explanation.set("scope", impl_->state.scopes.path_of(scope));
      result.explanation.set("conflicting_scope", impl_->state.scopes.path_of(node));
      result.explanation.set("exclusive_bindings", exclusive_count);
      result.explanation.add(ReasonCode::PolicyDeniesCoexistence,
                             "more than one exclusive authority binding exists for a scope");
      return result;
    }
    if (node == scope) {
      for (const AuthorityBinding* binding : live) {
        if (binding->kind == AuthorityKind::Canary || binding->kind == AuthorityKind::SplitTraffic) {
          result.overlays.push_back(binding);
        } else if (binding->kind == AuthorityKind::RollbackRetained) {
          result.retained.push_back(binding);
        }
      }
    }
    if (exclusive != nullptr) {
      const ModelVersionRecord* record =
          impl_->version_of(exclusive->model, exclusive->version);
      if (record == nullptr || record->retired()) {
        result.outcome = OutcomeCode::NO_AUTHORITATIVE_MODEL;
        result.explanation.set_outcome(OutcomeCode::NO_AUTHORITATIVE_MODEL);
        result.explanation.set("scope", impl_->state.scopes.path_of(scope));
        result.explanation.add(ReasonCode::VersionRetired,
                               "committed authority points at a generation that no longer exists "
                               "or is retired");
        return result;
      }
      result.outcome = OutcomeCode::Ok;
      result.binding = exclusive;
      result.inherited = node != scope;
      result.inherited_scope = node;
      Explanation& explanation = result.explanation;
      explanation.set_outcome(OutcomeCode::Ok);
      explanation.set("scope", impl_->state.scopes.path_of(scope));
      explanation.set("scope_generation", scope_record->generation.raw());
      explanation.set("authoritative_scope", impl_->state.scopes.path_of(node));
      explanation.set("inherited", result.inherited ? "yes" : "no");
      explanation.set("model_id", exclusive->model.raw());
      explanation.set("model_version_id", exclusive->version.raw());
      explanation.set("model_version_label", record->label);
      explanation.set("model_generation", exclusive->model_generation.raw());
      explanation.set("artifact_set_id", exclusive->artifact_set.raw());
      explanation.set("artifact_generation", exclusive->artifact_generation.raw());
      explanation.set("lifecycle_state", std::string(to_string(record->state)));
      explanation.set("authority_kind", std::string(to_string(exclusive->kind)));
      explanation.set("granted_sequence", exclusive->granted_sequence.raw());
      if (exclusive->rollout.valid()) {
        explanation.set("rollout_id", exclusive->rollout.raw());
        explanation.set("rollout_generation", exclusive->rollout_generation.raw());
        explanation.set("rollout_stage_id", exclusive->stage.raw());
        explanation.set("rollout_stage_generation", exclusive->stage_generation.raw());
      }
      if (exclusive->promotion.valid()) {
        explanation.set("promotion_id", exclusive->promotion.raw());
        explanation.set("promotion_generation", exclusive->promotion_generation.raw());
      }
      explanation.set("canary_overlays", result.overlays.size());
      explanation.set("rollback_retained", result.retained.size());
      if (result.inherited) {
        explanation.add(ReasonCode::PolicyScopeNotTargetable,
                        "no authority is committed at this scope; the answer is inherited from the "
                        "nearest ancestor that holds one");
      }
      return result;
    }
  }

  result.outcome = OutcomeCode::NO_AUTHORITATIVE_MODEL;
  result.explanation.set_outcome(OutcomeCode::NO_AUTHORITATIVE_MODEL);
  result.explanation.set("scope", impl_->state.scopes.path_of(scope));
  result.explanation.set("scope_generation", scope_record->generation.raw());
  result.explanation.set("canary_overlays", result.overlays.size());
  result.explanation.add(ReasonCode::NoCurrentAuthority,
                         "no scope in the ancestor chain holds a live exclusive binding");
  return result;
}

std::vector<AuthorityBinding> LifecycleEngine::authority_bindings(ModelId model,
                                                                 ModelVersionId version_id) const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AuthorityBinding> out;
  for (const AuthorityBinding* binding : impl_->state.authority.bindings_for_version(model, version_id)) {
    out.push_back(*binding);
  }
  return out;
}

std::vector<AuthorityBinding> LifecycleEngine::all_authority_bindings() const {
  std::lock_guard<std::mutex> guard(mutex_);
  std::vector<AuthorityBinding> out;
  for (const auto& entry : impl_->state.authority.raw()) {
    for (const AuthorityBinding& binding : entry.second) out.push_back(binding);
  }
  return out;
}

}  // namespace mlf
