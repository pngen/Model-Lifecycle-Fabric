#include "engine_impl.hpp"

#include <algorithm>

namespace mlf {
namespace {

constexpr char kAttemptKindNames[][24] = {"WARM",              "ACTIVATE", "DRAIN",
                                          "ROLLBACK",          "REHYDRATE", "START_CANARY_REPLICAS",
                                          "PROMOTE_REPLICA_GROUP", "EVICT_RETIRED"};

constexpr char kAttemptStateNames[][20] = {"DISPATCHED", "COMPLETED",    "FAILED",
                                           "OUTCOME_UNKNOWN", "RECONCILE_REQUIRED",
                                           "ABANDONED"};

constexpr char kFindingKindNames[][48] = {"CLEAN",
                                          "EXPECTED_RESIDENT_MISSING",
                                          "RETIRED_MODEL_STILL_RESIDENT",
                                          "ROLLOUT_STAGE_WITHOUT_WORKERS",
                                          "AUTHORITY_WITHOUT_REPLICAS",
                                          "REPLICA_FOR_NON_AUTHORITATIVE_GENERATION",
                                          "ARTIFACT_GENERATION_CHANGED",
                                          "COMPATIBILITY_CHANGED",
                                          "UNRECONCILED_ATTEMPT",
                                          "FENCED_WORKER_STILL_PUBLISHING",
                                          "STALE_PLAN_BINDING"};

}  // namespace

std::string_view to_string(AttemptKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kAttemptKindCount) return "UNKNOWN";
  return kAttemptKindNames[index];
}

bool parse_attempt_kind(std::string_view text, AttemptKind& out) noexcept {
  for (std::size_t i = 0; i < kAttemptKindCount; ++i) {
    if (text == kAttemptKindNames[i]) {
      out = static_cast<AttemptKind>(i);
      return true;
    }
  }
  return false;
}

std::string_view to_string(AttemptState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  if (index >= 6) return "UNKNOWN";
  return kAttemptStateNames[index];
}

std::string_view to_string(FindingKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= kFindingKindCount) return "UNKNOWN";
  return kFindingKindNames[index];
}

namespace detail {

bool valid_token(std::string_view text, std::size_t max_length) noexcept {
  if (text.empty() || text.size() > max_length) return false;
  for (char c : text) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ':' ||
                    c == '/' || c == '+' || c == '@';
    if (!ok) return false;
  }
  return true;
}

void clamp_detail(std::string& text, std::size_t max_length) {
  if (text.size() > max_length) text.resize(max_length);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Shared validation helpers
// ---------------------------------------------------------------------------

Decision LifecycleEngine::Impl::check_epoch(CoordinatorEpoch expected) const {
  Decision decision(OutcomeCode::Accepted);
  if (expected.valid() && expected != state.epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    decision.add(ReasonCode::CoordinatorEpochStale, "frame carries a superseded coordinator epoch");
  }
  return decision;
}

Decision LifecycleEngine::Impl::check_policy_generation(PolicyGeneration expected) const {
  Decision decision(OutcomeCode::Accepted);
  if (expected.valid() && expected != state.policy.base().generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    decision.add(ReasonCode::PolicyGenerationMismatch,
                 "request cites a superseded lifecycle policy generation");
  }
  return decision;
}

Decision LifecycleEngine::Impl::check_worker(WorkerId worker, WorkerBootId boot,
                                             CoordinatorEpoch expected_epoch) const {
  Decision decision(OutcomeCode::Accepted);
  if (!worker.valid()) return decision;
  const WorkerLease* lease = worker_of(worker);
  if (lease == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerUnknown, "worker is not registered with this coordinator");
  }
  if (lease->fenced) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerBootStale, "worker lease is fenced");
  }
  if (!boot.valid() || lease->boot != boot) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::WorkerBootStale,
                        "worker boot identity is not the current boot for this worker");
  }
  if (expected_epoch.valid() && lease->epoch != expected_epoch) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::CoordinatorEpochMismatch,
                        "worker lease belongs to a different coordinator epoch");
  }
  return decision;
}

Decision LifecycleEngine::Impl::check_scope(ScopeId scope, ScopeGeneration expected) const {
  Decision decision(OutcomeCode::Accepted);
  const ScopeRecord* record = state.scopes.find(scope);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "scope is not registered");
  }
  if (expected.valid() && expected != record->generation) {
    decision.set_outcome(OutcomeCode::STALE_PLAN);
    decision.add(ReasonCode::ScopeGenerationMismatch,
                 "scope generation advanced since the request was formed");
  }
  return decision;
}

EvidenceSubject LifecycleEngine::Impl::version_subject(const ModelVersionRecord& record,
                                                       ScopeId scope) const {
  EvidenceSubject subject;
  subject.model = record.model;
  subject.version = record.version;
  subject.model_generation = record.generation;
  subject.artifact_set = record.artifact.set_id;
  subject.artifact_generation = record.artifact.generation;
  subject.scope = scope;
  subject.compatibility_generation = record.compatibility_generation;
  return subject;
}

EvidenceSubject LifecycleEngine::Impl::stage_subject(const ModelVersionRecord& record,
                                                     const RolloutPlan& plan,
                                                     const StageDefinition& stage,
                                                     ScopeId scope) const {
  EvidenceSubject subject = version_subject(record, scope);
  subject.rollout = plan.id;
  subject.rollout_generation = plan.generation;
  subject.stage = stage.id;
  subject.stage_generation = plan.current_stage_generation;
  return subject;
}

Decision LifecycleEngine::Impl::check_evidence_list(
    const std::vector<EvidenceRequirement>& requirements, const EvidenceSubject& subject) const {
  Decision decision(OutcomeCode::Accepted);
  for (const EvidenceRequirement& requirement : requirements) {
    const EvidenceLookup lookup =
        state.evidence.resolve(requirement, subject, WorkerBootId{},
                               requirement.require_current_publisher, 0,
                               state.policy.base().max_evidence_age_ticks);
    switch (lookup.currentness) {
      case EvidenceCurrentness::Current:
        if (lookup.record->verdict != requirement.required_verdict) {
          decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
          decision.add(ReasonCode::EvidenceUnsatisfiedCriteria,
                       std::string(to_string(requirement.kind)) + " verdict is " +
                           std::string(to_string(lookup.record->verdict)));
        }
        break;
      case EvidenceCurrentness::Missing:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidenceMissing, std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::SubjectMismatch:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidenceSubjectMismatch, std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::GenerationStale:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidenceSubjectModelGenerationMismatch,
                     std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::PublisherStale:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidencePublisherBootStale,
                     std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::Expired:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidenceStale, std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::Conflicting:
        decision.set_outcome(OutcomeCode::AMBIGUOUS);
        decision.add(ReasonCode::EvidenceConflicting, std::string(to_string(requirement.kind)));
        break;
      case EvidenceCurrentness::UnknownProvenance:
        decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        decision.add(ReasonCode::EvidenceUnknownProvenance,
                     std::string(to_string(requirement.kind)));
        break;
    }
  }
  decision.normalize();
  return decision;
}

CompatibilityResult LifecycleEngine::Impl::resolve_compatibility_for(
    const ModelVersionRecord& record, std::string_view environment_key,
    const std::optional<EnvironmentProfile>& environment) const {
  CompatibilityResult result;
  const std::string key = environment_key.empty()
                              ? (environment.has_value() ? environment->key : std::string{})
                              : std::string(environment_key);
  if (!key.empty()) {
    const CompatibilityFact* fact = state.compatibility.lookup(record.version, key);
    if (fact != nullptr) {
      if (fact->model_generation != record.generation ||
          fact->artifact_generation != record.artifact.generation) {
        result.outcome = CompatibilityOutcome::STALE_EVIDENCE;
        result.decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
        result.decision.add(ReasonCode::CompatibilityStale,
                            "published compatibility targets an older model or artifact "
                            "generation");
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
  }
  if (environment.has_value()) {
    return mlf::evaluate_requirements(record.requirements, *environment);
  }
  result.outcome = CompatibilityOutcome::UNKNOWN;
  result.decision.set_outcome(OutcomeCode::STALE_EVIDENCE);
  result.decision.add(ReasonCode::CompatibilityMissing,
                      key.empty() ? "no compatibility fact and no environment profile supplied"
                                  : "no compatibility fact published for environment " + key);
  return result;
}

LifecycleEngine::Impl::CriterionResult LifecycleEngine::Impl::evaluate_criterion(
    const Criterion& criterion, const EvidenceSubject& subject) const {
  CriterionResult result;
  EvidenceRequirement requirement;
  requirement.kind = criterion.kind;
  requirement.required_verdict = EvidenceVerdict::Satisfied;
  requirement.require_exact_subject = true;
  const EvidenceLookup lookup = state.evidence.resolve(
      requirement, subject, WorkerBootId{}, false, 0, state.policy.base().max_evidence_age_ticks);
  if (lookup.currentness != EvidenceCurrentness::Current || lookup.record == nullptr) {
    result.present = false;
    result.detail = std::string(to_string(lookup.currentness));
    return result;
  }
  result.present = true;
  result.value = lookup.record->value;
  result.satisfied = criterion.comparison == CriterionComparison::AtLeast
                         ? result.value >= criterion.threshold
                         : result.value <= criterion.threshold;
  return result;
}

detail::PlanStaleness LifecycleEngine::Impl::check_plan_bindings(const RolloutPlan& plan) const {
  detail::PlanStaleness staleness;
  const ModelVersionRecord* record = version_of(plan.model, plan.candidate);
  if (record == nullptr) {
    staleness.stale = true;
    staleness.reason = ReasonCode::UnknownVersion;
    staleness.detail = "plan candidate no longer exists";
    return staleness;
  }
  if (record->retired()) {
    staleness.stale = true;
    staleness.reason = ReasonCode::PromotionCandidateRetired;
    staleness.detail = "plan candidate was retired";
    return staleness;
  }
  if (record->generation != plan.candidate_generation) {
    staleness.stale = true;
    staleness.reason = ReasonCode::ModelGenerationMismatch;
    staleness.detail = "candidate model generation advanced";
    return staleness;
  }
  if (record->artifact.set_id != plan.candidate_artifact_set ||
      record->artifact.generation != plan.candidate_artifact_generation) {
    staleness.stale = true;
    staleness.reason = ReasonCode::ArtifactGenerationMismatch;
    staleness.detail = "candidate artifact generation advanced";
    return staleness;
  }
  if (record->compatibility_generation != plan.compatibility_generation) {
    staleness.stale = true;
    staleness.reason = ReasonCode::CompatibilityStale;
    staleness.detail = "compatibility generation advanced or was invalidated";
    return staleness;
  }
  if (state.policy.base().generation != plan.policy_generation) {
    staleness.stale = true;
    staleness.reason = ReasonCode::PolicyGenerationMismatch;
    staleness.detail = "lifecycle policy generation advanced";
    return staleness;
  }
  const ScopeRecord* root = state.scopes.find(plan.root_scope);
  if (root == nullptr) {
    staleness.stale = true;
    staleness.reason = ReasonCode::UnknownScope;
    staleness.detail = "plan root scope no longer exists";
    return staleness;
  }
  if (root->generation != plan.root_scope_generation) {
    staleness.stale = true;
    staleness.reason = ReasonCode::ScopeGenerationMismatch;
    staleness.detail = "plan root scope generation advanced";
    return staleness;
  }
  return staleness;
}

Decision LifecycleEngine::Impl::transition(ModelVersionRecord& record, LifecycleState to) {
  Decision decision(OutcomeCode::Accepted);
  if (record.state == to) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidLifecycleTransition, "already in the target state");
  }
  if (!is_transition_allowed(record.state, to)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidLifecycleTransition,
                        std::string(to_string(record.state)) + " -> " + std::string(to_string(to)));
  }
  record.state = to;
  record.lifecycle_generation = next_generation(record.lifecycle_generation);
  return decision;
}

Decision LifecycleEngine::Impl::grant_authority(ModelVersionRecord& record, ScopeId scope,
                                                AuthorityKind kind, const RolloutPlan* plan,
                                                const StageDefinition* stage, PromotionId promotion,
                                                PromotionGeneration promotion_generation,
                                                RollbackId rollback,
                                                RollbackGeneration rollback_generation,
                                                bool supersede_existing,
                                                ModelVersionId retention_target,
                                                ModelGeneration retention_generation) {
  Decision decision(OutcomeCode::Accepted);
  if (record.retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PromotionCandidateRetired,
                        "a retired generation can never be granted authority");
  }
  const ScopeRecord* scope_record = state.scopes.find(scope);
  if (scope_record == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "authority target scope is not registered");
  }

  AuthorityBinding superseded;
  if (supersede_existing) {
    AuthorityBinding binding;
    binding.scope = scope;
    binding.scope_generation = scope_record->generation;
    binding.model = record.model;
    binding.version = record.version;
    binding.model_generation = record.generation;
    binding.artifact_set = record.artifact.set_id;
    binding.artifact_generation = record.artifact.generation;
    binding.lifecycle_state = record.state;
    binding.kind = AuthorityKind::Exclusive;
    binding.promotion = promotion;
    binding.promotion_generation = promotion_generation;
    binding.rollback = rollback;
    binding.rollback_generation = rollback_generation;
    binding.epoch = state.epoch;
    binding.evidence_generation = state.next_evidence_generation > 0
                                      ? EvidenceGeneration(state.next_evidence_generation - 1)
                                      : EvidenceGeneration{};
    binding.granted_sequence = bump_sequence();
    if (plan != nullptr) {
      binding.rollout = plan->id;
      binding.rollout_generation = plan->generation;
      if (stage != nullptr) {
        binding.stage = stage->id;
        binding.stage_generation = plan->current_stage_generation;
      }
    }
    Decision granted = state.authority.replace_exclusive(binding, superseded);
    if (!granted.allowed()) {
      decision.set_outcome(granted.outcome());
      for (const Reason& reason : granted.reasons()) decision.add(reason);
      return decision;
    }
  } else {
    AuthorityBinding binding;
    binding.scope = scope;
    binding.scope_generation = scope_record->generation;
    binding.model = record.model;
    binding.version = record.version;
    binding.model_generation = record.generation;
    binding.artifact_set = record.artifact.set_id;
    binding.artifact_generation = record.artifact.generation;
    binding.lifecycle_state = record.state;
    binding.kind = kind;
    binding.promotion = promotion;
    binding.promotion_generation = promotion_generation;
    binding.rollback = rollback;
    binding.rollback_generation = rollback_generation;
    binding.epoch = state.epoch;
    binding.evidence_generation = state.next_evidence_generation > 0
                                      ? EvidenceGeneration(state.next_evidence_generation - 1)
                                      : EvidenceGeneration{};
    binding.granted_sequence = bump_sequence();
    if (plan != nullptr) {
      binding.rollout = plan->id;
      binding.rollout_generation = plan->generation;
      if (stage != nullptr) {
        binding.stage = stage->id;
        binding.stage_generation = plan->current_stage_generation;
      }
    }
    const bool allow_coexistence = state.policy.effective(state.scopes, scope)
                                       .allow_split_traffic;
    Decision granted = state.authority.grant(binding, allow_coexistence);
    if (!granted.allowed()) {
      decision.set_outcome(granted.outcome());
      for (const Reason& reason : granted.reasons()) decision.add(reason);
      return decision;
    }
  }

  // The displaced generation is retained only when the caller named it as the
  // rollback target. Retention is what keeps a rollback executable; it is never
  // granted implicitly to whatever happened to hold the scope.
  if (superseded.version.valid()) {
    ModelVersionRecord* previous = version_mut(superseded.model, superseded.version);
    if (previous != nullptr && !previous->retired()) {
      const bool is_retention_target =
          retention_target.valid() && retention_target == superseded.version &&
          (!retention_generation.valid() || retention_generation == superseded.model_generation);
      if (is_retention_target) {
        AuthorityBinding retention;
        retention.scope = scope;
        retention.scope_generation = scope_record->generation;
        retention.model = superseded.model;
        retention.version = superseded.version;
        retention.model_generation = superseded.model_generation;
        retention.artifact_set = superseded.artifact_set;
        retention.artifact_generation = superseded.artifact_generation;
        retention.lifecycle_state = previous->state;
        retention.kind = AuthorityKind::RollbackRetained;
        retention.epoch = state.epoch;
        retention.granted_sequence = bump_sequence();
        retention.rollout = superseded.rollout;
        retention.rollout_generation = superseded.rollout_generation;
        Decision retained = state.authority.grant(retention, true);
        if (!retained.allowed()) {
          decision.set_outcome(retained.outcome());
          for (const Reason& reason : retained.reasons()) decision.add(reason);
          return decision;
        }
      }
      // A generation that no longer holds exclusive authority anywhere is
      // draining, not current: it must not be reported as authoritative.
      if (!holds_live_authority(previous->model, previous->version, false)) {
        advance_lifecycle_to(*previous, LifecycleState::DRAINING);
      } else if (previous->state == LifecycleState::CURRENT) {
        advance_lifecycle_to(*previous, LifecycleState::PARTIALLY_PROMOTED);
      }
    }
  }
  state.scopes.touch(scope);
  return decision;
}

bool LifecycleEngine::Impl::holds_live_authority(ModelId model, ModelVersionId version,
                                                 bool include_canary) const {
  const std::vector<const AuthorityBinding*> bindings =
      state.authority.bindings_for_version(model, version);
  for (const AuthorityBinding* binding : bindings) {
    if (binding->superseded) continue;
    if (binding->kind == AuthorityKind::Exclusive) return true;
    if (include_canary && (binding->kind == AuthorityKind::Canary ||
                           binding->kind == AuthorityKind::SplitTraffic)) {
      return true;
    }
    if (include_canary && binding->kind == AuthorityKind::Draining) return true;
  }
  return false;
}

std::vector<const RolloutPlan*> LifecycleEngine::Impl::live_plans_for(ModelVersionId version) const {
  std::vector<const RolloutPlan*> out;
  for (const auto& entry : state.rollouts) {
    const RolloutPlan& plan = entry.second;
    if (plan.candidate != version) continue;
    if (!plan.live()) continue;
    out.push_back(&plan);
  }
  return out;
}

// ---------------------------------------------------------------------------
// Construction and registry
// ---------------------------------------------------------------------------

LifecycleEngine::LifecycleEngine(EngineConfig config) : impl_(std::make_unique<Impl>()), config_(config) {
  impl_->state.scopes.set_max_scopes(config.bounds.max_scopes);
  impl_->state.compatibility.set_max_facts(8192);
  impl_->state.evidence.set_max_records(config.bounds.max_evidence);
  impl_->state.authority.set_max_bindings(config.bounds.max_authority_bindings);
  impl_->state.policy.set_base(config.policy);
  impl_->state.scopes.ensure_root();
}

LifecycleEngine::~LifecycleEngine() = default;

Decision LifecycleEngine::register_model(std::string_view name, std::string_view family,
                                         Provenance provenance, ModelId& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  out = ModelId{};
  Decision decision(OutcomeCode::Accepted);
  if (!detail::valid_token(name, config_.bounds.max_name_length)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "model name must be a bounded token");
  }
  if (!family.empty() && !detail::valid_token(family, config_.bounds.max_name_length)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "model family must be a bounded token");
  }
  if (impl_->state.model_names.find(std::string(name)) != impl_->state.model_names.end()) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::DuplicateModel, std::string(name));
  }
  if (impl_->state.models.size() >= config_.bounds.max_models) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsModelCount, "model registry bound reached");
  }
  ModelRecord record;
  record.id = ModelId(impl_->state.next_model_id++);
  record.name.assign(name);
  record.family.assign(family);
  record.provenance = provenance == Provenance::Unknown ? config_.default_provenance : provenance;
  record.created_sequence = impl_->bump_sequence();
  out = record.id;
  impl_->state.model_names.emplace(record.name, record.id);
  impl_->state.models.emplace(record.id.raw(), std::move(record));
  return decision;
}

Decision LifecycleEngine::register_version(const RegisterVersionRequest& request,
                                           ModelVersionId& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  out = ModelVersionId{};
  Decision decision(OutcomeCode::Accepted);
  if (impl_->state.models.find(request.model.raw()) == impl_->state.models.end()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownModel, "model is not registered");
  }
  if (!detail::valid_token(request.label, config_.bounds.max_name_length)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidVersionLabel, "version label must be a bounded token");
  }
  if (!request.artifact.set_id.valid() || !request.artifact.generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ArtifactSetMissing,
                        "version must bind a concrete artifact set and generation");
  }
  if (!valid_digest(request.artifact.digest, config_.bounds.max_digest_length)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidDigest, "artifact digest is empty or malformed");
  }
  const auto label_key = std::make_pair(request.model.raw(), request.label);
  if (impl_->state.version_labels.find(label_key) != impl_->state.version_labels.end()) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::DuplicateVersion, request.label);
  }
  std::size_t version_count = 0;
  for (const auto& entry : impl_->state.versions) {
    if (entry.first.first == request.model.raw()) ++version_count;
  }
  if (version_count >= config_.bounds.max_versions_per_model) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsVersionCount, "per-model version bound reached");
  }
  if (request.predecessor.valid()) {
    const ModelVersionRecord* predecessor =
        impl_->version_of(request.model, request.predecessor);
    if (predecessor == nullptr) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::ParentVersionMissing,
                          "declared predecessor is not registered for this model");
    }
    if (predecessor->retired() && !config_.policy.allow_version_label_reuse_after_retirement) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::VersionRetired,
                          "predecessor is retired; policy forbids extending a retired lineage");
    }
  }

  ModelVersionRecord record;
  record.model = request.model;
  record.version = ModelVersionId(impl_->state.next_version_id++);
  record.label = request.label;
  record.generation = ModelGeneration(kFirstGeneration);
  record.predecessor = request.predecessor;
  if (request.predecessor.valid()) {
    const ModelVersionRecord* predecessor =
        impl_->version_of(request.model, request.predecessor);
    record.predecessor_generation = predecessor->generation;
  }
  record.artifact = request.artifact;
  record.requirements = request.requirements;
  record.state = LifecycleState::REGISTERED;
  record.lifecycle_generation = LifecycleGeneration(kFirstGeneration);
  record.provenance = request.provenance == Provenance::Unknown ? config_.default_provenance
                                                                : request.provenance;
  record.created_sequence = impl_->bump_sequence();
  record.updated_sequence = record.created_sequence;
  out = record.version;
  impl_->state.version_labels.emplace(label_key, record.version);
  impl_->state.version_index.emplace(
      record.version.raw(), std::make_pair(record.model.raw(), record.version.raw()));
  impl_->state.versions.emplace(std::make_pair(record.model.raw(), record.version.raw()),
                                std::move(record));
  return decision;
}

Decision LifecycleEngine::revise_version(const ReviseVersionRequest& request) {
  std::lock_guard<std::mutex> guard(mutex_);
  ModelVersionRecord* record = impl_->version_mut(request.model, request.version);
  Decision decision(OutcomeCode::Accepted);
  if (record == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "version is not registered");
  }
  if (record->retired()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired, "a retired generation cannot be revised");
  }
  if (request.expected_generation.valid() && request.expected_generation != record->generation) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::ModelGenerationMismatch,
                        "revision expected a different model generation");
  }
  if (!request.artifact.set_id.valid() || !request.artifact.generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ArtifactSetMissing, "revision requires a concrete artifact");
  }
  if (!valid_digest(request.artifact.digest, config_.bounds.max_digest_length)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidDigest, "artifact digest is empty or malformed");
  }
  if (request.artifact == record->artifact && request.requirements.runtime ==
                                                  record->requirements.runtime &&
      request.requirements.backend == record->requirements.backend &&
      request.requirements.architecture == record->requirements.architecture &&
      request.requirements.precision == record->requirements.precision &&
      request.requirements.tokenizer_generation == record->requirements.tokenizer_generation &&
      request.requirements.adapter_set == record->requirements.adapter_set) {
    // Nothing material changed: do not manufacture a new generation.
    return decision;
  }

  record->generation = next_generation(record->generation);
  record->artifact = request.artifact;
  record->requirements = request.requirements;
  record->compatibility_generation = CompatibilityGeneration{};
  record->updated_sequence = impl_->bump_sequence();
  impl_->state.compatibility.invalidate_version(record->version);
  impl_->state.evidence.invalidate_model_generation(record->version, record->generation);
  if (is_terminal(record->state)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::VersionRetired, "cannot revise a terminal generation");
  }
  // Every plan bound to the previous generation is now stale by construction.
  for (auto& entry : impl_->state.rollouts) {
    RolloutPlan& plan = entry.second;
    if (plan.candidate != record->version || !plan.live()) continue;
    plan.superseded = true;
    plan.updated_sequence = impl_->bump_sequence();
    impl_->state.authority.supersede_rollout(plan.id, plan.generation);
  }
  if (record->state != LifecycleState::REGISTERED && record->state != LifecycleState::VALIDATING &&
      record->state != LifecycleState::REVALIDATION_REQUIRED &&
      record->state != LifecycleState::PROMOTION_BLOCKED) {
    impl_->transition(*record, LifecycleState::REVALIDATION_REQUIRED);
  }
  return decision;
}

Decision LifecycleEngine::register_scope(std::string_view path, ScopeId& out) {
  std::lock_guard<std::mutex> guard(mutex_);
  return impl_->state.scopes.ensure_path(path, out);
}

Decision LifecycleEngine::set_policy(LifecyclePolicy policy) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  if (!policy.generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::PolicyGenerationMismatch, "policy generation must be non-zero");
  }
  if (policy.max_active_generations_per_scope == 0 ||
      policy.max_active_generations_per_scope > 64) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::BoundsModelCount,
                        "max active generations per scope must be 1..64");
  }
  if (policy.max_blast_radius_percent == 0 || policy.max_blast_radius_percent > 100) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::BlastRadiusExceeded, "blast radius must be 1..100 percent");
  }
  if (!policy.promotable_scope_kinds.empty()) {
    for (ScopeKind kind : policy.promotable_scope_kinds) {
      if (static_cast<std::size_t>(kind) >= kScopeKindCount) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::PolicyScopeNotTargetable, "unknown scope kind in policy");
      }
    }
  }
  for (EvidenceKind kind : policy.globally_required_evidence) {
    if (static_cast<std::size_t>(kind) >= kEvidenceKindCount) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::PolicyRequiresEvidence, "unknown evidence kind in policy");
    }
  }
  const PolicyGeneration previous = impl_->state.policy.base().generation;
  if (policy.generation <= previous) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::PolicyGenerationMismatch,
                        "policy generations must strictly increase");
  }
  impl_->state.policy.set_base(policy);
  impl_->bump_sequence();
  return decision;
}

Decision LifecycleEngine::set_scope_policy(const ScopePolicyOverride& override_policy) {
  std::lock_guard<std::mutex> guard(mutex_);
  Decision decision(OutcomeCode::Accepted);
  if (impl_->state.scopes.find(override_policy.scope) == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "policy override targets an unknown scope");
  }
  Decision applied = impl_->state.policy.set_override(override_policy);
  if (!applied.allowed()) return applied;
  impl_->bump_sequence();
  return decision;
}

}  // namespace mlf
