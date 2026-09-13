#include "mlf/policy.hpp"

#include <algorithm>

namespace mlf {
namespace {

void sort_unique_evidence(std::vector<EvidenceKind>& kinds) {
  std::sort(kinds.begin(), kinds.end(), [](EvidenceKind a, EvidenceKind b) {
    return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
  });
  kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
}

void sort_unique_scope_kinds(std::vector<ScopeKind>& kinds) {
  std::sort(kinds.begin(), kinds.end(), [](ScopeKind a, ScopeKind b) {
    return static_cast<std::uint8_t>(a) < static_cast<std::uint8_t>(b);
  });
  kinds.erase(std::unique(kinds.begin(), kinds.end()), kinds.end());
}

}  // namespace

Decision PolicySet::set_override(const ScopePolicyOverride& override_policy) {
  Decision decision(OutcomeCode::Accepted);
  if (!override_policy.scope.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "policy override needs a scope identity");
  }
  if (override_policy.additional_required_evidence.size() > kEvidenceKindCount) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsEvidenceCount, "too many required evidence kinds");
  }
  ScopePolicyOverride normalized = override_policy;
  sort_unique_evidence(normalized.additional_required_evidence);
  sort_unique_scope_kinds(normalized.additional_promotable_scope_kinds);
  overrides_[normalized.scope.raw()] = std::move(normalized);
  return decision;
}

LifecyclePolicy PolicySet::effective(const ScopeRegistry& scopes, ScopeId scope) const {
  LifecyclePolicy policy = base_;
  const std::vector<ScopeId> chain = scopes.chain(scope);
  bool may_refine = true;
  for (ScopeId node : chain) {
    const auto found = overrides_.find(node.raw());
    if (found == overrides_.end()) continue;
    const ScopePolicyOverride& override_policy = found->second;
    if (!may_refine) {
      // A parent refinement did not permit further refinement: the child's
      // override is ignored rather than silently applied.
      continue;
    }
    if (override_policy.max_active_generations_per_scope != 0) {
      policy.max_active_generations_per_scope = override_policy.max_active_generations_per_scope;
    }
    if (override_policy.require_rollback_target >= 0) {
      policy.require_rollback_target = override_policy.require_rollback_target != 0;
    }
    if (override_policy.require_evidence_for_stage_advance >= 0) {
      policy.require_evidence_for_stage_advance =
          override_policy.require_evidence_for_stage_advance != 0;
    }
    if (override_policy.require_residency_before_stage_entry >= 0) {
      policy.require_residency_before_stage_entry =
          override_policy.require_residency_before_stage_entry != 0;
    }
    for (EvidenceKind kind : override_policy.additional_required_evidence) {
      policy.globally_required_evidence.push_back(kind);
    }
    for (ScopeKind kind : override_policy.additional_promotable_scope_kinds) {
      policy.promotable_scope_kinds.push_back(kind);
    }
    may_refine = override_policy.permits_child_override;
  }
  sort_unique_evidence(policy.globally_required_evidence);
  sort_unique_scope_kinds(policy.promotable_scope_kinds);
  return policy;
}

bool PolicySet::restore(std::map<std::uint64_t, ScopePolicyOverride> overrides,
                        LifecyclePolicy base) {
  if (!base.generation.valid()) return false;
  if (base.max_active_generations_per_scope == 0) return false;
  if (base.max_active_generations_per_scope > 64) return false;
  if (base.max_blast_radius_percent > 100) return false;
  if (base.globally_required_evidence.size() > kEvidenceKindCount) return false;
  for (const auto& entry : overrides) {
    if (entry.first == 0) return false;
    if (entry.second.scope.raw() != entry.first) return false;
    if (entry.second.additional_required_evidence.size() > kEvidenceKindCount) return false;
  }
  overrides_ = std::move(overrides);
  base_ = std::move(base);
  return true;
}

}  // namespace mlf
