// Model Lifecycle Fabric — lifecycle policy and deterministic scope inheritance.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "mlf/evidence.hpp"
#include "mlf/identity.hpp"
#include "mlf/scope.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Policy governing lifecycle authority. One revision is identified by its
/// PolicyGeneration; plans and evidence bound to a superseded policy generation
/// are stale.
struct LifecyclePolicy {
  PolicyGeneration generation{PolicyGeneration(kFirstGeneration)};

  /// Maximum simultaneously authoritative generations per scope. 1 forbids
  /// coexistence; a canary that needs coexistence requires at least 2.
  std::uint32_t max_active_generations_per_scope{2};
  /// Whether a scope may carry split traffic between two generations.
  bool allow_split_traffic{true};
  /// Whether a promotion must name a rollback target.
  bool require_rollback_target{true};
  /// Whether automatic rollback may commit without an explicit decision.
  bool allow_automatic_rollback{false};
  /// Whether promotion requires a recorded administrative approval.
  bool require_manual_approval_for_promotion{false};
  /// Largest share of the target scope a single stage may move, in percent.
  std::uint32_t max_blast_radius_percent{100};
  /// Whether a stage advance requires current evidence for every required kind.
  bool require_evidence_for_stage_advance{true};
  /// Evidence older than this many logical ticks is stale. Zero disables
  /// wall-independent age expiry; freshness then depends on generation binding.
  std::uint64_t max_evidence_age_ticks{0};
  /// Whether a candidate must be resident before a stage may be entered.
  bool require_residency_before_stage_entry{false};
  /// Whether the superseded generation must be drained after a stage commit.
  bool require_drain_after_stage_commit{false};
  /// Whether immediate cutover is an admissible strategy.
  bool allow_immediate_cutover{true};
  /// Scope kinds promotion may target. Empty means every kind is targetable.
  std::vector<ScopeKind> promotable_scope_kinds{};
  /// Evidence kinds every promotion must carry, in addition to plan-specific
  /// requirements.
  std::vector<EvidenceKind> globally_required_evidence{};
  /// Whether retirement may proceed while a rollback retention obligation is
  /// outstanding.
  bool allow_retirement_without_drain{false};
  /// Number of predecessor generations that must stay rollback-eligible.
  std::uint32_t rollback_retention_generations{1};
  /// Whether a new promotion supersedes an in-flight rollout of the same model.
  bool supersede_inflight_rollout_on_new_promotion{true};
  /// Whether a retired generation may be replaced by a new generation that
  /// reuses the same version label.
  bool allow_version_label_reuse_after_retirement{false};
};

/// A scope-specific policy refinement.
struct ScopePolicyOverride {
  ScopeId scope{};
  /// Whether this override may be refined further by descendants. A child scope
  /// may not silently override a parent policy: without this flag the child's
  /// refinement is ignored and the parent's effective policy applies.
  bool permits_child_override{false};
  /// Zero means inherit.
  std::uint32_t max_active_generations_per_scope{0};
  /// Negative overrides are explicit opt-outs.
  std::int8_t require_rollback_target{-1};
  std::int8_t require_evidence_for_stage_advance{-1};
  std::int8_t require_residency_before_stage_entry{-1};
  /// Additional evidence kinds required for this scope.
  std::vector<EvidenceKind> additional_required_evidence{};
  /// Additional scope kinds that become targetable here.
  std::vector<ScopeKind> additional_promotable_scope_kinds{};
};

/// Base policy plus per-scope refinements, resolved deterministically along the
/// scope chain from the root down.
class PolicySet {
 public:
  PolicySet() = default;

  void set_base(LifecyclePolicy policy) { base_ = std::move(policy); }
  [[nodiscard]] const LifecyclePolicy& base() const noexcept { return base_; }

  Decision set_override(const ScopePolicyOverride& override_policy);

  /// Effective policy for a scope. A refinement applies only when every ancestor
  /// refinement that carries it permitted further refinement.
  [[nodiscard]] LifecyclePolicy effective(const ScopeRegistry& scopes, ScopeId scope) const;

  [[nodiscard]] const std::map<std::uint64_t, ScopePolicyOverride>& overrides() const noexcept {
    return overrides_;
  }
  [[nodiscard]] bool restore(std::map<std::uint64_t, ScopePolicyOverride> overrides,
                             LifecyclePolicy base);

 private:
  LifecyclePolicy base_{};
  std::map<std::uint64_t, ScopePolicyOverride> overrides_{};
};

}  // namespace mlf
