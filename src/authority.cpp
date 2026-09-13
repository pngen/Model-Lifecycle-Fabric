#include "mlf/authority.hpp"

#include <algorithm>

namespace mlf {
namespace {

constexpr char kKindNames[][20] = {"EXCLUSIVE", "CANARY", "SPLIT_TRAFFIC", "ROLLBACK_RETAINED",
                                   "DRAINING"};

}  // namespace

std::string_view to_string(AuthorityKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index >= 5) return "UNKNOWN";
  return kKindNames[index];
}

std::size_t AuthorityTable::size() const noexcept {
  std::size_t total = 0;
  for (const auto& entry : bindings_) total += entry.second.size();
  return total;
}

std::size_t AuthorityTable::live_count() const noexcept {
  std::size_t total = 0;
  for (const auto& entry : bindings_) {
    for (const AuthorityBinding& binding : entry.second) {
      if (!binding.superseded) ++total;
    }
  }
  return total;
}

bool AuthorityTable::has_conflict() const {
  for (const auto& entry : bindings_) {
    std::size_t exclusive = 0;
    for (const AuthorityBinding& binding : entry.second) {
      if (!binding.superseded && binding.kind == AuthorityKind::Exclusive) ++exclusive;
    }
    if (exclusive > 1) return true;
  }
  return false;
}

Decision AuthorityTable::grant(const AuthorityBinding& binding, bool allow_coexistence) {
  Decision decision(OutcomeCode::Accepted);
  if (!binding.scope.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "authority binding has no scope");
  }
  if (!binding.version.valid() || !binding.model.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownVersion, "authority binding has no model version");
  }
  if (!binding.model_generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ModelGenerationMismatch,
                        "authority binding has no model generation");
  }
  if (!binding.scope_generation.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::ScopeGenerationMismatch,
                        "authority binding has no scope generation");
  }

  const auto existing = bindings_.find(binding.scope.raw());
  if (existing != bindings_.end()) {
    for (const AuthorityBinding& current : existing->second) {
      if (current.superseded) continue;
      if (binding.kind == AuthorityKind::Exclusive &&
          current.kind == AuthorityKind::Exclusive) {
        decision.set_outcome(OutcomeCode::CONFLICT);
        return decision.add(ReasonCode::PolicyDeniesCoexistence,
                            "scope already holds exclusive authority for another generation");
      }
      if (binding.kind != AuthorityKind::Exclusive &&
          binding.kind != AuthorityKind::RollbackRetained && !allow_coexistence) {
        decision.set_outcome(OutcomeCode::CONFLICT);
        return decision.add(ReasonCode::PolicyDeniesCoexistence,
                            "policy does not permit coexistence in this scope");
      }
    }
  } else if (size() >= max_bindings_) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsHistoryCount, "authority binding bound reached");
  }

  bindings_[binding.scope.raw()].push_back(binding);
  return decision;
}

Decision AuthorityTable::replace_exclusive(const AuthorityBinding& binding,
                                           AuthorityBinding& superseded) {
  superseded = AuthorityBinding{};
  AuthorityBinding replacement = binding;
  replacement.kind = AuthorityKind::Exclusive;

  Decision decision = grant(replacement, true);
  if (!decision.allowed()) {
    // grant() refuses a second Exclusive; replacing is exactly the sanctioned
    // way to move exclusive authority, so supersede first and retry once.
    if (decision.has(ReasonCode::PolicyDeniesCoexistence)) {
      const auto existing = bindings_.find(replacement.scope.raw());
      if (existing != bindings_.end()) {
        for (AuthorityBinding& current : existing->second) {
          if (current.superseded || current.kind != AuthorityKind::Exclusive) continue;
          current.superseded = true;
          superseded = current;
          break;
        }
      }
      decision = Decision(OutcomeCode::Accepted);
      bindings_[replacement.scope.raw()].push_back(replacement);
      return decision;
    }
    return decision;
  }
  return decision;
}

std::size_t AuthorityTable::supersede_scope(ScopeId scope) {
  std::size_t changed = 0;
  const auto existing = bindings_.find(scope.raw());
  if (existing == bindings_.end()) return 0;
  for (AuthorityBinding& binding : existing->second) {
    if (binding.superseded) continue;
    binding.superseded = true;
    ++changed;
  }
  return changed;
}

std::size_t AuthorityTable::supersede_rollout(RolloutId rollout, RolloutGeneration generation) {
  std::size_t changed = 0;
  for (auto& entry : bindings_) {
    for (AuthorityBinding& binding : entry.second) {
      if (binding.superseded) continue;
      if (binding.rollout != rollout || binding.rollout_generation != generation) continue;
      binding.superseded = true;
      ++changed;
    }
  }
  return changed;
}

std::size_t AuthorityTable::mark_draining(ModelId model, ModelVersionId version, ScopeId scope) {
  std::size_t changed = 0;
  const auto existing = bindings_.find(scope.raw());
  if (existing == bindings_.end()) return 0;
  for (AuthorityBinding& binding : existing->second) {
    if (binding.superseded) continue;
    if (binding.model != model || binding.version != version) continue;
    if (binding.kind == AuthorityKind::Draining || binding.kind == AuthorityKind::RollbackRetained) {
      continue;
    }
    binding.kind = AuthorityKind::Draining;
    ++changed;
  }
  return changed;
}

std::vector<const AuthorityBinding*> AuthorityTable::bindings_for(ScopeId scope) const {
  std::vector<const AuthorityBinding*> out;
  const auto existing = bindings_.find(scope.raw());
  if (existing == bindings_.end()) return out;
  out.reserve(existing->second.size());
  for (const AuthorityBinding& binding : existing->second) out.push_back(&binding);
  return out;
}

std::vector<const AuthorityBinding*> AuthorityTable::live_bindings_for(ScopeId scope) const {
  std::vector<const AuthorityBinding*> out;
  const auto existing = bindings_.find(scope.raw());
  if (existing == bindings_.end()) return out;
  for (const AuthorityBinding& binding : existing->second) {
    if (!binding.superseded) out.push_back(&binding);
  }
  return out;
}

std::vector<const AuthorityBinding*> AuthorityTable::bindings_for_version(
    ModelId model, ModelVersionId version) const {
  std::vector<const AuthorityBinding*> out;
  for (const auto& entry : bindings_) {
    for (const AuthorityBinding& binding : entry.second) {
      if (binding.model != model || binding.version != version) continue;
      out.push_back(&binding);
    }
  }
  return out;
}

std::vector<const AuthorityBinding*> AuthorityTable::subtree_bindings(const ScopeRegistry& scopes,
                                                                     ScopeId root) const {
  std::vector<const AuthorityBinding*> out;
  for (const auto& entry : bindings_) {
    const ScopeId scope(entry.first);
    if (!scopes.is_ancestor_or_self(root, scope)) continue;
    for (const AuthorityBinding& binding : entry.second) {
      if (!binding.superseded) out.push_back(&binding);
    }
  }
  return out;
}

bool AuthorityTable::restore(std::map<std::uint64_t, std::vector<AuthorityBinding>> bindings) {
  std::size_t total = 0;
  for (const auto& entry : bindings) {
    if (entry.first == 0) return false;
    std::size_t exclusive = 0;
    for (const AuthorityBinding& binding : entry.second) {
      if (binding.scope.raw() != entry.first) return false;
      if (!binding.scope.valid() || !binding.model.valid() || !binding.version.valid()) return false;
      if (!binding.model_generation.valid()) return false;
      if (!binding.scope_generation.valid()) return false;
      if (binding.superseded) {
        // Superseded history is not durable: it is reconstructed from sequences.
        return false;
      }
      if (binding.kind == AuthorityKind::Exclusive) ++exclusive;
    }
    if (exclusive > 1) return false;
    total += entry.second.size();
  }
  if (total > max_bindings_) return false;
  bindings_ = std::move(bindings);
  return true;
}

}  // namespace mlf
