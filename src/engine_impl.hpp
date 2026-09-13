// Model Lifecycle Fabric — internal engine state and shared validation helpers.
//
// Private to the library. Every helper here assumes the caller already holds the
// engine mutex; none of them perform socket I/O, filesystem work, callbacks, or
// external calls.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/engine.hpp"
#include "mlf/state_store.hpp"

namespace mlf {
namespace detail {

/// Longest human-readable detail retained on a reason.
inline constexpr std::size_t kMaxReasonDetail = 160;
/// Longest free-text detail retained on an attempt or record.
inline constexpr std::size_t kMaxRecordDetail = 256;

/// Validate a bounded token: non-empty, at most max_length, and made of
/// characters that survive canonical serialization unchanged.
[[nodiscard]] bool valid_token(std::string_view text, std::size_t max_length) noexcept;

/// Clamp a detail string to the retained bound.
void clamp_detail(std::string& text, std::size_t max_length = kMaxReasonDetail);

/// All mutable lifecycle state. Grouped so that export, import and volatility
/// invalidation are explicit and auditable.
struct EngineState {
  ScopeRegistry scopes{};

  std::map<std::uint64_t, ModelRecord> models{};
  std::map<std::string, ModelId> model_names{};

  std::map<std::pair<std::uint64_t, std::uint64_t>, ModelVersionRecord> versions{};
  std::map<std::pair<std::uint64_t, std::string>, ModelVersionId> version_labels{};
  /// Version identity to (model, version) key. Version identities are globally
  /// unique, so this resolves a version without scanning the registry.
  std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> version_index{};

  CompatibilityRegistry compatibility{};
  EvidenceStore evidence{};
  PolicySet policy{};

  std::map<std::uint64_t, RolloutPlan> rollouts{};
  AuthorityTable authority{};

  std::map<std::uint64_t, Attempt> attempts{};
  std::map<std::uint64_t, WorkerLease> workers{};
  /// Volatile connection state. Never persisted, never restored as current.
  std::map<std::uint64_t, bool> worker_liveness{};
  /// Volatile affinity from a rollout to the worker boot driving it.
  std::map<std::uint64_t, std::uint64_t> rollout_driver_boot{};

  std::set<std::uint64_t> committed_promotions{};
  std::set<std::pair<std::uint64_t, std::uint64_t>> committed_rollbacks{};

  std::uint64_t next_model_id{1};
  std::uint64_t next_version_id{1};
  std::uint64_t next_rollout_id{1};
  std::uint64_t next_stage_id{1};
  std::uint64_t next_cohort_id{1};
  std::uint64_t next_promotion_id{1};
  std::uint64_t next_rollback_id{1};
  std::uint64_t next_attempt_id{1};
  std::uint64_t next_worker_id{1};
  std::uint64_t next_evidence_id{1};
  std::uint64_t next_evidence_generation{1};
  std::uint64_t next_compatibility_generation{1};

  Sequence sequence{Sequence(kFirstGeneration)};
  CoordinatorEpoch epoch{CoordinatorEpoch(kFirstGeneration)};
  SnapshotGeneration snapshot_generation{};
  std::uint64_t requeued_frames{0};
};

/// Why a plan binding no longer matches registry truth.
struct PlanStaleness {
  bool stale{false};
  ReasonCode reason{ReasonCode::None};
  std::string detail{};
};

}  // namespace detail

struct LifecycleEngine::Impl {
  detail::EngineState state{};

  // --- lookups ------------------------------------------------------------
  [[nodiscard]] const ModelVersionRecord* version_of(ModelId model, ModelVersionId id) const {
    const auto found = state.versions.find(std::make_pair(model.raw(), id.raw()));
    return found == state.versions.end() ? nullptr : &found->second;
  }
  [[nodiscard]] ModelVersionRecord* version_mut(ModelId model, ModelVersionId id) {
    const auto found = state.versions.find(std::make_pair(model.raw(), id.raw()));
    return found == state.versions.end() ? nullptr : &found->second;
  }
  /// Resolve a version by its globally unique identity, without knowing the
  /// model. Returns nullptr when the identity is unknown.
  [[nodiscard]] const ModelVersionRecord* version_of_id(ModelVersionId id) const {
    const auto index = state.version_index.find(id.raw());
    if (index == state.version_index.end()) return nullptr;
    return version_of(ModelId(index->second.first), ModelVersionId(index->second.second));
  }
  [[nodiscard]] ModelVersionRecord* version_mut_id(ModelVersionId id) {
    const auto index = state.version_index.find(id.raw());
    if (index == state.version_index.end()) return nullptr;
    return version_mut(ModelId(index->second.first), ModelVersionId(index->second.second));
  }
  [[nodiscard]] const RolloutPlan* rollout_of(RolloutId id) const {
    const auto found = state.rollouts.find(id.raw());
    return found == state.rollouts.end() ? nullptr : &found->second;
  }
  [[nodiscard]] RolloutPlan* rollout_mut(RolloutId id) {
    const auto found = state.rollouts.find(id.raw());
    return found == state.rollouts.end() ? nullptr : &found->second;
  }
  [[nodiscard]] const WorkerLease* worker_of(WorkerId id) const {
    const auto found = state.workers.find(id.raw());
    return found == state.workers.end() ? nullptr : &found->second;
  }

  // --- sequence and identity ---------------------------------------------
  Sequence bump_sequence() {
    state.sequence = next_generation(state.sequence);
    return state.sequence;
  }

  // --- shared validation --------------------------------------------------
  Decision check_epoch(CoordinatorEpoch expected) const;
  Decision check_policy_generation(PolicyGeneration expected) const;
  Decision check_worker(WorkerId worker, WorkerBootId boot, CoordinatorEpoch expected_epoch) const;
  Decision check_scope(ScopeId scope, ScopeGeneration expected) const;

  /// Evidence subject for version-level promotion or retirement evidence.
  [[nodiscard]] EvidenceSubject version_subject(const ModelVersionRecord& record, ScopeId scope) const;
  /// Evidence subject for a rollout stage.
  [[nodiscard]] EvidenceSubject stage_subject(const ModelVersionRecord& record,
                                              const RolloutPlan& plan, const StageDefinition& stage,
                                              ScopeId scope) const;

  Decision check_evidence_list(const std::vector<EvidenceRequirement>& requirements,
                               const EvidenceSubject& subject) const;

  /// Resolve the compatibility governing a version. Prefers a published fact
  /// bound to the current model/artifact generation; falls back to structural
  /// evaluation when the caller supplies an environment profile.
  [[nodiscard]] CompatibilityResult resolve_compatibility_for(
      const ModelVersionRecord& record, std::string_view environment_key,
      const std::optional<EnvironmentProfile>& environment) const;

  /// Evaluate one numeric criterion against current evidence.
  struct CriterionResult {
    bool satisfied{false};
    bool present{false};
    double value{0.0};
    std::string detail{};
  };
  [[nodiscard]] CriterionResult evaluate_criterion(const Criterion& criterion,
                                                   const EvidenceSubject& subject) const;

  /// Determine whether a plan still matches registry truth.
  [[nodiscard]] detail::PlanStaleness check_plan_bindings(const RolloutPlan& plan) const;

  /// Commit a single lifecycle transition, enforcing the state machine.
  Decision transition(ModelVersionRecord& record, LifecycleState to);

  /// Advance a record to a target state by applying the shortest legal sequence
  /// of transitions. Every intermediate edge is checked against the state
  /// machine, so no state is ever skipped or fabricated.
  Decision advance_lifecycle_to(ModelVersionRecord& record, LifecycleState target);

  /// Grant authority for a version over a scope, superseding the current
  /// exclusive holder when requested. Appends retained/rollback bindings.
  Decision grant_authority(ModelVersionRecord& record, ScopeId scope, AuthorityKind kind,
                           const RolloutPlan* plan, const StageDefinition* stage,
                           PromotionId promotion, PromotionGeneration promotion_generation,
                           RollbackId rollback, RollbackGeneration rollback_generation,
                           bool supersede_existing, ModelVersionId retention_target,
                           ModelGeneration retention_generation);

  /// True when the version currently holds live exclusive authority somewhere.
  [[nodiscard]] bool holds_live_authority(ModelId model, ModelVersionId version,
                                          bool include_canary) const;

  /// Version states that require rollback retention before retirement.
  [[nodiscard]] std::vector<const RolloutPlan*> live_plans_for(ModelVersionId version) const;
};

}  // namespace mlf
