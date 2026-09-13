// Model Lifecycle Fabric — scope-bound authority bindings and resolution.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "mlf/identity.hpp"
#include "mlf/lifecycle.hpp"
#include "mlf/scope.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// How a binding holds a scope.
enum class AuthorityKind : std::uint8_t {
  /// The generation is the authoritative serving generation for the scope.
  Exclusive = 0,
  /// The generation serves only the canary share of the scope.
  Canary,
  /// The generation shares the scope under an explicit split-traffic policy.
  SplitTraffic,
  /// The generation is retained so that a rollback can be executed.
  RollbackRetained,
  /// The generation is draining; it must not receive new authority.
  Draining,
};

[[nodiscard]] std::string_view to_string(AuthorityKind kind) noexcept;

/// One committed authority binding. A scope holds at most one Exclusive binding
/// unless policy explicitly permits split traffic, in which case the additional
/// bindings carry SplitTraffic or Canary.
struct AuthorityBinding {
  ScopeId scope{};
  ScopeGeneration scope_generation{};
  ModelId model{};
  ModelVersionId version{};
  ModelGeneration model_generation{};
  ArtifactSetId artifact_set{};
  ArtifactGeneration artifact_generation{};
  LifecycleState lifecycle_state{LifecycleState::REGISTERED};
  AuthorityKind kind{AuthorityKind::Exclusive};
  RolloutId rollout{};
  RolloutGeneration rollout_generation{};
  RolloutStageId stage{};
  RolloutStageGeneration stage_generation{};
  PromotionId promotion{};
  PromotionGeneration promotion_generation{};
  RollbackId rollback{};
  RollbackGeneration rollback_generation{};
  CoordinatorEpoch epoch{};
  EvidenceGeneration evidence_generation{};
  Sequence granted_sequence{};
  bool superseded{false};

  /// Identity of the model generation this binding grants authority to.
  [[nodiscard]] bool same_generation_as(const AuthorityBinding& other) const noexcept {
    return model == other.model && version == other.version &&
           model_generation == other.model_generation;
  }
};

/// Deterministic answer to: which exact generation is authoritative for scope X
/// now, and why.
struct AuthorityQueryResult {
  OutcomeCode outcome{OutcomeCode::NO_AUTHORITATIVE_MODEL};
  ScopeId scope{};
  ScopeGeneration scope_generation{};
  /// The exclusive binding that answers the question, when one exists.
  const AuthorityBinding* binding{nullptr};
  /// True when the answer came from an ancestor scope rather than the scope
  /// itself.
  bool inherited{false};
  ScopeId inherited_scope{};
  /// Canary or split-traffic overlays active at the queried scope.
  std::vector<const AuthorityBinding*> overlays{};
  /// Rollback-retained bindings visible at the queried scope.
  std::vector<const AuthorityBinding*> retained{};
  Explanation explanation{};
};

/// Bounded authority table.
class AuthorityTable {
 public:
  explicit AuthorityTable(std::size_t max_bindings = 16384) : max_bindings_(max_bindings) {}

  /// Grant a binding. Refuses a second Exclusive binding for the same scope
  /// unless coexistence is explicitly allowed by policy.
  Decision grant(const AuthorityBinding& binding, bool allow_coexistence);

  /// Rewrite the Exclusive binding for a scope to a new generation, returning
  /// the superseded binding.
  Decision replace_exclusive(const AuthorityBinding& binding, AuthorityBinding& superseded);

  /// Mark every live binding for a scope superseded. Returns how many changed.
  std::size_t supersede_scope(ScopeId scope);

  /// Supersede every live binding that points at a specific rollout generation.
  std::size_t supersede_rollout(RolloutId rollout, RolloutGeneration generation);

  /// Mark live bindings for a version as draining.
  std::size_t mark_draining(ModelId model, ModelVersionId version, ScopeId scope);

  [[nodiscard]] std::vector<const AuthorityBinding*> bindings_for(ScopeId scope) const;
  [[nodiscard]] std::vector<const AuthorityBinding*> live_bindings_for(ScopeId scope) const;
  [[nodiscard]] std::vector<const AuthorityBinding*> bindings_for_version(
      ModelId model, ModelVersionId version) const;
  /// Every live binding under a scope subtree, in deterministic scope order.
  [[nodiscard]] std::vector<const AuthorityBinding*> subtree_bindings(
      const ScopeRegistry& scopes, ScopeId root) const;

  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::size_t live_count() const noexcept;
  void set_max_bindings(std::size_t value) noexcept { max_bindings_ = value; }
  [[nodiscard]] std::size_t max_bindings() const noexcept { return max_bindings_; }

  [[nodiscard]] const std::map<std::uint64_t, std::vector<AuthorityBinding>>& raw() const noexcept {
    return bindings_;
  }
  /// Mutable access for lifecycle fencing. Callers must preserve the table
  /// invariants they touch; the engine only uses this to supersede bindings.
  [[nodiscard]] std::map<std::uint64_t, std::vector<AuthorityBinding>>& raw_mutable() noexcept {
    return bindings_;
  }
  /// Replace the table during recovery. Validates the single-exclusive invariant
  /// per scope; on failure the table is untouched.
  [[nodiscard]] bool restore(std::map<std::uint64_t, std::vector<AuthorityBinding>> bindings);

  /// True when the table currently violates the single-exclusive invariant.
  [[nodiscard]] bool has_conflict() const;

 private:
  std::map<std::uint64_t, std::vector<AuthorityBinding>> bindings_{};
  std::size_t max_bindings_{16384};
};

}  // namespace mlf
