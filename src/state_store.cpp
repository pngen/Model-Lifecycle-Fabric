#include "mlf/state_store.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace mlf {
namespace {

constexpr std::uint8_t kMagic[4] = {'M', 'L', 'F', 'S'};
constexpr std::size_t kHeaderSize = 4 + 2 + 2 + 4;
constexpr std::size_t kTrailerSize = 4;

std::uint32_t crc_table_entry(std::uint32_t index) noexcept {
  std::uint32_t value = index;
  for (int bit = 0; bit < 8; ++bit) {
    value = (value & 1u) != 0u ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
  }
  return value;
}

const std::uint32_t* crc_table() noexcept {
  static const std::uint32_t table[256] = {
#define MLF_CRC_ROW(n)                                                                     \
  crc_table_entry(n * 16 + 0), crc_table_entry(n * 16 + 1), crc_table_entry(n * 16 + 2),   \
      crc_table_entry(n * 16 + 3), crc_table_entry(n * 16 + 4), crc_table_entry(n * 16 + 5), \
      crc_table_entry(n * 16 + 6), crc_table_entry(n * 16 + 7), crc_table_entry(n * 16 + 8), \
      crc_table_entry(n * 16 + 9), crc_table_entry(n * 16 + 10), crc_table_entry(n * 16 + 11), \
      crc_table_entry(n * 16 + 12), crc_table_entry(n * 16 + 13),                             \
      crc_table_entry(n * 16 + 14), crc_table_entry(n * 16 + 15)
      MLF_CRC_ROW(0),  MLF_CRC_ROW(1),  MLF_CRC_ROW(2),  MLF_CRC_ROW(3),  MLF_CRC_ROW(4),
      MLF_CRC_ROW(5),  MLF_CRC_ROW(6),  MLF_CRC_ROW(7),  MLF_CRC_ROW(8),  MLF_CRC_ROW(9),
      MLF_CRC_ROW(10), MLF_CRC_ROW(11), MLF_CRC_ROW(12), MLF_CRC_ROW(13), MLF_CRC_ROW(14),
      MLF_CRC_ROW(15)
#undef MLF_CRC_ROW
  };
  return table;
}

class ByteWriter {
 public:
  explicit ByteWriter(std::vector<std::uint8_t>& out) : out_(out) {}

  void u8(std::uint8_t value) { out_.push_back(value); }
  void boolean(bool value) { u8(value ? 1u : 0u); }
  void u16(std::uint16_t value) {
    u8(static_cast<std::uint8_t>(value & 0xffu));
    u8(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  }
  void u32(std::uint32_t value) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
  }
  void u64(std::uint64_t value) {
    for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
  }
  void i8(std::int8_t value) { u8(static_cast<std::uint8_t>(value)); }
  void f64(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    u64(bits);
  }
  void str(std::string_view value) {
    u32(static_cast<std::uint32_t>(value.size()));
    for (char c : value) u8(static_cast<std::uint8_t>(c));
  }
  void raw(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out_.insert(out_.end(), bytes, bytes + size);
  }

 private:
  std::vector<std::uint8_t>& out_;
};

class ByteReader {
 public:
  ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool exhausted() const noexcept { return pos_ == size_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - pos_; }
  [[nodiscard]] bool skip(std::size_t count) {
    if (!ok_ || remaining() < count) {
      ok_ = false;
      return false;
    }
    pos_ += count;
    return true;
  }

  std::uint8_t u8() {
    if (!ok_ || remaining() < 1) {
      ok_ = false;
      return 0;
    }
    return data_[pos_++];
  }
  bool boolean() { return u8() != 0; }
  std::uint16_t u16() {
    std::uint16_t value = 0;
    for (int i = 0; i < 2; ++i) value = static_cast<std::uint16_t>(value | (static_cast<std::uint16_t>(u8()) << (8 * i)));
    return value;
  }
  std::uint32_t u32() {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(u8()) << (8 * i);
    return value;
  }
  std::uint64_t u64() {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<std::uint64_t>(u8()) << (8 * i);
    return value;
  }
  std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
  double f64() {
    const std::uint64_t bits = u64();
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  std::string str(std::size_t max_length) {
    const std::uint32_t length = u32();
    if (!ok_ || length > max_length || remaining() < length) {
      ok_ = false;
      return {};
    }
    std::string out(reinterpret_cast<const char*>(data_ + pos_), length);
    pos_ += length;
    return out;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_{0};
  bool ok_{true};
};

template <class Tag>
void write_id(ByteWriter& w, Id<Tag> id) {
  w.u64(id.raw());
}

template <class Tag>
Id<Tag> read_id(ByteReader& r) {
  return Id<Tag>(r.u64());
}

// Sections identify the durable structure that follows.
enum class Section : std::uint16_t {
  Header = 1,
  Scopes = 2,
  Policy = 3,
  Models = 4,
  Versions = 5,
  Compatibility = 6,
  Rollouts = 7,
  Authority = 8,
  Workers = 9,
  Evidence = 10,
  Attempts = 11,
  Committed = 12,
  Allocators = 13,
};

constexpr std::uint16_t kSectionCount = 13;

void write_subject(ByteWriter& w, const EvidenceSubject& subject) {
  write_id(w, subject.model);
  write_id(w, subject.version);
  write_id(w, subject.model_generation);
  write_id(w, subject.artifact_set);
  write_id(w, subject.artifact_generation);
  write_id(w, subject.scope);
  write_id(w, subject.rollout);
  write_id(w, subject.rollout_generation);
  write_id(w, subject.stage);
  write_id(w, subject.stage_generation);
  write_id(w, subject.compatibility_generation);
}

void read_subject(ByteReader& r, EvidenceSubject& subject) {
  subject.model = read_id<ModelIdTag>(r);
  subject.version = read_id<ModelVersionIdTag>(r);
  subject.model_generation = read_id<ModelGenerationTag>(r);
  subject.artifact_set = read_id<ArtifactSetIdTag>(r);
  subject.artifact_generation = read_id<ArtifactGenerationTag>(r);
  subject.scope = read_id<ScopeIdTag>(r);
  subject.rollout = read_id<RolloutIdTag>(r);
  subject.rollout_generation = read_id<RolloutGenerationTag>(r);
  subject.stage = read_id<RolloutStageIdTag>(r);
  subject.stage_generation = read_id<RolloutStageGenerationTag>(r);
  subject.compatibility_generation = read_id<CompatibilityGenerationTag>(r);
}

void write_requirements(ByteWriter& w, const ModelRequirements& value) {
  w.str(value.runtime);
  w.str(value.runtime_min_version);
  w.str(value.backend);
  w.str(value.architecture);
  w.str(value.precision);
  w.str(value.tokenizer_generation);
  w.str(value.adapter_set);
}

void read_requirements(ByteReader& r, const DecodeLimits& limits, ModelRequirements& value) {
  value.runtime = r.str(limits.max_string);
  value.runtime_min_version = r.str(limits.max_string);
  value.backend = r.str(limits.max_string);
  value.architecture = r.str(limits.max_string);
  value.precision = r.str(limits.max_string);
  value.tokenizer_generation = r.str(limits.max_string);
  value.adapter_set = r.str(limits.max_string);
}

void write_binding(ByteWriter& w, const ArtifactBinding& value) {
  write_id(w, value.set_id);
  write_id(w, value.generation);
  w.str(value.digest);
}

void read_binding(ByteReader& r, const DecodeLimits& limits, ArtifactBinding& value) {
  value.set_id = read_id<ArtifactSetIdTag>(r);
  value.generation = read_id<ArtifactGenerationTag>(r);
  value.digest = r.str(limits.max_digest);
}

void write_criterion(ByteWriter& w, const Criterion& value) {
  w.u8(static_cast<std::uint8_t>(value.kind));
  w.u8(static_cast<std::uint8_t>(value.comparison));
  w.f64(value.threshold);
}

void read_criterion(ByteReader& r, Criterion& value) {
  value.kind = static_cast<EvidenceKind>(r.u8());
  value.comparison = static_cast<CriterionComparison>(r.u8());
  value.threshold = r.f64();
}

void write_evidence_requirement(ByteWriter& w, const EvidenceRequirement& value) {
  w.u8(static_cast<std::uint8_t>(value.kind));
  w.u8(static_cast<std::uint8_t>(value.required_verdict));
  w.boolean(value.require_exact_subject);
  w.boolean(value.require_current_publisher);
}

void read_evidence_requirement(ByteReader& r, EvidenceRequirement& value) {
  value.kind = static_cast<EvidenceKind>(r.u8());
  value.required_verdict = static_cast<EvidenceVerdict>(r.u8());
  value.require_exact_subject = r.boolean();
  value.require_current_publisher = r.boolean();
}

bool valid_evidence_kind(std::uint8_t raw) noexcept {
  return static_cast<std::size_t>(raw) < kEvidenceKindCount;
}

bool valid_verdict(std::uint8_t raw) noexcept { return raw <= 2; }

bool read_policy(ByteReader& r, const DecodeLimits& limits, LifecyclePolicy& policy) {
  policy.generation = read_id<PolicyGenerationTag>(r);
  policy.max_active_generations_per_scope = r.u32();
  policy.allow_split_traffic = r.boolean();
  policy.require_rollback_target = r.boolean();
  policy.allow_automatic_rollback = r.boolean();
  policy.require_manual_approval_for_promotion = r.boolean();
  policy.max_blast_radius_percent = r.u32();
  policy.require_evidence_for_stage_advance = r.boolean();
  policy.max_evidence_age_ticks = r.u64();
  policy.require_residency_before_stage_entry = r.boolean();
  policy.require_drain_after_stage_commit = r.boolean();
  policy.allow_immediate_cutover = r.boolean();
  policy.allow_retirement_without_drain = r.boolean();
  policy.rollback_retention_generations = r.u32();
  policy.supersede_inflight_rollout_on_new_promotion = r.boolean();
  policy.allow_version_label_reuse_after_retirement = r.boolean();
  const std::uint32_t scope_kinds = r.u32();
  if (!r.ok() || scope_kinds > kScopeKindCount) return false;
  for (std::uint32_t i = 0; i < scope_kinds; ++i) {
    const std::uint8_t kind = r.u8();
    if (kind >= kScopeKindCount) return false;
    policy.promotable_scope_kinds.push_back(static_cast<ScopeKind>(kind));
  }
  const std::uint32_t evidence_kinds = r.u32();
  if (!r.ok() || evidence_kinds > limits.max_string) return false;
  if (evidence_kinds > kEvidenceKindCount * 4) return false;
  for (std::uint32_t i = 0; i < evidence_kinds; ++i) {
    const std::uint8_t kind = r.u8();
    if (!valid_evidence_kind(kind)) return false;
    policy.globally_required_evidence.push_back(static_cast<EvidenceKind>(kind));
  }
  return r.ok();
}

void write_policy(ByteWriter& w, const LifecyclePolicy& policy) {
  write_id(w, policy.generation);
  w.u32(policy.max_active_generations_per_scope);
  w.boolean(policy.allow_split_traffic);
  w.boolean(policy.require_rollback_target);
  w.boolean(policy.allow_automatic_rollback);
  w.boolean(policy.require_manual_approval_for_promotion);
  w.u32(policy.max_blast_radius_percent);
  w.boolean(policy.require_evidence_for_stage_advance);
  w.u64(policy.max_evidence_age_ticks);
  w.boolean(policy.require_residency_before_stage_entry);
  w.boolean(policy.require_drain_after_stage_commit);
  w.boolean(policy.allow_immediate_cutover);
  w.boolean(policy.allow_retirement_without_drain);
  w.u32(policy.rollback_retention_generations);
  w.boolean(policy.supersede_inflight_rollout_on_new_promotion);
  w.boolean(policy.allow_version_label_reuse_after_retirement);
  w.u32(static_cast<std::uint32_t>(policy.promotable_scope_kinds.size()));
  for (ScopeKind kind : policy.promotable_scope_kinds) w.u8(static_cast<std::uint8_t>(kind));
  w.u32(static_cast<std::uint32_t>(policy.globally_required_evidence.size()));
  for (EvidenceKind kind : policy.globally_required_evidence) w.u8(static_cast<std::uint8_t>(kind));
}

bool valid_scope_kind(std::uint8_t raw) noexcept { return raw < kScopeKindCount; }

}  // namespace

std::uint32_t crc32(const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  const std::uint32_t* table = crc_table();
  std::uint32_t value = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    value = table[(value ^ bytes[i]) & 0xFFu] ^ (value >> 8);
  }
  return value ^ 0xFFFFFFFFu;
}

std::string_view to_string(StateStoreStatus status) noexcept {
  switch (status) {
    case StateStoreStatus::Ok:                  return "OK";
    case StateStoreStatus::IoError:             return "IO_ERROR";
    case StateStoreStatus::BadMagic:            return "BAD_MAGIC";
    case StateStoreStatus::UnsupportedVersion:  return "UNSUPPORTED_VERSION";
    case StateStoreStatus::IntegrityFailure:    return "INTEGRITY_FAILURE";
    case StateStoreStatus::Truncated:           return "TRUNCATED";
    case StateStoreStatus::BoundsExceeded:      return "BOUNDS_EXCEEDED";
    case StateStoreStatus::InvalidContent:      return "INVALID_CONTENT";
    case StateStoreStatus::AtomicReplaceFailed: return "ATOMIC_REPLACE_FAILED";
  }
  return "UNKNOWN";
}

DecodeLimits DecodeLimits::from_bounds(const RegistryBounds& bounds) {
  DecodeLimits limits;
  limits.max_models = bounds.max_models;
  limits.max_versions = bounds.max_models * bounds.max_versions_per_model;
  limits.max_scopes = bounds.max_scopes;
  limits.max_rollouts = bounds.max_rollouts;
  limits.max_authority_bindings = bounds.max_authority_bindings;
  limits.max_workers = bounds.max_workers;
  limits.max_evidence = bounds.max_evidence;
  limits.max_attempts = bounds.max_attempts;
  limits.max_string = bounds.max_name_length * 2;
  limits.max_digest = bounds.max_digest_length;
  return limits;
}

StateStoreResult serialize_durable_state(const DurableState& state, const DecodeLimits& limits,
                                         std::vector<std::uint8_t>& out) {
  StateStoreResult result;
  std::vector<std::uint8_t> payload;
  payload.reserve(64 * 1024);

  ByteWriter w(payload);

  // --- header section -----------------------------------------------------
  w.u16(static_cast<std::uint16_t>(Section::Header));
  w.u32(1);
  w.u16(state.format_version);
  write_id(w, state.generation);
  write_id(w, state.epoch);
  write_id(w, state.sequence);

  // --- scopes -------------------------------------------------------------
  if (state.scopes.size() > limits.max_scopes) {
    return {StateStoreStatus::BoundsExceeded, "scope count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Scopes));
  w.u32(static_cast<std::uint32_t>(state.scopes.size()));
  write_id(w, state.root_scope);
  w.u32(static_cast<std::uint32_t>(state.policy_overrides.size()));
  for (const auto& entry : state.policy_overrides) {
    const ScopePolicyOverride& override_policy = entry.second;
    write_id(w, override_policy.scope);
    w.boolean(override_policy.permits_child_override);
    w.u32(override_policy.max_active_generations_per_scope);
    w.i8(override_policy.require_rollback_target);
    w.i8(override_policy.require_evidence_for_stage_advance);
    w.i8(override_policy.require_residency_before_stage_entry);
    w.u32(static_cast<std::uint32_t>(override_policy.additional_required_evidence.size()));
    for (EvidenceKind kind : override_policy.additional_required_evidence) {
      w.u8(static_cast<std::uint8_t>(kind));
    }
    w.u32(static_cast<std::uint32_t>(override_policy.additional_promotable_scope_kinds.size()));
    for (ScopeKind kind : override_policy.additional_promotable_scope_kinds) {
      w.u8(static_cast<std::uint8_t>(kind));
    }
  }
  for (const ScopeRecord& scope : state.scopes) {
    write_id(w, scope.id);
    write_id(w, scope.parent);
    w.u8(static_cast<std::uint8_t>(scope.kind));
    w.str(scope.name);
    w.str(scope.path);
    write_id(w, scope.generation);
    w.u32(scope.depth);
  }

  // --- policy -------------------------------------------------------------
  w.u16(static_cast<std::uint16_t>(Section::Policy));
  w.u32(1);
  write_policy(w, state.base_policy);

  // --- models -------------------------------------------------------------
  if (state.models.size() > limits.max_models) {
    return {StateStoreStatus::BoundsExceeded, "model count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Models));
  w.u32(static_cast<std::uint32_t>(state.models.size()));
  for (const auto& entry : state.models) {
    const ModelRecord& record = entry.second;
    write_id(w, record.id);
    w.str(record.name);
    w.str(record.family);
    w.u8(static_cast<std::uint8_t>(record.provenance));
    write_id(w, record.created_sequence);
  }

  // --- versions -----------------------------------------------------------
  if (state.versions.size() > limits.max_versions) {
    return {StateStoreStatus::BoundsExceeded, "version count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Versions));
  w.u32(static_cast<std::uint32_t>(state.versions.size()));
  for (const auto& entry : state.versions) {
    const ModelVersionRecord& record = entry.second;
    write_id(w, record.model);
    write_id(w, record.version);
    w.str(record.label);
    write_id(w, record.generation);
    write_id(w, record.predecessor);
    write_id(w, record.predecessor_generation);
    write_binding(w, record.artifact);
    write_requirements(w, record.requirements);
    w.u8(static_cast<std::uint8_t>(record.state));
    write_id(w, record.lifecycle_generation);
    write_id(w, record.last_promotion);
    write_id(w, record.promotion_generation);
    write_id(w, record.rollout);
    write_id(w, record.rollout_generation);
    write_id(w, record.current_stage);
    write_id(w, record.current_stage_generation);
    write_id(w, record.compatibility_generation);
    write_id(w, record.last_rollback);
    write_id(w, record.rollback_generation);
    w.u8(static_cast<std::uint8_t>(record.provenance));
    write_id(w, record.created_sequence);
    write_id(w, record.updated_sequence);
  }

  // --- compatibility ------------------------------------------------------
  if (state.compatibility.size() > limits.max_compatibility_facts) {
    return {StateStoreStatus::BoundsExceeded, "compatibility fact count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Compatibility));
  w.u32(static_cast<std::uint32_t>(state.compatibility.size()));
  for (const auto& entry : state.compatibility) {
    const CompatibilityFact& fact = entry.second;
    write_id(w, fact.model);
    write_id(w, fact.version);
    write_id(w, fact.model_generation);
    write_id(w, fact.artifact_set);
    write_id(w, fact.artifact_generation);
    write_id(w, fact.generation);
    w.str(fact.environment_key);
    w.u8(static_cast<std::uint8_t>(fact.outcome));
    w.str(fact.detail);
    w.u8(static_cast<std::uint8_t>(fact.provenance));
    write_id(w, fact.sequence);
  }

  // --- rollouts -----------------------------------------------------------
  if (state.rollouts.size() > limits.max_rollouts) {
    return {StateStoreStatus::BoundsExceeded, "rollout count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Rollouts));
  w.u32(static_cast<std::uint32_t>(state.rollouts.size()));
  for (const auto& entry : state.rollouts) {
    const RolloutPlan& plan = entry.second;
    write_id(w, plan.id);
    write_id(w, plan.generation);
    write_id(w, plan.model);
    write_id(w, plan.candidate);
    write_id(w, plan.candidate_generation);
    write_id(w, plan.candidate_artifact_set);
    write_id(w, plan.candidate_artifact_generation);
    write_id(w, plan.previous);
    write_id(w, plan.previous_generation);
    write_id(w, plan.compatibility_generation);
    write_id(w, plan.policy_generation);
    write_id(w, plan.evidence_generation);
    write_id(w, plan.root_scope);
    write_id(w, plan.root_scope_generation);
    w.u8(static_cast<std::uint8_t>(plan.strategy));
    write_id(w, plan.promotion);
    write_id(w, plan.promotion_generation);
    write_id(w, plan.rollback_target);
    write_id(w, plan.rollback_target_generation);
    w.u32(plan.max_blast_radius_percent);
    w.boolean(plan.manual_progression);
    w.u32(static_cast<std::uint32_t>(plan.cohorts.size()));
    for (const CohortDefinition& cohort : plan.cohorts) {
      write_id(w, cohort.id);
      w.str(cohort.name);
      w.u32(static_cast<std::uint32_t>(cohort.scopes.size()));
      for (ScopeId scope : cohort.scopes) write_id(w, scope);
      w.u32(cohort.traffic_percent);
      w.u32(cohort.max_blast_radius_percent);
    }
    w.u32(static_cast<std::uint32_t>(plan.stages.size()));
    for (const StageDefinition& stage : plan.stages) {
      write_id(w, stage.id);
      w.str(stage.name);
      w.u32(static_cast<std::uint32_t>(stage.cohorts.size()));
      for (CohortId cohort : stage.cohorts) write_id(w, cohort);
      w.u32(static_cast<std::uint32_t>(stage.required_evidence.size()));
      for (const EvidenceRequirement& requirement : stage.required_evidence) {
        write_evidence_requirement(w, requirement);
      }
      w.u32(static_cast<std::uint32_t>(stage.acceptance.size()));
      for (const Criterion& criterion : stage.acceptance) write_criterion(w, criterion);
      w.u32(static_cast<std::uint32_t>(stage.failure.size()));
      for (const Criterion& criterion : stage.failure) write_criterion(w, criterion);
      w.boolean(stage.require_residency_before_entry);
      w.boolean(stage.require_drain_after_commit);
    }
    w.u32(static_cast<std::uint32_t>(plan.current_stage_index));
    write_id(w, plan.current_stage_generation);
    w.boolean(plan.stage_entered);
    w.u32(static_cast<std::uint32_t>(plan.history.size()));
    for (const StageRecord& record : plan.history) {
      write_id(w, record.stage);
      write_id(w, record.generation);
      write_id(w, record.entered_sequence);
      write_id(w, record.decided_sequence);
      w.u8(static_cast<std::uint8_t>(record.decision));
      w.u32(static_cast<std::uint32_t>(record.reasons.size()));
      for (const Reason& reason : record.reasons) {
        w.u16(static_cast<std::uint16_t>(reason.code));
        w.str(reason.detail);
      }
    }
    w.boolean(plan.completed);
    w.boolean(plan.failed);
    w.boolean(plan.superseded);
    write_id(w, plan.last_rollback);
    write_id(w, plan.last_rollback_generation);
    write_id(w, plan.created_sequence);
    write_id(w, plan.updated_sequence);
    w.u8(static_cast<std::uint8_t>(plan.provenance));
  }

  // --- authority ----------------------------------------------------------
  std::size_t authority_total = 0;
  for (const auto& entry : state.authority) authority_total += entry.second.size();
  if (authority_total > limits.max_authority_bindings) {
    return {StateStoreStatus::BoundsExceeded, "authority binding count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Authority));
  w.u32(static_cast<std::uint32_t>(state.authority.size()));
  for (const auto& entry : state.authority) {
    w.u32(static_cast<std::uint32_t>(entry.second.size()));
    for (const AuthorityBinding& binding : entry.second) {
      write_id(w, binding.scope);
      write_id(w, binding.scope_generation);
      write_id(w, binding.model);
      write_id(w, binding.version);
      write_id(w, binding.model_generation);
      write_id(w, binding.artifact_set);
      write_id(w, binding.artifact_generation);
      w.u8(static_cast<std::uint8_t>(binding.lifecycle_state));
      w.u8(static_cast<std::uint8_t>(binding.kind));
      write_id(w, binding.rollout);
      write_id(w, binding.rollout_generation);
      write_id(w, binding.stage);
      write_id(w, binding.stage_generation);
      write_id(w, binding.promotion);
      write_id(w, binding.promotion_generation);
      write_id(w, binding.rollback);
      write_id(w, binding.rollback_generation);
      write_id(w, binding.epoch);
      write_id(w, binding.evidence_generation);
      write_id(w, binding.granted_sequence);
    }
  }

  // --- workers ------------------------------------------------------------
  if (state.worker_leases.size() > limits.max_workers) {
    return {StateStoreStatus::BoundsExceeded, "worker lease count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Workers));
  w.u32(static_cast<std::uint32_t>(state.worker_leases.size()));
  for (const auto& entry : state.worker_leases) {
    const WorkerLease& lease = entry.second;
    write_id(w, lease.id);
    write_id(w, lease.boot);
    write_id(w, lease.epoch);
    w.u32(static_cast<std::uint32_t>(lease.scopes.size()));
    for (ScopeId scope : lease.scopes) write_id(w, scope);
    w.boolean(lease.fenced);
    write_id(w, lease.registered_sequence);
    write_id(w, lease.fenced_sequence);
    w.str(lease.detail);
  }

  // --- evidence -----------------------------------------------------------
  if (state.evidence.size() > limits.max_evidence) {
    return {StateStoreStatus::BoundsExceeded, "evidence count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Evidence));
  w.u32(static_cast<std::uint32_t>(state.evidence.size()));
  for (const EvidenceRecord& record : state.evidence) {
    write_id(w, record.id);
    write_id(w, record.generation);
    w.u8(static_cast<std::uint8_t>(record.kind));
    w.u8(static_cast<std::uint8_t>(record.verdict));
    write_subject(w, record.subject);
    w.u8(static_cast<std::uint8_t>(record.provenance));
    write_id(w, record.worker);
    write_id(w, record.boot);
    write_id(w, record.epoch);
    write_id(w, record.policy_generation);
    w.f64(record.value);
    w.str(record.unit);
    w.u64(record.tick);
    w.str(record.detail);
  }

  // --- attempts -----------------------------------------------------------
  if (state.unsettled_attempts.size() > limits.max_attempts) {
    return {StateStoreStatus::BoundsExceeded, "attempt count exceeds decode limits"};
  }
  w.u16(static_cast<std::uint16_t>(Section::Attempts));
  w.u32(static_cast<std::uint32_t>(state.unsettled_attempts.size()));
  for (const auto& entry : state.unsettled_attempts) {
    const Attempt& attempt = entry.second;
    write_id(w, attempt.id);
    w.u8(static_cast<std::uint8_t>(attempt.kind));
    write_id(w, attempt.worker);
    write_id(w, attempt.boot);
    write_id(w, attempt.epoch);
    write_id(w, attempt.model);
    write_id(w, attempt.version);
    write_id(w, attempt.model_generation);
    write_id(w, attempt.artifact_generation);
    write_id(w, attempt.scope);
    write_id(w, attempt.scope_generation);
    write_id(w, attempt.rollout);
    write_id(w, attempt.rollout_generation);
    write_id(w, attempt.stage);
    write_id(w, attempt.stage_generation);
    write_id(w, attempt.residency_generation);
    w.u8(static_cast<std::uint8_t>(attempt.state));
    w.boolean(attempt.idempotent);
    write_id(w, attempt.issued_sequence);
    write_id(w, attempt.settled_sequence);
    w.str(attempt.detail);
  }

  // --- committed ----------------------------------------------------------
  w.u16(static_cast<std::uint16_t>(Section::Committed));
  w.u32(2);
  w.u32(static_cast<std::uint32_t>(state.committed_promotions.size()));
  for (std::uint64_t id : state.committed_promotions) w.u64(id);
  w.u32(static_cast<std::uint32_t>(state.committed_rollbacks.size()));
  for (const auto& entry : state.committed_rollbacks) {
    w.u64(entry.first);
    w.u64(entry.second);
  }

  // --- allocators ---------------------------------------------------------
  w.u16(static_cast<std::uint16_t>(Section::Allocators));
  w.u32(13);
  w.u64(state.next_model_id);
  w.u64(state.next_version_id);
  w.u64(state.next_rollout_id);
  w.u64(state.next_stage_id);
  w.u64(state.next_cohort_id);
  w.u64(state.next_promotion_id);
  w.u64(state.next_rollback_id);
  w.u64(state.next_attempt_id);
  w.u64(state.next_worker_id);
  w.u64(state.next_evidence_id);
  w.u64(state.next_evidence_generation);
  w.u64(state.next_compatibility_generation);
  w.u64(0);

  if (payload.size() > limits.max_payload_bytes) {
    return {StateStoreStatus::BoundsExceeded, "encoded state exceeds the payload bound"};
  }

  out.clear();
  out.reserve(kHeaderSize + payload.size() + kTrailerSize);
  ByteWriter header(out);
  header.raw(kMagic, sizeof(kMagic));
  header.u16(state.format_version);
  header.u16(0);
  header.u32(static_cast<std::uint32_t>(payload.size()));
  header.raw(payload.data(), payload.size());
  const std::uint32_t checksum = crc32(out.data(), out.size());
  header.u32(checksum);
  return result;
}

StateStoreResult deserialize_durable_state(const std::uint8_t* data, std::size_t size,
                                           const DecodeLimits& limits, DurableState& out) {
  if (data == nullptr || size < kHeaderSize + kTrailerSize) {
    return {StateStoreStatus::Truncated, "state file shorter than the minimum frame"};
  }
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    return {StateStoreStatus::BadMagic, "state file magic does not match MLFS"};
  }
  ByteReader header(data + 4, size - 4);
  const std::uint16_t format = header.u16();
  static_cast<void>(header.u16());
  const std::uint32_t payload_length = header.u32();
  if (!header.ok()) return {StateStoreStatus::Truncated, "truncated header"};
  if (format != kStateFormatVersion) {
    return {StateStoreStatus::UnsupportedVersion, "unsupported state format version"};
  }
  if (payload_length > limits.max_payload_bytes) {
    return {StateStoreStatus::BoundsExceeded, "declared payload exceeds the decode bound"};
  }
  if (size < kHeaderSize + static_cast<std::size_t>(payload_length) + kTrailerSize) {
    return {StateStoreStatus::Truncated, "payload shorter than declared length"};
  }
  const std::size_t framed = kHeaderSize + static_cast<std::size_t>(payload_length);
  std::uint32_t stored_crc = 0;
  std::memcpy(&stored_crc, data + framed, sizeof(stored_crc));
  const std::uint32_t computed = crc32(data, framed);
  if (stored_crc != computed) {
    return {StateStoreStatus::IntegrityFailure, "integrity check failed"};
  }

  StateStoreResult result;
  DurableState decoded;
  decoded.format_version = format;

  ByteReader r(data + kHeaderSize, payload_length);
  std::uint16_t sections_seen = 0;

  while (!r.exhausted() && r.ok()) {
    const std::uint16_t tag = r.u16();
    const std::uint32_t count = r.u32();
    if (!r.ok()) return {StateStoreStatus::Truncated, "truncated section header"};
    ++sections_seen;
    if (sections_seen > kSectionCount + 4) {
      return {StateStoreStatus::InvalidContent, "too many sections"};
    }

    switch (static_cast<Section>(tag)) {
      case Section::Header: {
        if (count != 1) return {StateStoreStatus::InvalidContent, "bad header section count"};
        decoded.format_version = r.u16();
        decoded.generation = read_id<SnapshotGenerationTag>(r);
        decoded.epoch = read_id<CoordinatorEpochTag>(r);
        decoded.sequence = read_id<SequenceTag>(r);
        break;
      }
      case Section::Scopes: {
        if (count > limits.max_scopes) {
          return {StateStoreStatus::BoundsExceeded, "scope count exceeds the decode bound"};
        }
        decoded.root_scope = read_id<ScopeIdTag>(r);
        const std::uint32_t overrides = r.u32();
        if (!r.ok() || overrides > limits.max_scopes) {
          return {StateStoreStatus::BoundsExceeded, "policy override count exceeds the bound"};
        }
        for (std::uint32_t i = 0; i < overrides; ++i) {
          ScopePolicyOverride override_policy;
          override_policy.scope = read_id<ScopeIdTag>(r);
          override_policy.permits_child_override = r.boolean();
          override_policy.max_active_generations_per_scope = r.u32();
          override_policy.require_rollback_target = r.i8();
          override_policy.require_evidence_for_stage_advance = r.i8();
          override_policy.require_residency_before_stage_entry = r.i8();
          const std::uint32_t evidence_count = r.u32();
          if (!r.ok() || evidence_count > kEvidenceKindCount * 4) {
            return {StateStoreStatus::BoundsExceeded, "override evidence count out of range"};
          }
          for (std::uint32_t k = 0; k < evidence_count; ++k) {
            const std::uint8_t kind = r.u8();
            if (!valid_evidence_kind(kind)) {
              return {StateStoreStatus::InvalidContent, "unknown evidence kind in override"};
            }
            override_policy.additional_required_evidence.push_back(static_cast<EvidenceKind>(kind));
          }
          const std::uint32_t scope_kind_count = r.u32();
          if (!r.ok() || scope_kind_count > kScopeKindCount) {
            return {StateStoreStatus::BoundsExceeded, "override scope kind count out of range"};
          }
          for (std::uint32_t k = 0; k < scope_kind_count; ++k) {
            const std::uint8_t kind = r.u8();
            if (!valid_scope_kind(kind)) {
              return {StateStoreStatus::InvalidContent, "unknown scope kind in override"};
            }
            override_policy.additional_promotable_scope_kinds.push_back(
                static_cast<ScopeKind>(kind));
          }
          decoded.policy_overrides[override_policy.scope.raw()] = std::move(override_policy);
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          ScopeRecord scope;
          scope.id = read_id<ScopeIdTag>(r);
          scope.parent = read_id<ScopeIdTag>(r);
          const std::uint8_t kind = r.u8();
          if (!valid_scope_kind(kind)) {
            return {StateStoreStatus::InvalidContent, "unknown scope kind"};
          }
          scope.kind = static_cast<ScopeKind>(kind);
          scope.name = r.str(limits.max_string);
          scope.path = r.str(limits.max_string * 8);
          scope.generation = read_id<ScopeGenerationTag>(r);
          scope.depth = r.u32();
          decoded.scopes.push_back(std::move(scope));
        }
        break;
      }
      case Section::Policy: {
        if (count != 1) return {StateStoreStatus::InvalidContent, "bad policy section count"};
        if (!read_policy(r, limits, decoded.base_policy)) {
          return {StateStoreStatus::InvalidContent, "policy section could not be decoded"};
        }
        break;
      }
      case Section::Models: {
        if (count > limits.max_models) {
          return {StateStoreStatus::BoundsExceeded, "model count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          ModelRecord record;
          record.id = read_id<ModelIdTag>(r);
          record.name = r.str(limits.max_string);
          record.family = r.str(limits.max_string);
          const std::uint8_t provenance = r.u8();
          if (provenance > 3) return {StateStoreStatus::InvalidContent, "bad provenance"};
          record.provenance = static_cast<Provenance>(provenance);
          record.created_sequence = read_id<SequenceTag>(r);
          if (!record.id.valid()) return {StateStoreStatus::InvalidContent, "model id is zero"};
          if (decoded.models.find(record.id.raw()) != decoded.models.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate model id"};
          }
          const std::uint64_t model_id = record.id.raw();
          decoded.models[model_id] = std::move(record);
        }
        break;
      }
      case Section::Versions: {
        if (count > limits.max_versions) {
          return {StateStoreStatus::BoundsExceeded, "version count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          ModelVersionRecord record;
          record.model = read_id<ModelIdTag>(r);
          record.version = read_id<ModelVersionIdTag>(r);
          record.label = r.str(limits.max_string);
          record.generation = read_id<ModelGenerationTag>(r);
          record.predecessor = read_id<ModelVersionIdTag>(r);
          record.predecessor_generation = read_id<ModelGenerationTag>(r);
          read_binding(r, limits, record.artifact);
          read_requirements(r, limits, record.requirements);
          const std::uint8_t state = r.u8();
          if (state >= kLifecycleStateCount) {
            return {StateStoreStatus::InvalidContent, "impossible lifecycle state"};
          }
          record.state = static_cast<LifecycleState>(state);
          record.lifecycle_generation = read_id<LifecycleGenerationTag>(r);
          record.last_promotion = read_id<PromotionIdTag>(r);
          record.promotion_generation = read_id<PromotionGenerationTag>(r);
          record.rollout = read_id<RolloutIdTag>(r);
          record.rollout_generation = read_id<RolloutGenerationTag>(r);
          record.current_stage = read_id<RolloutStageIdTag>(r);
          record.current_stage_generation = read_id<RolloutStageGenerationTag>(r);
          record.compatibility_generation = read_id<CompatibilityGenerationTag>(r);
          record.last_rollback = read_id<RollbackIdTag>(r);
          record.rollback_generation = read_id<RollbackGenerationTag>(r);
          const std::uint8_t provenance = r.u8();
          if (provenance > 3) return {StateStoreStatus::InvalidContent, "bad provenance"};
          record.provenance = static_cast<Provenance>(provenance);
          record.created_sequence = read_id<SequenceTag>(r);
          record.updated_sequence = read_id<SequenceTag>(r);
          if (!record.model.valid() || !record.version.valid()) {
            return {StateStoreStatus::InvalidContent, "version identity is zero"};
          }
          if (!record.generation.valid()) {
            return {StateStoreStatus::InvalidContent, "model generation is zero"};
          }
          if (!record.lifecycle_generation.valid()) {
            return {StateStoreStatus::InvalidContent, "lifecycle generation is zero"};
          }
          const auto key = std::make_pair(record.model.raw(), record.version.raw());
          if (decoded.versions.find(key) != decoded.versions.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate model version id"};
          }
          decoded.versions[key] = std::move(record);
        }
        break;
      }
      case Section::Compatibility: {
        if (count > limits.max_compatibility_facts) {
          return {StateStoreStatus::BoundsExceeded, "compatibility count exceeds the bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          CompatibilityFact fact;
          fact.model = read_id<ModelIdTag>(r);
          fact.version = read_id<ModelVersionIdTag>(r);
          fact.model_generation = read_id<ModelGenerationTag>(r);
          fact.artifact_set = read_id<ArtifactSetIdTag>(r);
          fact.artifact_generation = read_id<ArtifactGenerationTag>(r);
          fact.generation = read_id<CompatibilityGenerationTag>(r);
          fact.environment_key = r.str(limits.max_string);
          const std::uint8_t outcome = r.u8();
          if (outcome >= kCompatibilityOutcomeCount) {
            return {StateStoreStatus::InvalidContent, "unknown compatibility outcome"};
          }
          fact.outcome = static_cast<CompatibilityOutcome>(outcome);
          fact.detail = r.str(limits.max_string * 2);
          const std::uint8_t provenance = r.u8();
          if (provenance > 3) return {StateStoreStatus::InvalidContent, "bad provenance"};
          fact.provenance = static_cast<Provenance>(provenance);
          fact.sequence = read_id<SequenceTag>(r);
          if (!fact.version.valid() || !fact.generation.valid() || fact.environment_key.empty()) {
            return {StateStoreStatus::InvalidContent, "invalid compatibility fact identity"};
          }
          const auto key = std::make_pair(fact.version.raw(), fact.environment_key);
          if (decoded.compatibility.find(key) != decoded.compatibility.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate compatibility fact"};
          }
          decoded.compatibility[key] = std::move(fact);
        }
        break;
      }
      case Section::Rollouts: {
        if (count > limits.max_rollouts) {
          return {StateStoreStatus::BoundsExceeded, "rollout count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          RolloutPlan plan;
          plan.id = read_id<RolloutIdTag>(r);
          plan.generation = read_id<RolloutGenerationTag>(r);
          plan.model = read_id<ModelIdTag>(r);
          plan.candidate = read_id<ModelVersionIdTag>(r);
          plan.candidate_generation = read_id<ModelGenerationTag>(r);
          plan.candidate_artifact_set = read_id<ArtifactSetIdTag>(r);
          plan.candidate_artifact_generation = read_id<ArtifactGenerationTag>(r);
          plan.previous = read_id<ModelVersionIdTag>(r);
          plan.previous_generation = read_id<ModelGenerationTag>(r);
          plan.compatibility_generation = read_id<CompatibilityGenerationTag>(r);
          plan.policy_generation = read_id<PolicyGenerationTag>(r);
          plan.evidence_generation = read_id<EvidenceGenerationTag>(r);
          plan.root_scope = read_id<ScopeIdTag>(r);
          plan.root_scope_generation = read_id<ScopeGenerationTag>(r);
          const std::uint8_t strategy = r.u8();
          if (strategy >= kRolloutStrategyCount) {
            return {StateStoreStatus::InvalidContent, "unknown rollout strategy"};
          }
          plan.strategy = static_cast<RolloutStrategy>(strategy);
          plan.promotion = read_id<PromotionIdTag>(r);
          plan.promotion_generation = read_id<PromotionGenerationTag>(r);
          plan.rollback_target = read_id<ModelVersionIdTag>(r);
          plan.rollback_target_generation = read_id<ModelGenerationTag>(r);
          plan.max_blast_radius_percent = r.u32();
          plan.manual_progression = r.boolean();
          const std::uint32_t cohort_count = r.u32();
          if (!r.ok() || cohort_count > limits.max_cohorts_per_rollout) {
            return {StateStoreStatus::BoundsExceeded, "cohort count exceeds the decode bound"};
          }
          for (std::uint32_t c = 0; c < cohort_count; ++c) {
            CohortDefinition cohort;
            cohort.id = read_id<CohortIdTag>(r);
            cohort.name = r.str(limits.max_string);
            const std::uint32_t scope_count = r.u32();
            if (!r.ok() || scope_count > limits.max_scopes) {
              return {StateStoreStatus::BoundsExceeded, "cohort scope count out of range"};
            }
            for (std::uint32_t s = 0; s < scope_count; ++s) {
              cohort.scopes.push_back(read_id<ScopeIdTag>(r));
            }
            cohort.traffic_percent = r.u32();
            cohort.max_blast_radius_percent = r.u32();
            if (cohort.traffic_percent > 100 || cohort.max_blast_radius_percent > 100) {
              return {StateStoreStatus::InvalidContent, "cohort percentage out of range"};
            }
            plan.cohorts.push_back(std::move(cohort));
          }
          const std::uint32_t stage_count = r.u32();
          if (!r.ok() || stage_count > limits.max_stages_per_rollout) {
            return {StateStoreStatus::BoundsExceeded, "stage count exceeds the decode bound"};
          }
          for (std::uint32_t s = 0; s < stage_count; ++s) {
            StageDefinition stage;
            stage.id = read_id<RolloutStageIdTag>(r);
            stage.name = r.str(limits.max_string);
            const std::uint32_t stage_cohorts = r.u32();
            if (!r.ok() || stage_cohorts > limits.max_cohorts_per_rollout) {
              return {StateStoreStatus::BoundsExceeded, "stage cohort count out of range"};
            }
            for (std::uint32_t c = 0; c < stage_cohorts; ++c) {
              stage.cohorts.push_back(read_id<CohortIdTag>(r));
            }
            const std::uint32_t requirements = r.u32();
            if (!r.ok() || requirements > kEvidenceKindCount * 4) {
              return {StateStoreStatus::BoundsExceeded, "stage requirement count out of range"};
            }
            for (std::uint32_t k = 0; k < requirements; ++k) {
              EvidenceRequirement requirement;
              read_evidence_requirement(r, requirement);
              if (!valid_evidence_kind(static_cast<std::uint8_t>(requirement.kind)) ||
                  !valid_verdict(static_cast<std::uint8_t>(requirement.required_verdict))) {
                return {StateStoreStatus::InvalidContent, "invalid evidence requirement"};
              }
              stage.required_evidence.push_back(requirement);
            }
            const std::uint32_t acceptance = r.u32();
            if (!r.ok() || acceptance > 64) {
              return {StateStoreStatus::BoundsExceeded, "acceptance criteria out of range"};
            }
            for (std::uint32_t k = 0; k < acceptance; ++k) {
              Criterion criterion;
              read_criterion(r, criterion);
              if (!valid_evidence_kind(static_cast<std::uint8_t>(criterion.kind))) {
                return {StateStoreStatus::InvalidContent, "invalid acceptance criterion"};
              }
              stage.acceptance.push_back(criterion);
            }
            const std::uint32_t failure = r.u32();
            if (!r.ok() || failure > 64) {
              return {StateStoreStatus::BoundsExceeded, "failure criteria out of range"};
            }
            for (std::uint32_t k = 0; k < failure; ++k) {
              Criterion criterion;
              read_criterion(r, criterion);
              if (!valid_evidence_kind(static_cast<std::uint8_t>(criterion.kind))) {
                return {StateStoreStatus::InvalidContent, "invalid failure criterion"};
              }
              stage.failure.push_back(criterion);
            }
            stage.require_residency_before_entry = r.boolean();
            stage.require_drain_after_commit = r.boolean();
            plan.stages.push_back(std::move(stage));
          }
          plan.current_stage_index = r.u32();
          plan.current_stage_generation = read_id<RolloutStageGenerationTag>(r);
          plan.stage_entered = r.boolean();
          const std::uint32_t history = r.u32();
          if (!r.ok() || history > limits.max_history_per_rollout) {
            return {StateStoreStatus::BoundsExceeded, "stage history exceeds the decode bound"};
          }
          for (std::uint32_t h = 0; h < history; ++h) {
            StageRecord record;
            record.stage = read_id<RolloutStageIdTag>(r);
            record.generation = read_id<RolloutStageGenerationTag>(r);
            record.entered_sequence = read_id<SequenceTag>(r);
            record.decided_sequence = read_id<SequenceTag>(r);
            const std::uint8_t decision = r.u8();
            if (decision > 5) return {StateStoreStatus::InvalidContent, "unknown stage decision"};
            record.decision = static_cast<StageDecision>(decision);
            const std::uint32_t reasons = r.u32();
            if (!r.ok() || reasons > 64) {
              return {StateStoreStatus::BoundsExceeded, "stage reason count out of range"};
            }
            for (std::uint32_t k = 0; k < reasons; ++k) {
              const std::uint16_t code = r.u16();
              Reason reason;
              reason.code = static_cast<ReasonCode>(code);
              reason.detail = r.str(limits.max_string);
              record.reasons.push_back(std::move(reason));
            }
            plan.history.push_back(std::move(record));
          }
          plan.completed = r.boolean();
          plan.failed = r.boolean();
          plan.superseded = r.boolean();
          plan.last_rollback = read_id<RollbackIdTag>(r);
          plan.last_rollback_generation = read_id<RollbackGenerationTag>(r);
          plan.created_sequence = read_id<SequenceTag>(r);
          plan.updated_sequence = read_id<SequenceTag>(r);
          const std::uint8_t provenance = r.u8();
          if (provenance > 3) return {StateStoreStatus::InvalidContent, "bad provenance"};
          plan.provenance = static_cast<Provenance>(provenance);
          if (!plan.id.valid() || !plan.generation.valid() || !plan.candidate.valid()) {
            return {StateStoreStatus::InvalidContent, "invalid rollout identity"};
          }
          if (plan.stages.size() > limits.max_stages_per_rollout) {
            return {StateStoreStatus::BoundsExceeded, "stage count exceeds the decode bound"};
          }
          if (decoded.rollouts.find(plan.id.raw()) != decoded.rollouts.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate rollout id"};
          }
          decoded.rollouts[plan.id.raw()] = std::move(plan);
        }
        break;
      }
      case Section::Authority: {
        for (std::uint32_t i = 0; i < count; ++i) {
          const std::uint32_t binding_count = r.u32();
          if (!r.ok() || binding_count > limits.max_authority_bindings) {
            return {StateStoreStatus::BoundsExceeded, "authority binding count out of range"};
          }
          std::vector<AuthorityBinding> bindings;
          bindings.reserve(binding_count);
          for (std::uint32_t b = 0; b < binding_count; ++b) {
            AuthorityBinding binding;
            binding.scope = read_id<ScopeIdTag>(r);
            binding.scope_generation = read_id<ScopeGenerationTag>(r);
            binding.model = read_id<ModelIdTag>(r);
            binding.version = read_id<ModelVersionIdTag>(r);
            binding.model_generation = read_id<ModelGenerationTag>(r);
            binding.artifact_set = read_id<ArtifactSetIdTag>(r);
            binding.artifact_generation = read_id<ArtifactGenerationTag>(r);
            const std::uint8_t state = r.u8();
            if (state >= kLifecycleStateCount) {
              return {StateStoreStatus::InvalidContent, "impossible lifecycle state in authority"};
            }
            binding.lifecycle_state = static_cast<LifecycleState>(state);
            const std::uint8_t kind = r.u8();
            if (kind > 4) return {StateStoreStatus::InvalidContent, "unknown authority kind"};
            binding.kind = static_cast<AuthorityKind>(kind);
            binding.rollout = read_id<RolloutIdTag>(r);
            binding.rollout_generation = read_id<RolloutGenerationTag>(r);
            binding.stage = read_id<RolloutStageIdTag>(r);
            binding.stage_generation = read_id<RolloutStageGenerationTag>(r);
            binding.promotion = read_id<PromotionIdTag>(r);
            binding.promotion_generation = read_id<PromotionGenerationTag>(r);
            binding.rollback = read_id<RollbackIdTag>(r);
            binding.rollback_generation = read_id<RollbackGenerationTag>(r);
            binding.epoch = read_id<CoordinatorEpochTag>(r);
            binding.evidence_generation = read_id<EvidenceGenerationTag>(r);
            binding.granted_sequence = read_id<SequenceTag>(r);
            binding.superseded = false;
            if (!binding.scope.valid() || !binding.model.valid() || !binding.version.valid()) {
              return {StateStoreStatus::InvalidContent, "invalid authority binding identity"};
            }
            bindings.push_back(binding);
          }
          if (bindings.empty()) continue;
          const std::uint64_t scope_raw = bindings.front().scope.raw();
          if (decoded.authority.find(scope_raw) != decoded.authority.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate authority scope entry"};
          }
          decoded.authority[scope_raw] = std::move(bindings);
        }
        break;
      }
      case Section::Workers: {
        if (count > limits.max_workers) {
          return {StateStoreStatus::BoundsExceeded, "worker count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          WorkerLease lease;
          lease.id = read_id<WorkerIdTag>(r);
          lease.boot = read_id<WorkerBootIdTag>(r);
          lease.epoch = read_id<CoordinatorEpochTag>(r);
          const std::uint32_t scope_count = r.u32();
          if (!r.ok() || scope_count > limits.max_scopes) {
            return {StateStoreStatus::BoundsExceeded, "worker scope count out of range"};
          }
          for (std::uint32_t s = 0; s < scope_count; ++s) {
            lease.scopes.push_back(read_id<ScopeIdTag>(r));
          }
          lease.fenced = r.boolean();
          lease.registered_sequence = read_id<SequenceTag>(r);
          lease.fenced_sequence = read_id<SequenceTag>(r);
          lease.detail = r.str(limits.max_string);
          if (!lease.id.valid() || !lease.boot.valid()) {
            return {StateStoreStatus::InvalidContent, "invalid worker lease identity"};
          }
          if (decoded.worker_leases.find(lease.id.raw()) != decoded.worker_leases.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate worker lease"};
          }
          decoded.worker_leases[lease.id.raw()] = std::move(lease);
        }
        break;
      }
      case Section::Evidence: {
        if (count > limits.max_evidence) {
          return {StateStoreStatus::BoundsExceeded, "evidence count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          EvidenceRecord record;
          record.id = read_id<EvidenceIdTag>(r);
          record.generation = read_id<EvidenceGenerationTag>(r);
          const std::uint8_t kind = r.u8();
          if (!valid_evidence_kind(kind)) {
            return {StateStoreStatus::InvalidContent, "unknown evidence kind"};
          }
          record.kind = static_cast<EvidenceKind>(kind);
          const std::uint8_t verdict = r.u8();
          if (!valid_verdict(verdict)) {
            return {StateStoreStatus::InvalidContent, "unknown evidence verdict"};
          }
          record.verdict = static_cast<EvidenceVerdict>(verdict);
          read_subject(r, record.subject);
          const std::uint8_t provenance = r.u8();
          if (provenance > 3) return {StateStoreStatus::InvalidContent, "bad provenance"};
          record.provenance = static_cast<Provenance>(provenance);
          record.worker = read_id<WorkerIdTag>(r);
          record.boot = read_id<WorkerBootIdTag>(r);
          record.epoch = read_id<CoordinatorEpochTag>(r);
          record.policy_generation = read_id<PolicyGenerationTag>(r);
          record.value = r.f64();
          record.unit = r.str(limits.max_string / 4);
          record.tick = r.u64();
          record.detail = r.str(limits.max_string * 2);
          if (!record.subject.version.valid() || !record.generation.valid()) {
            return {StateStoreStatus::InvalidContent, "invalid evidence identity"};
          }
          decoded.evidence.push_back(std::move(record));
        }
        break;
      }
      case Section::Attempts: {
        if (count > limits.max_attempts) {
          return {StateStoreStatus::BoundsExceeded, "attempt count exceeds the decode bound"};
        }
        for (std::uint32_t i = 0; i < count; ++i) {
          Attempt attempt;
          attempt.id = read_id<AttemptIdTag>(r);
          const std::uint8_t kind = r.u8();
          if (kind >= kAttemptKindCount) {
            return {StateStoreStatus::InvalidContent, "unknown attempt kind"};
          }
          attempt.kind = static_cast<AttemptKind>(kind);
          attempt.worker = read_id<WorkerIdTag>(r);
          attempt.boot = read_id<WorkerBootIdTag>(r);
          attempt.epoch = read_id<CoordinatorEpochTag>(r);
          attempt.model = read_id<ModelIdTag>(r);
          attempt.version = read_id<ModelVersionIdTag>(r);
          attempt.model_generation = read_id<ModelGenerationTag>(r);
          attempt.artifact_generation = read_id<ArtifactGenerationTag>(r);
          attempt.scope = read_id<ScopeIdTag>(r);
          attempt.scope_generation = read_id<ScopeGenerationTag>(r);
          attempt.rollout = read_id<RolloutIdTag>(r);
          attempt.rollout_generation = read_id<RolloutGenerationTag>(r);
          attempt.stage = read_id<RolloutStageIdTag>(r);
          attempt.stage_generation = read_id<RolloutStageGenerationTag>(r);
          attempt.residency_generation = read_id<ResidencyGenerationTag>(r);
          const std::uint8_t state = r.u8();
          if (state > 5) return {StateStoreStatus::InvalidContent, "unknown attempt state"};
          attempt.state = static_cast<AttemptState>(state);
          attempt.idempotent = r.boolean();
          attempt.issued_sequence = read_id<SequenceTag>(r);
          attempt.settled_sequence = read_id<SequenceTag>(r);
          attempt.detail = r.str(limits.max_string);
          if (!attempt.id.valid()) return {StateStoreStatus::InvalidContent, "attempt id is zero"};
          if (decoded.unsettled_attempts.find(attempt.id.raw()) != decoded.unsettled_attempts.end()) {
            return {StateStoreStatus::InvalidContent, "duplicate attempt id"};
          }
          decoded.unsettled_attempts[attempt.id.raw()] = std::move(attempt);
        }
        break;
      }
      case Section::Committed: {
        if (count != 2) return {StateStoreStatus::InvalidContent, "bad committed section shape"};
        const std::uint32_t promotions = r.u32();
        if (!r.ok() || promotions > limits.max_models * 64u) {
          return {StateStoreStatus::BoundsExceeded, "committed promotion count out of range"};
        }
        for (std::uint32_t i = 0; i < promotions; ++i) {
          const std::uint64_t id = r.u64();
          if (!decoded.committed_promotions.insert(id).second) {
            return {StateStoreStatus::InvalidContent, "duplicate committed promotion"};
          }
        }
        const std::uint32_t rollbacks = r.u32();
        if (!r.ok() || rollbacks > limits.max_models * 64u) {
          return {StateStoreStatus::BoundsExceeded, "committed rollback count out of range"};
        }
        for (std::uint32_t i = 0; i < rollbacks; ++i) {
          const std::uint64_t id = r.u64();
          const std::uint64_t generation = r.u64();
          if (!decoded.committed_rollbacks.emplace(id, generation).second) {
            return {StateStoreStatus::InvalidContent, "duplicate committed rollback"};
          }
        }
        break;
      }
      case Section::Allocators: {
        if (count != 13) return {StateStoreStatus::InvalidContent, "bad allocator section shape"};
        decoded.next_model_id = r.u64();
        decoded.next_version_id = r.u64();
        decoded.next_rollout_id = r.u64();
        decoded.next_stage_id = r.u64();
        decoded.next_cohort_id = r.u64();
        decoded.next_promotion_id = r.u64();
        decoded.next_rollback_id = r.u64();
        decoded.next_attempt_id = r.u64();
        decoded.next_worker_id = r.u64();
        decoded.next_evidence_id = r.u64();
        decoded.next_evidence_generation = r.u64();
        decoded.next_compatibility_generation = r.u64();
        static_cast<void>(r.u64());
        if (decoded.next_model_id == 0 || decoded.next_version_id == 0 ||
            decoded.next_rollout_id == 0 || decoded.next_promotion_id == 0 ||
            decoded.next_rollback_id == 0 || decoded.next_attempt_id == 0 ||
            decoded.next_worker_id == 0 || decoded.next_evidence_id == 0 ||
            decoded.next_compatibility_generation == 0) {
          return {StateStoreStatus::InvalidContent, "allocator counters must be non-zero"};
        }
        break;
      }
      default:
        return {StateStoreStatus::InvalidContent, "unknown section tag"};
    }
    if (!r.ok()) return {StateStoreStatus::Truncated, "section payload truncated"};
  }

  if (!r.ok()) return {StateStoreStatus::Truncated, "state payload truncated"};
  if (!r.exhausted()) return {StateStoreStatus::InvalidContent, "trailing bytes in state payload"};

  // Cross-section consistency. A structurally valid file can still describe an
  // impossible lifecycle world; that is rejected here rather than partially
  // applied.
  if (decoded.scopes.empty()) {
    return {StateStoreStatus::InvalidContent, "state contains no scope tree"};
  }
  if (decoded.scopes.front().id.raw() != 1) {
    return {StateStoreStatus::InvalidContent, "scope identity is not contiguous from one"};
  }
  for (std::size_t i = 0; i < decoded.scopes.size(); ++i) {
    if (decoded.scopes[i].id.raw() != static_cast<std::uint64_t>(i) + 1) {
      return {StateStoreStatus::InvalidContent, "scope identity is not contiguous"};
    }
  }
  for (const auto& entry : decoded.versions) {
    if (decoded.models.find(entry.second.model.raw()) == decoded.models.end()) {
      return {StateStoreStatus::InvalidContent, "version references an unknown model"};
    }
    const ModelVersionRecord& record = entry.second;
    if (record.retired() && record.state != LifecycleState::RETIRED) {
      return {StateStoreStatus::InvalidContent, "retired-but-not-retired contradiction"};
    }
    if (record.predecessor.valid()) {
      if (record.predecessor == record.version && record.model == record.model) {
        return {StateStoreStatus::InvalidContent, "version is its own predecessor"};
      }
      const auto parent = std::make_pair(record.model.raw(), record.predecessor.raw());
      const auto found = decoded.versions.find(parent);
      if (found == decoded.versions.end()) {
        return {StateStoreStatus::InvalidContent, "version references an unknown predecessor"};
      }
      if (found->second.predecessor == record.version) {
        return {StateStoreStatus::InvalidContent, "predecessor cycle between two versions"};
      }
    }
  }
  for (const auto& entry : decoded.compatibility) {
    const auto key = std::make_pair(entry.second.model.raw(), entry.second.version.raw());
    if (decoded.versions.find(key) == decoded.versions.end()) {
      return {StateStoreStatus::InvalidContent, "compatibility fact references an unknown version"};
    }
  }
  for (const auto& entry : decoded.rollouts) {
    const RolloutPlan& plan = entry.second;
    const auto key = std::make_pair(plan.model.raw(), plan.candidate.raw());
    const auto found = decoded.versions.find(key);
    if (found == decoded.versions.end()) {
      return {StateStoreStatus::InvalidContent, "rollout references an unknown candidate version"};
    }
    if (plan.stages.empty()) {
      return {StateStoreStatus::InvalidContent, "rollout has no stages"};
    }
    if (!plan.live() && plan.current_stage_generation.valid() &&
        plan.history.empty()) {
      return {StateStoreStatus::InvalidContent, "completed rollout has no stage history"};
    }
    for (const CohortDefinition& cohort : plan.cohorts) {
      for (ScopeId scope : cohort.scopes) {
        if (scope.raw() == 0 || scope.raw() > decoded.scopes.size()) {
          return {StateStoreStatus::InvalidContent, "cohort references an unknown scope"};
        }
      }
    }
    for (const StageDefinition& stage : plan.stages) {
      for (CohortId cohort : stage.cohorts) {
        bool found_cohort = false;
        for (const CohortDefinition& candidate : plan.cohorts) {
          if (candidate.id == cohort) found_cohort = true;
        }
        if (!found_cohort) {
          return {StateStoreStatus::InvalidContent, "stage references an unknown cohort"};
        }
      }
    }
    if (plan.current_stage_index >= plan.stages.size() && plan.live()) {
      return {StateStoreStatus::InvalidContent, "live rollout stage index out of range"};
    }
  }
  for (const auto& entry : decoded.authority) {
    for (const AuthorityBinding& binding : entry.second) {
      const auto key = std::make_pair(binding.model.raw(), binding.version.raw());
      if (decoded.versions.find(key) == decoded.versions.end()) {
        return {StateStoreStatus::InvalidContent, "authority references an unknown version"};
      }
      if (binding.scope.raw() == 0 || binding.scope.raw() > decoded.scopes.size()) {
        return {StateStoreStatus::InvalidContent, "authority references an unknown scope"};
      }
      if (binding.rollout.valid()) {
        const auto rollout = decoded.rollouts.find(binding.rollout.raw());
        if (rollout == decoded.rollouts.end()) {
          return {StateStoreStatus::InvalidContent, "authority references an unknown rollout"};
        }
        if (rollout->second.generation != binding.rollout_generation) {
          return {StateStoreStatus::InvalidContent,
                  "authority rollout generation does not match the stored rollout"};
        }
      }
      const ModelVersionRecord& record = decoded.versions.at(key);
      if (record.retired()) {
        return {StateStoreStatus::InvalidContent, "retired generation still holds authority"};
      }
      if (binding.model_generation > record.generation) {
        return {StateStoreStatus::InvalidContent,
                "authority cites a model generation newer than the version record"};
      }
    }
  }
  for (const auto& entry : decoded.policy_overrides) {
    if (entry.first == 0 || entry.first > decoded.scopes.size()) {
      return {StateStoreStatus::InvalidContent, "policy override references an unknown scope"};
    }
  }
  for (const EvidenceRecord& record : decoded.evidence) {
    const auto key = std::make_pair(record.subject.model.raw(), record.subject.version.raw());
    if (decoded.versions.find(key) == decoded.versions.end()) {
      return {StateStoreStatus::InvalidContent, "evidence references an unknown version"};
    }
    switch (record.kind) {
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
        break;
      default:
        return {StateStoreStatus::InvalidContent,
                "dynamic evidence kind must not appear in durable state"};
    }
  }

  out = std::move(decoded);
  return result;
}

StateStoreResult save_durable_state(const DurableState& state, const DecodeLimits& limits,
                                    const std::string& path) {
  std::vector<std::uint8_t> bytes;
  StateStoreResult encoded = serialize_durable_state(state, limits, bytes);
  if (!encoded.ok()) return encoded;

  const std::filesystem::path destination(path);
  std::filesystem::path temporary = destination;
  temporary += ".tmp";

  std::error_code ec;
  if (destination.has_parent_path()) {
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) return {StateStoreStatus::IoError, "cannot create state directory: " + ec.message()};
  }

  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) return {StateStoreStatus::IoError, "cannot open temporary state file"};
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    if (!stream) return {StateStoreStatus::IoError, "temporary state file write failed"};
  }

  std::filesystem::rename(temporary, destination, ec);
  if (ec) {
    std::error_code remove_ec;
    std::filesystem::remove(destination, remove_ec);
    std::error_code retry_ec;
    std::filesystem::rename(temporary, destination, retry_ec);
    if (retry_ec) {
      std::error_code cleanup_ec;
      std::filesystem::remove(temporary, cleanup_ec);
      return {StateStoreStatus::AtomicReplaceFailed,
              "atomic replacement failed: " + retry_ec.message()};
    }
  }
  return {};
}

StateStoreResult load_durable_state(const std::string& path, const DecodeLimits& limits,
                                    DurableState& out) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) return {StateStoreStatus::IoError, "cannot open state file"};
  const std::streampos end = stream.tellg();
  if (end < 0) return {StateStoreStatus::IoError, "cannot size state file"};
  const auto size = static_cast<std::size_t>(end);
  if (size == 0) return {StateStoreStatus::Truncated, "state file is empty"};
  if (size > limits.max_payload_bytes + kHeaderSize + kTrailerSize) {
    return {StateStoreStatus::BoundsExceeded, "state file exceeds the decode bound"};
  }
  stream.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(size);
  stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
  if (stream.gcount() != static_cast<std::streamsize>(size)) {
    return {StateStoreStatus::Truncated, "state file could not be read completely"};
  }
  return deserialize_durable_state(bytes.data(), bytes.size(), limits, out);
}

}  // namespace mlf
