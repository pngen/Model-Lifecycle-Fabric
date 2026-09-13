#include "mlf/scope.hpp"

#include <algorithm>

namespace mlf {
namespace {

struct KindName {
  ScopeKind kind;
  std::string_view name;
};

constexpr KindName kKindNames[] = {
    {ScopeKind::Global, "global"},
    {ScopeKind::Site, "site"},
    {ScopeKind::Region, "region"},
    {ScopeKind::Cluster, "cluster"},
    {ScopeKind::Environment, "environment"},
    {ScopeKind::Service, "service"},
    {ScopeKind::Tenant, "tenant"},
    {ScopeKind::ReplicaGroup, "replica-group"},
    {ScopeKind::WorkloadClass, "workload-class"},
};

constexpr std::size_t kMaxScopeNameLength = 64;
constexpr std::size_t kMaxScopePathLength = 1024;
constexpr std::uint32_t kMaxScopeDepth = 32;

}  // namespace

std::string_view to_string(ScopeKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) return entry.name;
  }
  return "unknown";
}

bool parse_scope_kind(std::string_view text, ScopeKind& out) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.name == text) {
      out = entry.kind;
      return true;
    }
  }
  if (text == "replicagroup") {
    out = ScopeKind::ReplicaGroup;
    return true;
  }
  if (text == "workloadclass") {
    out = ScopeKind::WorkloadClass;
    return true;
  }
  return false;
}

bool ScopeRegistry::valid_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > kMaxScopeNameLength) return false;
  for (char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

ScopeId ScopeRegistry::ensure_root() {
  if (root_.valid() && !records_.empty()) return root_;
  ScopeRecord root;
  root.id = ScopeId(1);
  root.parent = ScopeId{};
  root.kind = ScopeKind::Global;
  root.name = "global";
  root.path = "global";
  root.generation = ScopeGeneration(kFirstGeneration);
  root.depth = 0;
  records_.push_back(std::move(root));
  children_.emplace_back();
  path_index_.emplace("global", ScopeId(1));
  root_ = ScopeId(1);
  return root_;
}

const ScopeRecord* ScopeRegistry::find(ScopeId id) const noexcept {
  if (!id.valid()) return nullptr;
  const std::uint64_t index = id.raw() - 1;
  if (index >= records_.size()) return nullptr;
  return &records_[static_cast<std::size_t>(index)];
}

ScopeRecord* ScopeRegistry::find_mutable(ScopeId id) noexcept {
  if (!id.valid()) return nullptr;
  const std::uint64_t index = id.raw() - 1;
  if (index >= records_.size()) return nullptr;
  return &records_[static_cast<std::size_t>(index)];
}

std::string ScopeRegistry::path_of(ScopeId id) const {
  const ScopeRecord* record = find(id);
  return record != nullptr ? record->path : std::string{};
}

Decision ScopeRegistry::add_child(ScopeId parent, ScopeKind kind, std::string_view name,
                                  ScopeId& out) {
  out = ScopeId{};
  ensure_root();
  Decision decision(OutcomeCode::Accepted);

  const ScopeRecord* parent_record = find(parent);
  if (parent_record == nullptr) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "parent scope is not registered");
  }
  if (kind == ScopeKind::Global) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "only one global scope exists");
  }
  if (!valid_name(name)) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "scope name must be [A-Za-z0-9._-]{1,64}");
  }
  if (records_.size() >= max_scopes_) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsScopeCount, "scope bound reached");
  }
  if (parent_record->depth + 1 > kMaxScopeDepth) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::InvalidName, "scope depth bound reached");
  }

  std::string path = parent_record->path;
  path.push_back('/');
  path.append(to_string(kind));
  path.push_back(':');
  path.append(name);
  if (path.size() > kMaxScopePathLength) {
    decision.set_outcome(OutcomeCode::BOUNDS_EXCEEDED);
    return decision.add(ReasonCode::BoundsStringLength, "scope path too long");
  }
  if (path_index_.find(path) != path_index_.end()) {
    decision.set_outcome(OutcomeCode::CONFLICT);
    return decision.add(ReasonCode::InvalidName, "scope path already exists");
  }

  ScopeRecord record;
  record.id = ScopeId(static_cast<std::uint64_t>(records_.size()) + 1);
  record.parent = parent;
  record.kind = kind;
  record.name.assign(name);
  record.path = std::move(path);
  record.generation = ScopeGeneration(kFirstGeneration);
  record.depth = parent_record->depth + 1;

  const ScopeId created = record.id;
  path_index_.emplace(record.path, created);
  records_.push_back(std::move(record));
  children_.emplace_back();

  const std::uint64_t parent_index = parent.raw() - 1;
  children_[static_cast<std::size_t>(parent_index)].push_back(created);
  out = created;
  return decision;
}

Decision ScopeRegistry::ensure_path(std::string_view path, ScopeId& out) {
  out = ScopeId{};
  Decision decision(OutcomeCode::Accepted);
  if (path.empty() || path.size() > kMaxScopePathLength) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::BoundsStringLength, "scope path empty or oversized");
  }

  ensure_root();
  if (const auto found = path_index_.find(std::string(path)); found != path_index_.end()) {
    out = found->second;
    return decision;
  }

  std::size_t cursor = 0;
  std::string prefix;
  ScopeId current = root_;
  bool first_segment = true;
  while (cursor <= path.size()) {
    const std::size_t slash = path.find('/', cursor);
    const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
    const std::string_view segment = path.substr(cursor, end - cursor);
    if (segment.empty()) {
      decision.set_outcome(OutcomeCode::Rejected);
      return decision.add(ReasonCode::InvalidName, "empty scope path segment");
    }
    if (first_segment) {
      if (segment != "global") {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::InvalidName, "scope path must start at global");
      }
      prefix = "global";
      first_segment = false;
    } else {
      const std::size_t colon = segment.find(':');
      if (colon == std::string_view::npos) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::InvalidName, "scope segment must be kind:name");
      }
      ScopeKind kind{};
      if (!parse_scope_kind(segment.substr(0, colon), kind)) {
        decision.set_outcome(OutcomeCode::Rejected);
        return decision.add(ReasonCode::InvalidName, "unknown scope kind");
      }
      const std::string_view name = segment.substr(colon + 1);
      std::string candidate = prefix;
      candidate.push_back('/');
      candidate.append(segment);
      if (const auto found = path_index_.find(candidate); found != path_index_.end()) {
        current = found->second;
      } else {
        ScopeId created{};
        Decision added = add_child(current, kind, name, created);
        if (!added.allowed()) {
          decision.set_outcome(added.outcome());
          for (const Reason& reason : added.reasons()) decision.add(reason);
          return decision;
        }
        current = created;
      }
      prefix = std::move(candidate);
    }
    if (slash == std::string_view::npos) break;
    cursor = slash + 1;
  }

  if (!current.valid()) {
    decision.set_outcome(OutcomeCode::Rejected);
    return decision.add(ReasonCode::UnknownScope, "path resolution failed");
  }
  out = current;
  return decision;
}

std::optional<ScopeId> ScopeRegistry::find_path(std::string_view path) const {
  const auto found = path_index_.find(std::string(path));
  if (found == path_index_.end()) return std::nullopt;
  return found->second;
}

bool ScopeRegistry::is_ancestor_or_self(ScopeId ancestor, ScopeId descendant) const noexcept {
  if (!ancestor.valid() || !descendant.valid()) return false;
  if (ancestor == descendant) return true;
  const ScopeRecord* record = find(descendant);
  std::uint32_t guard = 0;
  while (record != nullptr && record->parent.valid()) {
    if (record->parent == ancestor) return true;
    record = find(record->parent);
    if (++guard > kMaxScopeDepth + 1) return false;
  }
  return false;
}

std::vector<ScopeId> ScopeRegistry::chain(ScopeId id) const {
  std::vector<ScopeId> out;
  const ScopeRecord* record = find(id);
  if (record == nullptr) return out;
  out.reserve(record->depth + 1);
  while (record != nullptr) {
    out.push_back(record->id);
    if (!record->parent.valid()) break;
    record = find(record->parent);
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<ScopeId> ScopeRegistry::all_ids() const {
  std::vector<ScopeId> out;
  out.reserve(records_.size());
  for (const ScopeRecord& record : records_) out.push_back(record.id);
  return out;
}

void ScopeRegistry::touch(ScopeId id) {
  if (ScopeRecord* record = find_mutable(id); record != nullptr) {
    record->generation = next_generation(record->generation);
  }
}

const std::vector<ScopeId>& ScopeRegistry::children_of(ScopeId id) const {
  static const std::vector<ScopeId> kEmpty{};
  if (!id.valid()) return kEmpty;
  const std::uint64_t index = id.raw() - 1;
  if (index >= children_.size()) return kEmpty;
  return children_[static_cast<std::size_t>(index)];
}

bool ScopeRegistry::restore(std::vector<ScopeRecord> records, ScopeId root) {
  if (records.empty()) return false;
  if (!root.valid() || root.raw() != 1) return false;
  if (records.size() > max_scopes_) return false;

  std::vector<std::vector<ScopeId>> children(records.size());
  std::map<std::string, ScopeId> index;

  for (std::size_t i = 0; i < records.size(); ++i) {
    ScopeRecord& record = records[i];
    // Identity must be the contiguous 1-based creation index: recovery must not
    // be able to introduce a hole that later allocation would collide with.
    if (record.id.raw() != static_cast<std::uint64_t>(i) + 1) return false;
    if (!record.generation.valid()) return false;
    if (record.path.empty() || record.path.size() > kMaxScopePathLength) return false;
    if (!valid_name(record.name)) return false;
    if (i == 0) {
      if (record.kind != ScopeKind::Global) return false;
      if (record.parent.valid()) return false;
      if (record.path != "global") return false;
      if (record.depth != 0) return false;
    } else {
      if (!record.parent.valid()) return false;
      if (record.parent.raw() == 0 || record.parent.raw() > records.size()) return false;
      if (record.parent.raw() >= record.id.raw()) return false;
      const ScopeRecord& parent = records[static_cast<std::size_t>(record.parent.raw() - 1)];
      if (record.depth != parent.depth + 1) return false;
      if (record.depth > kMaxScopeDepth) return false;
      std::string expected = parent.path;
      expected.push_back('/');
      expected.append(to_string(record.kind));
      expected.push_back(':');
      expected.append(record.name);
      if (expected != record.path) return false;
      children[static_cast<std::size_t>(record.parent.raw() - 1)].push_back(record.id);
    }
    if (!index.emplace(record.path, record.id).second) return false;
  }

  records_ = std::move(records);
  children_ = std::move(children);
  path_index_ = std::move(index);
  root_ = root;
  return true;
}

}  // namespace mlf
