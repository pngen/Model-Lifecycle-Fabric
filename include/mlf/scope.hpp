// Model Lifecycle Fabric — deterministic rollout scope model.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mlf/identity.hpp"
#include "mlf/status.hpp"

namespace mlf {

/// Scope level. The hierarchy is a strict tree rooted at Global; a child scope
/// may never silently override a parent policy unless the policy model says so.
enum class ScopeKind : std::uint8_t {
  Global = 0,
  Site = 1,
  Region = 2,
  Cluster = 3,
  Environment = 4,
  Service = 5,
  Tenant = 6,
  ReplicaGroup = 7,
  WorkloadClass = 8,
};

inline constexpr std::size_t kScopeKindCount = 9;

[[nodiscard]] std::string_view to_string(ScopeKind kind) noexcept;
/// Parse a scope-kind token such as "cluster". Returns false when unknown.
[[nodiscard]] bool parse_scope_kind(std::string_view text, ScopeKind& out) noexcept;

/// One node of the scope tree.
struct ScopeRecord {
  ScopeId id{};
  ScopeId parent{};
  ScopeKind kind{ScopeKind::Global};
  std::string name{};
  /// Canonical path such as "global/region:us-east/cluster:c1".
  std::string path{};
  /// Increments whenever this scope's lifecycle binding changes: an authority
  /// binding rewrite or a policy override attach/detach. Frames and plans that
  /// cite a superseded scope generation are rejected.
  ScopeGeneration generation{};
  std::uint32_t depth{0};
};

/// Bounded, deterministic scope tree.
///
/// A ScopeId is the 1-based position of its record in creation order, so the same
/// sequence of registration calls always yields the same identity assignment and
/// lookup is constant time.
class ScopeRegistry {
 public:
  ScopeRegistry() = default;

  /// Create the root Global scope. Idempotent.
  ScopeId ensure_root();

  /// Create a child scope under an existing parent.
  Decision add_child(ScopeId parent, ScopeKind kind, std::string_view name, ScopeId& out);

  /// Create every missing node along a canonical path such as
  /// "global/cluster:c1/tenant:t1". Deterministic.
  Decision ensure_path(std::string_view path, ScopeId& out);

  /// Resolve a path without creating anything.
  [[nodiscard]] std::optional<ScopeId> find_path(std::string_view path) const;

  [[nodiscard]] const ScopeRecord* find(ScopeId id) const noexcept;
  [[nodiscard]] ScopeRecord* find_mutable(ScopeId id) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

  /// True when ancestor is descendant or an ancestor of it.
  [[nodiscard]] bool is_ancestor_or_self(ScopeId ancestor, ScopeId descendant) const noexcept;

  /// Root-first chain including the scope itself.
  [[nodiscard]] std::vector<ScopeId> chain(ScopeId id) const;

  /// Every registered scope, in creation order.
  [[nodiscard]] std::vector<ScopeId> all_ids() const;

  /// Bump a scope generation because its lifecycle binding changed.
  void touch(ScopeId id);

  /// Direct children of a scope, in creation order.
  [[nodiscard]] const std::vector<ScopeId>& children_of(ScopeId id) const;

  void set_max_scopes(std::size_t value) noexcept { max_scopes_ = value; }
  [[nodiscard]] std::size_t max_scopes() const noexcept { return max_scopes_; }

  /// Replace the whole tree during recovery. Returns false when the incoming
  /// structure cannot be made consistent (missing root, unknown parent,
  /// non-contiguous identity, duplicate path, depth mismatch); on failure the
  /// registry is left completely untouched.
  [[nodiscard]] bool restore(std::vector<ScopeRecord> records, ScopeId root);

  [[nodiscard]] const std::vector<ScopeRecord>& records() const noexcept { return records_; }
  [[nodiscard]] ScopeId root() const noexcept { return root_; }

  /// Canonical path of a scope, or an empty string when unknown.
  [[nodiscard]] std::string path_of(ScopeId id) const;

 private:
  [[nodiscard]] static bool valid_name(std::string_view name) noexcept;

  std::vector<ScopeRecord> records_{};
  std::vector<std::vector<ScopeId>> children_{};
  std::map<std::string, ScopeId> path_index_{};
  std::size_t max_scopes_{4096};
  ScopeId root_{};
};

}  // namespace mlf
